// SPDX-License-Identifier: GPL-2.0
/*
 * Hyper-V Isolation VM interface with paravisor and hypervisor
 *
 * Author:
 *  Tianyu Lan <Tianyu.Lan@microsoft.com>
 */

#include <linux/bitfield.h>
#include <linux/hyperv.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <asm/svm.h>
#include <asm/sev.h>
#include <asm/io.h>
#include <asm/mshyperv.h>
#include <asm/hypervisor.h>

#ifdef CONFIG_AMD_MEM_ENCRYPT

#define GHCB_USAGE_HYPERV_CALL	1

union hv_ghcb {
	struct ghcb ghcb;
	struct {
		u64 hypercalldata[509];
		u64 outputgpa;
		union {
			union {
				struct {
					u32 callcode        : 16;
					u32 isfast          : 1;
					u32 reserved1       : 14;
					u32 isnested        : 1;
					u32 countofelements : 12;
					u32 reserved2       : 4;
					u32 repstartindex   : 12;
					u32 reserved3       : 4;
				};
				u64 asuint64;
			} hypercallinput;
			union {
				struct {
					u16 callstatus;
					u16 reserved1;
					u32 elementsprocessed : 12;
					u32 reserved2         : 20;
				};
				u64 asunit64;
			} hypercalloutput;
		};
		u64 reserved2;
	} hypercall;
} __packed __aligned(HV_HYP_PAGE_SIZE);

static u16 hv_ghcb_version __ro_after_init;

u64 hv_ghcb_hypercall(u64 control, void *input, void *output, u32 input_size)
{
	union hv_ghcb *hv_ghcb;
	void **ghcb_base;
	unsigned long flags;
	u64 status;

	if (!hv_ghcb_pg)
		return -EFAULT;

	WARN_ON(in_nmi());

	local_irq_save(flags);
	ghcb_base = (void **)this_cpu_ptr(hv_ghcb_pg);
	hv_ghcb = (union hv_ghcb *)*ghcb_base;
	if (!hv_ghcb) {
		local_irq_restore(flags);
		return -EFAULT;
	}

	hv_ghcb->ghcb.protocol_version = GHCB_PROTOCOL_MAX;
	hv_ghcb->ghcb.ghcb_usage = GHCB_USAGE_HYPERV_CALL;

	hv_ghcb->hypercall.outputgpa = (u64)output;
	hv_ghcb->hypercall.hypercallinput.asuint64 = 0;
	hv_ghcb->hypercall.hypercallinput.callcode = control;

	if (input_size)
		memcpy(hv_ghcb->hypercall.hypercalldata, input, input_size);

	VMGEXIT();

	hv_ghcb->ghcb.ghcb_usage = 0xffffffff;
	memset(hv_ghcb->ghcb.save.valid_bitmap, 0,
	       sizeof(hv_ghcb->ghcb.save.valid_bitmap));

	status = hv_ghcb->hypercall.hypercalloutput.callstatus;

	local_irq_restore(flags);

	return status;
}

static inline u64 rd_ghcb_msr(void)
{
	return __rdmsr(MSR_AMD64_SEV_ES_GHCB);
}

static inline void wr_ghcb_msr(u64 val)
{
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, val);
}

static enum es_result hv_ghcb_hv_call(struct ghcb *ghcb, u64 exit_code,
				   u64 exit_info_1, u64 exit_info_2)
{
	/* Fill in protocol and format specifiers */
	ghcb->protocol_version = hv_ghcb_version;
	ghcb->ghcb_usage       = GHCB_DEFAULT_USAGE;

	ghcb_set_sw_exit_code(ghcb, exit_code);
	ghcb_set_sw_exit_info_1(ghcb, exit_info_1);
	ghcb_set_sw_exit_info_2(ghcb, exit_info_2);

	VMGEXIT();

	if (ghcb->save.sw_exit_info_1 & GENMASK_ULL(31, 0))
		return ES_VMM_ERROR;
	else
		return ES_OK;
}

void hv_ghcb_terminate(unsigned int set, unsigned int reason)
{
	u64 val = GHCB_MSR_TERM_REQ;

	/* Tell the hypervisor what went wrong. */
	val |= GHCB_SEV_TERM_REASON(set, reason);

	/* Request Guest Termination from Hypvervisor */
	wr_ghcb_msr(val);
	VMGEXIT();

	while (true)
		asm volatile("hlt\n" : : : "memory");
}

