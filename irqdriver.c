// ЛР №4, вариант 5: hrtimer + три варианта нижней половины.
// Режимы: mode=tasklet, mode=workqueue, mode=kthread.
// Совместимость ориентирована на современные ядра Linux (в т.ч. Debian 13).

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/math64.h>

#define PROC_NAME "irqdriver"
#define TSQ_SIZE 4096

static unsigned int interval_ms = 10;
module_param(interval_ms, uint, 0644);
MODULE_PARM_DESC(interval_ms, "Период hrtimer в миллисекундах (минимум 1)");

static char *mode = "workqueue";
module_param(mode, charp, 0444);
MODULE_PARM_DESC(mode, "Режим нижней половины: tasklet, workqueue или kthread");

static struct hrtimer tick_timer;
static struct proc_dir_entry *proc_entry;

static atomic64_t top_count = ATOMIC64_INIT(0);
static atomic64_t bottom_count = ATOMIC64_INIT(0);
static atomic64_t queue_overruns = ATOMIC64_INIT(0);
static atomic64_t jitter_sum_ns = ATOMIC64_INIT(0);
static atomic64_t jitter_max_ns = ATOMIC64_INIT(0);
static atomic64_t latency_sum_ns = ATOMIC64_INIT(0);
static atomic64_t latency_max_ns = ATOMIC64_INIT(0);

struct tick_event {
	u64 expected_ns;
	u64 actual_ns;
};

static struct tick_event events[TSQ_SIZE];
static unsigned int ev_head, ev_tail, ev_count;
static DEFINE_SPINLOCK(ev_lock);

static struct work_struct bottom_work;
static struct tasklet_struct bottom_tasklet;
static struct task_struct *bottom_thread;
static DECLARE_WAIT_QUEUE_HEAD(thread_waitq);
static atomic_t thread_wakeup = ATOMIC_INIT(0);
static bool stopping;

static void update_max(atomic64_t *target, u64 value)
{
	s64 old = atomic64_read(target);

	while ((u64)old < value) {
		s64 prev = atomic64_cmpxchg(target, old, (s64)value);
		if (prev == old)
			break;
		old = prev;
	}
}

static bool pop_event(struct tick_event *event)
{
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&ev_lock, flags);
	if (ev_count) {
		*event = events[ev_tail];
		ev_tail = (ev_tail + 1) % TSQ_SIZE;
		ev_count--;
		found = true;
	}
	spin_unlock_irqrestore(&ev_lock, flags);
	return found;
}

static void process_events(void)
{
	struct tick_event event;
	u64 now, latency;

	while (pop_event(&event)) {
		now = ktime_get_ns();
		latency = now - event.actual_ns;

		atomic64_inc(&bottom_count);
		atomic64_add(latency, &latency_sum_ns);
		update_max(&latency_max_ns, latency);
	}
}

static void work_bottom(struct work_struct *work)
{
	process_events();
}

static void tasklet_bottom(struct tasklet_struct *tasklet)
{
	process_events();
}

static int thread_bottom(void *unused)
{
	while (!kthread_should_stop()) {
		wait_event_interruptible(thread_waitq,
			atomic_xchg(&thread_wakeup, 0) ||
			kthread_should_stop());

		if (kthread_should_stop())
			break;

		process_events();
	}
	return 0;
}

static enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
	u64 actual = ktime_get_ns();
	u64 expected = hrtimer_get_expires_ns(timer);
	u64 jitter = (actual >= expected) ? actual - expected : expected - actual;
	unsigned long flags;
	bool queued = false;

	atomic64_inc(&top_count);
	atomic64_add(jitter, &jitter_sum_ns);
	update_max(&jitter_max_ns, jitter);

	spin_lock_irqsave(&ev_lock, flags);
	if (ev_count < TSQ_SIZE) {
		events[ev_head].expected_ns = expected;
		events[ev_head].actual_ns = actual;
		ev_head = (ev_head + 1) % TSQ_SIZE;
		ev_count++;
		queued = true;
	}
	spin_unlock_irqrestore(&ev_lock, flags);

	if (!queued) {
		atomic64_inc(&queue_overruns);
	} else if (!strcmp(mode, "tasklet")) {
		tasklet_schedule(&bottom_tasklet);
	} else if (!strcmp(mode, "workqueue")) {
		schedule_work(&bottom_work);
	} else {
		atomic_set(&thread_wakeup, 1);
		wake_up_interruptible(&thread_waitq);
	}

	hrtimer_forward_now(timer, ms_to_ktime(max(1U, READ_ONCE(interval_ms))));
	return HRTIMER_RESTART;
}

static void reset_stats(void)
{
	atomic64_set(&top_count, 0);
	atomic64_set(&bottom_count, 0);
	atomic64_set(&queue_overruns, 0);
	atomic64_set(&jitter_sum_ns, 0);
	atomic64_set(&jitter_max_ns, 0);
	atomic64_set(&latency_sum_ns, 0);
	atomic64_set(&latency_max_ns, 0);
}

