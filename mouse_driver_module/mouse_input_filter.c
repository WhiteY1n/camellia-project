/*
 * mouse_input_filter.c — bộ lọc sự kiện chuột USB
 *
 * LUỒNG DỮ LIỆU (ghi để nhớ):
 *   Chuột phần cứng
 *     → input subsystem (kernel tổng hợp event)
 *     → mouse_filter() (bộ lọc chính, chặn/đổi event theo yêu cầu)
 *         ├── EV_REL  : gom dx/dy, flush khi gặp SYN_REPORT
 *         ├── EV_SYN  : flush movement, ghi log + entropy
 *         └── EV_KEY  : left click → chặn gốc, queue đổi thành right
 *                       right click thật → block hoàn toàn
 *                       middle click → toggle logging
 *     → workqueue (mouse_right_click_worker) inject BTN_RIGHT trở lại
 *     → /proc/mouse_log, /proc/mouse_entropy để đọc từ userspace
 *
 * Tại sao dùng input_handler thay vì cách khác:
 *   - input_handler là cơ chế chính thức của kernel để "nghe lén" toàn bộ event
 *     của một thiết bị input mà KHÔNG cần sửa driver gốc.
 *   - filter callback trả về true => nuốt event (không cho đi tiếp).
 *   - filter callback trả về false => cho event đi bình thường.
 */

#include <linux/init.h>        /* module_init / module_exit */
#include <linux/input.h>       /* input_handler, input_handle, input_inject_event */
#include <linux/kernel.h>      /* printk, pr_info */
#include <linux/module.h>      /* MODULE_LICENSE, MODULE_AUTHOR, ... */
#include <linux/proc_fs.h>     /* proc_create, remove_proc_entry */
#include <linux/seq_file.h>    /* seq_printf, single_open */
#include <linux/slab.h>        /* kzalloc, kfree */
#include <linux/spinlock.h>    /* spinlock_t, spin_lock_irqsave */
#include <linux/timekeeping.h> /* ktime_get_real_ts64 */
#include <linux/workqueue.h>   /* INIT_WORK, schedule_work */

#define DRV_NAME        "mouse_input_filter" /* Tên module, dùng trong log printk */
#define PROC_LOG_NAME   "mouse_log"       /* /proc/mouse_log   — đọc event log */
#define PROC_ENTROPY_NAME "mouse_entropy" /* /proc/mouse_entropy — đọc mẫu dx/dy */

/* Kích thước ring buffer cho log và entropy — đủ dùng, không quá tốn RAM kernel. */
#define LOG_RING_SIZE     256
#define LOG_LINE_SIZE     128
#define ENTROPY_RING_SIZE 1024

/* Hàng đợi chuyển đổi click: tối đa 32 event đang chờ worker xử lý là đủ. */
#define RIGHT_QUEUE_SIZE  32

/* --- Struct cho từng entry log --- */
struct mouse_log_entry {
	struct timespec64 ts;
	char line[LOG_LINE_SIZE];
};

/* --- Struct cho mẫu entropy (chỉ cần dx/dy nhỏ, ép về s16 là ổn) --- */
struct mouse_entropy_sample {
	struct timespec64 ts;
	s16 dx;
	s16 dy;
};

/*
 * mouse_ctx — context riêng cho mỗi thiết bị chuột được kết nối.
 * Mỗi lần mouse_connect() được gọi (có chuột cắm vào) thì tạo 1 ctx mới.
 *
 * pending_dx / pending_dy:
 *   REL_X và REL_Y có thể đến rời rạc trong 1 frame. Mình gom chúng lại
 *   rồi flush 1 lần tại SYN_REPORT thay vì log từng event nhỏ lẻ.
 *
 * right_queue / right_lock:
 *   Hàng đợi FIFO nhỏ để truyền "giá trị click" từ filter → worker an toàn.
 *
 * injecting_right:
 *   Cờ quan trọng: khi worker đang inject BTN_RIGHT vào lại input core,
 *   nếu không đánh dấu thì filter sẽ block luôn cái event đó — vòng lặp chết.
 */
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
	bool injecting_right; /* true khi worker đang inject, để filter bỏ qua */
};

/* --- Biến toàn cục --- */
static struct proc_dir_entry *proc_log_entry;
static struct proc_dir_entry *proc_entropy_entry;

