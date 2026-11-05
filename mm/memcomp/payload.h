/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_PAYLOAD_H
#define _MM_MEMCOMP_PAYLOAD_H

/* Prepared outside T; charge and zs ownership transfer only on commit. */
struct mc16_payload {
	u64 word;
	u32 bytes;
	u32 charge;
};

/* Finish before putting the guard: its key still owns the accounting owner. */
struct mc16_payload_tail {
	struct folio *resident;
	u64 word;
	u32 bytes;
	u32 new_bytes;
	u32 charge;
	u16 length;
	u16 flags;
};

static inline int mc16_payload_charge(struct memcomp_ref *ref,
				      struct mc16_payload *payload)
{
	struct obj_cgroup *objcg = ref->cluster->key->objcg;
	int ret;

	if (payload->bytes > PAGE_SIZE || payload->charge)
		return -EINVAL;
	if (!objcg || !payload->bytes)
		return 0;
	ret = memcomp_objcg_charge(objcg, GFP_NOWAIT | __GFP_NOWARN,
				   payload->bytes);
	if (!ret)
		payload->charge = payload->bytes;
	return ret;
}

/* The guard must still be held; same-fill word is never a zs handle. */
static inline void mc16_payload_abort(struct memcomp_ref *ref,
				      struct mc16_payload *payload)
{
	if (payload->bytes && payload->word)
		memcomp_zs_free(0, payload->word);
	memcomp_objcg_uncharge(ref->cluster->key->objcg, payload->charge);
	*payload = (struct mc16_payload) { };
}

static inline void mc16_payload_account(unsigned int old_state, u32 old_bytes,
					unsigned int new_state, u32 new_bytes)
{
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	bool old_same = old_state == MC16_SAMEFILL;
	bool new_same = new_state == MC16_SAMEFILL;
	bool old_raw = !old_same && old_bytes == PAGE_SIZE;
	bool new_raw = !new_same && new_bytes == PAGE_SIZE;

	if (old_same != new_same)
		this_cpu_add(memcomp_state_stats.samefill_pages, new_same ? 1 : -1);
	if (old_raw != new_raw)
		this_cpu_add(memcomp_state_stats.raw_pages, new_raw ? 1 : -1);
#endif
}

/*
 * T is already held. No new ref, allocation, or T/X acquisition is needed.
 * A rejected identity/state leaves both payloads and their charges intact.
 * OWNER keeps this member attached; owner removal normalizes it later.
 */
static inline int
mc16_payload_commit_locked(struct memcomp_ref *ref, struct folio *folio,
			   unsigned int nr_pages, unsigned int page_idx,
			   struct mc16_payload *payload,
			   struct mc16_payload_tail *tail)
{
	struct mc16_view view;
	unsigned int old_state, new_state;
	u32 old_bytes = 0;
	u16 length, flags;

	if (!mc16_view_locked(ref, &view) || !view.record ||
	    !(*view.flags & MC16_F_OWNER) || view.member->owner_folio != folio ||
	    view.record->nr_pages != nr_pages ||
	    ref->token - view.record->token_base != page_idx)
		return -ESTALE;
	old_state = mc16_flags_state(*view.flags);
	if (old_state == MC16_WRITEBACK)
		return -EAGAIN;
	if (old_state != MC16_PENDING && old_state != MC16_STORED &&
	    old_state != MC16_SAMEFILL && old_state != MC16_SWAPPED)
		return -EIO;
	if (old_state != MC16_PENDING &&
	    !mc16_payload_length_decode(old_state, *view.length, &old_bytes))
		return -EIO;
	new_state = payload->bytes ? MC16_STORED : MC16_SAMEFILL;
	if (payload->bytes > PAGE_SIZE ||
	    !mc16_payload_length_encode(new_state, payload->bytes, &length) ||
	    payload->charge != (view.record->objcg ? payload->bytes : 0))
		return -EINVAL;
	*tail = (struct mc16_payload_tail) {
		.resident = (*view.flags & MC16_F_RESIDENT) ?
			    view.member->owner_folio : NULL,
		.word = *view.word, .bytes = old_bytes,
		.new_bytes = payload->bytes, .charge = view.member->payload_charge,
		.length = *view.length, .flags = *view.flags,
	};
	flags = *view.flags & ~(MC16_STATE_MASK | MC16_F_RESIDENT |
				 MC16_F_REFRESH_PENDING);
	*view.word = payload->word;
	*view.length = length;
	*view.flags = flags | new_state;
	view.member->payload_charge = payload->charge;
	mc16_payload_account(old_state, old_bytes, new_state, payload->bytes);
	mc16_fifo_transition(ref, old_state, new_state);
	*payload = (struct mc16_payload) { };
	return 0;
}

static inline void mc16_payload_finish(struct memcomp_ref *ref,
				       struct mc16_payload_tail *tail)
{
	unsigned int state = mc16_flags_state(tail->flags);

	if (state == MC16_STORED) {
		memcomp_zs_free(0, tail->word);
	} else if (state == MC16_SWAPPED) {
#ifdef CONFIG_SWAP
		struct mc16_swap_fields swap;

		if (WARN_ON_ONCE(!mc16_swap_decode(tail->word, tail->length, &swap)))
			return;
		if (swap.packed)
			memcomp_slot_put_packed(swp_entry(swap.type, swap.swap_offset));
		else
			memcomp_slot_put(swp_entry(swap.type, swap.swap_offset));
		memcomp_debug_long_dec(&memcomp_swapped_entries);
#endif
	}
	memcomp_objcg_uncharge(ref->cluster->key->objcg, tail->charge);
	if (tail->resident)
		folio_put(tail->resident);
	if (state == MC16_PENDING)
		memcomp_debug_dec(&memcomp_pending_pages);
	if (tail->new_bytes != tail->bytes) {
		memcomp_compressed_bytes_add(tail->new_bytes);
		memcomp_compressed_bytes_sub(tail->bytes);
	}
	*tail = (struct mc16_payload_tail) { };
}
#endif /* _MM_MEMCOMP_PAYLOAD_H */
