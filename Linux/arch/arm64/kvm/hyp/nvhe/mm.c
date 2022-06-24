// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Google LLC
 * Author: Quentin Perret <qperret@google.com>
 */

#include <linux/kvm_host.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/kvm_pkvm.h>
#include <asm/spectre.h>

#include <nvhe/early_alloc.h>
#include <nvhe/gfp.h>
#include <nvhe/memory.h>
#include <nvhe/mm.h>
#include <nvhe/spinlock.h>

struct kvm_pgtable pkvm_pgtable;
hyp_spinlock_t pkvm_pgd_lock;

struct memblock_region hyp_memory[HYP_MEMBLOCK_REGIONS];
unsigned int hyp_memblock_nr;

static u64 __io_map_base;

static int __pkvm_create_mappings(unsigned long start, unsigned long size,
				  unsigned long phys, enum kvm_pgtable_prot prot)
{
	int err;

	hyp_spin_lock(&pkvm_pgd_lock);
	err = kvm_pgtable_hyp_map(&pkvm_pgtable, start, size, phys, prot);
	hyp_spin_unlock(&pkvm_pgd_lock);

	return err;
}

/**
 * pkvm_alloc_private_va_range - Allocates a private VA range.
 * @size:	The size of the VA range to reserve.
 * @haddr:	The hypervisor virtual start address of the allocation.
 *
 * The private virtual address (VA) range is allocated above __io_map_base
 * and aligned based on the order of @size.
 *
 * Return: 0 on success or negative error code on failure.
 */
int pkvm_alloc_private_va_range(size_t size, unsigned long *haddr)
{
	unsigned long base, addr;
	int ret = 0;

	hyp_spin_lock(&pkvm_pgd_lock);

	/* Align the allocation based on the order of its size */
	addr = ALIGN(__io_map_base, PAGE_SIZE << get_order(size));

	/* The allocated size is always a multiple of PAGE_SIZE */
	base = addr + PAGE_ALIGN(size);

	/* Are we overflowing on the vmemmap ? */
	if (!addr || base > __hyp_vmemmap)
		ret = -ENOMEM;
	else {
		__io_map_base = base;
		*haddr = addr;
	}

	hyp_spin_unlock(&pkvm_pgd_lock);

	return ret;
}

int __pkvm_create_private_mapping(phys_addr_t phys, size_t size,
				  enum kvm_pgtable_prot prot,
				  unsigned long *haddr)
{
	unsigned long addr;
	int err;

	size = PAGE_ALIGN(size + offset_in_page(phys));
	err = pkvm_alloc_private_va_range(size, &addr);
	if (err)
		return err;

	err = __pkvm_create_mappings(addr, size, phys, prot);
	if (err)
		return err;

	*haddr = addr + offset_in_page(phys);
	return err;
}

int pkvm_create_mappings_locked(void *from, void *to, enum kvm_pgtable_prot prot)
{
	unsigned long start = (unsigned long)from;
	unsigned long end = (unsigned long)to;
	unsigned long virt_addr;
	phys_addr_t phys;

	hyp_assert_lock_held(&pkvm_pgd_lock);

	start = start & PAGE_MASK;
	end = PAGE_ALIGN(end);

	for (virt_addr = start; virt_addr < end; virt_addr += PAGE_SIZE) {
		int err;

		phys = hyp_virt_to_phys((void *)virt_addr);
		err = kvm_pgtable_hyp_map(&pkvm_pgtable, virt_addr, PAGE_SIZE,
					  phys, prot);
		if (err)
			return err;
	}

	return 0;
}

int pkvm_create_mappings(void *from, void *to, enum kvm_pgtable_prot prot)
{
	int ret;

	hyp_spin_lock(&pkvm_pgd_lock);
	ret = pkvm_create_mappings_locked(from, to, prot);
	hyp_spin_unlock(&pkvm_pgd_lock);

	return ret;
}

int hyp_back_vmemmap(phys_addr_t phys, unsigned long size, phys_addr_t back)
{
	unsigned long start, end;

	hyp_vmemmap_range(phys, size, &start, &end);

	return __pkvm_create_mappings(start, end - start, back, PAGE_HYP);
}

static void *__hyp_bp_vect_base;
int pkvm_cpu_set_vector(enum arm64_hyp_spectre_vector slot)
{
	void *vector;

	switch (slot) {
	case HYP_VECTOR_DIRECT: {
		vector = __kvm_hyp_vector;
		break;
	}
	case HYP_VECTOR_SPECTRE_DIRECT: {
		vector = __bp_harden_hyp_vecs;
		break;
	}
	case HYP_VECTOR_INDIRECT:
	case HYP_VECTOR_SPECTRE_INDIRECT: {
		vector = (void *)__hyp_bp_vect_base;
		break;
	}
	default:
		return -EINVAL;
	}

	vector = __kvm_vector_slot2addr(vector, slot);
	*this_cpu_ptr(&kvm_hyp_vector) = (unsigned long)vector;

	return 0;
}

int hyp_map_vectors(void)
{
	phys_addr_t phys;
	unsigned long bp_base;
	int ret;

	if (!kvm_system_needs_idmapped_vectors()) {
		__hyp_bp_vect_base = __bp_harden_hyp_vecs;
		return 0;
	}

	phys = __hyp_pa(__bp_harden_hyp_vecs);
	ret = __pkvm_create_private_mapping(phys, __BP_HARDEN_HYP_VECS_SZ,
					    PAGE_HYP_EXEC, &bp_base);
	if (ret)
		return ret;

	__hyp_bp_vect_base = (void *)bp_base;

	return 0;
}

int hyp_create_idmap(u32 hyp_va_bits)
{
	unsigned long start, end;

	start = hyp_virt_to_phys((void *)__hyp_idmap_text_start);
	start = ALIGN_DOWN(start, PAGE_SIZE);

	end = hyp_virt_to_phys((void *)__hyp_idmap_text_end);
	end = ALIGN(end, PAGE_SIZE);

	/*
	 * One half of the VA space is reserved to linearly map portions of
	 * memory -- see va_layout.c for more details. The other half of the VA
	 * space contains the trampoline page, and needs some care. Split that
	 * second half in two and find the quarter of VA space not conflicting
	 * with the idmap to place the IOs and the vmemmap. IOs use the lower
	 * half of the quarter and the vmemmap the upper half.
	 */
	__io_map_base = start & BIT(hyp_va_bits - 2);
	__io_map_base ^= BIT(hyp_va_bits - 2);
	__hyp_vmemmap = __io_map_base | BIT(hyp_va_bits - 3);

	return __pkvm_create_mappings(start, end - start, start, PAGE_HYP_EXEC);
}
