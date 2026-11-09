/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_LOAD_H
#define _MM_MEMCOMP_LOAD_H

/* A slot guard protects lifetime; T separately protects mutable payloads. */
static inline int mc16_load_page(struct memcomp_ref *ref, void *dst)
{
	struct mc16_view view;
	unsigned long word;
	unsigned int state;
	u32 bytes;
	void *src;
	int ret;

	if (!ref->slot || !dst)
		return -EINVAL;
	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view) || !mc16_flags_valid(*view.flags)) {
		ret = -EIO;
		goto out;
	}
	if (*view.flags & MC16_F_REFRESH_PENDING) {
		ret = -EAGAIN;
		goto out;
	}
	state = mc16_flags_state(*view.flags);
	if (state == MC16_PENDING) {
		ret = -EAGAIN;
		goto out;
	}
	if (!mc16_payload_length_decode(state, *view.length, &bytes)) {
		ret = -EIO;
		goto out;
	}
	/* Reuse this T scope for the FIFO second-chance hint. */
	*view.flags |= MC16_F_REFERENCED;
	word = *view.word;
	if (state == MC16_SAMEFILL) {
		mutex_unlock(memcomp_lock_of(ref->token));
		memset_l(dst, word, PAGE_SIZE / sizeof(unsigned long));
		return 0;
	}
#ifdef CONFIG_SWAP
	if (state == MC16_SWAPPED) {
		struct mc16_swap_fields fields;
		swp_entry_t swap;

		if (!mc16_swap_decode(word, *view.length, &fields)) {
			ret = -EIO;
			goto out;
		}
		swap = swp_entry(fields.type, fields.swap_offset);
		if (!swap.val) {
			ret = -EIO;
			goto out;
		}
		/* Swapoff may consume the entry's distinct owner after T drops. */
		ret = memcomp_slot_dup(swap);
		mutex_unlock(memcomp_lock_of(ref->token));
		if (ret)
			return ret == -ENOMEM ? -ENOMEM : -EIO;
		ret = memcomp_read_swapped_page(swap, fields.packed,
						fields.byte_offset, fields.length,
						dst, GFP_HIGHUSER_MOVABLE |
						     __GFP_NOWARN);
		memcomp_slot_put(swap);
		if (!ret)
			memcomp_debug_inc(&memcomp_swapped_read_io);
		return ret;
	}
#endif
	if ((state != MC16_STORED && state != MC16_WRITEBACK) || !word) {
		ret = -EIO;
		goto out;
	}
	/* Refresh and writeback cannot free this handle while T is held. */
	src = zs_map_object(memcomp_pools[0], word, ZS_MM_RO);
	if (!src) {
		ret = -EIO;
		goto out;
	}
	ret = memcomp_decompress_payload(src, bytes, dst);
	zs_unmap_object(memcomp_pools[0], word);
out:
	mutex_unlock(memcomp_lock_of(ref->token));
	return ret;
}
#endif /* _MM_MEMCOMP_LOAD_H */