/* Ring buffer log: ghi các event đáng chú ý, đọc qua /proc/mouse_log */
static struct mouse_log_entry log_ring[LOG_RING_SIZE];
static unsigned int log_head;
static unsigned int log_count;
static DEFINE_SPINLOCK(log_lock);

/* Ring buffer entropy: lưu dx/dy từng frame, đọc qua /proc/mouse_entropy */
static struct mouse_entropy_sample entropy_ring[ENTROPY_RING_SIZE];
static unsigned int entropy_head;
static unsigned int entropy_count;
static DEFINE_SPINLOCK(entropy_lock);

/*
 * logging_enabled: bật/tắt log runtime bằng middle click.
 * Dùng READ_ONCE/WRITE_ONCE vì biến này có thể đọc/ghi từ nhiều CPU cùng lúc.
 */
static bool logging_enabled = true;

/* ===================================================================
 * HÀM HELPER: log + entropy
 * =================================================================== */

/*
 * mouse_log_add — thêm 1 dòng vào ring buffer log.
 *
 * @force: true = ghi dù logging đang tắt (dùng cho event quan trọng như
 *         CONNECT/DISCONNECT/WARN).
 *
 * Dùng spin_lock_irqsave vì hàm này có thể gọi từ ngữ cảnh interrupt
 * (filter callback chạy trong interrupt context trên một số kernel path).
 */
static void mouse_log_add(bool force, const char *fmt, ...)
{
	struct mouse_log_entry entry;
	unsigned long flags;
	va_list args;

	/* Nếu logging tắt và không phải log bắt buộc thì bỏ qua ngay */
	if (!force && !READ_ONCE(logging_enabled))
		return;

	ktime_get_real_ts64(&entry.ts);

	va_start(args, fmt);
	vscnprintf(entry.line, sizeof(entry.line), fmt, args);
	va_end(args);

	spin_lock_irqsave(&log_lock, flags);
	log_ring[log_head] = entry;
	log_head = (log_head + 1) % LOG_RING_SIZE; /* vòng tròn, ghi đè cũ nhất */
	if (log_count < LOG_RING_SIZE)
		log_count++;
	spin_unlock_irqrestore(&log_lock, flags);
}

/*
 * mouse_entropy_add — lưu 1 mẫu chuyển động (dx, dy) vào ring buffer entropy.
 *
 * Gọi sau mỗi SYN_REPORT — lúc đó dx/dy đã được gom đủ cho 1 frame,
 * nên mẫu ổn định hơn là lấy từng REL_X / REL_Y riêng lẻ.
 *
 * Ép về s16 vì chuyển động chuột thông thường nằm gọn trong ±32767,
 * tiết kiệm bộ nhớ ring buffer mà không mất thông tin hữu ích.
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
 * mouse_flush_pending_move — flush dx/dy đang gom, ghi log + entropy, reset về 0.
 *
 * Gọi tại SYN_REPORT. Lý do gom rồi mới flush thay vì xử lý từng REL_X/REL_Y:
 *   - Kernel có thể gửi nhiều REL_X trong 1 frame trước khi SYN_REPORT đến.
 *   - Gom hết rồi flush 1 lần → log gọn hơn, entropy có mẫu đúng nghĩa hơn.
 */
static void mouse_flush_pending_move(struct mouse_ctx *ctx)
{
	if (!ctx->pending_dx && !ctx->pending_dy)
		return; /* không có chuyển động, bỏ qua */

	mouse_log_add(false, "MOVE: dx=%d dy=%d", ctx->pending_dx, ctx->pending_dy);
	mouse_entropy_add(ctx->pending_dx, ctx->pending_dy);

	ctx->pending_dx = 0;
	ctx->pending_dy = 0;
}

/* ===================================================================
 * WORKER: đổi left click → right click (chạy ngoài interrupt context)
 * =================================================================== */

