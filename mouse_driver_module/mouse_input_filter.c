#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define DRV_NAME "mouse_input_filter"
#define PROC_LOG_NAME "mouse_log"
#define PROC_ENTROPY_NAME "mouse_entropy"

#define LOG_RING_SIZE 256
#define LOG_LINE_SIZE 128
#define ENTROPY_RING_SIZE 1024
#define RIGHT_QUEUE_SIZE 32

struct mouse_log_entry {
	struct timespec64 ts;
	char line[LOG_LINE_SIZE];
};

struct mouse_entropy_sample {
	struct timespec64 ts;
	s16 dx;
	s16 dy;
};

struct mouse_ctx {
	struct input_handle handle;
	int pending_dx;
	int pending_dy;
	struct work_struct right_click_work;
	spinlock_t right_lock;
	int right_queue[RIGHT_QUEUE_SIZE];
	unsigned int right_head;
	unsigned int right_tail;
	unsigned int right_count;
	bool injecting_right;
};

static struct proc_dir_entry *proc_log_entry;
static struct proc_dir_entry *proc_entropy_entry;

/* Log text de debug nhanh hanh vi filter (doc qua /proc/mouse_log). */
static struct mouse_log_entry log_ring[LOG_RING_SIZE];
static unsigned int log_head;
static unsigned int log_count;
static DEFINE_SPINLOCK(log_lock);

/* Luu mau chuyen dong thuc te (dx/dy) de trich entropy qua /proc/mouse_entropy. */
static struct mouse_entropy_sample entropy_ring[ENTROPY_RING_SIZE];
static unsigned int entropy_head;
static unsigned int entropy_count;
static DEFINE_SPINLOCK(entropy_lock);

static bool logging_enabled = true;

/*
 * Muc dich: ghi 1 dong log vao ring buffer.
 * Khi goi: duoc goi boi filter/worker/connect/disconnect moi khi can de lai dau vet.
 */
static void mouse_log_add(bool force, const char *fmt, ...)
{
	struct mouse_log_entry entry;
	unsigned long flags;
	va_list args;

	if (!force && !READ_ONCE(logging_enabled))
		return;

	ktime_get_real_ts64(&entry.ts);

	va_start(args, fmt);
	vscnprintf(entry.line, sizeof(entry.line), fmt, args);
	va_end(args);

	spin_lock_irqsave(&log_lock, flags);
	log_ring[log_head] = entry;
	log_head = (log_head + 1) % LOG_RING_SIZE;
	if (log_count < LOG_RING_SIZE)
		log_count++;
	spin_unlock_irqrestore(&log_lock, flags);
}

/*
 * Muc dich: cat 1 mau chuyen dong (dx, dy) vao entropy ring.
 * Khi goi: sau moi SYN_REPORT, khi da gom xong delta REL_X/REL_Y trong 1 frame.
 */
static void mouse_entropy_add(int dx, int dy)
{
	struct mouse_entropy_sample sample;
	unsigned long flags;

	ktime_get_real_ts64(&sample.ts);
	sample.dx = (s16)dx;
	sample.dy = (s16)dy;

	spin_lock_irqsave(&entropy_lock, flags);
	entropy_ring[entropy_head] = sample;
	entropy_head = (entropy_head + 1) % ENTROPY_RING_SIZE;
	if (entropy_count < ENTROPY_RING_SIZE)
		entropy_count++;
	spin_unlock_irqrestore(&entropy_lock, flags);
}

/*
 * Muc dich: day cap dx/dy dang pending ra log + entropy roi reset ve 0.
 * Khi goi: o moc SYN_REPORT, de gom event REL_X/REL_Y theo tung frame chuot.
 */
static void mouse_flush_pending_move(struct mouse_ctx *ctx)
{
	if (!ctx->pending_dx && !ctx->pending_dy)
		return;

	/*
	 * dx/dy la tong delta trong 1 frame SYN_REPORT, khong phai toa do tuyet doi.
	 * Gom theo frame de log/entropy on dinh hon viec xu ly tung event roi rac.
	 */
	mouse_log_add(false, "MOVE: dx=%d dy=%d", ctx->pending_dx, ctx->pending_dy);
	mouse_entropy_add(ctx->pending_dx, ctx->pending_dy);

	ctx->pending_dx = 0;
	ctx->pending_dy = 0;
}

