/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_FOLIO_H
#define _MM_MEMCOMP_FOLIO_H

/*
 * Author: nakixii
 *
 * Include after the generic eligibility, PTE-map/resident-map, hash/statistics
 * helpers and the mc16 anon/span/owner helpers. Production dispatch is separate.
 * All producer entry points require the original folio's lock. No record or
 * member pointer is retained across T; only the head's slot guard survives.
 */
enum mc16_folio_state {
	MC16_FOLIO_NONE,
	MC16_FOLIO_PENDING,
	MC16_FOLIO_STORED,
};

static inline bool mc16_folio_geometry(struct folio *folio, u32 *token,
				       unsigned int *nr)
{
	unsigned long value = swp_offset(folio->swap);

	*nr = folio_nr_pages(folio);
	if (!value || !*nr || *nr > MEMCOMP_MAX_COMPRESS_PAGES ||
	    value > U32_MAX - (*nr - 1))
		return false;
	*token = value;
	return true;
}

/* A successful uniform-span probe transfers one continuous head guard out. */
static inline enum mc16_folio_state
mc16_folio_span(struct folio *folio, struct memcomp_ref *head)
{
	enum mc16_folio_state span = MC16_FOLIO_NONE;
	unsigned int nr, i;
	u32 token;

	*head = (struct memcomp_ref) { };
	if (!mc16_folio_geometry(folio, &token, &nr) || !mc16_get(token, head))
		return MC16_FOLIO_NONE;
	for (i = 0; i < nr; i++) {
		struct memcomp_ref sibling = { }, *ref = head;
		enum mc16_folio_state state = MC16_FOLIO_NONE;
		struct mc16_view view;

		if (i) {
			if (!mc16_get(token + i, &sibling))
				goto fail;
			ref = &sibling;
		}
		mutex_lock(memcomp_lock_of(ref->token));
		if (mc16_owner_matches_locked(ref, folio, nr, i) &&
		    mc16_view_locked(ref, &view) && mc16_flags_valid(*view.flags)) {
			unsigned int effective = mc16_flags_state(*view.flags);

			if (effective == MC16_PENDING && (*view.flags & MC16_F_RESIDENT))
				state = MC16_FOLIO_PENDING;
			else if (effective >= MC16_STORED && effective <= MC16_SAMEFILL &&
				 !(*view.flags & MC16_F_RESIDENT))
				state = MC16_FOLIO_STORED;
		}
		mutex_unlock(memcomp_lock_of(ref->token));
		if (i)
			mc16_ref_put(&sibling);
		if (state == MC16_FOLIO_NONE || (i && state != span))
			goto fail;
		span = state;
	}
	return span;
fail:
	mc16_ref_put(head);
	return MC16_FOLIO_NONE;
}

/* Transfer producer-owned bookkeeping before any member can be detached. */
static inline struct anon_vma *
mc16_folio_take(struct folio *folio, struct memcomp_ref *head, unsigned int nr,
		bool *incompressible)
{
	struct anon_vma *anon_vma = NULL;
	struct mc16_view view;

	mutex_lock(memcomp_lock_of(head->token));
	if (mc16_owner_matches_locked(head, folio, nr, 0) &&
	    mc16_view_locked(head, &view) && mc16_flags_valid(*view.flags)) {
		anon_vma = mc16_span_take_anon_vma_locked(head, folio, nr);
		if (incompressible) {
			*incompressible = *view.flags & MC16_F_INCOMPRESSIBLE;
			*view.flags &= ~MC16_F_INCOMPRESSIBLE;
		}
	}
	mutex_unlock(memcomp_lock_of(head->token));
	return anon_vma;
}

/* Consumes head, including its lookup, last. Never inspect its record again. */
static inline void mc16_folio_detach(struct folio *folio,
				     struct memcomp_ref *head, unsigned int nr)
{
	unsigned int i;
	u32 token = head->token;

	for (i = nr; i > 1; ) {
		struct memcomp_ref sibling;

		i--;
		if (mc16_get(token + i, &sibling))
			mc16_owner_drop_folio_held(&sibling, folio, nr, i);
	}
	mc16_owner_drop_folio_held(head, folio, nr, 0);
}

static inline void mc16_invalidate_folio(struct folio *folio)
{
	struct anon_vma *anon_vma;
	struct memcomp_ref head;
	unsigned int nr;
	u32 token;

	if (!folio_test_locked(folio) || !folio_memcomp_reserved(folio))
		return;
	if (!mc16_folio_geometry(folio, &token, &nr) || !mc16_get(token, &head)) {
		folio->swap.val = 0;
		return;
	}
	anon_vma = mc16_folio_take(folio, &head, nr, NULL);
	mc16_folio_detach(folio, &head, nr);
	folio->swap.val = 0;
	if (anon_vma)
		put_anon_vma(anon_vma);
}

