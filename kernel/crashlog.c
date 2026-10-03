// SPDX-License-Identifier: GPL-2.0
/*
 * crashlog - keep the kernel log in the device's reserved crash area and
 * expose it through pstore after the next boot.
 *
 * The kernel log lives in a ring buffer in RAM and is gone the moment the
 * machine reboots, which is exactly when it is wanted most.  On the Xiaomi
 * SM8550 (vermeer) the bootloader reserves a region for that purpose and the
 * device tree describes it as
 *
 *	&soc {
 *		mtdoops_pmsg@0xa7000000 {
 *			compatible = "mtdoops_pmsg";
 *			reg = <0xa7000000 0x400000>;
 *			console-size = <0x200000>;
 *			pmsg-size = <0x200000>;
 *		};
 *	};
 *
 * crashlog memremap()s the console part of that region, hands it to pstore
 * as a backend and lets the pstore kmsg dumper write the log into it when
 * the kernel panics.  On the next boot pstore reads the records back and
 * creates files under /sys/fs/pstore, so the previous boot's log is
 * available even when the current boot did not crash.
 *
 * The reserved region is still part of the System RAM iomem resource, so it
 * must not be claimed with request_mem_region() (that always fails with
 * -EBUSY, just as it would for ramoops).  memremap(MEMREMAP_WB) is used
 * instead: for RAM it returns the existing linear mapping and otherwise
 * falls back to a fresh ioremap(), which is exactly what the pstore ram
 * backend does.
 *
 * The region is a sequence of self-describing records, each followed by its
 * payload:
 *
 *	+----------------------------+ offset 0
 *	| struct crashlog_hdr        |
 *	+----------------------------+
 *	| payload (record->size)     |
 *	+----------------------------+ <-- next record
 *	| ...                        |
 *	+----------------------------+
 *
 * A record is written by first storing the payload, then the header and
 * finally the magic.  A reset in the middle of a dump therefore leaves a
 * partial record with no magic, which is skipped, instead of a torn one.
 *
 * The payload may be compressed (pstore enables compression by default), so
 * the compressed flag is stored and handed back to pstore on read for it to
 * decompress.
 *
 * Only one pstore backend can be active at a time, and the generic ramoops
 * driver (postcore_initcall) binds the overlapping ramoops@0xa7000000 node
 * before this driver even gets a chance to probe.  To let crashlog own the
 * region either build without CONFIG_PSTORE_RAM, or boot with
 * pstore.backend=crashlog so that ramoops fails its own registration and
 * releases the region before crashlog probes.
 */

#define pr_fmt(fmt)	"crashlog: " fmt

#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/pstore.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/uaccess.h>

#define CRASHLOG_MAGIC		0x474c5243	/* "CRLG" */
#define CRASHLOG_VERSION	1

/* the record payload was compressed by pstore */
#define CRASHLOG_F_COMPRESSED	BIT(0)

struct crashlog_hdr {
	__le32	magic;
	__le32	version;
	__le32	flags;
	__le32	len;		/* payload length */
	__le64	time_s;
	__le32	time_ns;
	__le32	reason;
	__le32	id;
} __packed;

struct crashlog {
	struct device		*dev;
	void			*base;
	phys_addr_t		phys;
	size_t			size;
	size_t			console_size;
	size_t			pmsg_size;

	struct pstore_info	pstore;
	void			*buf;

	/* next free offset while a dump is being accumulated */
	size_t			write_len;
	/* offset of the next record handed to pstore */
	size_t			read_off;
	/* id handed out to the next record written */
	u32			next_id;
};

static struct crashlog cl;

/* Read and sanity check the record header at @off.  Returns false at the end
 * of the records (invalid magic or a truncated/malformed record).
 */
static bool crashlog_read_hdr(size_t off, struct crashlog_hdr *hdr)
{
	u32 len;

	if (off + sizeof(*hdr) > cl.console_size)
		return false;

	memcpy(hdr, cl.base + off, sizeof(*hdr));
	if (le32_to_cpu(hdr->magic) != CRASHLOG_MAGIC ||
	    le32_to_cpu(hdr->version) != CRASHLOG_VERSION)
		return false;

	len = le32_to_cpu(hdr->len);
	if (len > cl.console_size - off - sizeof(*hdr))
		return false;

	return true;
}

/* Erase the magic of the record at @off so readers skip it. */
static void crashlog_zap(size_t off)
{
	u32 zero = 0;

	memcpy(cl.base + off, &zero, sizeof(zero));
}

/* Store the magic last, once the rest of the record is in place. */
static void crashlog_publish_magic(size_t off)
{
	__le32 magic = cpu_to_le32(CRASHLOG_MAGIC);

	/* order the magic store after the header/payload stores */
	wmb();
	memcpy(cl.base + off, &magic, sizeof(magic));
}

