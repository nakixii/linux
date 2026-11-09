/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_RESIDENT_H
#define _MM_MEMCOMP_RESIDENT_H

/* Include after memcomp_map_resident() and memcomp_fault_sigbus_or_retry(). */
static inline vm_fault_t
mc16_resident_lock_or_retry(struct folio *folio, struct vm_fault *vmf)
{
	return folio_lock_or_retry(folio, vmf);
}

/*
 * Borrow ref until handled is set: nonresident returns leave it intact. Every
 * handled path consumes the lookup, and a successful PTE install consumes the
 * removed token's PTE reference too, without another registry lookup.
 *
 * The folio_get under the first T protects the folio, not record/member. Re-read
 * the effective view after locking the folio; never retain a record across T.
 * Release T before taking PTL. The locked folio then excludes the producer's
 * store/owner transition while the original complete PTE is checked.
 *
 * Success transfers this folio_get to the PTE. The producer's OWNER, resident
 * hold and record anon_vma hold remain its responsibility. In particular this
 * fault does not detach the head or put the producer's anon_vma. Retry inherits
 * folio_lock_or_retry's fault-lock contract, including RETRY_NOWAIT retaining it.
 */
static inline vm_fault_t
mc16_load_resident(struct vm_fault *vmf, struct memcomp_ref *ref, bool *handled)
{
	struct vm_area_struct *vma = vmf->vma;
	struct mc16_view view;
	struct folio *folio;
	unsigned int page_idx, index;
	vm_fault_t ret;
	bool valid;

	*handled = false;
	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view) || !mc16_flags_valid(*view.flags))
		goto invalid;
	if (mc16_flags_state(*view.flags) != MC16_PENDING ||
	    !(*view.flags & MC16_F_RESIDENT)) {
		mutex_unlock(memcomp_lock_of(ref->token));
		return 0;
	}
	if (!view.record || !view.member ||
	    !mc16_member_index(view.record, ref->token, &page_idx))
		goto invalid;
	folio = view.member->owner_folio;
	if (!folio || page_idx >= folio_nr_pages(folio))
		goto invalid;
	folio_get(folio);
	mutex_unlock(memcomp_lock_of(ref->token));
	*handled = true;

	ret = mc16_resident_lock_or_retry(folio, vmf);
	if (ret) {
		folio_put(folio);
		mc16_ref_put(ref);
		return ret;
	}
	mutex_lock(memcomp_lock_of(ref->token));
	valid = mc16_view_locked(ref, &view) && mc16_flags_valid(*view.flags) &&
		mc16_flags_state(*view.flags) == MC16_PENDING &&
		(*view.flags & MC16_F_RESIDENT) && view.record && view.member &&
		view.member->owner_folio == folio &&
		mc16_member_index(view.record, ref->token, &index) &&
		index == page_idx && index < folio_nr_pages(folio);
	mutex_unlock(memcomp_lock_of(ref->token));
	if (!valid)
		goto out_put_folio;

	if (!memcomp_pte_map_lock(vma->vm_mm, vmf->pmd, vmf->address,
				  &vmf->pte, &vmf->ptl))
		goto out_put_folio;
	if (!pte_same(ptep_get(vmf->pte), vmf->orig_pte)) {
		pte_unmap_unlock(vmf->pte, vmf->ptl);
		vmf->pte = NULL;
		goto out_put_folio;
	}
	if (memcomp_map_resident(vma, vmf->address, vmf->pte, vmf->orig_pte,
				 folio, folio_page(folio, page_idx),
				 vmf->flags & FAULT_FLAG_WRITE))
		vmf->flags &= ~FAULT_FLAG_WRITE;
	pte_unmap_unlock(vmf->pte, vmf->ptl);
	vmf->pte = NULL;
	folio_unlock(folio);
	/* This folio_get is now the PTE's; lookup + old PTE leave together. */
	mc16_ref_put_count(ref, 2);
	return VM_FAULT_NOPAGE;

out_put_folio:
	folio_unlock(folio);
	folio_put(folio);
	mc16_ref_put(ref);
	return 0;
invalid:
	mutex_unlock(memcomp_lock_of(ref->token));
	mc16_ref_put(ref);
	*handled = true;
	return memcomp_fault_sigbus_or_retry(vmf);
}
#endif /* _MM_MEMCOMP_RESIDENT_H */
