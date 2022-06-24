// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 *
 * Derived from MIPS:
 * Copyright (C) 2000, 2001 Kanoj Sarcar
 * Copyright (C) 2000, 2001 Ralf Baechle
 * Copyright (C) 2000, 2001 Silicon Graphics, Inc.
 * Copyright (C) 2000, 2001, 2003 Broadcom Corporation
 */
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <linux/export.h>
#include <linux/time.h>
#include <linux/tracepoint.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/task_stack.h>

#include <asm/cpu.h>
#include <asm/idle.h>
#include <asm/loongson.h>
#include <asm/mmu_context.h>
#include <asm/numa.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <asm/time.h>

int __cpu_number_map[NR_CPUS];   /* Map physical to logical */
EXPORT_SYMBOL(__cpu_number_map);

int __cpu_logical_map[NR_CPUS];		/* Map logical to physical */
EXPORT_SYMBOL(__cpu_logical_map);

/* Number of threads (siblings) per CPU core */
int smp_num_siblings = 1;
EXPORT_SYMBOL(smp_num_siblings);

/* Representing the threads (siblings) of each logical CPU */
cpumask_t cpu_sibling_map[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(cpu_sibling_map);

/* Representing the core map of multi-core chips of each logical CPU */
cpumask_t cpu_core_map[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(cpu_core_map);

static DECLARE_COMPLETION(cpu_starting);
static DECLARE_COMPLETION(cpu_running);

/*
 * A logcal cpu mask containing only one VPE per core to
 * reduce the number of IPIs on large MT systems.
 */
cpumask_t cpu_foreign_map[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(cpu_foreign_map);

/* representing cpus for which sibling maps can be computed */
static cpumask_t cpu_sibling_setup_map;

/* representing cpus for which core maps can be computed */
static cpumask_t cpu_core_setup_map;

struct secondary_data cpuboot_data;
static DEFINE_PER_CPU(int, cpu_state);

enum ipi_msg_type {
	IPI_RESCHEDULE,
	IPI_CALL_FUNCTION,
};

static const char *ipi_types[NR_IPI] __tracepoint_string = {
	[IPI_RESCHEDULE] = "Rescheduling interrupts",
	[IPI_CALL_FUNCTION] = "Function call interrupts",
};

void show_ipi_list(struct seq_file *p, int prec)
{
	unsigned int cpu, i;

	for (i = 0; i < NR_IPI; i++) {
		seq_printf(p, "%*s%u:%s", prec - 1, "IPI", i, prec >= 4 ? " " : "");
		for_each_online_cpu(cpu)
			seq_printf(p, "%10u ", per_cpu(irq_stat, cpu).ipi_irqs[i]);
		seq_printf(p, " LoongArch  %d  %s\n", i + 1, ipi_types[i]);
	}
}

/* Send mailbox buffer via Mail_Send */
static void csr_mail_send(uint64_t data, int cpu, int mailbox)
{
	uint64_t val;

	/* Send high 32 bits */
	val = IOCSR_MBUF_SEND_BLOCKING;
	val |= (IOCSR_MBUF_SEND_BOX_HI(mailbox) << IOCSR_MBUF_SEND_BOX_SHIFT);
	val |= (cpu << IOCSR_MBUF_SEND_CPU_SHIFT);
	val |= (data & IOCSR_MBUF_SEND_H32_MASK);
	iocsr_write64(val, LOONGARCH_IOCSR_MBUF_SEND);

	/* Send low 32 bits */
	val = IOCSR_MBUF_SEND_BLOCKING;
	val |= (IOCSR_MBUF_SEND_BOX_LO(mailbox) << IOCSR_MBUF_SEND_BOX_SHIFT);
	val |= (cpu << IOCSR_MBUF_SEND_CPU_SHIFT);
	val |= (data << IOCSR_MBUF_SEND_BUF_SHIFT);
	iocsr_write64(val, LOONGARCH_IOCSR_MBUF_SEND);
};

static u32 ipi_read_clear(int cpu)
{
	u32 action;

	/* Load the ipi register to figure out what we're supposed to do */
	action = iocsr_read32(LOONGARCH_IOCSR_IPI_STATUS);
	/* Clear the ipi register to clear the interrupt */
	iocsr_write32(action, LOONGARCH_IOCSR_IPI_CLEAR);
	smp_mb();

	return action;
}

static void ipi_write_action(int cpu, u32 action)
{
	unsigned int irq = 0;

	while ((irq = ffs(action))) {
		uint32_t val = IOCSR_IPI_SEND_BLOCKING;

		val |= (irq - 1);
		val |= (cpu << IOCSR_IPI_SEND_CPU_SHIFT);
		iocsr_write32(val, LOONGARCH_IOCSR_IPI_SEND);
		action &= ~BIT(irq - 1);
	}
}

void loongson3_send_ipi_single(int cpu, unsigned int action)
{
	ipi_write_action(cpu_logical_map(cpu), (u32)action);
}

void loongson3_send_ipi_mask(const struct cpumask *mask, unsigned int action)
{
	unsigned int i;

	for_each_cpu(i, mask)
		ipi_write_action(cpu_logical_map(i), (u32)action);
}

irqreturn_t loongson3_ipi_interrupt(int irq, void *dev)
{
	unsigned int action;
	unsigned int cpu = smp_processor_id();

	action = ipi_read_clear(cpu_logical_map(cpu));

	if (action & SMP_RESCHEDULE) {
		scheduler_ipi();
		per_cpu(irq_stat, cpu).ipi_irqs[IPI_RESCHEDULE]++;
	}

	if (action & SMP_CALL_FUNCTION) {
		generic_smp_call_function_interrupt();
		per_cpu(irq_stat, cpu).ipi_irqs[IPI_CALL_FUNCTION]++;
	}

	return IRQ_HANDLED;
}

void __init loongson3_smp_setup(void)
{
	cpu_data[0].core = cpu_logical_map(0) % loongson_sysconf.cores_per_package;
	cpu_data[0].package = cpu_logical_map(0) / loongson_sysconf.cores_per_package;

	iocsr_write32(0xffffffff, LOONGARCH_IOCSR_IPI_EN);
	pr_info("Detected %i available CPU(s)\n", loongson_sysconf.nr_cpus);
}

void __init loongson3_prepare_cpus(unsigned int max_cpus)
{
	int i = 0;

	for (i = 0; i < loongson_sysconf.nr_cpus; i++) {
		set_cpu_present(i, true);
		csr_mail_send(0, __cpu_logical_map[i], 0);
	}

	per_cpu(cpu_state, smp_processor_id()) = CPU_ONLINE;
}

/*
 * Setup the PC, SP, and TP of a secondary processor and start it running!
 */
void loongson3_boot_secondary(int cpu, struct task_struct *idle)
{
	unsigned long entry;

	pr_info("Booting CPU#%d...\n", cpu);

	entry = __pa_symbol((unsigned long)&smpboot_entry);
	cpuboot_data.stack = (unsigned long)__KSTK_TOS(idle);
	cpuboot_data.thread_info = (unsigned long)task_thread_info(idle);

	csr_mail_send(entry, cpu_logical_map(cpu), 0);

	loongson3_send_ipi_single(cpu, SMP_BOOT_CPU);
}

/*
 * SMP init and finish on secondary CPUs
 */
void loongson3_init_secondary(void)
{
	unsigned int cpu = smp_processor_id();
	unsigned int imask = ECFGF_IP0 | ECFGF_IP1 | ECFGF_IP2 |
			     ECFGF_IPI | ECFGF_PMC | ECFGF_TIMER;

	change_csr_ecfg(ECFG0_IM, imask);

	iocsr_write32(0xffffffff, LOONGARCH_IOCSR_IPI_EN);

#ifdef CONFIG_NUMA
	numa_add_cpu(cpu);
#endif
	per_cpu(cpu_state, cpu) = CPU_ONLINE;
	cpu_data[cpu].core =
		     cpu_logical_map(cpu) % loongson_sysconf.cores_per_package;
	cpu_data[cpu].package =
		     cpu_logical_map(cpu) / loongson_sysconf.cores_per_package;
}

void loongson3_smp_finish(void)
{
	local_irq_enable();
	iocsr_write64(0, LOONGARCH_IOCSR_MBUF0);
	pr_info("CPU#%d finished\n", smp_processor_id());
}

#ifdef CONFIG_HOTPLUG_CPU

static bool io_master(int cpu)
{
	if (cpu == 0)
		return true;

	return false;
}

int loongson3_cpu_disable(void)
{
	unsigned long flags;
	unsigned int cpu = smp_processor_id();

	if (io_master(cpu))
		return -EBUSY;

#ifdef CONFIG_NUMA
	numa_remove_cpu(cpu);
#endif
	set_cpu_online(cpu, false);
	calculate_cpu_foreign_map();
	local_irq_save(flags);
	irq_migrate_all_off_this_cpu();
	clear_csr_ecfg(ECFG0_IM);
	local_irq_restore(flags);
	local_flush_tlb_all();

	return 0;
}

void loongson3_cpu_die(unsigned int cpu)
{
	while (per_cpu(cpu_state, cpu) != CPU_DEAD)
		cpu_relax();

	mb();
}

/*
 * The target CPU should go to XKPRANGE (uncached area) and flush
 * ICache/DCache/VCache before the control CPU can safely disable its clock.
 */
static void loongson3_play_dead(int *state_addr)
{
	register int val;
	register void *addr;
	register void (*init_fn)(void);

	__asm__ __volatile__(
		"   li.d %[addr], 0x8000000000000000\n"
		"1: cacop 0x8, %[addr], 0           \n" /* flush ICache */
		"   cacop 0x8, %[addr], 1           \n"
		"   cacop 0x8, %[addr], 2           \n"
		"   cacop 0x8, %[addr], 3           \n"
		"   cacop 0x9, %[addr], 0           \n" /* flush DCache */
		"   cacop 0x9, %[addr], 1           \n"
		"   cacop 0x9, %[addr], 2           \n"
		"   cacop 0x9, %[addr], 3           \n"
		"   addi.w %[sets], %[sets], -1     \n"
		"   addi.d %[addr], %[addr], 0x40   \n"
		"   bnez %[sets], 1b                \n"
		"   li.d %[addr], 0x8000000000000000\n"
		"2: cacop 0xa, %[addr], 0           \n" /* flush VCache */
		"   cacop 0xa, %[addr], 1           \n"
		"   cacop 0xa, %[addr], 2           \n"
		"   cacop 0xa, %[addr], 3           \n"
		"   cacop 0xa, %[addr], 4           \n"
		"   cacop 0xa, %[addr], 5           \n"
		"   cacop 0xa, %[addr], 6           \n"
		"   cacop 0xa, %[addr], 7           \n"
		"   cacop 0xa, %[addr], 8           \n"
		"   cacop 0xa, %[addr], 9           \n"
		"   cacop 0xa, %[addr], 10          \n"
		"   cacop 0xa, %[addr], 11          \n"
		"   cacop 0xa, %[addr], 12          \n"
		"   cacop 0xa, %[addr], 13          \n"
		"   cacop 0xa, %[addr], 14          \n"
		"   cacop 0xa, %[addr], 15          \n"
		"   addi.w %[vsets], %[vsets], -1   \n"
		"   addi.d %[addr], %[addr], 0x40   \n"
		"   bnez   %[vsets], 2b             \n"
		"   li.w   %[val], 0x7              \n" /* *state_addr = CPU_DEAD; */
		"   st.w   %[val], %[state_addr], 0 \n"
		"   dbar 0                          \n"
		"   cacop 0x11, %[state_addr], 0    \n" /* flush entry of *state_addr */
		: [addr] "=&r" (addr), [val] "=&r" (val)
		: [state_addr] "r" (state_addr),
		  [sets] "r" (cpu_data[smp_processor_id()].dcache.sets),
		  [vsets] "r" (cpu_data[smp_processor_id()].vcache.sets));

	local_irq_enable();
	change_csr_ecfg(ECFG0_IM, ECFGF_IPI);

	__asm__ __volatile__(
		"   idle      0			    \n"
		"   li.w      $t0, 0x1020	    \n"
		"   iocsrrd.d %[init_fn], $t0	    \n" /* Get init PC */
		: [init_fn] "=&r" (addr)
		: /* No Input */
		: "a0");
	init_fn = __va(addr);

	init_fn();
	unreachable();
}

void play_dead(void)
{
	int *state_addr;
	unsigned int cpu = smp_processor_id();
	void (*play_dead_uncached)(int *s);

	idle_task_exit();
	play_dead_uncached = (void *)TO_UNCACHE(__pa((unsigned long)loongson3_play_dead));
	state_addr = &per_cpu(cpu_state, cpu);
	mb();
	play_dead_uncached(state_addr);
}

static int loongson3_enable_clock(unsigned int cpu)
{
	uint64_t core_id = cpu_data[cpu].core;
	uint64_t package_id = cpu_data[cpu].package;

	LOONGSON_FREQCTRL(package_id) |= 1 << (core_id * 4 + 3);

	return 0;
}

static int loongson3_disable_clock(unsigned int cpu)
{
	uint64_t core_id = cpu_data[cpu].core;
	uint64_t package_id = cpu_data[cpu].package;

	LOONGSON_FREQCTRL(package_id) &= ~(1 << (core_id * 4 + 3));

	return 0;
}

static int register_loongson3_notifier(void)
{
	return cpuhp_setup_state_nocalls(CPUHP_LOONGARCH_SOC_PREPARE,
					 "loongarch/loongson:prepare",
					 loongson3_enable_clock,
					 loongson3_disable_clock);
}
early_initcall(register_loongson3_notifier);

#endif

/*
 * Power management
 */
#ifdef CONFIG_PM

static int loongson3_ipi_suspend(void)
{
	return 0;
}

static void loongson3_ipi_resume(void)
{
	iocsr_write32(0xffffffff, LOONGARCH_IOCSR_IPI_EN);
}

static struct syscore_ops loongson3_ipi_syscore_ops = {
	.resume         = loongson3_ipi_resume,
	.suspend        = loongson3_ipi_suspend,
};

/*
 * Enable boot cpu ipi before enabling nonboot cpus
 * during syscore_resume.
 */
static int __init ipi_pm_init(void)
{
	register_syscore_ops(&loongson3_ipi_syscore_ops);
	return 0;
}

core_initcall(ipi_pm_init);
#endif

static inline void set_cpu_sibling_map(int cpu)
{
	int i;

	cpumask_set_cpu(cpu, &cpu_sibling_setup_map);

	if (smp_num_siblings <= 1)
		cpumask_set_cpu(cpu, &cpu_sibling_map[cpu]);
	else {
		for_each_cpu(i, &cpu_sibling_setup_map) {
			if (cpus_are_siblings(cpu, i)) {
				cpumask_set_cpu(i, &cpu_sibling_map[cpu]);
				cpumask_set_cpu(cpu, &cpu_sibling_map[i]);
			}
		}
	}
}

static inline void set_cpu_core_map(int cpu)
{
	int i;

	cpumask_set_cpu(cpu, &cpu_core_setup_map);

	for_each_cpu(i, &cpu_core_setup_map) {
		if (cpu_data[cpu].package == cpu_data[i].package) {
			cpumask_set_cpu(i, &cpu_core_map[cpu]);
			cpumask_set_cpu(cpu, &cpu_core_map[i]);
		}
	}
}

/*
 * Calculate a new cpu_foreign_map mask whenever a
 * new cpu appears or disappears.
 */
void calculate_cpu_foreign_map(void)
{
	int i, k, core_present;
	cpumask_t temp_foreign_map;

	/* Re-calculate the mask */
	cpumask_clear(&temp_foreign_map);
	for_each_online_cpu(i) {
		core_present = 0;
		for_each_cpu(k, &temp_foreign_map)
			if (cpus_are_siblings(i, k))
				core_present = 1;
		if (!core_present)
			cpumask_set_cpu(i, &temp_foreign_map);
	}

	for_each_online_cpu(i)
		cpumask_andnot(&cpu_foreign_map[i],
			       &temp_foreign_map, &cpu_sibling_map[i]);
}

/* Preload SMP state for boot cpu */
void smp_prepare_boot_cpu(void)
{
	unsigned int cpu, node, rr_node;

	set_cpu_possible(0, true);
	set_cpu_online(0, true);
	set_my_cpu_offset(per_cpu_offset(0));

	rr_node = first_node(node_online_map);
	for_each_possible_cpu(cpu) {
		node = early_cpu_to_node(cpu);

		/*
		 * The mapping between present cpus and nodes has been
		 * built during MADT and SRAT parsing.
		 *
		 * If possible cpus = present cpus here, early_cpu_to_node
		 * will return valid node.
		 *
		 * If possible cpus > present cpus here (e.g. some possible
		 * cpus will be added by cpu-hotplug later), for possible but
		 * not present cpus, early_cpu_to_node will return NUMA_NO_NODE,
		 * and we just map them to online nodes in round-robin way.
		 * Once hotplugged, new correct mapping will be built for them.
		 */
		if (node != NUMA_NO_NODE)
			set_cpu_numa_node(cpu, node);
		else {
			set_cpu_numa_node(cpu, rr_node);
			rr_node = next_node_in(rr_node, node_online_map);
		}
	}
}

/* called from main before smp_init() */
void __init smp_prepare_cpus(unsigned int max_cpus)
{
	init_new_context(current, &init_mm);
	current_thread_info()->cpu = 0;
	loongson3_prepare_cpus(max_cpus);
	set_cpu_sibling_map(0);
	set_cpu_core_map(0);
	calculate_cpu_foreign_map();
#ifndef CONFIG_HOTPLUG_CPU
	init_cpu_present(cpu_possible_mask);
#endif
}

int __cpu_up(unsigned int cpu, struct task_struct *tidle)
{
	loongson3_boot_secondary(cpu, tidle);

	/* Wait for CPU to start and be ready to sync counters */
	if (!wait_for_completion_timeout(&cpu_starting,
					 msecs_to_jiffies(5000))) {
		pr_crit("CPU%u: failed to start\n", cpu);
		return -EIO;
	}

	/* Wait for CPU to finish startup & mark itself online before return */
	wait_for_completion(&cpu_running);

	return 0;
}

/*
 * First C code run on the secondary CPUs after being started up by
 * the master.
 */
asmlinkage void start_secondary(void)
{
	unsigned int cpu;

	sync_counter();
	cpu = smp_processor_id();
	set_my_cpu_offset(per_cpu_offset(cpu));

	cpu_probe();
	constant_clockevent_init();
	loongson3_init_secondary();

	set_cpu_sibling_map(cpu);
	set_cpu_core_map(cpu);

	notify_cpu_starting(cpu);

	/* Notify boot CPU that we're starting */
	complete(&cpu_starting);

	/* The CPU is running, now mark it online */
	set_cpu_online(cpu, true);

	calculate_cpu_foreign_map();

	/*
	 * Notify boot CPU that we're up & online and it can safely return
	 * from __cpu_up()
	 */
	complete(&cpu_running);

	/*
	 * irq will be enabled in loongson3_smp_finish(), enabling it too
	 * early is dangerous.
	 */
	WARN_ON_ONCE(!irqs_disabled());
	loongson3_smp_finish();

	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}

void __init smp_cpus_done(unsigned int max_cpus)
{
}

static void stop_this_cpu(void *dummy)
{
	set_cpu_online(smp_processor_id(), false);
	calculate_cpu_foreign_map();
	local_irq_disable();
	while (true);
}

void smp_send_stop(void)
{
	smp_call_function(stop_this_cpu, NULL, 0);
}

int setup_profiling_timer(unsigned int multiplier)
{
	return 0;
}

static void flush_tlb_all_ipi(void *info)
{
	local_flush_tlb_all();
}

void flush_tlb_all(void)
{
	on_each_cpu(flush_tlb_all_ipi, NULL, 1);
}

static void flush_tlb_mm_ipi(void *mm)
{
	local_flush_tlb_mm((struct mm_struct *)mm);
}

void flush_tlb_mm(struct mm_struct *mm)
{
	if (atomic_read(&mm->mm_users) == 0)
		return;		/* happens as a result of exit_mmap() */

	preempt_disable();

	if ((atomic_read(&mm->mm_users) != 1) || (current->mm != mm)) {
		on_each_cpu_mask(mm_cpumask(mm), flush_tlb_mm_ipi, mm, 1);
	} else {
		unsigned int cpu;

		for_each_online_cpu(cpu) {
			if (cpu != smp_processor_id() && cpu_context(cpu, mm))
				cpu_context(cpu, mm) = 0;
		}
		local_flush_tlb_mm(mm);
	}

	preempt_enable();
}

struct flush_tlb_data {
	struct vm_area_struct *vma;
	unsigned long addr1;
	unsigned long addr2;
};

static void flush_tlb_range_ipi(void *info)
{
	struct flush_tlb_data *fd = info;

	local_flush_tlb_range(fd->vma, fd->addr1, fd->addr2);
}

void flush_tlb_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;

	preempt_disable();
	if ((atomic_read(&mm->mm_users) != 1) || (current->mm != mm)) {
		struct flush_tlb_data fd = {
			.vma = vma,
			.addr1 = start,
			.addr2 = end,
		};

		on_each_cpu_mask(mm_cpumask(mm), flush_tlb_range_ipi, &fd, 1);
	} else {
		unsigned int cpu;

		for_each_online_cpu(cpu) {
			if (cpu != smp_processor_id() && cpu_context(cpu, mm))
				cpu_context(cpu, mm) = 0;
		}
		local_flush_tlb_range(vma, start, end);
	}
	preempt_enable();
}

static void flush_tlb_kernel_range_ipi(void *info)
{
	struct flush_tlb_data *fd = info;

	local_flush_tlb_kernel_range(fd->addr1, fd->addr2);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	struct flush_tlb_data fd = {
		.addr1 = start,
		.addr2 = end,
	};

	on_each_cpu(flush_tlb_kernel_range_ipi, &fd, 1);
}

static void flush_tlb_page_ipi(void *info)
{
	struct flush_tlb_data *fd = info;

	local_flush_tlb_page(fd->vma, fd->addr1);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long page)
{
	preempt_disable();
	if ((atomic_read(&vma->vm_mm->mm_users) != 1) || (current->mm != vma->vm_mm)) {
		struct flush_tlb_data fd = {
			.vma = vma,
			.addr1 = page,
		};

		on_each_cpu_mask(mm_cpumask(vma->vm_mm), flush_tlb_page_ipi, &fd, 1);
	} else {
		unsigned int cpu;

		for_each_online_cpu(cpu) {
			if (cpu != smp_processor_id() && cpu_context(cpu, vma->vm_mm))
				cpu_context(cpu, vma->vm_mm) = 0;
		}
		local_flush_tlb_page(vma, page);
	}
	preempt_enable();
}
EXPORT_SYMBOL(flush_tlb_page);

static void flush_tlb_one_ipi(void *info)
{
	unsigned long vaddr = (unsigned long) info;

	local_flush_tlb_one(vaddr);
}

void flush_tlb_one(unsigned long vaddr)
{
	on_each_cpu(flush_tlb_one_ipi, (void *)vaddr, 1);
}
EXPORT_SYMBOL(flush_tlb_one);
