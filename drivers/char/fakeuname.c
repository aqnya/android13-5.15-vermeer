// SPDX-License-Identifier: GPL-2.0
/*
 * fakeuname - configurable utsname, driven from /sys/kernel/fakeuname/
 *
 * Root writes the value of a single utsname field to the attribute named
 * after it.  While "enable" is 1 that value is published to
 * init_uts_ns.name, which is the one storage behind uname(2),
 * gethostname(2), /proc/version and /proc/sys/kernel/{ostype,osrelease,
 * version,hostname,domainname} -- so every surface agrees and none of
 * them can be cross-checked against another.
 *
 *	/sys/kernel/fakeuname/enable		0|1
 *	/sys/kernel/fakeuname/sysname		uname -s
 *	/sys/kernel/fakeuname/nodename		uname -n
 *	/sys/kernel/fakeuname/release		uname -r
 *	/sys/kernel/fakeuname/version		uname -v
 *	/sys/kernel/fakeuname/machine		uname -m
 *	/sys/kernel/fakeuname/domainname	domainname
 *
 * Values may be staged while "enable" is 0, and one write to "enable"
 * publishes all of them at once -- enabling also remembers the real
 * fields, so writing 0 puts them back.  Writing an empty value un-fakes
 * just that field.  Re-writing a value that is already in effect is a
 * no-op, which keeps a boot script that re-applies its settings free.
 *
 * Reading a field back reports what the system reports right now, so a
 * read after a successful write confirms the override is in effect.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/utsname.h>

enum {
	FAKE_SYSNAME,
	FAKE_NODENAME,
	FAKE_RELEASE,
	FAKE_VERSION,
	FAKE_MACHINE,
	FAKE_DOMAINNAME,
	FAKE_NFIELDS,
};

static const size_t fake_offs[FAKE_NFIELDS] = {
	[FAKE_SYSNAME]		= offsetof(struct new_utsname, sysname),
	[FAKE_NODENAME]		= offsetof(struct new_utsname, nodename),
	[FAKE_RELEASE]		= offsetof(struct new_utsname, release),
	[FAKE_VERSION]		= offsetof(struct new_utsname, version),
	[FAKE_MACHINE]		= offsetof(struct new_utsname, machine),
	[FAKE_DOMAINNAME]	= offsetof(struct new_utsname, domainname),
};

/*
 * A utsname field is a plain char array, so it can be read or replaced in
 * place for as long as uts_sem is held.
 */
static char *fake_field(struct new_utsname *n, int i)
{
	return (char *)n + fake_offs[i];
}

/*
 * Serialises the bookkeeping below.  uts_sem is taken only for the
 * memcpy() that publishes a field, never while a value is being parsed,
 * so a write here cannot park every uname(2) in the system behind it.
 */
static DEFINE_MUTEX(fake_lock);

static bool fake_enabled;
static unsigned int fake_mask;		/* fields with a stored value */
static struct new_utsname fake_vals;	/* the stored values */
static struct new_utsname fake_orig;	/* the real values they cover */

/* Publish field @i of @n to the running utsname. */
static void fake_publish(int i, struct new_utsname *n)
{
	down_write(&uts_sem);
	memcpy(fake_field(&init_uts_ns.name, i), fake_field(n, i),
	       __NEW_UTS_LEN + 1);
	up_write(&uts_sem);
}

/* Does field @i already read exactly @val? */
static bool fake_live(int i, const char *val)
{
	bool same;

	down_read(&uts_sem);
	same = !strncmp(fake_field(&init_uts_ns.name, i), val,
			__NEW_UTS_LEN + 1);
	up_read(&uts_sem);

	return same;
}

/* Remember field @i of the running utsname as the value to go back to. */
static void fake_capture(int i)
{
	down_read(&uts_sem);
	memcpy(fake_field(&fake_orig, i), fake_field(&init_uts_ns.name, i),
	       __NEW_UTS_LEN + 1);
	up_read(&uts_sem);
}

/*
 * Take every stored value into or out of the running utsname under a
 * single lock hold, so no reader can observe a half-switched uname.
 * Enabling re-reads the real fields first, which is what disabling later
 * puts back.  Caller holds fake_lock.
 */
static void fake_switch(bool on)
{
	int i;

	down_write(&uts_sem);
	for (i = 0; i < FAKE_NFIELDS; i++) {
		if (!(fake_mask & BIT(i)))
			continue;
		if (on)
			memcpy(fake_field(&fake_orig, i),
			       fake_field(&init_uts_ns.name, i),
			       __NEW_UTS_LEN + 1);
		memcpy(fake_field(&init_uts_ns.name, i),
		       fake_field(on ? &fake_vals : &fake_orig, i),
		       __NEW_UTS_LEN + 1);
	}
	up_write(&uts_sem);
}

