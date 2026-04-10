#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>

#define DRV_NAME "mouse_input_filter"
#define PROC_LOG_NAME "mouse_log"
#define PROC_ENTROPY_NAME "mouse_entropy"

#define LOG_RING_SIZE 256
#define LOG_LINE_SIZE 128
#define ENTROPY_RING_SIZE 1024

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
};

static struct proc_dir_entry *proc_log_entry;
static struct proc_dir_entry *proc_entropy_entry;

/* Ring buffer for human-readable event log lines. */
static struct mouse_log_entry log_ring[LOG_RING_SIZE];
static unsigned int log_head;
static unsigned int log_count;
static DEFINE_SPINLOCK(log_lock);

/* Ring buffer for movement samples used as entropy input. */
static struct mouse_entropy_sample entropy_ring[ENTROPY_RING_SIZE];
static unsigned int entropy_head;
static unsigned int entropy_count;
static DEFINE_SPINLOCK(entropy_lock);

static bool logging_enabled = true;

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

static void mouse_flush_pending_move(struct mouse_ctx *ctx)
{
	if (!ctx->pending_dx && !ctx->pending_dy)
		return;

	mouse_log_add(false, "MOVE: dx=%d dy=%d", ctx->pending_dx, ctx->pending_dy);
	mouse_entropy_add(ctx->pending_dx, ctx->pending_dy);

	ctx->pending_dx = 0;
	ctx->pending_dy = 0;
}

/*
 * The filter callback runs inside the input_handler flow before events
 * are delivered to regular userspace-facing handlers (for example evdev).
 * Returning true consumes (blocks) the original event.
 */
static bool mouse_filter(struct input_handle *handle,
			 unsigned int type, unsigned int code, int value)
{
	struct mouse_ctx *ctx = container_of(handle, struct mouse_ctx, handle);

	switch (type) {
	case EV_REL:
		if (code == REL_X) {
			ctx->pending_dx += value;
			return false;
		}

		if (code == REL_Y) {
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
		/* Convert left click into right click and suppress original left event. */
		if (code == BTN_LEFT) {
			mouse_log_add(false, "BTN_LEFT intercepted -> BTN_RIGHT (value=%d)", value);
			input_inject_event(handle, EV_KEY, BTN_RIGHT, value);
			return true;
		}

		/* Block right click entirely. */
		if (code == BTN_RIGHT) {
			mouse_log_add(false, "BTN_RIGHT blocked (value=%d)", value);
			return true;
		}

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

static int mouse_log_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mouse_log_proc_show, NULL);
}

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

static void mouse_disconnect(struct input_handle *handle)
{
	struct mouse_ctx *ctx = container_of(handle, struct mouse_ctx, handle);

	mouse_flush_pending_move(ctx);
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

	err = input_register_handler(&mouse_input_handler);
	if (err) {
		remove_proc_entry(PROC_ENTROPY_NAME, NULL);
		remove_proc_entry(PROC_LOG_NAME, NULL);
		return err;
	}

	pr_info(DRV_NAME ": module loaded\n");
	return 0;
}

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