/* Success leaves the source frozen at zero, owned by the caller for freeing. */
static inline bool mc16_release_folio(struct folio *folio)
{
	struct mc16_owner_tail tail = { };
	struct anon_vma *anon_vma = NULL;
	struct memcomp_ref head;
	struct mc16_view view;
	unsigned int nr, i;
	u32 token;
	bool dropped = false;

	if (!folio_test_locked(folio) || !folio_memcomp_reserved(folio))
		return false;
	if (!swp_offset(folio->swap)) {
		/* Token0 transport owns no slot, record or zero ledger. */
		if (folio_test_large(folio) || folio_test_anon(folio) ||
		    folio_mapped(folio) || folio_mapping(folio) ||
		    !folio_ref_freeze(folio, 1))
			return false;
		folio->swap.val = 0;
		return true;
	}
	if (!mc16_folio_geometry(folio, &token, &nr) || !mc16_get(token, &head))
		return false;
	/* Resident PENDING holds reject the freeze without another T probe. */
	if (!folio_ref_freeze(folio, 1)) {
		mc16_ref_put(&head);
		return false;
	}
	/* The still-attached head owns the record throughout sibling detach. */
	for (i = nr; i > 1; ) {
		struct memcomp_ref sibling;

		i--;
		if (mc16_get(token + i, &sibling) &&
		    !mc16_owner_drop_folio_held(&sibling, folio, nr, i)) {
			mc16_ref_put(&head);
			folio_ref_unfreeze(folio, 1);
			return false;
		}
	}
	mutex_lock(memcomp_lock_of(token));
	if (mc16_owner_matches_locked(&head, folio, nr, 0) &&
	    mc16_view_locked(&head, &view) && mc16_flags_valid(*view.flags) &&
	    mc16_flags_state(*view.flags) != MC16_PENDING &&
	    !(*view.flags & MC16_F_RESIDENT)) {
		dropped = mc16_owner_drop_locked(&head, folio, nr, 0, &tail);
		/* The detach tail owns this member until its final put below. */
		if (dropped && tail.record &&
		    (tail.record->record_flags & MC16_RECORD_ANON_VMA)) {
			anon_vma = tail.record->anon_vma;
			tail.record->anon_vma = NULL;
			tail.record->record_flags &= ~MC16_RECORD_ANON_VMA;
		}
	}
	mutex_unlock(memcomp_lock_of(token));
	mc16_ref_put_count(&head, dropped ? 2 : 1);
	mc16_owner_tail_put(&tail);
	if (anon_vma)
		put_anon_vma(anon_vma);
	if (!dropped) {
		folio_ref_unfreeze(folio, 1);
		return false;
	}
	folio->swap.val = 0;
	return true;
}

/* OWNER + this probe's lookup is two, including the borrowed head lookup. */
static inline bool mc16_folio_pending_refs(struct folio *folio,
					   struct memcomp_ref *head, unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		struct memcomp_ref sibling = { }, *ref = head;
		struct mc16_view view;
		bool escaped;

		if (i) {
			if (!mc16_get(head->token + i, &sibling))
				continue;
			ref = &sibling;
		}
		mutex_lock(memcomp_lock_of(ref->token));
		escaped = mc16_owner_matches_locked(ref, folio, nr, i) &&
			mc16_view_locked(ref, &view) && mc16_flags_valid(*view.flags) &&
			mc16_flags_state(*view.flags) == MC16_PENDING &&
			(*view.flags & MC16_F_RESIDENT) && refcount_read(&ref->slot->refs) > 2;
		mutex_unlock(memcomp_lock_of(ref->token));
		if (i)
			mc16_ref_put(&sibling);
		if (escaped)
			return true;
	}
	return false;
}

struct mc16_folio_restore {
	u32 token;
	unsigned int found;
	bool count_only;
};

