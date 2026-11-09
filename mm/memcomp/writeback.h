/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_WRITEBACK_H
#define _MM_MEMCOMP_WRITEBACK_H

/* Include after access/fifo. All *_locked helpers borrow the caller's T. */
struct mc16_writeback {
	struct mc16_slot *slot;
	u64 word;
	u16 length;
};

/* Only old payload ownership, never a borrowed record/member pointer. */
struct mc16_writeback_tail {
	u64 word;
	u32 charge;
};

/*
 * The writer keeps a continuous slot guard from claim through finish. The
 * snapshot's length is encoded, not bytes. It remains valid if owner-drop
 * normalizes WRITEBACK; the guard alone does not pin the original record.
 * Rejection leaves the snapshot untouched. One original eligibility L only.
 */
static inline bool
mc16_writeback_claim_locked(struct memcomp_ref *ref,
			    struct mc16_writeback *snapshot)
{
	struct mc16_view view;
	u32 bytes;

	if (*mc16_shadow_ptr(ref) == MC16_SHADOW_PUBLISHING ||
	    !mc16_view_locked(ref, &view) ||
	    !mc16_flags_valid(*view.flags) ||
	    mc16_flags_state(*view.flags) != MC16_STORED ||
	    (*view.flags & MC16_F_RESIDENT) || !*view.word ||
	    !mc16_length_decode(*view.length, &bytes))
		return false;
	*snapshot = (struct mc16_writeback) {
		.slot = ref->slot, .word = *view.word, .length = *view.length,
	};
	*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_WRITEBACK;
	mc16_fifo_transition(ref, MC16_STORED, MC16_WRITEBACK);
	return true;
}

/* Idempotent failed-I/O rollback; no payload, charge or backing is consumed. */
static inline bool mc16_writeback_revert_locked(struct memcomp_ref *ref)
{
	struct mc16_view view;

	if (!mc16_view_locked(ref, &view) ||
	    mc16_flags_state(*view.flags) != MC16_WRITEBACK)
		return false;
	*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_STORED;
	mc16_fifo_transition(ref, MC16_WRITEBACK, MC16_STORED);
	return true;
}

/*
 * Caller already owns ONE backing reference described by swap, even for a
 * packed member. Success transfers it to this slot without a dup or put.
 * The backing fields are real device type/offset/byte offset/length, not
 * swap.val in word. Raw backing still preserves the old compressed length.
 * Backing allocation, I/O and backing-device validation belong to caller.
 *
 * tail must be empty. On success finish it outside T BEFORE putting this
 * guard, which keeps the key's accounting owner alive. OWNER retains the
 * record; owner-drop may instead already have detached/freed it. Resolve
 * the effective view afresh under T on every attempt. No extra T/A/X/RMW,
 * and no L on success (WRITEBACK and SWAPPED are both ineligible).
 *
 * Rejection leaves tail and caller's backing ownership untouched. A payload
 * identity mismatch rolls WRITEBACK back to STORED;
 * invalid backing encoding leaves WRITEBACK for the caller's I/O rollback.
 */
static inline int
mc16_writeback_settle_locked(struct memcomp_ref *ref,
			     const struct mc16_writeback *snapshot,
			     const struct mc16_swap_fields *swap,
			     struct mc16_writeback_tail *tail)
{
	struct mc16_view view;
	u64 word;
	u32 bytes;
	u16 length;

	lockdep_assert_held(memcomp_lock_of(ref->token));
	if (snapshot->slot != ref->slot || !mc16_view_locked(ref, &view) ||
	    mc16_flags_state(*view.flags) != MC16_WRITEBACK)
		return -ESTALE;
	if (*view.word != snapshot->word || *view.length != snapshot->length) {
		mc16_writeback_revert_locked(ref);
		return -ESTALE;
	}
	if (!mc16_flags_valid(*view.flags) ||
	    (*view.flags & MC16_F_RESIDENT) || !*view.word ||
	    !mc16_length_decode(*view.length, &bytes))
		return -EIO;
	if (tail->word || tail->charge || !swap || swap->length != bytes ||
	    !mc16_swap_encode(swap->type, swap->swap_offset, swap->byte_offset,
			      swap->length, swap->packed, &word, &length) ||
	    !swp_entry(swap->type, swap->swap_offset).val)
		return -EINVAL;

	*tail = (struct mc16_writeback_tail) {
		.word = *view.word,
		.charge = view.member ? view.member->payload_charge :
			(ref->cluster->key->objcg ? bytes : 0),
	};
	*view.word = word;
	*view.length = length;
	*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_SWAPPED;
	if (view.member)
		view.member->payload_charge = 0;
	/* Length, raw/samefill, stored and compressed gauges do not change. */
	memcomp_debug_long_inc(&memcomp_swapped_entries);
	if (swap->packed)
		memcomp_debug_inc(&memcomp_writeback_packed_payloads);
	return 0;
}

/* No backing put: that reference now belongs to the effective SWAPPED slot. */
static inline void
mc16_writeback_finish(struct memcomp_ref *ref,
		      struct mc16_writeback_tail *tail)
{
	if (!tail->word)
		return;
	memcomp_zs_free(0, tail->word);
	memcomp_objcg_uncharge(ref->cluster->key->objcg, tail->charge);
	*tail = (struct mc16_writeback_tail) { };
}
#endif /* _MM_MEMCOMP_WRITEBACK_H */