/* Store the value of field @i, or drop it when @val is empty. */
static void fake_store(int i, const char *val)
{
	if (!*val) {
		if (!(fake_mask & BIT(i)))
			return;
		if (fake_enabled)
			fake_publish(i, &fake_orig);
		fake_mask &= ~BIT(i);
		return;
	}

	/*
	 * Re-writing a value that is already in effect skips the uts_sem
	 * write lock altogether; a boot script re-applying its settings
	 * never gets that far.
	 */
	if ((fake_mask & BIT(i)) && !strcmp(fake_field(&fake_vals, i), val) &&
	    (!fake_enabled || fake_live(i, val)))
		return;

	if (fake_enabled && !(fake_mask & BIT(i)))
		fake_capture(i);

	strscpy(fake_field(&fake_vals, i), val, __NEW_UTS_LEN + 1);
	fake_mask |= BIT(i);

	if (fake_enabled)
		fake_publish(i, &fake_vals);
}

/*
 * Copy a @count-byte sysfs value into @dst, NUL-terminating it and
 * dropping the newline (and the CR, for values that came off a DOS
 * filesystem) that a shell adds.  Returns -EINVAL if it does not fit.
 */
static int fake_copy_value(char *dst, size_t dsz, const char *buf,
			   size_t count)
{
	size_t len = count;

	if (len && buf[len - 1] == '\n')
		len--;
	if (len && buf[len - 1] == '\r')
		len--;

	if (len > dsz - 1)
		return -EINVAL;

	memcpy(dst, buf, len);
	dst[len] = '\0';

	return 0;
}

static ssize_t fake_enable_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", fake_enabled);
}

static ssize_t fake_enable_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	char val[4];
	bool on;

	if (fake_copy_value(val, sizeof(val), buf, count))
		return -EINVAL;
	if (!strcmp(val, "1"))
		on = true;
	else if (!strcmp(val, "0"))
		on = false;
	else
		return -EINVAL;

	mutex_lock(&fake_lock);
	if (on != fake_enabled) {
		fake_switch(on);
		fake_enabled = on;
	}
	mutex_unlock(&fake_lock);

	return count;
}

static ssize_t fake_field_show(int i, char *buf)
{
	char val[__NEW_UTS_LEN + 1];

	down_read(&uts_sem);
	memcpy(val, fake_field(&init_uts_ns.name, i), sizeof(val));
	up_read(&uts_sem);
	val[__NEW_UTS_LEN] = '\0';	/* bound %s even if a field is full */

	return sysfs_emit(buf, "%s\n", val);
}

static ssize_t fake_field_store(int i, const char *buf, size_t count)
{
	char val[__NEW_UTS_LEN + 1];

	if (fake_copy_value(val, sizeof(val), buf, count))
		return -EINVAL;

	mutex_lock(&fake_lock);
	fake_store(i, val);
	mutex_unlock(&fake_lock);

	return count;
}

#define FAKE_ATTR(_name, _idx)						\
static ssize_t fake_##_name##_show(struct kobject *kobj,		\
				   struct kobj_attribute *attr, char *buf) \
{									\
	return fake_field_show(_idx, buf);				\
}									\
static ssize_t fake_##_name##_store(struct kobject *kobj,		\
				    struct kobj_attribute *attr,	\
				    const char *buf, size_t count)	\
{									\
	return fake_field_store(_idx, buf, count);			\
}									\
static struct kobj_attribute fake_##_name##_attr =			\
	__ATTR(_name, 0644, fake_##_name##_show, fake_##_name##_store)

FAKE_ATTR(sysname,	FAKE_SYSNAME);
FAKE_ATTR(nodename,	FAKE_NODENAME);
FAKE_ATTR(release,	FAKE_RELEASE);
FAKE_ATTR(version,	FAKE_VERSION);
FAKE_ATTR(machine,	FAKE_MACHINE);
FAKE_ATTR(domainname,	FAKE_DOMAINNAME);

static struct kobj_attribute fake_enable_attr =
	__ATTR(enable, 0644, fake_enable_show, fake_enable_store);

static struct attribute *fake_attrs[] = {
	&fake_enable_attr.attr,
	&fake_sysname_attr.attr,
	&fake_nodename_attr.attr,
	&fake_release_attr.attr,
	&fake_version_attr.attr,
	&fake_machine_attr.attr,
	&fake_domainname_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fake);

/*
 * Lives next to the /sys/kernel/{ostype,osrelease,version,hostname,
 * domainname} files it overrides, so the two are found together.
 */
static struct kobject *fake_kobj;

static int __init fakeuname_init(void)
{
	int ret;

	fake_kobj = kobject_create_and_add("fakeuname", kernel_kobj);
	if (!fake_kobj)
		return -ENOMEM;

	ret = sysfs_create_groups(fake_kobj, fake_groups);
	if (ret) {
		kobject_put(fake_kobj);
		fake_kobj = NULL;
	}

	return ret;
}
module_init(fakeuname_init);

MODULE_DESCRIPTION("Configurable utsname override in /sys/kernel/fakeuname");
MODULE_LICENSE("GPL");
