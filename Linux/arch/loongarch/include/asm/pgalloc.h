/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#ifndef _ASM_PGALLOC_H
#define _ASM_PGALLOC_H

#include <linux/mm.h>
#include <linux/sched.h>

#define __HAVE_ARCH_PMD_ALLOC_ONE
#define __HAVE_ARCH_PUD_ALLOC_ONE
#include <asm-generic/pgalloc.h>

static inline void pmd_populate_kernel(struct mm_struct *mm,
				       pmd_t *pmd, pte_t *pte)
{
	set_pmd(pmd, __pmd((unsigned long)pte));
}

static inline void pmd_populate(struct mm_struct *mm, pmd_t *pmd, pgtable_t pte)
{
	set_pmd(pmd, __pmd((unsigned long)page_address(pte)));
}

#ifndef __PAGETABLE_PMD_FOLDED

static inline void pud_populate(struct mm_struct *mm, pud_t *pud, pmd_t *pmd)
{
	set_pud(pud, __pud((unsigned long)pmd));
}
#endif

#ifndef __PAGETABLE_PUD_FOLDED

static inline void p4d_populate(struct mm_struct *mm, p4d_t *p4d, pud_t *pud)
{
	set_p4d(p4d, __p4d((unsigned long)pud));
}

#endif /* __PAGETABLE_PUD_FOLDED */

extern void pagetable_init(void);

/*
 * Initialize a new pmd table with invalid pointers.
 */
extern void pmd_init(unsigned long page, unsigned long pagetable);

/*
 * Initialize a new pgd / pmd table with invalid pointers.
 */
extern void pgd_init(unsigned long page);
extern pgd_t *pgd_alloc(struct mm_struct *mm);

#define __pte_free_tlb(tlb, pte, address)			\
do {							\
	pgtable_pte_page_dtor(pte);			\
	tlb_remove_page((tlb), pte);			\
} while (0)

#ifndef __PAGETABLE_PMD_FOLDED

static inline pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long address)
{
	pmd_t *pmd;
	struct page *pg;

	pg = alloc_pages(GFP_KERNEL_ACCOUNT, PMD_ORDER);
	if (!pg)
		return NULL;

	if (!pgtable_pmd_page_ctor(pg)) {
		__free_pages(pg, PMD_ORDER);
		return NULL;
	}

	pmd = (pmd_t *)page_address(pg);
	pmd_init((unsigned long)pmd, (unsigned long)invalid_pte_table);
	return pmd;
}

#define __pmd_free_tlb(tlb, x, addr)	pmd_free((tlb)->mm, x)

#endif

#ifndef __PAGETABLE_PUD_FOLDED

static inline pud_t *pud_alloc_one(struct mm_struct *mm, unsigned long address)
{
	pud_t *pud;

	pud = (pud_t *) __get_free_pages(GFP_KERNEL, PUD_ORDER);
	if (pud)
		pud_init((unsigned long)pud, (unsigned long)invalid_pmd_table);
	return pud;
}

#define __pud_free_tlb(tlb, x, addr)	pud_free((tlb)->mm, x)

#endif /* __PAGETABLE_PUD_FOLDED */

#endif /* _ASM_PGALLOC_H */