/*
 * mouse_right_click_worker — xử lý hàng đợi chuyển đổi BTN_LEFT → BTN_RIGHT.
 *
 * TẠI SAO không inject thẳng trong filter callback?
 *   Filter callback chạy trong đường đi của input core (có thể là interrupt
 *   hoặc softirq). Nếu gọi input_inject_event() ở đó, event mới sẽ quay
 *   ngược lại vào chính filter này → deadlock hoặc crash.
 *   Dùng workqueue để thoát khỏi ngữ cảnh đó, chạy trong process context
 *   an toàn hơn.
 *
 * Cờ injecting_right: bật trước khi inject, tắt ngay sau. Filter kiểm tra
 *   cờ này để "tha" cho BTN_RIGHT do chính mình tạo ra, không bị block lại.
 */
static void mouse_right_click_worker(struct work_struct *work)
{
	struct mouse_ctx *ctx = container_of(work, struct mouse_ctx, right_click_work);
	unsigned long flags;
	int value;

	/* Xử lý hết hàng đợi, mỗi vòng lấy ra 1 event */
	for (;;) {
		spin_lock_irqsave(&ctx->right_lock, flags);
		if (!ctx->right_count) {
			spin_unlock_irqrestore(&ctx->right_lock, flags);
			break; /* hàng đợi trống, xong */
		}

		value = ctx->right_queue[ctx->right_tail];
		ctx->right_tail = (ctx->right_tail + 1) % RIGHT_QUEUE_SIZE;
		ctx->right_count--;
		spin_unlock_irqrestore(&ctx->right_lock, flags);

		/*
		 * Đánh dấu đang inject để filter biết mà bỏ qua BTN_RIGHT này.
		 * Thứ tự quan trọng: set cờ TRƯỚC khi inject, clear NGAY SAU.
		 */
		WRITE_ONCE(ctx->injecting_right, true);
		input_inject_event(&ctx->handle, EV_KEY, BTN_RIGHT, value);
		input_inject_event(&ctx->handle, EV_SYN, SYN_REPORT, 0);
		WRITE_ONCE(ctx->injecting_right, false);

		mouse_log_add(false, "BTN_LEFT converted -> BTN_RIGHT (value=%d)", value);
	}
}

/*
 * mouse_queue_right_click — đưa 1 event left click vào hàng đợi để worker xử lý.
 *
 * Gọi từ filter callback (có thể là interrupt context), nên chỉ được thao tác
 * với spinlock, không được sleep hay gọi blocking function.
 *
 * Nếu hàng đợi đầy: bỏ event và log cảnh báo — chấp nhận mất click còn hơn
 * làm treo hệ thống input.
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
		schedule_work(&ctx->right_click_work); /* đánh thức worker */
		return;
	}
	spin_unlock_irqrestore(&ctx->right_lock, flags);

	/* Hàng đợi đầy — dùng force=true để log dù logging đang tắt */
	mouse_log_add(true, "WARN: right-click queue full, dropping event");
}

/* ===================================================================
 * FILTER CALLBACK — điểm chính, chạy mỗi khi có event từ chuột
 * =================================================================== */

/*
 * mouse_filter — intercept từng event trước khi input core chuyển tới userspace.
 *
 * ĐÂY LÀ TRUNG TÂM CỦA MODULE. Mọi can thiệp vào hành vi chuột đều bắt đầu ở đây.
 *
 * Tại sao bắt được event chuột:
 *   - Module đăng ký mouse_input_handler với .id_table = mouse_ids.
 *   - mouse_ids khai báo chỉ nhận thiết bị có EV_KEY + EV_REL + các bit phù hợp.
 *   - Khi chuột khớp, input core gọi mouse_connect() → tạo ctx và mở handle.
 *   - Từ đó, mọi event từ chuột đó đều đi qua mouse_filter này TRƯỚC.
 *
 * Quy tắc trả về (dễ nhầm!):
 *   return true  → NUỐT event, không cho đi tiếp (chặn)
 *   return false → cho event qua bình thường
 */
