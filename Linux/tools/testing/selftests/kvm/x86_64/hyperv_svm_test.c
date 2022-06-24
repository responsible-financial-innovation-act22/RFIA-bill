// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM_GET/SET_* tests
 *
 * Copyright (C) 2022, Red Hat, Inc.
 *
 * Tests for Hyper-V extensions to SVM.
 */
#define _GNU_SOURCE /* for program_invocation_short_name */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/bitmap.h>

#include "test_util.h"

#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"
#include "hyperv.h"

#define VCPU_ID		1
#define L2_GUEST_STACK_SIZE 256

struct hv_enlightenments {
	struct __packed hv_enlightenments_control {
		u32 nested_flush_hypercall:1;
		u32 msr_bitmap:1;
		u32 enlightened_npt_tlb: 1;
		u32 reserved:29;
	} __packed hv_enlightenments_control;
	u32 hv_vp_id;
	u64 hv_vm_id;
	u64 partition_assist_page;
	u64 reserved;
} __packed;

/*
 * Hyper-V uses the software reserved clean bit in VMCB
 */
#define VMCB_HV_NESTED_ENLIGHTENMENTS (1U << 31)

static inline void vmmcall(void)
{
	__asm__ __volatile__("vmmcall");
}

void l2_guest_code(void)
{
	GUEST_SYNC(3);
	/* Exit to L1 */
	vmmcall();

	/* MSR-Bitmap tests */
	rdmsr(MSR_FS_BASE); /* intercepted */
	rdmsr(MSR_FS_BASE); /* intercepted */
	rdmsr(MSR_GS_BASE); /* not intercepted */
	vmmcall();
	rdmsr(MSR_GS_BASE); /* intercepted */

	GUEST_SYNC(5);

	/* Done, exit to L1 and never come back.  */
	vmmcall();
}

static void __attribute__((__flatten__)) guest_code(struct svm_test_data *svm)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb = svm->vmcb;
	struct hv_enlightenments *hve =
		(struct hv_enlightenments *)vmcb->control.reserved_sw;

	GUEST_SYNC(1);

	wrmsr(HV_X64_MSR_GUEST_OS_ID, (u64)0x8100 << 48);

	GUEST_ASSERT(svm->vmcb_gpa);
	/* Prepare for L2 execution. */
	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	GUEST_SYNC(2);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_VMMCALL);
	GUEST_SYNC(4);
	vmcb->save.rip += 3;

	/* Intercept RDMSR 0xc0000100 */
	vmcb->control.intercept |= 1ULL << INTERCEPT_MSR_PROT;
	set_bit(2 * (MSR_FS_BASE & 0x1fff), svm->msr + 0x800);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_MSR);
	vmcb->save.rip += 2; /* rdmsr */

	/* Enable enlightened MSR bitmap */
	hve->hv_enlightenments_control.msr_bitmap = 1;
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_MSR);
	vmcb->save.rip += 2; /* rdmsr */

	/* Intercept RDMSR 0xc0000101 without telling KVM about it */
	set_bit(2 * (MSR_GS_BASE & 0x1fff), svm->msr + 0x800);
	/* Make sure HV_VMX_ENLIGHTENED_CLEAN_FIELD_MSR_BITMAP is set */
	vmcb->control.clean |= VMCB_HV_NESTED_ENLIGHTENMENTS;
	run_guest(vmcb, svm->vmcb_gpa);
	/* Make sure we don't see SVM_EXIT_MSR here so eMSR bitmap works */
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_VMMCALL);
	vmcb->save.rip += 3; /* vmcall */

	/* Now tell KVM we've changed MSR-Bitmap */
	vmcb->control.clean &= ~VMCB_HV_NESTED_ENLIGHTENMENTS;
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_MSR);
	vmcb->save.rip += 2; /* rdmsr */

	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_VMMCALL);
	GUEST_SYNC(6);

	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	vm_vaddr_t nested_gva = 0;

	struct kvm_vm *vm;
	struct kvm_run *run;
	struct ucall uc;
	int stage;

	if (!nested_svm_supported()) {
		print_skip("Nested SVM not supported");
		exit(KSFT_SKIP);
	}
	/* Create VM */
	vm = vm_create_default(VCPU_ID, 0, guest_code);
	vcpu_set_hv_cpuid(vm, VCPU_ID);
	run = vcpu_state(vm, VCPU_ID);
	vcpu_alloc_svm(vm, &nested_gva);
	vcpu_args_set(vm, VCPU_ID, 1, nested_gva);

	for (stage = 1;; stage++) {
		_vcpu_run(vm, VCPU_ID);
		TEST_ASSERT(run->exit_reason == KVM_EXIT_IO,
			    "Stage %d: unexpected exit reason: %u (%s),\n",
			    stage, run->exit_reason,
			    exit_reason_str(run->exit_reason));

		switch (get_ucall(vm, VCPU_ID, &uc)) {
		case UCALL_ABORT:
			TEST_FAIL("%s at %s:%ld", (const char *)uc.args[0],
				  __FILE__, uc.args[1]);
			/* NOT REACHED */
		case UCALL_SYNC:
			break;
		case UCALL_DONE:
			goto done;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}

		/* UCALL_SYNC is handled here.  */
		TEST_ASSERT(!strcmp((const char *)uc.args[0], "hello") &&
			    uc.args[1] == stage, "Stage %d: Unexpected register values vmexit, got %lx",
			    stage, (ulong)uc.args[1]);

	}

done:
	kvm_vm_free(vm);
}
