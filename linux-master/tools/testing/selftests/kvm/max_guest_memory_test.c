// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/atomic.h>

#include "kvm_util.h"
#include "test_util.h"
#include "guest_modes.h"
#include "processor.h"

static void guest_code(uint64_t start_gpa, uint64_t end_gpa, uint64_t stride)
{
	uint64_t gpa;

	for (gpa = start_gpa; gpa < end_gpa; gpa += stride)
		*((volatile uint64_t *)gpa) = gpa;

	GUEST_DONE();
}

struct vcpu_info {
	struct kvm_vm *vm;
	uint32_t id;
	uint64_t start_gpa;
	uint64_t end_gpa;
};

static int nr_vcpus;
static atomic_t rendezvous;

static void rendezvous_with_boss(void)
{
	int orig = atomic_read(&rendezvous);

	if (orig > 0) {
		atomic_dec_and_test(&rendezvous);
		while (atomic_read(&rendezvous) > 0)
			cpu_relax();
	} else {
		atomic_inc(&rendezvous);
		while (atomic_read(&rendezvous) < 0)
			cpu_relax();
	}
}

static void run_vcpu(struct kvm_vm *vm, uint32_t vcpu_id)
{
	vcpu_run(vm, vcpu_id);
	ASSERT_EQ(get_ucall(vm, vcpu_id, NULL), UCALL_DONE);
}

static void *vcpu_worker(void *data)
{
	struct vcpu_info *vcpu = data;
	struct kvm_vm *vm = vcpu->vm;
	struct kvm_sregs sregs;
	struct kvm_regs regs;

	vcpu_args_set(vm, vcpu->id, 3, vcpu->start_gpa, vcpu->end_gpa,
		      vm_get_page_size(vm));

	/* Snapshot regs before the first run. */
	vcpu_regs_get(vm, vcpu->id, &regs);
	rendezvous_with_boss();

	run_vcpu(vm, vcpu->id);
	rendezvous_with_boss();
	vcpu_regs_set(vm, vcpu->id, &regs);
	vcpu_sregs_get(vm, vcpu->id, &sregs);
#ifdef __x86_64__
	/* Toggle CR0.WP to trigger a MMU context reset. */
	sregs.cr0 ^= X86_CR0_WP;
#endif
	vcpu_sregs_set(vm, vcpu->id, &sregs);
	rendezvous_with_boss();

	run_vcpu(vm, vcpu->id);
	rendezvous_with_boss();

	return NULL;
}

static pthread_t *spawn_workers(struct kvm_vm *vm, uint64_t start_gpa,
				uint64_t end_gpa)
{
	struct vcpu_info *info;
	uint64_t gpa, nr_bytes;
	pthread_t *threads;
	int i;

	threads = malloc(nr_vcpus * sizeof(*threads));
	TEST_ASSERT(threads, "Failed to allocate vCPU threads");

	info = malloc(nr_vcpus * sizeof(*info));
	TEST_ASSERT(info, "Failed to allocate vCPU gpa ranges");

	nr_bytes = ((end_gpa - start_gpa) / nr_vcpus) &
			~((uint64_t)vm_get_page_size(vm) - 1);
	TEST_ASSERT(nr_bytes, "C'mon, no way you have %d CPUs", nr_vcpus);

	for (i = 0, gpa = start_gpa; i < nr_vcpus; i++, gpa += nr_bytes) {
		info[i].vm = vm;
		info[i].id = i;
		info[i].start_gpa = gpa;
		info[i].end_gpa = gpa + nr_bytes;
		pthread_create(&threads[i], NULL, vcpu_worker, &info[i]);
	}
	return threads;
}

static void rendezvous_with_vcpus(struct timespec *time, const char *name)
{
	int i, rendezvoused;

	pr_info("Waiting for vCPUs to finish %s...\n", name);

	rendezvoused = atomic_read(&rendezvous);
	for (i = 0; abs(rendezvoused) != 1; i++) {
		usleep(100);
		if (!(i & 0x3f))
			pr_info("\r%d vCPUs haven't rendezvoused...",
				abs(rendezvoused) - 1);
		rendezvoused = atomic_read(&rendezvous);
	}

	clock_gettime(CLOCK_MONOTONIC, time);

	/* Release the vCPUs after getting the time of the previous action. */
	pr_info("\rAll vCPUs finished %s, releasing...\n", name);
	if (rendezvoused > 0)
		atomic_set(&rendezvous, -nr_vcpus - 1);
	else
		atomic_set(&rendezvous, nr_vcpus + 1);
}

static void calc_default_nr_vcpus(void)
{
	cpu_set_t possible_mask;
	int r;

	r = sched_getaffinity(0, sizeof(possible_mask), &possible_mask);
	TEST_ASSERT(!r, "sched_getaffinity failed, errno = %d (%s)",
		    errno, strerror(errno));

	nr_vcpus = CPU_COUNT(&possible_mask) * 3/4;
	TEST_ASSERT(nr_vcpus > 0, "Uh, no CPUs?");
}