/*
 * Muc dich: worker bat tay viec doi left-click thanh right-click.
 * Khi goi: duoc schedule boi mouse_queue_right_click sau khi filter bat BTN_LEFT.
 *
 * Ly do dung workqueue: filter callback chay rat sau trong input path;
 * inject truc tiep tai do de gay de quy vao input core va de treo may.
 */
static void mouse_right_click_worker(struct work_struct *work)
{
	struct mouse_ctx *ctx = container_of(work, struct mouse_ctx, right_click_work);
	unsigned long flags;
	int value;

	for (;;) {
		spin_lock_irqsave(&ctx->right_lock, flags);
		if (!ctx->right_count) {
			spin_unlock_irqrestore(&ctx->right_lock, flags);
			break;
		}

		value = ctx->right_queue[ctx->right_tail];
		ctx->right_tail = (ctx->right_tail + 1) % RIGHT_QUEUE_SIZE;
		ctx->right_count--;
		spin_unlock_irqrestore(&ctx->right_lock, flags);

		WRITE_ONCE(ctx->injecting_right, true);
		input_inject_event(&ctx->handle, EV_KEY, BTN_RIGHT, value);
		input_inject_event(&ctx->handle, EV_SYN, SYN_REPORT, 0);
		WRITE_ONCE(ctx->injecting_right, false);

		mouse_log_add(false, "BTN_LEFT converted -> BTN_RIGHT (value=%d)", value);
	}
}

/*
 * Muc dich: queue su kien left-click de worker xu ly bat dong bo.
 * Khi goi: trong mouse_filter ngay khi bat gap BTN_LEFT.
 */
static void mouse_queue_right_click(struct mouse_ctx *ctx, int value)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->right_lock, flags);
	if (ctx->right_count < RIGHT_QUEUE_SIZE) {
		ctx->right_queue[ctx->right_head] = value;
		ctx->right_head = (ctx->right_head + 1) % RIGHT_QUEUE_SIZE;
		ctx->right_count++;
		spin_unlock_irqrestore(&ctx->right_lock, flags);
		schedule_work(&ctx->right_click_work);
		return;
	}
	spin_unlock_irqrestore(&ctx->right_lock, flags);

	/* Queue day thi bo event cho an toan, tranh block duong input chinh. */
	mouse_log_add(true, "WARN: right-click queue full, dropping event");
}

/*
 * Muc dich: day la diem cham chinh cua input_handler de can thiep event chuot.
 * Khi goi: input core goi truoc khi event duoc day toi cac handler user-space (evdev,...).
 *
 * Vi sao intercept o day:
 * - Day la cho som nhat de doi/chuan hoa hanh vi click theo y do module.
 * - Neu de muon hon (sau evdev) thi user-space da nhan event goc roi, khong chan duoc.
 *
 * Rule nho de tranh nham:
 * - return true  => an event goc (block).
 * - return false => cho event di tiep.
 *
 * Luong du lieu phia input:
 * Mouse HW -> input core -> mouse_filter -> pending dx/dy + queue click ->
 * worker/proc ring -> user-space doc /proc.
 */
