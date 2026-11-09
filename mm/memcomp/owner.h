/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_OWNER_H
#define _MM_MEMCOMP_OWNER_H

/* Include after access/refs and memcomp_state_drop_owner(). */
#include "workingset_store.h"

struct mc16_owner_tail {
	struct mc16_record *record;
	bool resident;
};

/*
 * T is held and ref owns a real lookup in addition to the attached OWNER.
 * tail is initially zero and remains untouched on rejection. Success moves
 * only detached member ownership to tail; payload/charge move to the slot.
 * No slot ref is consumed here. After unlocking, the caller invalidates arch
 * state for tail->resident, then puts lookup+OWNER with ONE put_count(ref, 2).
 * Rejection instead puts only the lookup. All record-dependent local work
 * must finish before owner_tail_put(), which is the last record access.
 *
 * The producer keeps its head member until every span/identity/anon_vma use
 * is complete and drops that head last. A slot guard is not a record pin.
 * anon_vma must be handed off by the producer, never put by a PTL finalizer.
 */
static inline bool
mc16_owner_can_drop_locked(struct memcomp_ref *ref, struct folio *folio,
			   unsigned int nr_pages, unsigned int page_idx)
{
	struct mc16_swap_fields swap;
	struct mc16_view view;
	unsigned int state;
	u32 bytes, index;
	u16 flags;
	bool resident;

	if (!mc16_view_locked(ref, &view) || !view.record ||
	    !mc16_member_index(view.record, ref->token, &index))
		return false;
	flags = *view.flags;
	if (!mc16_flags_valid(flags) || !(flags & MC16_F_OWNER) ||
	    view.member->owner_folio != folio ||
	    view.record->nr_pages != nr_pages || index != page_idx ||
	    refcount_read(&ref->slot->refs) < 2)
		return false;
	state = mc16_flags_state(flags);
	resident = state == MC16_PENDING && (flags & MC16_F_RESIDENT);
	if (!resident) {
		if ((flags & MC16_F_RESIDENT) ||
		    (state != MC16_STORED && state != MC16_WRITEBACK &&
		     state != MC16_SAMEFILL && state != MC16_SWAPPED) ||
		    !mc16_payload_length_decode(state, *view.length, &bytes))
			return false;
		if (((state == MC16_STORED || state == MC16_WRITEBACK) &&
		     !*view.word) ||
		    (state == MC16_SWAPPED &&
		     !mc16_swap_decode(*view.word, *view.length, &swap)))
			return false;
	}

	return true;
}

/*
 * Infallible detach after whole-span validation, PUBLISHING and folio freeze.
 * Source OWNER + guard keep the record attached despite concurrent PTE zap.
 * Do not use a sequence of fallible sibling drops after recording eviction.
 */
static inline void
mc16_owner_detach_locked(struct memcomp_ref *ref, struct mc16_owner_tail *tail)
{
	struct mc16_view view;
	u16 flags;

	lockdep_assert_held(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	flags = *view.flags;
	memcomp_state_drop_owner();
	view.member->owner_folio = NULL;
	view.member->payload_charge = 0;
	ref->slot->word = *view.word;
	ref->slot->length = *view.length;
	/* Publish the detached payload before lockless SAMEFILL readers run. */
	smp_store_release(&ref->slot->flags, flags & ~MC16_F_OWNER);
	*tail = (struct mc16_owner_tail) { .record = view.record };
}

static inline bool
mc16_owner_drop_locked(struct memcomp_ref *ref, struct folio *folio,
		       unsigned int nr_pages, unsigned int page_idx,
		       struct mc16_owner_tail *tail)
{
	struct mc16_view view;
	u16 flags;
	bool resident;

	if (!mc16_owner_can_drop_locked(ref, folio, nr_pages, page_idx))
		return false;
	mc16_view_locked(ref, &view);
	flags = *view.flags;
	resident = mc16_flags_state(flags) == MC16_PENDING &&
		   (flags & MC16_F_RESIDENT);
	/* Rollback/invalidation never creates a workingset shadow. */
	if (*mc16_shadow_ptr(ref) == MC16_SHADOW_PUBLISHING)
		*mc16_shadow_ptr(ref) = 0;
	memcomp_state_drop_owner();
	*view.flags = flags & ~MC16_F_OWNER;
	*tail = (struct mc16_owner_tail) { .resident = resident };
	if (!resident) {
		view.member->owner_folio = NULL;
		tail->record = mc16_normalize_locked(ref);
	} else {
		/* Mirror OWNER for refs-only queries; retain the attached record. */
		WRITE_ONCE(ref->slot->flags,
			   READ_ONCE(ref->slot->flags) & ~MC16_F_OWNER);
	}
	/* Resident PENDING retains its member and resident folio reference. */
	return true;
}

/* Outside T. Consume all saved record fields before this final member put. */
static inline void mc16_owner_tail_put(struct mc16_owner_tail *tail)
{
	struct mc16_record *record = tail->record;

	*tail = (struct mc16_owner_tail) { };
	if (record)
		mc16_record_member_put(record);
	/* Neither record nor any of its members may be accessed hereafter. */
}

/*
 * Consumes the lookup on either outcome, and OWNER only on success. Callers
 * needing record fields after unlocking use drop_locked()/tail_put() instead.
 * One existing T acquisition, no allocation or extra lookup/pin. A successful
 * nonresident drop costs one slot sub(2) and one detached member decrement.
 */
static inline bool
mc16_owner_drop_folio_held(struct memcomp_ref *ref, struct folio *folio,
			   unsigned int nr_pages, unsigned int page_idx)
{
	struct mc16_owner_tail tail = { };
	struct mutex *lock = memcomp_lock_of(ref->token);
	bool dropped;

	mutex_lock(lock);
	dropped = mc16_owner_drop_locked(ref, folio, nr_pages, page_idx, &tail);
	mutex_unlock(lock);
	/* Match the resident rollback invalidation even with surviving guards. */
	if (dropped && tail.resident)
		arch_swap_invalidate_page(SWP_MEMCOMP, ref->token);
	mc16_ref_put_count(ref, dropped ? 2 : 1);
	/* ref may already be empty and its slot/cluster/key already retired. */
	mc16_owner_tail_put(&tail);
	return dropped;
}
#endif /* _MM_MEMCOMP_OWNER_H */