static ssize_t proc_read(struct file *file, char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	char buf[512];
	u64 top = atomic64_read(&top_count);
	u64 bottom = atomic64_read(&bottom_count);
	u64 js = atomic64_read(&jitter_sum_ns);
	u64 ls = atomic64_read(&latency_sum_ns);
	u64 avg_jitter = top ? div64_u64(js, top) : 0;
	u64 avg_latency = bottom ? div64_u64(ls, bottom) : 0;
	int len;

	len = scnprintf(buf, sizeof(buf),
		"mode: %s\n"
		"interval_ms: %u\n"
		"top_half_events: %lld\n"
		"bottom_half_events: %lld\n"
		"queue_overruns: %lld\n"
		"avg_jitter_ns: %llu\n"
		"max_jitter_ns: %lld\n"
		"avg_bottom_latency_ns: %llu\n"
		"max_bottom_latency_ns: %lld\n"
		"pending_events: %u\n",
		mode, READ_ONCE(interval_ms),
		(long long)top, (long long)bottom,
		(long long)atomic64_read(&queue_overruns),
		(unsigned long long)avg_jitter,
		(long long)atomic64_read(&jitter_max_ns),
		(unsigned long long)avg_latency,
		(long long)atomic64_read(&latency_max_ns),
		READ_ONCE(ev_count));

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

/* Запись числа в /proc/irqdriver меняет период таймера в миллисекундах.
 * Команда "reset" обнуляет статистику. */
static ssize_t proc_write(struct file *file, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	char buf[32];
	unsigned int new_interval;
	size_t len = min(count, sizeof(buf) - 1);

	if (!len)
		return 0;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	strim(buf);

	if (!strcmp(buf, "reset")) {
		reset_stats();
		return count;
	}

	if (kstrtouint(buf, 10, &new_interval))
		return -EINVAL;
	if (new_interval < 1 || new_interval > 60000)
		return -ERANGE;

	WRITE_ONCE(interval_ms, new_interval);
	hrtimer_cancel(&tick_timer);
	hrtimer_start(&tick_timer, ms_to_ktime(new_interval), HRTIMER_MODE_REL);
	return count;
}

static const struct proc_ops proc_fops = {
	.proc_read = proc_read,
	.proc_write = proc_write,
	.proc_lseek = default_llseek,
};

static int __init irqdriver_init(void)
{
	int ret;

	if (strcmp(mode, "tasklet") && strcmp(mode, "workqueue") &&
	    strcmp(mode, "kthread")) {
		pr_err("irqdriver: mode must be tasklet, workqueue or kthread\n");
		return -EINVAL;
	}
	if (interval_ms < 1 || interval_ms > 60000)
		return -EINVAL;

	INIT_WORK(&bottom_work, work_bottom);
	tasklet_setup(&bottom_tasklet, tasklet_bottom);

	if (!strcmp(mode, "kthread")) {
		bottom_thread = kthread_run(thread_bottom, NULL, "irqdriver_bh");
		if (IS_ERR(bottom_thread)) {
			ret = PTR_ERR(bottom_thread);
			bottom_thread = NULL;
			tasklet_kill(&bottom_tasklet);
			return ret;
		}
	}

	proc_entry = proc_create(PROC_NAME, 0666, NULL, &proc_fops);
	if (!proc_entry) {
		if (bottom_thread)
			kthread_stop(bottom_thread);
		tasklet_kill(&bottom_tasklet);
		return -ENOMEM;
	}

	hrtimer_init(&tick_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	tick_timer.function = timer_callback;
	hrtimer_start(&tick_timer, ms_to_ktime(interval_ms), HRTIMER_MODE_REL);

	pr_info("irqdriver: loaded, mode=%s interval=%u ms\n", mode, interval_ms);
	return 0;
}

static void __exit irqdriver_exit(void)
{
	hrtimer_cancel(&tick_timer);
	WRITE_ONCE(stopping, true);

	if (!strcmp(mode, "tasklet"))
		tasklet_kill(&bottom_tasklet);
	else if (!strcmp(mode, "workqueue"))
		cancel_work_sync(&bottom_work);
	else if (bottom_thread)
		kthread_stop(bottom_thread);

	remove_proc_entry(PROC_NAME, NULL);
	pr_info("irqdriver: unloaded top=%lld bottom=%lld overruns=%lld\n",
		(long long)atomic64_read(&top_count),
		(long long)atomic64_read(&bottom_count),
		(long long)atomic64_read(&queue_overruns));
}

module_init(irqdriver_init);
module_exit(irqdriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Lab 4 variant 5: hrtimer and bottom-half comparison");