static bool mouse_filter(struct input_handle *handle,
			 unsigned int type, unsigned int code, int value)
{
	struct mouse_ctx *ctx = container_of(handle, struct mouse_ctx, handle);

	switch (type) {
	case EV_REL:
		if (code == REL_X) {
			/* dx la delta ngang theo tung event REL_X. */
			ctx->pending_dx += value;
			return false;
		}

		if (code == REL_Y) {
			/* dy la delta doc theo tung event REL_Y. */
			ctx->pending_dy += value;
			return false;
		}

		if (code == REL_WHEEL)
			mouse_log_add(false, "WHEEL: delta=%d", value);

		return false;

	case EV_SYN:
		if (code == SYN_REPORT)
			mouse_flush_pending_move(ctx);
		return false;

	case EV_KEY:
		/* Bat left click de doi qua right click theo yeu cau module. */
		if (code == BTN_LEFT) {
			mouse_queue_right_click(ctx, value);
			/* Chan event left goc, chi giu event da doi o worker. */
			return true;
		}

		/* Cho su kien right click do chinh worker inject quay lai. */
		if (code == BTN_RIGHT && READ_ONCE(ctx->injecting_right))
			return false;

		/* Right click that tu thiet bi bi chan hoan toan. */
		if (code == BTN_RIGHT) {
			mouse_log_add(false, "BTN_RIGHT blocked (value=%d)", value);
			return true;
		}

		/* Middle click dung lam nut bat/tat log nhanh luc test. */
		if (code == BTN_MIDDLE && value == 1) {
			bool new_state = !READ_ONCE(logging_enabled);

			WRITE_ONCE(logging_enabled, new_state);
			mouse_log_add(true, "LOGGING %s", new_state ? "ON" : "OFF");
			pr_info(DRV_NAME ": logging %s\n", new_state ? "enabled" : "disabled");
		}

		return false;

	default:
		return false;
	}
}

/*
 * Muc dich: dump snapshot log_ring ra /proc/mouse_log.
 * Khi goi: moi lan user-space read file proc.
 */
static int mouse_log_proc_show(struct seq_file *m, void *v)
{
	struct mouse_log_entry *snapshot;
	unsigned long flags;
	unsigned int i;
	unsigned int count;
	unsigned int head;

	snapshot = kcalloc(LOG_RING_SIZE, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	/* Copy snapshot duoi lock, in ra sau de tranh giu lock qua lau. */
	spin_lock_irqsave(&log_lock, flags);
	count = log_count;
	head = log_head;
	for (i = 0; i < count; i++) {
		unsigned int idx = (head + LOG_RING_SIZE - count + i) % LOG_RING_SIZE;

		snapshot[i] = log_ring[idx];
	}
	spin_unlock_irqrestore(&log_lock, flags);

	for (i = 0; i < count; i++) {
		seq_printf(m, "[%5lld.%03ld] %s\n",
			   (long long)snapshot[i].ts.tv_sec,
			   snapshot[i].ts.tv_nsec / 1000000,
			snapshot[i].line);
	}

	kfree(snapshot);
	return 0;
}

/*
 * Muc dich: dump snapshot entropy_ring ra /proc/mouse_entropy.
 * Khi goi: moi lan user-space read file proc.
 */
static int mouse_entropy_proc_show(struct seq_file *m, void *v)
{
	struct mouse_entropy_sample *snapshot;
	unsigned long flags;
	unsigned int i;
	unsigned int count;
	unsigned int head;

	snapshot = kcalloc(ENTROPY_RING_SIZE, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	/* Cach lam giong log: copy nhanh du lieu, nhe lock, roi moi format output. */
	spin_lock_irqsave(&entropy_lock, flags);
	count = entropy_count;
	head = entropy_head;
	for (i = 0; i < count; i++) {
		unsigned int idx = (head + ENTROPY_RING_SIZE - count + i) % ENTROPY_RING_SIZE;

		snapshot[i] = entropy_ring[idx];
	}
	spin_unlock_irqrestore(&entropy_lock, flags);

	for (i = 0; i < count; i++) {
		seq_printf(m, "[%5lld.%03ld] dx=%d dy=%d\n",
			   (long long)snapshot[i].ts.tv_sec,
			   snapshot[i].ts.tv_nsec / 1000000,
			   snapshot[i].dx,
			   snapshot[i].dy);
	}

	kfree(snapshot);
	return 0;
}

/*
 * Muc dich: proc open callback cho /proc/mouse_log.
 * Khi goi: VFS goi khi file proc duoc mo.
 */
static int mouse_log_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mouse_log_proc_show, NULL);
}

/*
 * Muc dich: proc open callback cho /proc/mouse_entropy.
 * Khi goi: VFS goi khi file proc duoc mo.
 */
static int mouse_entropy_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mouse_entropy_proc_show, NULL);
}

