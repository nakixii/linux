/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_DEFRAG_H
#define _MM_MEMCOMP_DEFRAG_H
#ifdef CONFIG_SWAP

/* Include after memcomp_defrag_ops, its scan/move limits, swapoff.h and scan.h. */
static inline void *mc16_defrag_buffer_alloc(void)
{
	return kmalloc(PAGE_SIZE, GFP_NOWAIT | __GFP_NOWARN);
}

/* Copy already-compressed bytes verbatim; no codec or slow allocation. */
static inline int mc16_defrag_copy_payload(const void *src, unsigned int bytes,
					   struct mc16_payload *payload)
{
	unsigned long handle = 0;
	unsigned int copied = 0;
	int ret;

	ret = memcomp_alloc_raw_payload(0, src, bytes, &copied, &handle,
					false, NULL);
	if (!ret)
		*payload = (struct mc16_payload) { .word = handle, .bytes = copied };
	return ret;
}

/* Unlike swapoff, defrag removes a packed owner and its cached slot page. */
static inline void mc16_defrag_finish(struct mc16_swapoff_tail *tail)
{
	if (!tail->swap.val)
		return;
	memcomp_slot_put_packed(tail->swap);
	memcomp_debug_long_dec(&memcomp_swapped_entries);
	if (tail->new_bytes != tail->old_bytes) {
		memcomp_compressed_bytes_add(tail->new_bytes);
		memcomp_compressed_bytes_sub(tail->old_bytes);
	}
	*tail = (struct mc16_swapoff_tail) { };
}

/*
 * Caller retains the same guard through the tail. ops->slot_count cannot
 * sleep under T; ops->read_page must use GFP_NOWAIT, as the existing defrag
 * reader does. Return 1 moved, 0 noncandidate/stale, or a negative errno.
 */
static inline int mc16_defrag_entry(struct memcomp_ref *ref,
				    const struct memcomp_defrag_ops *ops)
{
	struct mc16_swapoff_tail tail = { };
	struct mc16_payload payload = { };
	struct mc16_swap_fields fields;
	struct mc16_view view;
	swp_entry_t swap;
	u64 word;
	u16 length;
	void *buf;
	int ret;

	if (!ref || !ref->slot || !ops || !ops->slot_count || !ops->read_page)
		return -EINVAL;
	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view) || !mc16_flags_valid(*view.flags)) {
		ret = -EIO;
		goto unlock;
	}
	ret = 0;
	if (mc16_flags_state(*view.flags) != MC16_SWAPPED)
		goto unlock;
	word = *view.word;
	length = *view.length;
	if (!mc16_swap_decode(word, length, &fields)) {
		ret = -EIO;
		goto unlock;
	}
	if (!fields.packed)
		goto unlock;
	swap = swp_entry(fields.type, fields.swap_offset);
	if (!swap.val) {
		ret = -EIO;
		goto unlock;
	}
	if (ops->slot_count(swap) != 1)
		goto unlock;
	ret = memcomp_slot_dup(swap);
	mutex_unlock(memcomp_lock_of(ref->token));
	if (ret)
		return ret;

	buf = mc16_defrag_buffer_alloc();
	if (!buf) {
		ret = -ENOMEM;
		goto read_put;
	}
	ret = ops->read_page(swap, buf);
	if (!ret)
		ret = mc16_defrag_copy_payload(buf + fields.byte_offset,
					       fields.length, &payload);
	kfree(buf);
	if (ret)
		goto abort;
	ret = mc16_swapoff_commit(ref, word, length, &payload,
				  GFP_NOWAIT | __GFP_NOWARN, &tail);
	if (ret != 1)
		goto abort;
	/* The temporary read pin is ordinary; the consumed owner is packed. */
	memcomp_slot_put(swap);
	mc16_defrag_finish(&tail);
	memcomp_debug_inc(&memcomp_writeback_defrag_moved);
	return 1;
abort:
	/* Commit refunds its own failed charge; prepared ownership stays here. */
	mc16_payload_abort(ref, &payload);
read_put:
	memcomp_slot_put(swap);
	return ret;
unlock:
	mutex_unlock(memcomp_lock_of(ref->token));
	return ret;
}

/*
 * The caller supplies gating. Defrag does not take W: T snapshots, backing
 * read pins and commit revalidation protect each move independently of WB.
 * Retain only the numeric cursor between passes.
 */
static inline bool mc16_defrag_scan(u64 *cursor,
				    const struct memcomp_defrag_ops *ops)
{
	unsigned int scanned = 0, moved = 0;

	if (!cursor || !ops || !ops->slot_count || !ops->read_page)
		return false;
	while (scanned < MEMCOMP_WRITEBACK_DEFRAG_SCAN &&
	       moved < MEMCOMP_WRITEBACK_DEFRAG_MOVE) {
		struct memcomp_ref ref;
		enum mc16_scan_result result;
		unsigned int used;
		int ret;

		result = mc16_scan_next(cursor, MEMCOMP_WRITEBACK_DEFRAG_SCAN -
					scanned, &ref, &used);
		scanned += used;
		if (result == MC16_SCAN_END) {
			*cursor = 0;
			return false;
		}
		if (result == MC16_SCAN_BUDGET)
			return true;
		ret = mc16_defrag_entry(&ref, ops);
		mc16_ref_put(&ref);
		if (*cursor == MC16_SCAN_END_CURSOR) {
			*cursor = 0;
			return false;
		}
		if (ret < 0)
			return false;
		moved += ret;
		cond_resched();
	}
	return true;
}
#endif /* CONFIG_SWAP */
#endif /* _MM_MEMCOMP_DEFRAG_H */