/* Highest record id currently stored, so new records stay unique. */
static u32 crashlog_max_id(void)
{
	struct crashlog_hdr hdr;
	size_t off = 0;
	u32 max = 0;

	while (crashlog_read_hdr(off, &hdr)) {
		u32 id = le32_to_cpu(hdr.id);

		if (id > max)
			max = id;
		off += sizeof(hdr) + le32_to_cpu(hdr.len);
	}

	return max;
}

static int crashlog_pstore_open(struct pstore_info *psi)
{
	cl.read_off = 0;
	return 0;
}

static int crashlog_pstore_close(struct pstore_info *psi)
{
	return 0;
}

static ssize_t crashlog_pstore_read(struct pstore_record *record)
{
	struct crashlog_hdr hdr;
	size_t len;

	if (!crashlog_read_hdr(cl.read_off, &hdr))
		return 0;

	len = le32_to_cpu(hdr.len);
	if (!len)
		return 0;

	record->buf = kmalloc(len + 1, GFP_KERNEL);
	if (!record->buf)
		return -ENOMEM;

	memcpy(record->buf, cl.base + cl.read_off + sizeof(hdr), len);
	record->buf[len] = '\0';

	record->size		= len;
	record->type		= PSTORE_TYPE_DMESG;
	record->id		= le32_to_cpu(hdr.id);
	record->reason		= le32_to_cpu(hdr.reason);
	record->compressed	= !!(le32_to_cpu(hdr.flags) &
				       CRASHLOG_F_COMPRESSED);
	record->time.tv_sec	= le64_to_cpu(hdr.time_s);
	record->time.tv_nsec	= le32_to_cpu(hdr.time_ns);

	cl.read_off += sizeof(hdr) + len;
	return len;
}

static int crashlog_pstore_write(struct pstore_record *record)
{
	struct crashlog_hdr hdr;
	size_t hsize = sizeof(hdr);
	size_t len = record->size;
	size_t off;

	/* The first part of a dump replaces whatever was stored before. */
	if (record->part <= 1)
		cl.write_len = 0;

	off = cl.write_len;

	/* Not enough room left: start over. */
	if (hsize + len > cl.console_size - off) {
		off = 0;
		cl.write_len = 0;
		if (len > cl.console_size - hsize)
			len = cl.console_size - hsize;
	}

	memcpy(cl.base + off + hsize, record->buf, len);

	memset(&hdr, 0, sizeof(hdr));
	hdr.version	= cpu_to_le32(CRASHLOG_VERSION);
	hdr.flags	= cpu_to_le32(record->compressed ?
				       CRASHLOG_F_COMPRESSED : 0);
	hdr.len		= cpu_to_le32(len);
	hdr.time_s	= cpu_to_le64(record->time.tv_sec);
	hdr.time_ns	= cpu_to_le32(record->time.tv_nsec);
	hdr.reason	= cpu_to_le32(record->reason);
	hdr.id		= cpu_to_le32(cl.next_id++);
	memcpy(cl.base + off, &hdr, hsize);

	/* Publish the magic only once the record is complete. */
	crashlog_publish_magic(off);

	record->id = le32_to_cpu(hdr.id);
	cl.write_len = off + hsize + len;
	return 0;
}

static int crashlog_pstore_erase(struct pstore_record *record)
{
	struct crashlog_hdr hdr;
	size_t off = 0;

	while (crashlog_read_hdr(off, &hdr)) {
		if (le32_to_cpu(hdr.id) == record->id) {
			/* Reading stops at the first missing magic, so only
			 * the last record can be erased without hiding the
			 * ones after it.  This is good enough for crash logs.
			 */
			crashlog_zap(off);
			return 0;
		}
		off += sizeof(hdr) + le32_to_cpu(hdr.len);
	}

	return 0;
}

/*
 * /proc/crashlog: dump the records currently in the region and allow a
 * manual dump so the whole write -> read path can be exercised without
 * panicking.  Echoing '1' (or 't') triggers the same kmsg_dump() path a
 * real oops/panic uses.
 */
static int crashlog_show(struct seq_file *m, void *v)
{
	struct crashlog_hdr hdr;
	size_t off = 0;
	unsigned int nr = 0;

	seq_printf(m, "phys=%pa size=%zu console=%zu pmsg=%zu next_id=%u\n",
		   &cl.phys, cl.size, cl.console_size, cl.pmsg_size,
		   cl.next_id);

	while (crashlog_read_hdr(off, &hdr)) {
		u32 len = le32_to_cpu(hdr.len);
		u32 flags = le32_to_cpu(hdr.flags);

		seq_printf(m,
			   "record %u: off=%zu ver=%u flags=0x%x len=%u id=%u reason=%u time=%lld.%09u\n",
			   nr++, off, le32_to_cpu(hdr.version), flags, len,
			   le32_to_cpu(hdr.id), le32_to_cpu(hdr.reason),
			   (long long)le64_to_cpu(hdr.time_s),
			   le32_to_cpu(hdr.time_ns));

		if (len && !(flags & CRASHLOG_F_COMPRESSED)) {
			size_t show = min_t(size_t, len, 8192);

			seq_puts(m, "--- payload ---\n");
			seq_write(m, cl.base + off + sizeof(hdr), show);
			if (show < len)
				seq_puts(m, "\n...(truncated)\n");
			seq_putc(m, '\n');
		}

		off += sizeof(hdr) + len;
	}

	if (!nr)
		seq_puts(m, "no valid record\n");

	return 0;
}