static const struct proc_ops mouse_log_proc_ops = {
	.proc_open = mouse_log_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops mouse_entropy_proc_ops = {
	.proc_open = mouse_entropy_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/*
 * Muc dich: tao context rieng va attach handler vao input device vua match.
 * Khi goi: input core goi khi 1 thiet bi phu hop mouse_ids xuat hien.
 */
static int mouse_connect(struct input_handler *handler, struct input_dev *dev,
			 const struct input_device_id *id)
{
	struct mouse_ctx *ctx;
	int err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->handle.dev = dev;
	ctx->handle.handler = handler;
	ctx->handle.name = DRV_NAME;
	spin_lock_init(&ctx->right_lock);
	INIT_WORK(&ctx->right_click_work, mouse_right_click_worker);

	err = input_register_handle(&ctx->handle);
	if (err)
		goto err_free_ctx;

	err = input_open_device(&ctx->handle);
	if (err)
		goto err_unregister_handle;

	mouse_log_add(true, "CONNECTED: %s", dev->name ? dev->name : "unknown");
	pr_info(DRV_NAME ": connected to input device '%s'\n",
		dev->name ? dev->name : "unknown");

	return 0;

err_unregister_handle:
	input_unregister_handle(&ctx->handle);
err_free_ctx:
	kfree(ctx);
	return err;
}

/*
 * Muc dich: cleanup context khi device ngat ket noi.
 * Khi goi: input core goi trong duong disconnect.
 */
static void mouse_disconnect(struct input_handle *handle)
{
	struct mouse_ctx *ctx = container_of(handle, struct mouse_ctx, handle);

	mouse_flush_pending_move(ctx);
	cancel_work_sync(&ctx->right_click_work);
	mouse_log_add(true, "DISCONNECTED: %s",
		      handle->dev->name ? handle->dev->name : "unknown");

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(ctx);
}

static const struct input_device_id mouse_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_RELBIT,
		.evbit = { BIT_MASK(EV_KEY) | BIT_MASK(EV_REL) },
		.keybit = {
			[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) |
					      BIT_MASK(BTN_RIGHT) |
					      BIT_MASK(BTN_MIDDLE),
		},
		.relbit = {
			[BIT_WORD(REL_X)] = BIT_MASK(REL_X) |
					    BIT_MASK(REL_Y) |
					    BIT_MASK(REL_WHEEL),
		},
	},
	{ },
};
MODULE_DEVICE_TABLE(input, mouse_ids);

static struct input_handler mouse_input_handler = {
	.filter = mouse_filter,
	.connect = mouse_connect,
	.disconnect = mouse_disconnect,
	.name = DRV_NAME,
	.id_table = mouse_ids,
};

/*
 * Muc dich: khoi tao module, tao proc node va dang ky input handler.
 * Khi goi: luc module_init chay khi insmod.
 */
static int __init mouse_input_filter_init(void)
{
	int err;

	proc_log_entry = proc_create(PROC_LOG_NAME, 0444, NULL, &mouse_log_proc_ops);
	if (!proc_log_entry)
		return -ENOMEM;

	proc_entropy_entry = proc_create(PROC_ENTROPY_NAME, 0444, NULL,
					 &mouse_entropy_proc_ops);
	if (!proc_entropy_entry) {
		remove_proc_entry(PROC_LOG_NAME, NULL);
		return -ENOMEM;
	}

	/* Dang ky input_handler de bat dau intercept su kien chuot. */
	err = input_register_handler(&mouse_input_handler);
	if (err) {
		remove_proc_entry(PROC_ENTROPY_NAME, NULL);
		remove_proc_entry(PROC_LOG_NAME, NULL);
		return err;
	}

	pr_info(DRV_NAME ": module loaded\n");
	return 0;
}

/*
 * Muc dich: go dang ky handler/proc khi module roi khoi kernel.
 * Khi goi: luc module_exit chay khi rmmod.
 */
static void __exit mouse_input_filter_exit(void)
{
	input_unregister_handler(&mouse_input_handler);
	remove_proc_entry(PROC_ENTROPY_NAME, NULL);
	remove_proc_entry(PROC_LOG_NAME, NULL);
	pr_info(DRV_NAME ": module unloaded\n");
}

module_init(mouse_input_filter_init);
module_exit(mouse_input_filter_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub Copilot");
MODULE_DESCRIPTION("USB mouse input handler with event filtering and entropy export");