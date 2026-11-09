/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_ZERO_CONVERT_H
#define _MM_MEMCOMP_ZERO_CONVERT_H

/* Include after PTE map helpers, zero.h, refs.h and payload accessors. */
struct mc16_zero_walk {
	struct memcomp_ref ref;
	unsigned int converted;
};

static bool mc16_zero_convert_one(struct folio *folio,
				  struct vm_area_struct *vma,
				  unsigned long address, void *arg)
{
	struct mc16_zero_walk *walk = arg;
	struct mm_struct *mm = vma->vm_mm;
	struct memcomp_ref removed;
	spinlock_t *ptl; /* Protects the swap-format PTE transaction. */
	pte_t *ptep;
	pte_t orig;
	pmd_t *pmd;

	pmd = mm_find_pmd(mm, address);
	if (!pmd || !memcomp_pte_map_lock(mm, pmd, address, &ptep, &ptl))
		return true;
	orig = ptep_get(ptep);
	if (!is_swap_pte(orig) || pte_to_swp_entry(orig).val !=
	    make_memcomp_entry(walk->ref.token).val) {
		pte_unmap_unlock(ptep, ptl);
		return true;
	}

	/* Only the winning PTL transaction creates a logical zero owner. */
	set_pte_at(mm, address, ptep,
		   pte_move_swp_offset(orig, -(long)walk->ref.token));
	mc16_zero_pte_init(1);
	/* Transfer the removed PTE's existing ownership, without a new get. */
	removed = walk->ref;
	mc16_ref_put(&removed);
	pte_unmap_unlock(ptep, ptl);
	walk->converted++;
	return true;
}

/*
 * Run after releasing the codec context and before dropping the pinned
 * anon_vma. The folio remains locked through release_folio. A continuous
 * source OWNER protects the borrowed carrier; no record/member is carried.
 * Fork/MOVE may leave positive tokens behind: their SAMEFILL data stays
 * valid, so this is a best-effort metadata saving, not a correctness gate.
 */
static inline unsigned int
mc16_zero_convert_folio(struct folio *folio,
			const struct mc16_zero_carrier *carrier)
{
	struct mc16_zero_walk walk = { };
	struct rmap_walk_control rwc = {
		.rmap_one = mc16_zero_convert_one,
		.arg = &walk,
	};
	unsigned long token;

	if (!carrier || !carrier->valid || !carrier->ready ||
	    !carrier->ref.slot || !carrier->ref.cluster)
		return 0;
	token = carrier->token;
	if (!folio_test_locked(folio) || folio_test_large(folio) ||
	    !folio_test_anon(folio) || folio_mapped(folio) ||
	    folio_maybe_dma_pinned(folio) || !token || token > U32_MAX ||
	    carrier->ref.token != token ||
	    !pages_identical(&folio->page, ZERO_PAGE(0)))
		return 0;
	/* OWNER survives until release_folio; only matching PTEs consume refs. */
	walk.ref = carrier->ref;
	rmap_walk(folio, &rwc);
	return walk.converted;
}
#endif /* _MM_MEMCOMP_ZERO_CONVERT_H */
