/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_SWAPOFF_H
#define _MM_MEMCOMP_SWAPOFF_H
#ifdef CONFIG_SWAP

struct mc16_swapoff_tail {
	swp_entry_t swap;
	u32 old_bytes;
	u32 new_bytes;
};

/*
 * The caller holds a slot guard throughout preparation, commit and the tail.
 * No record pointer escapes T. The new payload stays caller-owned on failure.
 * expected_word/length identify the complete old encoded backing snapshot.
 */
static inline int
mc16_swapoff_commit(struct memcomp_ref *ref, u64 expected_word,
		    u16 expected_length, struct mc16_payload *payload,
		    gfp_t gfp, struct mc16_swapoff_tail *tail)
{
	struct obj_cgroup *objcg = ref->cluster->key->objcg;
	struct mc16_swap_fields old;
	struct mc16_view view;
	unsigned int state;
	u32 charged = 0;
	u16 length;
	int ret = 0;

	state = payload->bytes ? MC16_STORED : MC16_SAMEFILL;
	if (payload->charge || (payload->bytes && !payload->word) ||
	    !mc16_payload_length_encode(state, payload->bytes, &length) ||
	    !mc16_swap_decode(expected_word, expected_length, &old) ||
	    !swp_entry(old.type, old.swap_offset).val)
		return -EINVAL;
	if (objcg && payload->bytes) {
		ret = memcomp_objcg_charge(objcg, gfp, payload->bytes);
		if (ret)
			return ret;
		charged = payload->bytes;
	}
	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view) || !mc16_flags_valid(*view.flags)) {
		ret = -EIO;
	} else if (mc16_flags_state(*view.flags) == MC16_SWAPPED &&
	    *view.word == expected_word && *view.length == expected_length) {
		*tail = (struct mc16_swapoff_tail) {
			.swap = swp_entry(old.type, old.swap_offset),
			.old_bytes = old.length, .new_bytes = payload->bytes,
		};
		*view.word = payload->word;
		*view.length = length;
		*view.flags = (*view.flags & ~MC16_STATE_MASK) | state;
		if (view.member)
			view.member->payload_charge = charged;
		mc16_payload_account(MC16_SWAPPED, old.length, state, payload->bytes);
		mc16_fifo_transition(ref, MC16_SWAPPED, state);
		*payload = (struct mc16_payload) { };
		ret = 1;
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	if (ret != 1)
		memcomp_objcg_uncharge(objcg, charged);
	return ret;
}

/* Match swapoff's two ordinary backing puts; swap core drains its cache. */
static inline void mc16_swapoff_finish(struct mc16_swapoff_tail *tail)
{
	if (!tail->swap.val)
		return;
	memcomp_slot_put(tail->swap);
	memcomp_debug_long_dec(&memcomp_swapped_entries);
	if (tail->new_bytes != tail->old_bytes) {
		memcomp_compressed_bytes_add(tail->new_bytes);
		memcomp_compressed_bytes_sub(tail->old_bytes);
	}
	*tail = (struct mc16_swapoff_tail) { };
}

/* A guarded restore actor; W and periodic yield belong to the outer walker. */
static inline int
mc16_swapoff_entry(struct memcomp_ref *ref, unsigned int type,
		   const struct memcomp_swapoff_ops *ops)
{
	struct mc16_swapoff_tail tail = { };
	struct mc16_payload payload = { };
	struct mc16_swap_fields fields;
	struct mc16_view view;
	unsigned long handle = 0;
	unsigned int bytes = 0;
	swp_entry_t swap;
	u64 word;
	u16 length;
	u8 pool = 0;
	void *buf;
	int ret;

	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view) || !mc16_flags_valid(*view.flags)) {
		ret = -EIO;
		goto out_unlock;
	}
	if (mc16_flags_state(*view.flags) != MC16_SWAPPED) {
		ret = 0;
		goto out_unlock;
	}
	word = *view.word;
	length = *view.length;
	if (!mc16_swap_decode(word, length, &fields)) {
		ret = -EIO;
		goto out_unlock;
	}
	swap = swp_entry(fields.type, fields.swap_offset);
	if (!swap.val) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = memcomp_slot_dup(swap);
	mutex_unlock(memcomp_lock_of(ref->token));
	if (ret)
		return ret;
	if (fields.type != type) {
		ret = 0;
		goto out_read_ref;
	}
	if (ops->interrupted(ops->ctx)) {
		ret = -EINTR;
		goto out_read_ref;
	}
	buf = kmalloc(PAGE_SIZE, GFP_NOFS | __GFP_NOWARN);
	if (!buf) {
		ret = -ENOMEM;
		goto out_read_ref;
	}
	ret = ops->read_page(swap, buf);
	if (ret) {
		kfree(buf);
		goto out_failed;
	}
	memcomp_debug_inc(&memcomp_swapoff_read_io);
	if (fields.packed) {
		void *page = kmalloc(PAGE_SIZE, GFP_NOFS | __GFP_NOWARN);

		ret = page ? memcomp_decode_slot(buf, true, fields.byte_offset,
						fields.length, page) : -ENOMEM;
		kfree(buf);
		buf = page;
		if (ret) {
			kfree(buf);
			goto out_failed;
		}
	}
	ret = ops->alloc_payload(buf, &bytes, &handle, &pool, ops->ctx);
	kfree(buf);
	if (ret)
		goto out_failed;
	payload = (struct mc16_payload) { .word = handle, .bytes = bytes };
	ret = pool ? -EINVAL :
		mc16_swapoff_commit(ref, word, length, &payload,
				    GFP_NOIO | __GFP_NOWARN, &tail);
	if (ret <= 0) {
		if (bytes)
			ops->free_payload(pool, handle, ops->ctx);
		if (ret < 0)
			goto out_failed;
		goto out_read_ref;
	}
	/* Consume the read pin separately from the old slot-owned reference. */
	memcomp_slot_put(swap);
	mc16_swapoff_finish(&tail);
	memcomp_debug_inc(&memcomp_swapoff_entries);
	return 1;
out_failed:
	memcomp_debug_inc(&memcomp_swapoff_fail);
out_read_ref:
	memcomp_slot_put(swap);
	return ret;
out_unlock:
	if (ret < 0)
		memcomp_debug_inc(&memcomp_swapoff_fail);
	mutex_unlock(memcomp_lock_of(ref->token));
	return ret;
}
#endif /* CONFIG_SWAP */
#endif /* _MM_MEMCOMP_SWAPOFF_H */
