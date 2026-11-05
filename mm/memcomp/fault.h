/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_FAULT_H
#define _MM_MEMCOMP_FAULT_H

/*
 * Include after zero_load.h, memcomp_fault_sigbus_or_retry(), and the
 * resident actor. span.h/load.h and the generic THP/PTE helpers must be visible.
 * Caller has the same mmap/rmap preparation as the existing memcomp_load().
 * Resident handled=true consumes ref on every return; false retains it.
 * Stable non-PENDING slots bypass that actor without another T acquisition.
 */
static inline vm_fault_t mc16_load_resident(struct vm_fault *vmf,
					    struct memcomp_ref *ref, bool *handled);

#ifdef CONFIG_MEMCOMP_KUNIT_TEST
/* Failure/race injection only. Successful data, allocation and charge are real. */
static struct {
	struct task_struct *task;
	bool fail_large_alloc;
	bool fail_small_alloc;
	bool fail_large_charge;
	bool fail_small_charge;
	unsigned int large_allocs;
	unsigned int small_allocs;
	unsigned int large_charges;
	unsigned int small_charges;
	void (*after_alloc)(struct vm_fault *vmf, void *data);
	void (*before_install)(struct vm_fault *vmf, void *data);
	void *data;
} mc16_fault_probe;
#endif

