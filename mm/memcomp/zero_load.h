/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_ZERO_LOAD_H
#define _MM_MEMCOMP_ZERO_LOAD_H

#include <linux/highmem.h>

/* Include after zero.h and memcomp_pte_map_lock(), in memcomp.c. */

static inline vm_fault_t mc16_load_zero(struct vm_fault *vmf, bool *locked)
{
	struct vm_area_struct *vma = vmf->vma;
	struct folio *folio;
	vm_fault_t ret;
	pte_t pte;

	if (locked)
		*locked = false;
	ret = vmf_anon_prepare(vmf);
	if (ret)
		return ret;
	folio = vma_alloc_zeroed_movable_folio(vma, vmf->address);
	if (!folio)
		return VM_FAULT_OOM;
	if (mem_cgroup_charge(folio, vma->vm_mm, GFP_HIGHUSER_MOVABLE)) {
		folio_put(folio);
		return VM_FAULT_OOM;
	}
	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);
	folio->swap.val = 0;
	__folio_mark_uptodate(folio);
	if (!memcomp_pte_map_lock(vma->vm_mm, vmf->pmd, vmf->address,
				  &vmf->pte, &vmf->ptl))
		goto out_free;
	if (!pte_same(ptep_get(vmf->pte), vmf->orig_pte)) {
		pte_unmap_unlock(vmf->pte, vmf->ptl);
		vmf->pte = NULL;
		goto out_free;
	}

	pte = mk_pte(&folio->page, vma->vm_page_prot);
	if (pte_swp_soft_dirty(vmf->orig_pte))
		pte = pte_mksoft_dirty(pte);
	if (pte_swp_uffd_wp(vmf->orig_pte))
		pte = pte_mkuffd_wp(pte);
	if ((vma->vm_flags & VM_WRITE) && !userfaultfd_pte_wp(vma, pte) &&
	    !pte_needs_soft_dirty_wp(vma, pte)) {
		pte = pte_mkwrite(pte, vma);
		if (vmf->flags & FAULT_FLAG_WRITE)
			pte = pte_mkdirty(pte);
	}
	flush_icache_pages(vma, &folio->page, 1);
	folio_add_new_anon_rmap(folio, vma, vmf->address, RMAP_EXCLUSIVE);
	folio_add_lru_vma(folio, vma);
	set_pte_at(vma->vm_mm, vmf->address, vmf->pte, pte);
	add_mm_counter(vma->vm_mm, MM_SWAPENTS, -1);
	add_mm_counter(vma->vm_mm, MM_ANONPAGES, 1);
	mc16_zero_pte_put(1);
	arch_do_swap_page_nr(vma->vm_mm, vma, vmf->address, pte, pte, 1);
	update_mmu_cache_range(vmf, vma, vmf->address, vmf->pte, 1);
	folio_unlock(folio);
	pte_unmap_unlock(vmf->pte, vmf->ptl);
	vmf->pte = NULL;
	return VM_FAULT_NOPAGE;

out_free:
	folio_unlock(folio);
	folio_put(folio);
	return 0;
}
#endif /* _MM_MEMCOMP_ZERO_LOAD_H */