static bool mouse_filter(struct input_handle *handle,
			 unsigned int type, unsigned int code, int value)
{
	struct mouse_ctx *ctx = container_of(handle, struct mouse_ctx, handle);

	switch (type) {

	case EV_REL:
		/*
		 * Chuyển động chuột: gom delta lại, chưa xử lý ngay.
		 * Sẽ flush khi gặp SYN_REPORT để có được 1 frame hoàn chỉnh.
		 */
		if (code == REL_X) {
			ctx->pending_dx += value; /* cộng dồn delta ngang */
			return false;
		}
		if (code == REL_Y) {
			ctx->pending_dy += value; /* cộng dồn delta dọc */
			return false;
		}
		if (code == REL_WHEEL)
			mouse_log_add(false, "WHEEL: delta=%d", value); /* chỉ log, không chặn */
		return false;

	case EV_SYN:
		/* Cuối frame: flush chuyển động đang gom. */
		if (code == SYN_REPORT)
			mouse_flush_pending_move(ctx);
		return false;

	case EV_KEY:
		/*
		 * BTN_LEFT: đây là nơi đổi left → right click.
		 * Chặn event gốc (return true), đẩy vào hàng đợi để worker inject lại
		 * BTN_RIGHT. Không làm trực tiếp ở đây vì nguy cơ deadlock (xem worker).
		 */
		if (code == BTN_LEFT) {
			mouse_queue_right_click(ctx, value);
			return true; /* chặn BTN_LEFT gốc */
		}

		/*
		 * BTN_RIGHT do chính worker inject: cho qua, không block lại.
		 * Nếu thiếu check này, event inject sẽ bị chặn → click không có tác dụng.
		 */
		if (code == BTN_RIGHT && READ_ONCE(ctx->injecting_right))
			return false;

		/* BTN_RIGHT từ phần cứng thật: block hoàn toàn theo yêu cầu module. */
		if (code == BTN_RIGHT) {
			mouse_log_add(false, "BTN_RIGHT blocked (value=%d)", value);
			return true;
		}

		/*
		 * BTN_MIDDLE nhấn xuống (value==1): toggle logging on/off.
		 * Dùng READ_ONCE/WRITE_ONCE để thao tác atomic trên biến shared.
		 */
		if (code == BTN_MIDDLE && value == 1) {
			bool new_state = !READ_ONCE(logging_enabled);

			WRITE_ONCE(logging_enabled, new_state);
			/* force=true để log trạng thái mới dù đang tắt logging */
			mouse_log_add(true, "LOGGING %s", new_state ? "ON" : "OFF");
			pr_info(DRV_NAME ": logging %s\n", new_state ? "enabled" : "disabled");
		}
		return false;

	default:
		return false; /* event lạ: không quan tâm, cho qua */
	}
}

/* ===================================================================
 * /proc HANDLERS
 * =================================================================== */

/*
 * mouse_log_proc_show — in log_ring ra file /proc/mouse_log.
 *
 * Pattern: copy snapshot dưới lock → giải phóng lock → format output.
 * Không giữ lock trong lúc seq_printf để tránh chiếm lock quá lâu.
 */
static int mouse_log_proc_show(struct seq_file *m, void *v)
{
	struct mouse_log_entry *snapshot;
	unsigned long flags;
	unsigned int i, count, head;

	snapshot = kcalloc(LOG_RING_SIZE, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	/* Lấy snapshot nhanh dưới lock */
	spin_lock_irqsave(&log_lock, flags);
	count = log_count;
	head  = log_head;
	for (i = 0; i < count; i++) {
		unsigned int idx = (head + LOG_RING_SIZE - count + i) % LOG_RING_SIZE;

		snapshot[i] = log_ring[idx];
	}
	spin_unlock_irqrestore(&log_lock, flags);

	/* Format và in ra ngoài lock */
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
 * mouse_entropy_proc_show — in entropy_ring ra /proc/mouse_entropy.
 * Cùng pattern với log_proc_show: snapshot dưới lock, format ngoài lock.
 */
static int mouse_entropy_proc_show(struct seq_file *m, void *v)
{
	struct mouse_entropy_sample *snapshot;
	unsigned long flags;
	unsigned int i, count, head;

	snapshot = kcalloc(ENTROPY_RING_SIZE, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	spin_lock_irqsave(&entropy_lock, flags);
	count = entropy_count;
	head  = entropy_head;
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

/* Hai hàm open dưới đây là boilerplate chuẩn cho proc file dùng seq_file. */
static int mouse_log_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mouse_log_proc_show, NULL);
}

static int mouse_entropy_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mouse_entropy_proc_show, NULL);
}