static inline vm_fault_t mc16_load(struct vm_fault *vmf, bool *locked)
{
	const unsigned int max_pages = MEMCOMP_MAX_COMPRESS_PAGES;
	struct vm_area_struct *vma = vmf->vma;
	swp_entry_t swp = pte_to_swp_entry(vmf->orig_pte);
	unsigned long id = swp_offset(swp);
	struct memcomp_ref refs[MEMCOMP_MAX_COMPRESS_PAGES] = { };
	unsigned int nr_pages = 1, fault_index = 0, i;
	unsigned long start_addr = vmf->address, id_start = id;
	struct folio *folio = NULL;
	struct mc16_user_read scope;
	pte_t *install_pte;
	vm_fault_t ret = 0;
	gfp_t alloc_gfp;
	void *dst;
	pte_t pte;
	int err;

	if (locked)
		*locked = false;
	if (!is_memcomp_entry(swp))
		return VM_FAULT_SIGBUS;
	if (!id)
		return mc16_load_zero(vmf, locked);
	if (!mc16_get(id, &refs[0]))
		return memcomp_fault_sigbus_or_retry(vmf);

	/* Stable tags never become PENDING; the resident actor validates under T. */
	if (is_cow_mapping(vma->vm_flags) && vma->anon_vma &&
	    mc16_flags_state(READ_ONCE(refs[0].slot->flags)) == MC16_PENDING) {
		bool handled;

		ret = mc16_load_resident(vmf, &refs[0], &handled);
		if (handled)
			return ret;
	}

	if (!userfaultfd_armed(vma) && vma_is_anonymous(vma) && vma->anon_vma) {
		unsigned int encoded_pages, encoded_idx, order;
		unsigned long base_addr, max_end, orders;
		pte_t *start_pte;
		u32 base_id;

		encoded_pages = mc16_ref_geometry(&refs[0], &base_id, &encoded_idx);
		if (encoded_pages <= 1 || encoded_pages > max_pages ||
		    encoded_idx >= encoded_pages || id < encoded_idx || !base_id ||
		    base_id > U32_MAX - (encoded_pages - 1) ||
		    !is_power_of_2(encoded_pages))
			goto fallback_single;
		if (vmf->address < ((unsigned long)encoded_idx << PAGE_SHIFT))
			goto fallback_single;
		base_addr = vmf->address - ((unsigned long)encoded_idx << PAGE_SHIFT);
		max_end = pmd_addr_end(vmf->address, vma->vm_end);
		if (base_addr < max(vmf->address & PMD_MASK, vma->vm_start) ||
		    base_addr < vma->vm_start ||
		    base_addr + ((unsigned long)encoded_pages << PAGE_SHIFT) > max_end)
			goto fallback_single;
		orders = thp_vma_allowable_orders(vma, vma->vm_flags,
						  TVA_IN_PF | TVA_ENFORCE_SYSFS,
						  BIT(PMD_ORDER) - 1);
		orders = thp_vma_suitable_orders(vma, vmf->address, orders);
		order = order_base_2(encoded_pages);
		if (!(orders & BIT(order)))
			goto fallback_single;
		if (!memcomp_pte_map_lock(vma->vm_mm, vmf->pmd, vmf->address,
					  &vmf->pte, &vmf->ptl))
			goto out_put;
		if (!pte_same(ptep_get(vmf->pte), vmf->orig_pte))
			goto out_unlock;
		start_pte = vmf->pte - encoded_idx;
		if (!pte_same(ptep_get(start_pte),
			      pte_move_swp_offset(vmf->orig_pte, -(int)encoded_idx)) ||
		    memcomp_pte_batch(start_pte, encoded_pages) != encoded_pages) {
			pte_unmap_unlock(vmf->pte, vmf->ptl);
			vmf->pte = NULL;
			goto fallback_single;
		}
		pte_unmap_unlock(vmf->pte, vmf->ptl);
		vmf->pte = NULL;
		nr_pages = encoded_pages;
		fault_index = encoded_idx;
		start_addr = base_addr;
		id_start = base_id;
		goto fallback_alloc;
	}

fallback_single:
	nr_pages = 1;
	fault_index = 0;
	start_addr = vmf->address;
	id_start = id;
fallback_alloc:
	alloc_gfp = memcomp_fault_alloc_gfp(vma, nr_pages);
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	if (READ_ONCE(mc16_fault_probe.task) == current) {
		if (nr_pages > 1)
			mc16_fault_probe.large_allocs++;
		else
			mc16_fault_probe.small_allocs++;
		if (nr_pages > 1 ? mc16_fault_probe.fail_large_alloc :
				   mc16_fault_probe.fail_small_alloc)
			goto alloc_failed;
	}
#endif
	folio = vma_alloc_folio(alloc_gfp, order_base_2(nr_pages),
				vma, start_addr, false);
	if (!folio) {
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
alloc_failed:
#endif
		if (nr_pages > 1)
			goto fallback_single;
		ret = VM_FAULT_OOM;
		goto out_put;
	}
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	if (READ_ONCE(mc16_fault_probe.task) == current) {
		if (nr_pages > 1)
			mc16_fault_probe.large_charges++;
		else
			mc16_fault_probe.small_charges++;
	}
	if (READ_ONCE(mc16_fault_probe.task) == current &&
	    (nr_pages > 1 ? mc16_fault_probe.fail_large_charge :
			   mc16_fault_probe.fail_small_charge))
		err = -ENOMEM;
	else
#endif
		err = mc16_user_charge(&refs[0], folio, vma->vm_mm, alloc_gfp);
	if (err) {
		if (nr_pages > 1) {
			folio_put(folio);
			folio = NULL;
			goto fallback_single;
		}
		ret = VM_FAULT_OOM;
		goto out_put;
	}
	__folio_set_locked(folio);
	if (locked)
		*locked = true;
	__folio_set_swapbacked(folio);
	folio->swap.val = 0;
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	if (READ_ONCE(mc16_fault_probe.task) == current && mc16_fault_probe.after_alloc)
		mc16_fault_probe.after_alloc(vmf, mc16_fault_probe.data);
#endif
	if (nr_pages > 1) {
		struct memcomp_ref fault_ref = refs[0];
		bool batch_ok = true;

		refs[0] = (struct memcomp_ref) { };
		refs[fault_index] = fault_ref;
		for (i = 0; i < nr_pages; i++) {
			if (i != fault_index && !mc16_get(id_start + i, &refs[i])) {
				batch_ok = false;
				break;
			}
		}
		for (i = 0; batch_ok && i < nr_pages; i++) {
			unsigned int index;
			u32 base;

			if (mc16_ref_geometry(&refs[i], &base, &index) != nr_pages ||
			    base != id_start || index != i)
				batch_ok = false;
		}
		if (!batch_ok) {
			for (i = 0; i < nr_pages; i++) {
				if (i != fault_index && refs[i].slot)
					mc16_ref_put(&refs[i]);
			}
			refs[fault_index] = (struct memcomp_ref) { };
			refs[0] = fault_ref;
			folio_unlock(folio);
			folio_put(folio);
			folio = NULL;
			if (locked)
				*locked = false;
			goto fallback_single;
		}
	}
	err = mc16_user_refault(refs, nr_pages, folio);
	if (err) {
		ret = err == -EAGAIN ? 0 : VM_FAULT_SIGBUS;
		goto out_put;
	}
	mc16_user_read_start(&scope, folio);
	/* A local highmem mapping covers one page, even for a large folio. */
	for (i = 0, err = 0; i < nr_pages; i++) {
		dst = kmap_local_folio(folio, i * PAGE_SIZE);
		err = mc16_load_page(&refs[i], dst);
		kunmap_local(dst);
		if (err)
			break;
	}
	mc16_user_read_end(&scope);
	if (err) {
		if (err == -EAGAIN)
			ret = 0;
		else if (err == -ENOMEM)
			ret = VM_FAULT_OOM;
		else
			ret = memcomp_fault_sigbus_or_retry(vmf);
		goto out_put;
	}
	/* A new user folio was materialized; a later PTE race does not undo it. */
	mc16_user_major(vma->vm_mm);
	ret = VM_FAULT_MAJOR;
	memcomp_arch_restore(id_start, folio);
	__folio_mark_uptodate(folio);
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	if (READ_ONCE(mc16_fault_probe.task) == current && mc16_fault_probe.before_install)
		mc16_fault_probe.before_install(vmf, mc16_fault_probe.data);
#endif
	/* No T survives mc16_load_page; PTL is acquired only after it returns. */
	if (!memcomp_pte_map_lock(vma->vm_mm, vmf->pmd, vmf->address,
				  &vmf->pte, &vmf->ptl))
		goto out_put;
	if (!pte_same(ptep_get(vmf->pte), vmf->orig_pte))
		goto out_unlock;
	install_pte = vmf->pte;
	if (nr_pages > 1) {
		install_pte -= fault_index;
		if (!pte_same(ptep_get(install_pte),
			      pte_move_swp_offset(vmf->orig_pte, -(int)fault_index)) ||
		    memcomp_pte_batch(install_pte, nr_pages) != nr_pages)
			goto out_unlock;
		for (i = 0; i < nr_pages; i++) {
			swp_entry_t cur = pte_to_swp_entry(ptep_get(install_pte + i));

			if (!is_memcomp_entry(cur) || swp_offset(cur) != id_start + i)
				goto out_unlock;
		}
	}
	add_mm_counter(vma->vm_mm, MM_ANONPAGES, nr_pages);
	add_mm_counter(vma->vm_mm, MM_SWAPENTS, -(int)nr_pages);
	pte = mk_pte(folio_page(folio, 0), vma->vm_page_prot);
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
	flush_icache_pages(vma, folio_page(folio, 0), nr_pages);
	folio_add_new_anon_rmap(folio, vma, start_addr, RMAP_EXCLUSIVE);
	folio_add_lru_vma(folio, vma);
	folio_ref_add(folio, nr_pages - 1);
	if (nr_pages > 1) {
		deferred_split_folio(folio, false);
		set_ptes(vma->vm_mm, start_addr, install_pte, pte, nr_pages);
	} else {
		set_pte_at(vma->vm_mm, vmf->address, vmf->pte, pte);
	}
	arch_do_swap_page_nr(vma->vm_mm, vma, start_addr, pte, pte, nr_pages);
	update_mmu_cache_range(vmf, vma, start_addr, install_pte, nr_pages);
	folio_unlock(folio);
	if (locked)
		*locked = false;
	pte_unmap_unlock(vmf->pte, vmf->ptl);
	vmf->pte = NULL;
	/* Each committed PTE owns a folio ref and relinquishes its token owner. */
	for (i = 0; i < nr_pages; i++)
		mc16_ref_put_count(&refs[i], 2);
	return VM_FAULT_NOPAGE | ret;

out_unlock:
	pte_unmap_unlock(vmf->pte, vmf->ptl);
	vmf->pte = NULL;
out_put:
	for (i = 0; i < max_pages; i++) {
		if (refs[i].slot)
			mc16_ref_put(&refs[i]);
	}
	if (folio) {
		if (folio_test_locked(folio))
			folio_unlock(folio);
		folio_put(folio);
	}
	if (locked)
		*locked = false;
	return ret;
}
#endif /* _MM_MEMCOMP_FAULT_H */
