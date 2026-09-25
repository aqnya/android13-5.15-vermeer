// SPDX-License-Identifier: GPL-2.0
/*
 * crashlog - keep the kernel log in a file so that it survives a panic.
 *
 * The kernel log lives in a ring buffer in RAM and is gone the moment the
 * machine reboots, which is exactly when it is wanted most.  This keeps a
 * file open on a writable filesystem (/data/kernel-crash.log by default)
 * and, from a panic notifier, appends the whole log buffer to it and
 * fsync()s the result.
 *
 * The file is opened ahead of time by a delayed work item instead of in the
 * panic path: the panic path then only has to write (no path resolution, no
 * allocation) and, more importantly, the file's inode and directory entry
 * are long since on disk by the time the panic happens.
 *
 * Controls are built-in module parameters, so they also work on the kernel
 * command line:
 *
 *	crashlog.path=/data/kernel-crash.log		where to write
 *	echo 1 > /sys/module/crashlog/parameters/dump	dump right now
 *	cat /sys/module/crashlog/parameters/status	state and counters
 *
 * Caveats - this is a best effort last resort, not a reliable mechanism:
 *
 *  - panic() runs the notifier chain with interrupts disabled and, unless
 *    crash_kexec_post_notifiers is set, only after smp_send_stop() stopped
 *    every other CPU.  Filesystem I/O from there is neither legal (it can
 *    sleep, so expect a spurious "sleeping function called from invalid
 *    context" splat) nor always safe: if the panic interrupted a task
 *    holding a lock in the same filesystem, the write can deadlock instead
 *    of rebooting.  The pre-opened file keeps that window small.
 *  - A kmsg_dump() callback could not be used for this at all: it runs
 *    under rcu_read_lock() and is not allowed to sleep, hence a notifier
 *    plus kmsg_dump_get_line() here.
 *  - Only panics are caught.  A hard hang with no panic, a watchdog-only
 *    reboot or a power cut leaves nothing behind.
 *  - Writing to /data from the kernel is subject to SELinux like any other
 *    write.  If the open or the write fails, the errno shows up in the
 *    status parameter.
 */

#define pr_fmt(fmt)	"crashlog: " fmt

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/panic_notifier.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define CRASHLOG_PATH_MAX	256
#define CRASHLOG_LINE_MAX	2048
#define CRASHLOG_RETRY_INTERVAL	(10 * HZ)
#define CRASHLOG_RETRY_MAX	90

static char want_path[CRASHLOG_PATH_MAX] = "/data/kernel-crash.log";
static char have_path[CRASHLOG_PATH_MAX];
static struct file *crashlog_file;
static unsigned int open_tries;
static unsigned int dump_count;
static int last_errno;
static char dump_line[CRASHLOG_LINE_MAX];
static struct delayed_work open_work;
static DEFINE_MUTEX(ctrl_lock);

/* 只在进程上下文调用，持 ctrl_lock。panic 路径不碰这两个函数。 */
static void crashlog_close_locked(void)
{
	if (crashlog_file) {
		filp_close(crashlog_file, NULL);
		crashlog_file = NULL;
	}
	have_path[0] = '\0';
}