int main(int argc, char *argv[])
{
	/*
	 * Skip the first 4gb and slot0.  slot0 maps <1gb and is used to back
	 * the guest's code, stack, and page tables.  Because selftests creates
	 * an IRQCHIP, a.k.a. a local APIC, KVM creates an internal memslot
	 * just below the 4gb boundary.  This test could create memory at
	 * 1gb-3gb,but it's simpler to skip straight to 4gb.
	 */
	const uint64_t size_1gb = (1 << 30);
	const uint64_t start_gpa = (4ull * size_1gb);
	const int first_slot = 1;

	struct timespec time_start, time_run1, time_reset, time_run2;
	uint64_t max_gpa, gpa, slot_size, max_mem, i;
	int max_slots, slot, opt, fd;
	bool hugepages = false;
	pthread_t *threads;
	struct kvm_vm *vm;
	void *mem;

	/*
	 * Default to 2gb so that maxing out systems with MAXPHADDR=46, which
	 * are quite common for x86, requires changing only max_mem (KVM allows
	 * 32k memslots, 32k * 2gb == ~64tb of guest memory).
	 */
	slot_size = 2 * size_1gb;

	max_slots = kvm_check_cap(KVM_CAP_NR_MEMSLOTS);
	TEST_ASSERT(max_slots > first_slot, "KVM is broken");

	/* All KVM MMUs should be able to survive a 128gb guest. */
	max_mem = 128 * size_1gb;

	calc_default_nr_vcpus();

	while ((opt = getopt(argc, argv, "c:h:m:s:H")) != -1) {
		switch (opt) {
		case 'c':
			nr_vcpus = atoi(optarg);
			TEST_ASSERT(nr_vcpus > 0, "number of vcpus must be >0");
			break;
		case 'm':
			max_mem = atoi(optarg) * size_1gb;
			TEST_ASSERT(max_mem > 0, "memory size must be >0");
			break;
		case 's':
			slot_size = atoi(optarg) * size_1gb;
			TEST_ASSERT(slot_size > 0, "slot size must be >0");
			break;
		case 'H':
			hugepages = true;
			break;
		case 'h':
		default:
			printf("usage: %s [-c nr_vcpus] [-m max_mem_in_gb] [-s slot_size_in_gb] [-H]\n", argv[0]);
			exit(1);
		}
	}

	vm = vm_create_default_with_vcpus(nr_vcpus, 0, 0, guest_code, NULL);

	max_gpa = vm_get_max_gfn(vm) << vm_get_page_shift(vm);
	TEST_ASSERT(max_gpa > (4 * slot_size), "MAXPHYADDR <4gb ");

	fd = kvm_memfd_alloc(slot_size, hugepages);
	mem = mmap(NULL, slot_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap() failed");

	TEST_ASSERT(!madvise(mem, slot_size, MADV_NOHUGEPAGE), "madvise() failed");

	/* Pre-fault the memory to avoid taking mmap_sem on guest page faults. */
	for (i = 0; i < slot_size; i += vm_get_page_size(vm))
		((uint8_t *)mem)[i] = 0xaa;

	gpa = 0;
	for (slot = first_slot; slot < max_slots; slot++) {
		gpa = start_gpa + ((slot - first_slot) * slot_size);
		if (gpa + slot_size > max_gpa)
			break;

		if ((gpa - start_gpa) >= max_mem)
			break;

		vm_set_user_memory_region(vm, slot, 0, gpa, slot_size, mem);

#ifdef __x86_64__
		/* Identity map memory in the guest using 1gb pages. */
		for (i = 0; i < slot_size; i += size_1gb)
			__virt_pg_map(vm, gpa + i, gpa + i, PG_LEVEL_1G);
#else
		for (i = 0; i < slot_size; i += vm_get_page_size(vm))
			virt_pg_map(vm, gpa + i, gpa + i);
#endif
	}

	atomic_set(&rendezvous, nr_vcpus + 1);
	threads = spawn_workers(vm, start_gpa, gpa);

	pr_info("Running with %lugb of guest memory and %u vCPUs\n",
		(gpa - start_gpa) / size_1gb, nr_vcpus);

	rendezvous_with_vcpus(&time_start, "spawning");
	rendezvous_with_vcpus(&time_run1, "run 1");
	rendezvous_with_vcpus(&time_reset, "reset");
	rendezvous_with_vcpus(&time_run2, "run 2");

	time_run2  = timespec_sub(time_run2,   time_reset);
	time_reset = timespec_sub(time_reset, time_run1);
	time_run1  = timespec_sub(time_run1,   time_start);

	pr_info("run1 = %ld.%.9lds, reset = %ld.%.9lds, run2 =  %ld.%.9lds\n",
		time_run1.tv_sec, time_run1.tv_nsec,
		time_reset.tv_sec, time_reset.tv_nsec,
		time_run2.tv_sec, time_run2.tv_nsec);

	/*
	 * Delete even numbered slots (arbitrary) and unmap the first half of
	 * the backing (also arbitrary) to verify KVM correctly drops all
	 * references to the removed regions.
	 */
	for (slot = (slot - 1) & ~1ull; slot >= first_slot; slot -= 2)
		vm_set_user_memory_region(vm, slot, 0, 0, 0, NULL);

	munmap(mem, slot_size / 2);

	/* Sanity check that the vCPUs actually ran. */
	for (i = 0; i < nr_vcpus; i++)
		pthread_join(threads[i], NULL);

	/*
	 * Deliberately exit without deleting the remaining memslots or closing
	 * kvm_fd to test cleanup via mmu_notifier.release.
	 */
}
