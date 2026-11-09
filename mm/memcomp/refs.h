/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_REFS_H
#define _MM_MEMCOMP_REFS_H

/* Counters describe effective payloads, not the physical PENDING tag. */
static inline void mc16_account_state(struct memcomp_ref *ref, bool publish)
{
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct mc16_view view;
	unsigned int state;
	u32 bytes = 0;
	int delta = publish ? 1 : -1;

	if (!mc16_view_owned(ref, &view))
		return;
	state = mc16_flags_state(*view.flags);
	if (*view.flags & MC16_F_OWNER)
		this_cpu_add(memcomp_state_stats.folio_owner_pages, delta);
	if (state == MC16_SAMEFILL)
		this_cpu_add(memcomp_state_stats.samefill_pages, delta);
	else if (state != MC16_PENDING &&
		 mc16_payload_length_decode(state, *view.length, &bytes) &&
		 bytes == PAGE_SIZE)
		this_cpu_add(memcomp_state_stats.raw_pages, delta);
	if (ref->cluster->key->kind == MC16_KEY_SHMEM)
		this_cpu_add(memcomp_state_stats.shmem_pages, delta);
#endif
}

/* Producer owns these slots exclusively until its publication completes. */
static inline void mc16_account_reservation(struct memcomp_ref *ref)
{
	memcomp_stored_bytes_inc();
	memcomp_debug_inc(&memcomp_pending_pages);
	mc16_account_state(ref, true);
}

/* Unique refs-zero winner; no T, and no access after retiring the slot. */
static inline void mc16_ref_release(struct memcomp_ref *ref)
{
	struct mc16_view view;
	unsigned int state;
	u32 bytes = 0;

	if (!mc16_view_owned(ref, &view))
		return;
	state = mc16_flags_state(*view.flags);
	if (state != MC16_PENDING &&
	    WARN_ON_ONCE(!mc16_payload_length_decode(state, *view.length, &bytes)))
		return;
	/* L removal finishes before X can remove the last cluster live slot. */
	mc16_fifo_transition(ref, state, MC16_EMPTY);
	arch_swap_invalidate_page(SWP_MEMCOMP, ref->token);
	mc16_account_state(ref, false);
	if (state == MC16_PENDING)
		memcomp_debug_dec(&memcomp_pending_pages);
	else if (state == MC16_SWAPPED)
		memcomp_debug_long_dec(&memcomp_swapped_entries);
	memcomp_stored_bytes_dec();
	memcomp_compressed_bytes_sub(bytes);
	mc16_release_resources(ref);
	mc16_retire_slot(ref);
}

/* The caller proves ownership of nr actual references; no speculative sub. */
static inline void mc16_ref_put_count(struct memcomp_ref *ref, unsigned int nr)
{
	bool last;

	if (WARN_ON_ONCE(!ref->slot || !nr))
		return;
	last = refcount_sub_and_test(nr, &ref->slot->refs);
	if (last)
		mc16_ref_release(ref);
	/* A non-final put consumes the caller's guard just as a final one does. */
	*ref = (struct memcomp_ref) { };
}

static inline void mc16_ref_put(struct memcomp_ref *ref)
{
	mc16_ref_put_count(ref, 1);
}
#endif /* _MM_MEMCOMP_REFS_H */
