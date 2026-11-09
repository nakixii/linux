/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_FAULT_ALLOC_H
#define _MM_MEMCOMP_FAULT_ALLOC_H

#include <linux/gfp.h>

/* Use the returned mask for both folio allocation and memcg charging. */
static inline gfp_t memcomp_fault_alloc_gfp(struct vm_area_struct *vma,
					    unsigned int nr_pages)
{
	if (nr_pages > 1) {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		return vma_thp_gfp_mask(vma);
#else
		/* Keep the original large-folio mask without THP support. */
		return GFP_HIGHUSER_MOVABLE | __GFP_NORETRY | __GFP_NOWARN;
#endif
	}

	return GFP_HIGHUSER_MOVABLE;
}

#endif /* _MM_MEMCOMP_FAULT_ALLOC_H */
