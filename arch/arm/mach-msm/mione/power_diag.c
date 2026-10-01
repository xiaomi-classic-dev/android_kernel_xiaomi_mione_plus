/*
 * Persistent MiOne power diagnostics in the unused part of the existing
 * 1 MiB RAM-console reservation. Warm-reset retention depends on firmware.
 * Each CPU owns a ring; the CPU0 watchdog worker owns the third ring.
 * Commit/checksum detects interrupted records. No global lock, IPIs,
 * register reads, printk, or memory allocation in the hot-path writer.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <asm/sizes.h>
#include <mach/mione_power_diag.h>

#ifdef CONFIG_ANDROID_RAM_CONSOLE
#define DIAG_SIZE SZ_16K
#define TZ_OFFSET SZ_8K
#define DIAG_MAGIC 0x4d504431
#define DIAG_VERSION 2
#define DIAG_RINGS 3
#define DIAG_RECORDS 32

struct mione_record {
	u32 commit, stage;
	u64 ns;
	u32 cpu, target, pid, irq_disabled;
	u32 a, b, c, d, sequence, preempt, reserved[2];
};

struct mione_diag {
	u32 magic, version, boot_count, tz_registered;
	u32 sequence[DIAG_RINGS];
	char kernel[128];
	struct mione_record ring[DIAG_RINGS][DIAG_RECORDS];
};

static phys_addr_t diag_phys;
static struct mione_diag *diag;
static void *previous;
static bool previous_valid;

void __init mione_power_diag_reserve(phys_addr_t start)
{
	diag_phys = start;
}

static u32 record_checksum(const struct mione_record *r)
{
	const u32 *words = (const u32 *)r;
	u32 sum = DIAG_MAGIC;
	unsigned int i;

	for (i = 1; i < sizeof(*r) / sizeof(u32); i++)
		sum ^= words[i];
	return sum ? sum : 1;
}

void mione_power_trace(unsigned int stage, int target, u32 a, u32 b,
		       u32 c, u32 d)
{
	struct mione_record r = { .stage = stage, .target = target,
		.a = a, .b = b, .c = c, .d = d };
	struct mione_record *slot;
	unsigned long flags;
	unsigned int channel;
	u32 sequence;

	if (!diag)
		return;
	r.irq_disabled = irqs_disabled();
	r.preempt = preempt_count();
	local_irq_save(flags);
	r.cpu = raw_smp_processor_id();
	if (r.cpu > 1)
		goto out;
	channel = stage == MIONE_WDOG_PET ? 2 : r.cpu;
	sequence = diag->sequence[channel] + 1;
	if (!sequence)
		sequence = 1;
	r.sequence = sequence;
	r.ns = sched_clock();
	r.pid = current->pid;
	slot = &diag->ring[channel][sequence % DIAG_RECORDS];
	slot->commit = 0;
	wmb();
	memcpy((char *)slot + sizeof(u32), (char *)&r + sizeof(u32),
	       sizeof(r) - sizeof(u32));
	wmb();
	slot->commit = record_checksum(&r);
	wmb();
	diag->sequence[channel] = sequence;
	mb();
out:
	local_irq_restore(flags);
}

void *mione_power_diag_tz_buffer(phys_addr_t *phys)
{
	if (!diag)
		return NULL;
	*phys = diag_phys + TZ_OFFSET;
	return (char *)diag + TZ_OFFSET;
}

void mione_power_diag_tz_registered(void)
{
	if (diag) {
		diag->tz_registered = 1;
		mb();
	}
}

static const char *stage_name(unsigned int stage)
{
#define STAGE(s) case MIONE_##s: return #s
	switch (stage) {
	STAGE(FREQ_TARGET); STAGE(FREQ_CANCEL); STAGE(FREQ_QUEUE);
	STAGE(FREQ_WORK); STAGE(FREQ_PRE); STAGE(FREQ_CLOCK);
	STAGE(FREQ_POST); STAGE(FREQ_DONE); STAGE(FREQ_WAIT_DONE);
	STAGE(ACPU_ENTER); STAGE(ACPU_LOCK); STAGE(ACPU_LOCKED);
	STAGE(VDD_MEM_UP); STAGE(VDD_DIG_UP); STAGE(VDD_SC_UP);
	STAGE(CPU_SWITCH); STAGE(L2_LOCK); STAGE(L2_LOCKED);
	STAGE(L2_DONE); STAGE(BUS_VOTE); STAGE(VDD_SC_DOWN);
	STAGE(VDD_DIG_DOWN); STAGE(VDD_MEM_DOWN); STAGE(ACPU_DONE);
	STAGE(PLL_START); STAGE(PLL_WAIT_START); STAGE(PLL_WAIT_DONE);
	STAGE(PLL_DONE); STAGE(SAW_ENTER); STAGE(SAW_DONE);
	STAGE(SAW_ERROR); STAGE(WDOG_INIT); STAGE(WDOG_PET);
	STAGE(WDOG_SUSPEND); STAGE(WDOG_RESUME); STAGE(WDOG_BARK);
	STAGE(WDOG_PANIC); STAGE(REBOOT);
	default: return "UNKNOWN";
	}
#undef STAGE
}

static void show_diag(struct seq_file *m, const char *label,
		      const struct mione_diag *data)
{
	unsigned int channel, i;
	u32 last, sequence, before;
	struct mione_record r;

	seq_printf(m, "%s boot_count=%u kernel=%.*s tz_registered=%u\n",
		   label, data->boot_count, (int)sizeof(data->kernel),
		   data->kernel, data->tz_registered);
	for (channel = 0; channel < DIAG_RINGS; channel++) {
		last = ACCESS_ONCE(data->sequence[channel]);
		seq_printf(m, "channel=%u last_sequence=%u\n", channel, last);
		for (i = 0; i < DIAG_RECORDS; i++) {
			const struct mione_record *slot;

			sequence = last - (DIAG_RECORDS - 1 - i);
			if (!sequence || sequence > last)
				continue;
			slot = &data->ring[channel][sequence % DIAG_RECORDS];
			before = ACCESS_ONCE(slot->commit);
			rmb();
			memcpy(&r, slot, sizeof(r));
			rmb();
			if (!before || before != ACCESS_ONCE(slot->commit) ||
			    before != record_checksum(&r) ||
			    r.sequence != sequence)
				continue;
			seq_printf(m, "seq=%u ns=%llu cpu=%u target=%d pid=%u "
				   "irq=%u preempt=%x stage=%s(%u) "
				   "a=%u b=%u c=%u d=%u\n", r.sequence,
				   r.ns, r.cpu, (int)r.target, r.pid,
				   r.irq_disabled, r.preempt,
				   stage_name(r.stage), r.stage,
				   r.a, r.b, r.c, r.d);
		}
	}
}

static int power_diag_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "mione_power_diag version=%u physical=0x%08llx "
		   "bytes=%u tz_physical=0x%08llx\n", DIAG_VERSION,
		   (unsigned long long)diag_phys, DIAG_SIZE,
		   (unsigned long long)(diag_phys + TZ_OFFSET));
	if (previous_valid)
		show_diag(m, "previous", previous);
	else
		seq_puts(m, "previous: unavailable (first boot or RAM not retained)\n");
	show_diag(m, "current", diag);
	return 0;
}

static int power_diag_open(struct inode *inode, struct file *file)
{
	return single_open(file, power_diag_show, NULL);
}

static const struct file_operations power_diag_fops = {
	.open = power_diag_open, .read = seq_read, .llseek = seq_lseek,
	.release = single_release,
};

static ssize_t tz_last_read(struct file *file, char __user *buf,
			    size_t count, loff_t *pos)
{
	if (!previous_valid || !((struct mione_diag *)previous)->tz_registered)
		return 0;
	return simple_read_from_buffer(buf, count, pos,
				      (char *)previous + TZ_OFFSET, PAGE_SIZE);
}

static const struct file_operations tz_last_fops = {
	.read = tz_last_read, .llseek = default_llseek,
};

static int __init power_diag_init(void)
{
	struct page *pages[DIAG_SIZE / PAGE_SIZE];
	void *mapped;
	unsigned int i, boot_count = 1;
	struct mione_diag *old;

	BUILD_BUG_ON(sizeof(struct mione_record) != 64);
	BUILD_BUG_ON(sizeof(struct mione_diag) > TZ_OFFSET);
	if (!diag_phys)
		return 0;
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		pages[i] = pfn_to_page((diag_phys >> PAGE_SHIFT) + i);
	mapped = vmap(pages, ARRAY_SIZE(pages), VM_MAP,
		      pgprot_noncached(PAGE_KERNEL));
	if (!mapped)
		return -ENOMEM;
	previous = kmemdup(mapped, DIAG_SIZE, GFP_KERNEL);
	if (!previous) {
		vunmap(mapped);
		return -ENOMEM;
	}
	old = previous;
	previous_valid = old->magic == DIAG_MAGIC &&
			 old->version == DIAG_VERSION;
	if (previous_valid)
		boot_count = old->boot_count + 1;
	memset(mapped, 0, DIAG_SIZE);
	old = mapped;
	old->version = DIAG_VERSION;
	old->boot_count = boot_count;
	scnprintf(old->kernel, sizeof(old->kernel), "%s %s %s",
		  utsname()->release, utsname()->version, utsname()->machine);
	wmb();
	old->magic = DIAG_MAGIC;
	mb();
	diag = mapped;
	if (!proc_create("mione_power_diag", 0400, NULL, &power_diag_fops) ||
	    !proc_create("mione_tz_last", 0400, NULL, &tz_last_fops))
		pr_err("mione_power: failed to expose diagnostic proc files\n");
	pr_info("mione_power: persistent diag v2 at %08llx, previous=%u, "
		"cpufreq=stock-priority\n", (unsigned long long)diag_phys,
		previous_valid);
	return 0;
}
/* RAM console and platform initialization have completed before this. */
device_initcall(power_diag_init);
#endif