static bool mc16_folio_restore_one(struct folio *folio,
				   struct vm_area_struct *vma,
				   unsigned long address, void *arg)
{
	struct mc16_folio_restore *walk = arg;
	struct mm_struct *mm = vma->vm_mm;
	unsigned int nr = folio_nr_pages(folio), index = 0;

	if (nr > 1) {
		pgoff_t page_index = linear_page_index(vma, address);
		pgoff_t first = folio_pgoff(folio);

		if (page_index < first || page_index - first >= nr)
			return true;
		index = page_index - first;
	}
	for (; index < nr && address < vma->vm_end; index++, address += PAGE_SIZE) {
		struct memcomp_ref removed;
		spinlock_t *ptl; /* Protects the complete original token PTE. */
		pte_t *ptep, orig;
		pmd_t *pmd = mm_find_pmd(mm, address);
		u32 token = walk->token + index;

		if (!pmd || !memcomp_pte_map_lock(mm, pmd, address, &ptep, &ptl))
			continue;
		orig = ptep_get(ptep);
		if (!is_swap_pte(orig) || pte_to_swp_entry(orig).val !=
		    make_memcomp_entry(token).val)
			goto out_pte;
		if (walk->count_only) {
			walk->found++;
			goto out_pte;
		}
		/* Get before publishing: a failed lookup leaves the PTE unchanged. */
		if (!mc16_get(token, &removed))
			goto out_pte;
		folio_get(folio);
		memcomp_map_resident(vma, address, ptep, orig, folio,
				     folio_page(folio, index), false);
		pte_unmap_unlock(ptep, ptl);
		/* The folio_get now belongs to the new PTE; retire lookup + token. */
		mc16_ref_put_count(&removed, 2);
		memcomp_debug_inc(&memcomp_restore_ptes);
		walk->found++;
		continue;
out_pte:
		pte_unmap_unlock(ptep, ptl);
	}
	return true;
}

static inline unsigned int
mc16_folio_restore_span(struct folio *folio, u32 token, bool count_only)
{
	struct mc16_folio_restore walk = { .token = token, .count_only = count_only };
	struct rmap_walk_control rwc = {
		.rmap_one = mc16_folio_restore_one,
		.arg = &walk,
	};
	u64 start = memcomp_debug_clock();

	rmap_walk(folio, &rwc);
	if (!count_only) {
		if (folio_test_large(folio))
			deferred_split_folio_remapped(folio);
		memcomp_debug_elapsed(start, &memcomp_restore_ns, &memcomp_restore_max_ns);
	}
	return walk.found;
}

#define MC16_FOLIO_RESTORE_WALKS 3U

static inline void mc16_rollback_folio(struct folio *folio)
{
	struct anon_vma *anon_vma;
	struct memcomp_ref head;
	unsigned int nr, walks;
	bool incompressible = false;
	u32 token, hash = 0;

	if (!folio_test_locked(folio) || !folio_memcomp_reserved(folio))
		return;
	if (!mc16_folio_geometry(folio, &token, &nr) || !mc16_get(token, &head)) {
		folio->swap.val = 0;
		return;
	}
	anon_vma = mc16_folio_take(folio, &head, nr, &incompressible);
	/* Hash before any restored mapping can be written by another task. */
	if (incompressible)
		hash = memcomp_folio_hash(folio);
	if (anon_vma) {
		for (walks = 0; walks < MC16_FOLIO_RESTORE_WALKS; walks++) {
			if (!mc16_folio_pending_refs(folio, &head, nr))
				break;
			if (!walks) {
				memcomp_debug_inc(&memcomp_restore_spans);
			} else {
				memcomp_debug_inc(&memcomp_restore_rewalks);
				cond_resched();
			}
			if (!mc16_folio_restore_span(folio, token, false) && walks)
				break;
		}
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		memcomp_debug_add(mc16_folio_restore_span(folio, token, true),
				  &memcomp_restore_missed_pages);
#endif
	}
	mc16_folio_detach(folio, &head, nr);
	if (incompressible) {
		folio_set_incompressible(folio, hash);
		memcomp_debug_add(nr, &memcomp_incompressible_marked_pages);
	} else {
		folio->swap.val = 0;
	}
	if (anon_vma)
		put_anon_vma(anon_vma);
}

static inline bool mc16_reserve_folio(struct folio *folio)
{
	struct memcomp_ref head;
	struct anon_vma *anon_vma;
	unsigned long token;
	int ret;

	if (!memcomp_reserve_eligible(folio))
		return false;
	if (folio_memcomp_reserved(folio)) {
		enum mc16_folio_state span = mc16_folio_span(folio, &head);

		if (head.slot)
			mc16_ref_put(&head);
		if (span == MC16_FOLIO_PENDING)
			return true;
		mc16_rollback_folio(folio);
	}
	anon_vma = folio_get_anon_vma(folio);
	ret = mc16_anon_reserve(folio, folio_nr_pages(folio), anon_vma, &token);
	if (ret) {
		if (anon_vma)
			put_anon_vma(anon_vma);
		return false;
	}
	folio->swap = make_memcomp_entry(token);
	return true;
}
#endif /* _MM_MEMCOMP_FOLIO_H */
