/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_SPAN_H
#define _MM_MEMCOMP_SPAN_H

/* Immutable order bits suffice even after a member has been normalized. */
static inline unsigned int mc16_ref_geometry(const struct memcomp_ref *ref,
					     u32 *base, unsigned int *index)
{
	unsigned int nr, offset;
	u16 flags;

	if (!ref->slot)
		return 0;
	/* Normalize release-publishes the payload and preserves the order. */
	flags = smp_load_acquire(&ref->slot->flags);
	if (!mc16_flags_valid(flags))
		return 0;
	nr = 1U << ((flags & MC16_ORDER_MASK) >> MC16_ORDER_SHIFT);
	offset = ref->token & (nr - 1);
	if (base)
		*base = ref->token - offset;
	if (index)
		*index = offset;
	return nr;
}

/* Owns one lookup per successful output; failure consumes every lookup. */
static inline bool mc16_span_get(u32 token, struct folio *folio,
				 unsigned int nr_pages, bool pending,
				 struct memcomp_ref *refs)
{
	unsigned int i, state, nr_found = 0;
	struct mc16_view view;
	bool match;

	if (!nr_pages || nr_pages > MEMCOMP_MAX_COMPRESS_PAGES ||
	    !is_power_of_2(nr_pages))
		return false;
	for (i = 0; i < nr_pages; i++)
		refs[i] = (struct memcomp_ref) { };
	if (!token || token > U32_MAX - (nr_pages - 1))
		return false;
	for (i = 0; i < nr_pages; i++) {
		if (!mc16_get(token + i, &refs[i]))
			goto fail;
		nr_found++;
		mutex_lock(memcomp_lock_of(refs[i].token));
		match = mc16_owner_matches_locked(&refs[i], folio, nr_pages, i);
		if (match) {
			mc16_view_locked(&refs[i], &view);
			state = mc16_flags_state(*view.flags);
			match = mc16_flags_valid(*view.flags) && (pending ?
				(state == MC16_PENDING &&
				 (*view.flags & MC16_F_RESIDENT)) :
				(state >= MC16_STORED && state <= MC16_SAMEFILL));
		}
		mutex_unlock(memcomp_lock_of(refs[i].token));
		if (!match)
			goto fail;
	}
	return true;
fail:
	while (nr_found)
		mc16_ref_put(&refs[--nr_found]);
	return false;
}

/*
 * The locked folio's producer still owns its head member. Transfer its one
 * anon_vma hold before head detach; put_anon_vma belongs to that sleepable
 * producer tail, never to the PTL-safe final slot/member release.
 */
static inline struct anon_vma *
mc16_span_take_anon_vma_locked(struct memcomp_ref *head, struct folio *folio,
			       unsigned int nr_pages)
{
	struct mc16_view view;
	struct anon_vma *anon_vma;

	if (!mc16_owner_matches_locked(head, folio, nr_pages, 0) ||
	    !mc16_view_locked(head, &view) ||
	    !(view.record->record_flags & MC16_RECORD_ANON_VMA))
		return NULL;
	anon_vma = view.record->anon_vma;
	view.record->anon_vma = NULL;
	view.record->record_flags &= ~MC16_RECORD_ANON_VMA;
	return anon_vma;
}
#endif /* _MM_MEMCOMP_SPAN_H */
