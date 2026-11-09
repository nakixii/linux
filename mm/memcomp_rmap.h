/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_RMAP_H
#define _MM_MEMCOMP_RMAP_H

#ifdef CONFIG_MEMCOMP
/*
 * Unlike the newer generic lazyfree batching path, this backport only batches
 * a reserved memcomp span. Mixed protection or exclusive bits stay on the
 * existing single-PTE path. The caller owns folio lock, PTL and notifier scope.
 */
static bool memcomp_unmap_exclusive_uniform(struct folio *folio)
{
	bool exclusive = PageAnonExclusive(folio_page(folio, 0));
	unsigned int i;

	for (i = 1; i < folio_nr_pages(folio); i++) {
		if (PageAnonExclusive(folio_page(folio, i)) != exclusive)
			return false;
	}
	return true;
}

static int memcomp_share_anon_ptes(struct folio *folio, struct page *page,
				   unsigned int nr)
{
	unsigned int i;

	if (IS_ENABLED(CONFIG_HAVE_GUP_FAST))
		/* Paired with the memory barrier in try_grab_folio(). */
		smp_mb();
	if (unlikely(folio_maybe_dma_pinned(folio)))
		return -EBUSY;
	for (i = 0; i < nr; i++)
		ClearPageAnonExclusive(page + i);
	if (IS_ENABLED(CONFIG_HAVE_GUP_FAST))
		/* Paired with the smp_rmb() in gup_must_unshare(). */
		smp_mb__after_atomic();
	return 0;
}

/* 1: use the original actor; >1: complete batch; <0: restored, abort walk. */
static int memcomp_unmap_ptes(struct page_vma_mapped_walk *pvmw,
			      struct folio *folio, struct vm_area_struct *vma,
			      enum ttu_flags flags)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long address = pvmw->address, end, token;
	unsigned int i, nr, got = 0;
	struct page *page;
	swp_entry_t entry;
	pte_t first, old, swap;
	bool exclusive;

	if (!folio_memcomp_reserved(folio) || !folio_test_large(folio) ||
	    !folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
	    folio_test_hugetlb(folio) || (flags & TTU_HWPOISON) ||
	    !memcomp_unmap_exclusive_uniform(folio))
		return 1;
	first = ptep_get(pvmw->pte);
	if (!pte_present(first) || pte_unused(first))
		return 1;
	page = pfn_to_page(pte_pfn(first));
	nr = folio_nr_pages(folio) - folio_page_idx(folio, page);
	end = min(pmd_addr_end(address, vma->vm_end), vma->vm_end);
	nr = min_t(unsigned int, nr, (end - address) >> PAGE_SHIFT);
	if (nr < 2)
		return 1;
	for (i = 1; i < nr; i++) {
		if (!pte_same(pte_advance_pfn(first, i), ptep_get(pvmw->pte + i)))
			break;
	}
	nr = i;
	if (nr < 2)
		return 1;
	entry = page_swap_entry(page);
	token = swp_offset(entry);
	if (!is_memcomp_entry(entry) || !token || token > U32_MAX - (nr - 1))
		return 1;
	/* Each future PTE gets a real owner; failure unwinds the whole prefix. */
	for (; got < nr; got++) {
		if (!memcomp_entry_get(token + got))
			goto refs_fail;
	}
	exclusive = PageAnonExclusive(page);
	flush_cache_range(vma, address, address + nr * PAGE_SIZE);
	old = get_and_clear_full_ptes(mm, address, pvmw->pte, nr, 0);
	if (should_defer_flush(mm, flags)) {
		for (i = 0; i < nr; i++)
			set_tlb_ubc_flush_pending(mm, old, address + i * PAGE_SIZE);
	} else {
		flush_tlb_range(vma, address, address + nr * PAGE_SIZE);
	}
	if (pte_dirty(old))
		folio_mark_dirty(folio);
	update_hiwater_rss(mm);
	for (i = 0; i < nr; i++) {
		if (arch_unmap_one(mm, vma, address + i * PAGE_SIZE,
				   pte_advance_pfn(old, i)) < 0)
			goto restore;
	}
	if (exclusive && memcomp_share_anon_ptes(folio, page, nr))
		goto restore;
	if (list_empty(&mm->mmlist)) {
		spin_lock(&mmlist_lock);
		if (list_empty(&mm->mmlist))
			list_add(&mm->mmlist, &init_mm.mmlist);
		spin_unlock(&mmlist_lock);
	}
	add_mm_counter(mm, MM_ANONPAGES, -(int)nr);
	add_mm_counter(mm, MM_SWAPENTS, nr);
	swap = swp_entry_to_pte(entry);
	if (exclusive)
		swap = pte_swp_mkexclusive(swap);
	if (pte_soft_dirty(old))
		swap = pte_swp_mksoft_dirty(swap);
	if (pte_uffd_wp(old))
		swap = pte_swp_mkuffd_wp(swap);
	for (i = 0; i < nr; i++)
		set_pte_at(mm, address + i * PAGE_SIZE, pvmw->pte + i,
			   pte_move_swp_offset(swap, i));
	folio_remove_rmap_ptes(folio, page, nr, vma);
	if (vma->vm_flags & VM_LOCKED)
		mlock_drain_local();
	folio_put_refs(folio, nr);
	pvmw->pte += nr - 1;
	pvmw->address += (nr - 1) * PAGE_SIZE;
	return nr;
restore:
	/* Complete invalidation before set_ptes() may form a contpte range. */
	flush_tlb_range(vma, address, address + nr * PAGE_SIZE);
	/* Dirty/young aggregation is conservative; protection bits were uniform. */
	set_ptes(mm, address, pvmw->pte, old, nr);
refs_fail:
	while (got)
		memcomp_invalidate(token + --got);
	return -EBUSY;
}
#endif /* CONFIG_MEMCOMP */
#endif /* _MM_MEMCOMP_RMAP_H */