static int crashlog_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, crashlog_show, NULL);
}

static ssize_t crashlog_proc_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	char c;

	if (!count || get_user(c, ubuf))
		return -EFAULT;

	if (c == '1' || c == 't' || c == 'd') {
		pr_info("triggering a manual kmsg dump\n");
		kmsg_dump(KMSG_DUMP_OOPS);
	}

	return count;
}

static const struct proc_ops crashlog_proc_ops = {
	.proc_open	= crashlog_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= crashlog_proc_write,
};

static int crashlog_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	u32 console_size = 0, pmsg_size = 0;
	size_t hsize = sizeof(struct crashlog_hdr);
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no memory resource in device tree\n");
		return -EINVAL;
	}

	cl.dev = &pdev->dev;
	cl.phys = res->start;
	cl.size = resource_size(res);

	of_property_read_u32(np, "console-size", &console_size);
	of_property_read_u32(np, "pmsg-size", &pmsg_size);
	if (!console_size || console_size > cl.size)
		console_size = cl.size;
	if (console_size <= hsize) {
		dev_err(&pdev->dev, "region too small (%zu bytes)\n", cl.size);
		return -EINVAL;
	}
	cl.console_size = console_size;
	cl.pmsg_size = pmsg_size;

	/*
	 * Do not use request_mem_region(): the reserved region is still part
	 * of the System RAM iomem resource, so it is always busy.  For RAM,
	 * memremap(MEMREMAP_WB) returns the existing linear mapping; for
	 * everything else it falls back to ioremap().
	 */
	cl.base = memremap(cl.phys, cl.size, MEMREMAP_WB);
	if (!cl.base) {
		dev_err(&pdev->dev, "cannot map %pa (%zu bytes)\n",
			&cl.phys, cl.size);
		return -ENOMEM;
	}

	/* keep ids unique with records left over from a previous boot */
	cl.next_id = crashlog_max_id() + 1;

	cl.pstore.bufsize = cl.console_size - hsize;
	cl.pstore.buf = kvmalloc(cl.pstore.bufsize, GFP_KERNEL);
	if (!cl.pstore.buf) {
		ret = -ENOMEM;
		goto err_map;
	}

	cl.pstore.owner		= THIS_MODULE;
	cl.pstore.name		= "crashlog";
	cl.pstore.open		= crashlog_pstore_open;
	cl.pstore.close		= crashlog_pstore_close;
	cl.pstore.read		= crashlog_pstore_read;
	cl.pstore.write		= crashlog_pstore_write;
	cl.pstore.erase		= crashlog_pstore_erase;
	cl.pstore.flags		= PSTORE_FLAGS_DMESG;
	cl.pstore.max_reason	= KMSG_DUMP_MAX;

	ret = pstore_register(&cl.pstore);
	if (ret) {
		dev_err(&pdev->dev,
			"pstore_register failed (%d): ramoops active, use pstore.backend=crashlog\n",
			ret);
		goto err_buf;
	}

	proc_create("crashlog", 0600, NULL, &crashlog_proc_ops);

	dev_info(&pdev->dev,
		 "%zu bytes at %pa (console %zu, pmsg %zu)\n",
		 cl.size, &cl.phys, cl.console_size, cl.pmsg_size);
	return 0;

err_buf:
	kvfree(cl.pstore.buf);
	cl.pstore.buf = NULL;
err_map:
	memunmap(cl.base);
	cl.base = NULL;
	return ret;
}

static int crashlog_remove(struct platform_device *pdev)
{
	pstore_unregister(&cl.pstore);

	remove_proc_entry("crashlog", NULL);

	kvfree(cl.pstore.buf);
	cl.pstore.buf = NULL;

	memunmap(cl.base);
	cl.base = NULL;

	return 0;
}

static const struct of_device_id crashlog_of_match[] = {
	{ .compatible = "mtdoops_pmsg" },
	{ .compatible = "xiaomi,crashlog" },
	{ }
};
MODULE_DEVICE_TABLE(of, crashlog_of_match);

static struct platform_driver crashlog_driver = {
	.probe	= crashlog_probe,
	.remove	= crashlog_remove,
	.driver	= {
		.name		= "crashlog",
		.of_match_table	= crashlog_of_match,
	},
};

/*
 * The mtdoops_pmsg node lives on a simple-bus, so its platform device is
 * created by the arch_initcall_sync DT population (before this driver is
 * registered) and is bound here as soon as the driver registers.
 */
static int __init crashlog_init(void)
{
	return platform_driver_register(&crashlog_driver);
}
subsys_initcall(crashlog_init);

MODULE_DESCRIPTION("Save the kernel log to the reserved crash region and expose it via pstore");
MODULE_LICENSE("GPL");