static const struct proc_ops mouse_log_proc_ops = {
	.proc_open    = mouse_log_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops mouse_entropy_proc_ops = {
	.proc_open    = mouse_entropy_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ===================================================================
 * CONNECT / DISCONNECT
 * =================================================================== */

/*
 * mouse_connect — input core gọi khi phát hiện thiết bị khớp mouse_ids.
 *
 * Việc cần làm:
 *   1. Cấp phát ctx cho thiết bị này.
 *   2. Đăng ký handle với input core.
 *   3. Mở thiết bị → từ đây filter bắt đầu nhận event.
 *
 * Nếu bước nào lỗi thì dọn dẹp theo thứ tự ngược lại (goto pattern chuẩn kernel).
 */
static int mouse_connect(struct input_handler *handler, struct input_dev *dev,
			 const struct input_device_id *id)
{
	struct mouse_ctx *ctx;
	int err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->handle.dev     = dev;
	ctx->handle.handler = handler;
	ctx->handle.name    = DRV_NAME;
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
 * mouse_disconnect — dọn dẹp khi thiết bị rút ra hoặc module bị unload.
 *
 * Thứ tự quan trọng:
 *   1. Flush pending move (để không mất dữ liệu đang gom).
 *   2. cancel_work_sync: đợi worker đang chạy xong rồi mới hủy — tránh UAF.
 *   3. Đóng device và unregister handle trước khi free ctx.
 */
static void mouse_disconnect(struct input_handle *handle)
{
	struct mouse_ctx *ctx = container_of(handle, struct mouse_ctx, handle);

	mouse_flush_pending_move(ctx);
	cancel_work_sync(&ctx->right_click_work); /* chờ worker xong trước khi free ctx */
	mouse_log_add(true, "DISCONNECTED: %s",
		      handle->dev->name ? handle->dev->name : "unknown");

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(ctx);
}

/* ===================================================================
 * ID TABLE: bộ lọc thiết bị — chỉ match chuột USB/HID thông thường
 * =================================================================== */

/*
 * mouse_ids — khai báo loại thiết bị mà module này muốn nhận.
 *
 * Phải có đủ 3 bit:
 *   EV_KEY + EV_REL (evbit): thiết bị có nút bấm và chuyển động tương đối.
 *   BTN_LEFT/RIGHT/MIDDLE (keybit): xác nhận đây là chuột, không phải bàn phím.
 *   REL_X/REL_Y/REL_WHEEL (relbit): có đủ trục di chuyển và cuộn.
 *
 * Thiếu 1 trong số này → không match → mouse_connect không được gọi.
 */
static const struct input_device_id mouse_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_RELBIT,
		.evbit  = { BIT_MASK(EV_KEY) | BIT_MASK(EV_REL) },
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
	{ }, /* sentinel — kết thúc danh sách */
};
MODULE_DEVICE_TABLE(input, mouse_ids);

/*
 * mouse_input_handler — "móc" chính để gắn vào input subsystem.
 *
 * .filter:     mouse_filter() — interceptor event, trả true để chặn.
 * .connect:    cấp phát ctx khi chuột được cắm / module load.
 * .disconnect: giải phóng ctx khi chuột rút ra / module unload.
 * .id_table:   chỉ match thiết bị khớp mouse_ids, không ảnh hưởng device khác.
 */
static struct input_handler mouse_input_handler = {
	.filter     = mouse_filter,
	.connect    = mouse_connect,
	.disconnect = mouse_disconnect,
	.name       = DRV_NAME,
	.id_table   = mouse_ids,
};

/* ===================================================================
 * INIT / EXIT
 * =================================================================== */

/*
 * mouse_input_filter_init — khởi động module.
 *
 * Thứ tự: tạo /proc entries trước, rồi mới đăng ký handler.
 * Lý do: nếu handler đăng ký trước mà /proc chưa có, có thể có race
 * rất nhỏ trong log — đảo ngược để an toàn hơn.
 *
 * Cleanup khi lỗi: xóa /proc entries theo thứ tự ngược nếu bước sau thất bại.
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

	/* Từ đây trở đi, mouse_filter bắt đầu được gọi khi có chuột kết nối. */
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
 * mouse_input_filter_exit — dọn dẹp khi rmmod.
 *
 * Unregister handler trước: input core sẽ gọi disconnect cho tất cả thiết bị
 * đang kết nối → ctx được free sạch trước khi xóa /proc entries.
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