bool hv_ghcb_negotiate_protocol(void)
{
	u64 ghcb_gpa;
	u64 val;

	/* Save ghcb page gpa. */
	ghcb_gpa = rd_ghcb_msr();

	/* Do the GHCB protocol version negotiation */
	wr_ghcb_msr(GHCB_MSR_SEV_INFO_REQ);
	VMGEXIT();
	val = rd_ghcb_msr();

	if (GHCB_MSR_INFO(val) != GHCB_MSR_SEV_INFO_RESP)
		return false;

	if (GHCB_MSR_PROTO_MAX(val) < GHCB_PROTOCOL_MIN ||
	    GHCB_MSR_PROTO_MIN(val) > GHCB_PROTOCOL_MAX)
		return false;

	hv_ghcb_version = min_t(size_t, GHCB_MSR_PROTO_MAX(val),
			     GHCB_PROTOCOL_MAX);

	/* Write ghcb page back after negotiating protocol. */
	wr_ghcb_msr(ghcb_gpa);
	VMGEXIT();

	return true;
}

void hv_ghcb_msr_write(u64 msr, u64 value)
{
	union hv_ghcb *hv_ghcb;
	void **ghcb_base;
	unsigned long flags;

	if (!hv_ghcb_pg)
		return;

	WARN_ON(in_nmi());

	local_irq_save(flags);
	ghcb_base = (void **)this_cpu_ptr(hv_ghcb_pg);
	hv_ghcb = (union hv_ghcb *)*ghcb_base;
	if (!hv_ghcb) {
		local_irq_restore(flags);
		return;
	}

	ghcb_set_rcx(&hv_ghcb->ghcb, msr);
	ghcb_set_rax(&hv_ghcb->ghcb, lower_32_bits(value));
	ghcb_set_rdx(&hv_ghcb->ghcb, upper_32_bits(value));

	if (hv_ghcb_hv_call(&hv_ghcb->ghcb, SVM_EXIT_MSR, 1, 0))
		pr_warn("Fail to write msr via ghcb %llx.\n", msr);

	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(hv_ghcb_msr_write);

void hv_ghcb_msr_read(u64 msr, u64 *value)
{
	union hv_ghcb *hv_ghcb;
	void **ghcb_base;
	unsigned long flags;

	/* Check size of union hv_ghcb here. */
	BUILD_BUG_ON(sizeof(union hv_ghcb) != HV_HYP_PAGE_SIZE);

	if (!hv_ghcb_pg)
		return;

	WARN_ON(in_nmi());

	local_irq_save(flags);
	ghcb_base = (void **)this_cpu_ptr(hv_ghcb_pg);
	hv_ghcb = (union hv_ghcb *)*ghcb_base;
	if (!hv_ghcb) {
		local_irq_restore(flags);
		return;
	}

	ghcb_set_rcx(&hv_ghcb->ghcb, msr);
	if (hv_ghcb_hv_call(&hv_ghcb->ghcb, SVM_EXIT_MSR, 0, 0))
		pr_warn("Fail to read msr via ghcb %llx.\n", msr);
	else
		*value = (u64)lower_32_bits(hv_ghcb->ghcb.save.rax)
			| ((u64)lower_32_bits(hv_ghcb->ghcb.save.rdx) << 32);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(hv_ghcb_msr_read);
#endif

enum hv_isolation_type hv_get_isolation_type(void)
{
	if (!(ms_hyperv.priv_high & HV_ISOLATION))
		return HV_ISOLATION_TYPE_NONE;
	return FIELD_GET(HV_ISOLATION_TYPE, ms_hyperv.isolation_config_b);
}
EXPORT_SYMBOL_GPL(hv_get_isolation_type);

/*
 * hv_is_isolation_supported - Check system runs in the Hyper-V
 * isolation VM.
 */
bool hv_is_isolation_supported(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_HYPERVISOR))
		return false;

	if (!hypervisor_is_type(X86_HYPER_MS_HYPERV))
		return false;

	return hv_get_isolation_type() != HV_ISOLATION_TYPE_NONE;
}

DEFINE_STATIC_KEY_FALSE(isolation_type_snp);

/*
 * hv_isolation_type_snp - Check system runs in the AMD SEV-SNP based
 * isolation VM.
 */