/* 0 = 已打开，负 = 打开失败。持 ctrl_lock 调用。 */
static int crashlog_open_locked(void)
{
	struct file *f;

	crashlog_close_locked();

	f = filp_open(want_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (IS_ERR(f)) {
		last_errno = (int)PTR_ERR(f);
		return last_errno;
	}

	crashlog_file = f;
	strscpy(have_path, want_path, sizeof(have_path));
	last_errno = 0;
	pr_info("kernel log will be saved to %s\n", have_path);
	return 0;
}

/*
 * /data only shows up once userspace mounted it, so keep retrying until it
 * does (and again whenever the path is changed).  Once the file is open this
 * stops polling.
 */
static void crashlog_open_work_fn(struct work_struct *work)
{
	int ret;

	mutex_lock(&ctrl_lock);

	if (crashlog_file && !strcmp(have_path, want_path)) {
		open_tries = 0;
		mutex_unlock(&ctrl_lock);
		return;
	}

	ret = crashlog_open_locked();
	if (!ret) {
		open_tries = 0;
		mutex_unlock(&ctrl_lock);
		return;
	}

	if (++open_tries > CRASHLOG_RETRY_MAX) {
		if (open_tries == CRASHLOG_RETRY_MAX + 1)
			pr_warn("cannot open %s (%d), giving up\n",
				want_path, last_errno);
		mutex_unlock(&ctrl_lock);
		return;
	}

	mutex_unlock(&ctrl_lock);
	schedule_delayed_work(&open_work, CRASHLOG_RETRY_INTERVAL);
}

/* kernel_write() may write less than asked for; keep going until it is all out. */
static int crashlog_write_all(struct file *f, const char *buf, size_t len,
			      loff_t *pos)
{
	while (len) {
		ssize_t ret = kernel_write(f, buf, len, pos);

		if (ret < 0)
			return (int)ret;
		if (!ret)
			return -EIO;
		buf += ret;
		len -= ret;
	}
	return 0;
}

/*
 * Append one full copy of the kernel log.  dump_line is shared with the
 * manual trigger; a panic landing in the middle of one only garbles a line.
 */
static void crashlog_dump(struct file *f, const char *reason)
{
	struct kmsg_dump_iter iter;
	char hdr[160];
	loff_t pos = 0;
	size_t len;
	int ret, n;

	n = scnprintf(hdr, sizeof(hdr), "\n===== crashlog: %s =====\n", reason);
	ret = crashlog_write_all(f, hdr, n, &pos);

	if (!ret) {
		kmsg_dump_rewind(&iter);
		while (kmsg_dump_get_line(&iter, false, dump_line,
					  sizeof(dump_line), &len)) {
			if (!len)
				continue;
			ret = crashlog_write_all(f, dump_line, len, &pos);
			if (ret)
				break;
		}
	}

	/* Flush the data *and* the metadata: the reboot that follows a panic
	 * does not sync anything by itself.
	 */
	if (!ret)
		ret = vfs_fsync(f, 0);

	n = scnprintf(hdr, sizeof(hdr), "===== crashlog: %s (%s) =====\n\n",
		      reason, ret ? "incomplete" : "complete");
	crashlog_write_all(f, hdr, n, &pos);

	last_errno = ret;
	dump_count++;
}

static int crashlog_panic_notify(struct notifier_block *nb, unsigned long action,
				 void *data)
{
	struct file *f = crashlog_file;
	bool opened_here = false;

	if (!f) {
		/* Path changed a moment ago, or /data showed up too late. */
		f = filp_open(have_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (IS_ERR(f)) {
			last_errno = (int)PTR_ERR(f);
			return NOTIFY_DONE;
		}
		opened_here = true;
	}

	crashlog_dump(f, data ? (const char *)data : "panic");

	if (opened_here)
		filp_close(f, NULL);

	return NOTIFY_DONE;
}

static struct notifier_block crashlog_panic_nb = {
	.notifier_call	= crashlog_panic_notify,
	.priority	= INT_MAX,
};

static int crashlog_path_set(const char *val, const struct kernel_param *kp)
{
	char tmp[CRASHLOG_PATH_MAX];
	size_t len;

	if (!val)
		return -EINVAL;

	/* sysfs may hand us a trailing newline */
	len = strnlen(val, sizeof(tmp));
	if (len == sizeof(tmp))
		return -ENAMETOOLONG;
	memcpy(tmp, val, len);
	tmp[len] = '\0';
	strim(tmp);

	if (tmp[0] != '/')
		return -EINVAL;

	mutex_lock(&ctrl_lock);
	strscpy(want_path, tmp, sizeof(want_path));
	crashlog_close_locked();
	open_tries = 0;
	mutex_unlock(&ctrl_lock);

	schedule_delayed_work(&open_work, 0);
	return 0;
}

static int crashlog_path_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%s\n", want_path);
}

static int crashlog_dump_set(const char *val, const struct kernel_param *kp)
{
	mutex_lock(&ctrl_lock);
	if (!crashlog_file)
		crashlog_open_locked();
	if (crashlog_file)
		crashlog_dump(crashlog_file, "manual");
	else if (!last_errno)
		last_errno = -ENODEV;
	mutex_unlock(&ctrl_lock);

	return last_errno;
}

static int crashlog_status_get(char *buffer, const struct kernel_param *kp)
{
	const char *state;

	if (!crashlog_file)
		state = "not open";
	else if (strcmp(have_path, want_path))
		state = "stale";
	else
		state = "open";

	return scnprintf(buffer, PAGE_SIZE,
			 "state:    %s\n"
			 "want:     %s\n"
			 "open:     %s\n"
			 "tries:    %u\n"
			 "dumps:    %u\n"
			 "last err: %d\n",
			 state, want_path,
			 crashlog_file ? have_path : "-",
			 open_tries, dump_count, last_errno);
}

static const struct kernel_param_ops crashlog_path_ops = {
	.set	= crashlog_path_set,
	.get	= crashlog_path_get,
};

static const struct kernel_param_ops crashlog_dump_ops = {
	.set	= crashlog_dump_set,
};

static const struct kernel_param_ops crashlog_status_ops = {
	.get	= crashlog_status_get,
};

module_param_cb(path, &crashlog_path_ops, NULL, 0644);
module_param_cb(dump, &crashlog_dump_ops, NULL, 0200);
module_param_cb(status, &crashlog_status_ops, NULL, 0444);

static int __init crashlog_init(void)
{
	INIT_DELAYED_WORK(&open_work, crashlog_open_work_fn);
	schedule_delayed_work(&open_work, 0);

	atomic_notifier_chain_register(&panic_notifier_list, &crashlog_panic_nb);
	return 0;
}
device_initcall(crashlog_init);

MODULE_DESCRIPTION("Save the kernel log to a file when the kernel panics");
MODULE_LICENSE("GPL");
