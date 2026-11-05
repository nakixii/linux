/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_ACCESS_H
#define _MM_MEMCOMP_ACCESS_H

/* A borrowed view is valid under T, or for the unique refs-zero finalizer. */
struct mc16_view {
	struct mc16_record *record;
	struct mc16_member *member;
	u64 *word;
	u16 *length;
	u16 *flags;
};

static inline bool mc16_view_owned(struct memcomp_ref *ref, struct mc16_view *view)
{
	struct mc16_record *record;
	u32 index;

	*view = (struct mc16_view) { };
	if (mc16_flags_state(ref->slot->flags) != MC16_PENDING) {
		view->word = &ref->slot->word;
		view->length = &ref->slot->length;
		view->flags = &ref->slot->flags;
		return true;
	}
	record = (void *)(unsigned long)ref->slot->word;
	if (WARN_ON_ONCE(!mc16_member_index(record, ref->token, &index)))
		return false;
	view->record = record;
	view->member = &record->member[index];
	view->word = &view->member->word;
	view->length = &view->member->length;
	view->flags = &view->member->flags;
	return true;
}

static inline bool mc16_view_locked(struct memcomp_ref *ref, struct mc16_view *view)
{
	lockdep_assert_held(memcomp_lock_of(ref->token));
	return mc16_view_owned(ref, view);
}

/*
 * Transfer the attached member to a private detach tail. The tail consumes
 * its own saved fields before exactly one member put, outside T if desired.
 * A payload's charge/ownership transfers to the stable slot, never to tail.
 */
static inline struct mc16_record *mc16_normalize_locked(struct memcomp_ref *ref)
{
	struct mc16_view view;
	unsigned int state;
	u16 flags;

	if (!mc16_view_locked(ref, &view) || !view.record)
		return NULL;
	flags = *view.flags;
	state = mc16_flags_state(flags);
	if ((flags & (MC16_F_OWNER | MC16_F_RESIDENT)) ||
	    state == MC16_PENDING || state == MC16_EMPTY)
		return NULL;
	ref->slot->word = *view.word;
	ref->slot->length = *view.length;
	/* Immutable SAMEFILL readers pair with this release publication. */
	smp_store_release(&ref->slot->flags, flags);
	view.member->payload_charge = 0;
	return view.record;
}

static inline bool
mc16_owner_matches_locked(struct memcomp_ref *ref, struct folio *folio,
			  unsigned int nr_pages, unsigned int page_idx)
{
	struct mc16_view view;
	u32 index;

	if (!mc16_view_locked(ref, &view) || !view.record ||
	    !mc16_member_index(view.record, ref->token, &index))
		return false;
	return (*view.flags & MC16_F_OWNER) &&
		view.member->owner_folio == folio &&
		view.record->nr_pages == nr_pages && index == page_idx;
}

/*
 * Sole refs-zero winner only. Call state/FIFO/stat retirement before this
 * helper, while the slot and cluster are still live. It frees the payload
 * synchronously; the caller then invokes mc16_retire_slot() exactly once.
 * No T is acquired, so PTL/atomic final puts remain legal.
 */
static inline void mc16_release_resources(struct memcomp_ref *ref)
{
	struct mc16_view view;
	struct obj_cgroup *objcg;
	unsigned int state;
	u32 charge = 0;

	if (WARN_ON_ONCE(refcount_read(&ref->slot->refs)) ||
	    !mc16_view_owned(ref, &view))
		return;
	state = mc16_flags_state(*view.flags);
	objcg = view.record ? view.record->objcg : ref->cluster->key->objcg;
	if (view.member) {
		charge = view.member->payload_charge;
	} else if (objcg && (state == MC16_STORED || state == MC16_WRITEBACK)) {
		if (WARN_ON_ONCE(!mc16_length_decode(*view.length, &charge)))
			return;
	}
	if (state == MC16_STORED || state == MC16_WRITEBACK) {
		memcomp_zs_free(0, *view.word);
	} else if (state == MC16_SWAPPED) {
#ifdef CONFIG_SWAP
		struct mc16_swap_fields swap;

		if (WARN_ON_ONCE(!mc16_swap_decode(*view.word, *view.length, &swap)))
			return;
		if (swap.packed)
			memcomp_slot_put_packed(swp_entry(swap.type, swap.swap_offset));
		else
			memcomp_slot_put(swp_entry(swap.type, swap.swap_offset));
#endif
	}
	memcomp_objcg_uncharge(objcg, charge);
	if (*view.flags & MC16_F_RESIDENT) {
		if (WARN_ON_ONCE(!view.member || !view.member->owner_folio))
			return;
		folio_put(view.member->owner_folio);
	}
	/* This is the final access to record/member by this owner. */
	if (view.record)
		mc16_record_member_put(view.record);
}
#endif /* _MM_MEMCOMP_ACCESS_H */