bool hv_isolation_type_snp(void)
{
	return static_branch_unlikely(&isolation_type_snp);
}

/*
 * hv_mark_gpa_visibility - Set pages visible to host via hvcall.
 *
 * In Isolation VM, all guest memory is encrypted from host and guest
 * needs to set memory visible to host via hvcall before sharing memory
 * with host.
 */
static int hv_mark_gpa_visibility(u16 count, const u64 pfn[],
			   enum hv_mem_host_visibility visibility)
{
	struct hv_gpa_range_for_visibility **input_pcpu, *input;
	u16 pages_processed;
	u64 hv_status;
	unsigned long flags;

	/* no-op if partition isolation is not enabled */
	if (!hv_is_isolation_supported())
		return 0;

	if (count > HV_MAX_MODIFY_GPA_REP_COUNT) {
		pr_err("Hyper-V: GPA count:%d exceeds supported:%lu\n", count,
			HV_MAX_MODIFY_GPA_REP_COUNT);
		return -EINVAL;
	}

	local_irq_save(flags);
	input_pcpu = (struct hv_gpa_range_for_visibility **)
			this_cpu_ptr(hyperv_pcpu_input_arg);
	input = *input_pcpu;
	if (unlikely(!input)) {
		local_irq_restore(flags);
		return -EINVAL;
	}

	input->partition_id = HV_PARTITION_ID_SELF;
	input->host_visibility = visibility;
	input->reserved0 = 0;
	input->reserved1 = 0;
	memcpy((void *)input->gpa_page_list, pfn, count * sizeof(*pfn));
	hv_status = hv_do_rep_hypercall(
			HVCALL_MODIFY_SPARSE_GPA_PAGE_HOST_VISIBILITY, count,
			0, input, &pages_processed);
	local_irq_restore(flags);

	if (hv_result_success(hv_status))
		return 0;
	else
		return -EFAULT;
}

/*
 * hv_set_mem_host_visibility - Set specified memory visible to host.
 *
 * In Isolation VM, all guest memory is encrypted from host and guest
 * needs to set memory visible to host via hvcall before sharing memory
 * with host. This function works as wrap of hv_mark_gpa_visibility()
 * with memory base and size.
 */
int hv_set_mem_host_visibility(unsigned long kbuffer, int pagecount, bool visible)
{
	enum hv_mem_host_visibility visibility = visible ?
			VMBUS_PAGE_VISIBLE_READ_WRITE : VMBUS_PAGE_NOT_VISIBLE;
	u64 *pfn_array;
	int ret = 0;
	int i, pfn;

	if (!hv_is_isolation_supported() || !hv_hypercall_pg)
		return 0;

	pfn_array = kmalloc(HV_HYP_PAGE_SIZE, GFP_KERNEL);
	if (!pfn_array)
		return -ENOMEM;

	for (i = 0, pfn = 0; i < pagecount; i++) {
		pfn_array[pfn] = virt_to_hvpfn((void *)kbuffer + i * HV_HYP_PAGE_SIZE);
		pfn++;

		if (pfn == HV_MAX_MODIFY_GPA_REP_COUNT || i == pagecount - 1) {
			ret = hv_mark_gpa_visibility(pfn, pfn_array,
						     visibility);
			if (ret)
				goto err_free_pfn_array;
			pfn = 0;
		}
	}

 err_free_pfn_array:
	kfree(pfn_array);
	return ret;
}

/*
 * hv_map_memory - map memory to extra space in the AMD SEV-SNP Isolation VM.
 */
void *hv_map_memory(void *addr, unsigned long size)
{
	unsigned long *pfns = kcalloc(size / PAGE_SIZE,
				      sizeof(unsigned long), GFP_KERNEL);
	void *vaddr;
	int i;

	if (!pfns)
		return NULL;

	for (i = 0; i < size / PAGE_SIZE; i++)
		pfns[i] = vmalloc_to_pfn(addr + i * PAGE_SIZE) +
			(ms_hyperv.shared_gpa_boundary >> PAGE_SHIFT);

	vaddr = vmap_pfn(pfns, size / PAGE_SIZE, PAGE_KERNEL_IO);
	kfree(pfns);

	return vaddr;
}

void hv_unmap_memory(void *addr)
{
	vunmap(addr);
}
