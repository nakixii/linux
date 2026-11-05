/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_WRITEBACK_BATCH_H
#define _MM_MEMCOMP_WRITEBACK_BATCH_H

#ifdef CONFIG_SWAP
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
#include <kunit/static_stub.h>
#endif

/*
 * Include in memcomp.c after the old writeback batch implementation.  Uses
 * the existing W, backing slot pool, swapcache folio helpers and I/O plug.
 * No production dispatch is changed by including this header.
 *
 * The caller holds memcomp_writeback_lock once for the whole pass, within
 * its existing nofs/noreclaim scope.  Never mix old and mc16 live batches:
 * both borrow the shared slot pool and plug.  Flush before dropping W,
 * including every error/early-exit path and before a swapoff walk.
 *
 * stage_locked() borrows the caller's existing T acquisition to claim and
 * clone exactly one staging guard.  Release T, then stage() consumes that
 * guard on every outcome.  The caller still puts its own lookup guard.
 * stage_abort() is the alternative if the caller cannot proceed to stage().
 * No I/O, allocation, numeric lookup or new lock acquisition occurs in
 * stage_locked().  The batch guard survives OWNER removal/normalization;
 * no record/member address is saved here.  The successful pass retains the
 * existing T3/L2/refs4/W1 budget; the selector's and validation's T/L scopes
 * remain the caller's, and settle uses one T with no eligibility L.
 */
struct mc16_writeback_member {
	struct memcomp_ref ref;
	struct mc16_writeback snapshot;
	u32 length;
	u16 offset;
};

struct mc16_writeback_slot {
	struct folio *folio;
	swp_entry_t swap;
	unsigned int first;
	u8 nr;
	bool packed;
};

static struct mc16_writeback_slot
	mc16_writeback_batch[MEMCOMP_WRITEBACK_BATCH];
static struct mc16_writeback_member
	mc16_writeback_members[MEMCOMP_WRITEBACK_BATCH *
			       MEMCOMP_WRITEBACK_PACK_MAX];
static unsigned int mc16_writeback_batch_used;
static unsigned int mc16_writeback_members_used;
static unsigned int mc16_writeback_open_first;
static unsigned int mc16_writeback_open_bytes;

/* Test seams preserve the production allocator, submit and backing APIs. */
static int mc16_writeback_folio_alloc(struct folio **folio, swp_entry_t *swap)
{
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	KUNIT_STATIC_STUB_REDIRECT(mc16_writeback_folio_alloc, folio, swap);
#endif
	return memcomp_swap_folio_alloc(folio, swap);
}

static void mc16_writeback_submit(struct folio *folio,
				  struct writeback_control *wbc)
{
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	KUNIT_STATIC_STUB_REDIRECT(mc16_writeback_submit, folio, wbc);
#endif
	__swap_writepage(folio, wbc);
}

static int mc16_writeback_backing_dup(swp_entry_t swap)
{
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	KUNIT_STATIC_STUB_REDIRECT(mc16_writeback_backing_dup, swap);
#endif
	return memcomp_slot_dup(swap);
}

static void mc16_writeback_member_put(struct mc16_writeback_member *m)
{
	if (m->ref.slot)
		mc16_ref_put(&m->ref);
	*m = (struct mc16_writeback_member) { };
}

/* Outside T. Only the staging guard is consumed; never the caller's guard. */
static void mc16_writeback_stage_abort(struct mc16_writeback_member *m)
{
	lockdep_assert_held(&memcomp_writeback_lock);
	if (!m->ref.slot)
		return;
	mutex_lock(memcomp_lock_of(m->ref.token));
	mc16_writeback_revert_locked(&m->ref);
	mutex_unlock(memcomp_lock_of(m->ref.token));
	mc16_writeback_member_put(m);
}

static inline bool mc16_writeback_batch_full(void)
{
	return mc16_writeback_batch_used + 1 >= MEMCOMP_WRITEBACK_BATCH ||
	       mc16_writeback_members_used >= ARRAY_SIZE(mc16_writeback_members);
}

/*
 * staged is an empty caller-owned temporary, not another lookup.  On error
 * it owns nothing and ref/state are unchanged.  A full batch must be flushed
 * outside T before retrying; -ENOBUFS is not backing-device -ENOSPC.
 */
static inline int
mc16_writeback_stage_locked(struct memcomp_ref *ref,
			    struct mc16_writeback_member *staged)
{
	struct mc16_writeback snapshot;
	u32 length = 0;

	lockdep_assert_held(&memcomp_writeback_lock);
	if (!ref || !ref->slot || !staged || staged->ref.slot)
		return -EINVAL;
	lockdep_assert_held(memcomp_lock_of(ref->token));
	if (mc16_writeback_batch_full())
		return -ENOBUFS;
	if (!mc16_writeback_claim_locked(ref, &snapshot))
		return -EAGAIN;
	/* Claim already validated this encoding in its effective view. */
	mc16_length_decode(snapshot.length, &length);
	refcount_inc(&ref->slot->refs);
	*staged = (struct mc16_writeback_member) {
		.ref = *ref, .snapshot = snapshot, .length = length,
	};
	return 0;
}

/*
 * One owned backing reference enters; success transfers it to the slot.
 * Resolve the effective view under T, then finish before putting this guard.
 * Rejections, including an unencodable backing, revert in this same T scope.
 */
static int mc16_writeback_convert(struct mc16_writeback_member *m,
				  swp_entry_t swap, bool packed)
{
	struct mc16_swap_fields fields = {
		.swap_offset = swp_offset(swap),
		.byte_offset = packed ? m->offset : 0,
		.length = m->length,
		.type = swp_type(swap),
		.packed = packed,
	};
	struct mc16_writeback_tail tail = { };
	int ret;

	mutex_lock(memcomp_lock_of(m->ref.token));
	ret = mc16_writeback_settle_locked(&m->ref, &m->snapshot, &fields,
					   &tail);
	if (ret)
		mc16_writeback_revert_locked(&m->ref);
	mutex_unlock(memcomp_lock_of(m->ref.token));
	mc16_writeback_finish(&m->ref, &tail);
	return ret;
}

/* The swapcache folio still pins the backing while duplicates are taken. */
static int mc16_writeback_resolve(struct mc16_writeback_slot *s,
				  unsigned long *freedp)
{
	bool base = true;
	unsigned int i;
	int error = 0;

	for (i = s->first; i < s->first + s->nr; i++) {
		struct mc16_writeback_member *m = &mc16_writeback_members[i];
		int ret;

		ret = base ? 0 : mc16_writeback_backing_dup(s->swap);
		if (ret) {
			/* Failed dup created no backing reference to refund. */
			mc16_writeback_stage_abort(m);
		} else {
			base = false;
			ret = mc16_writeback_convert(m, s->swap, s->packed);
			if (ret) {
				memcomp_slot_put(s->swap);
			} else {
				memcomp_debug_inc(&memcomp_writeback_payloads);
				if (freedp)
					(*freedp)++;
			}
			mc16_writeback_member_put(m);
		}
		if (ret && !error)
			error = ret;
	}
	return error;
}

/*
 * Submit one raw page or a packed slot, without T.  The snapshots remain
 * immutable while WRITEBACK, even if a producer detaches its record.  Pool 0
 * is the mc16 payload pool; snapshot.length is encoded, m->length is bytes.
 */
static int mc16_writeback_write_slot(unsigned int first, unsigned int nr)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_NONE,
		.swap_plug = &memcomp_writeback_plugged,
	};
	struct mc16_writeback_slot *s;
	bool packed = nr > 1;
	unsigned int off = 0, i;
	struct folio *folio;
	swp_entry_t swap;
	void *dst;
	int ret;

	lockdep_assert_held(&memcomp_writeback_lock);
	if (WARN_ON_ONCE(mc16_writeback_batch_used >= MEMCOMP_WRITEBACK_BATCH ||
			 !nr || nr > MEMCOMP_WRITEBACK_PACK_MAX ||
			 first > mc16_writeback_members_used ||
			 nr > mc16_writeback_members_used - first))
		return -EIO;
	ret = mc16_writeback_folio_alloc(&folio, &swap);
	if (ret)
		return ret;

	dst = kmap_local_folio(folio, 0);
	for (i = first; i < first + nr; i++) {
		struct mc16_writeback_member *m = &mc16_writeback_members[i];
		u16 encoded_length;
		u64 encoded_word;
		void *src;

		/* Refuse unsupported backing geometry before any I/O is issued. */
		if (!swap.val || !mc16_swap_encode(swp_type(swap), swp_offset(swap),
						   packed ? off : 0, m->length,
						   packed, &encoded_word,
						   &encoded_length)) {
			ret = -ERANGE;
			break;
		}
		src = zs_map_object(memcomp_pools[0], m->snapshot.word, ZS_MM_RO);
		if (!src) {
			ret = -EIO;
			break;
		}
		m->offset = packed ? off : 0;
		if (packed) {
			memcpy(dst + off, src, m->length);
			off += m->length;
		} else {
			ret = memcomp_decompress_payload(src, m->length, dst);
			if (ret)
				memcomp_debug_inc(&memcomp_writeback_decompress_error);
		}
		zs_unmap_object(memcomp_pools[0], m->snapshot.word);
		if (ret)
			break;
	}
	if (!ret && packed)
		memset(dst + off, 0, PAGE_SIZE - off);
	kunmap_local(dst);
	if (ret) {
		memcomp_drop_swapcache_folio(folio, false);
		memcomp_slot_put(swap);
		return ret;
	}

	folio_set_reclaim(folio);
	__folio_mark_uptodate(folio);
	mc16_writeback_submit(folio, &wbc);
	s = &mc16_writeback_batch[mc16_writeback_batch_used++];
	*s = (struct mc16_writeback_slot) {
		.folio = folio, .swap = swap, .first = first,
		.nr = nr, .packed = packed,
	};
	if (packed)
		memcomp_debug_inc(&memcomp_writeback_packed_slots);
	return 0;
}

static int mc16_writeback_close_open(void)
{
	unsigned int first = mc16_writeback_open_first;
	unsigned int nr = mc16_writeback_members_used - first;
	unsigned int i;
	int ret = 0;

	if (nr)
		ret = mc16_writeback_write_slot(first, nr);
	if (ret) {
		for (i = first; i < first + nr; i++)
			mc16_writeback_stage_abort(&mc16_writeback_members[i]);
		mc16_writeback_members_used = first;
	}
	mc16_writeback_open_first = mc16_writeback_members_used;
	mc16_writeback_open_bytes = 0;
	return ret;
}

/*
 * Outside T, consumes staged on success AND failure.  A failed close of the
 * preceding open page also returns this newly claimed member to STORED.
 * Caller must flush submitted slots even if this function returns an error.
 */
static inline int mc16_writeback_stage(struct mc16_writeback_member *staged)
{
	struct mc16_writeback_member *m;
	unsigned int nr;
	int ret;

	lockdep_assert_held(&memcomp_writeback_lock);
	if (!staged || !staged->ref.slot)
		return -EINVAL;
	if (WARN_ON_ONCE(mc16_writeback_batch_full())) {
		ret = -ENOBUFS;
		goto abort;
	}
	nr = mc16_writeback_members_used - mc16_writeback_open_first;
	if (!memcomp_writeback_pack_fits(nr, mc16_writeback_open_bytes,
					 staged->length)) {
		ret = mc16_writeback_close_open();
		if (ret)
			goto abort;
	}
	m = &mc16_writeback_members[mc16_writeback_members_used++];
	*m = *staged;
	*staged = (struct mc16_writeback_member) { };
	mc16_writeback_open_bytes += m->length;
	if (m->length == PAGE_SIZE)
		return mc16_writeback_close_open();
	return 0;
abort:
	mc16_writeback_stage_abort(staged);
	return ret;
}

/*
 * Unplug before the wait sweep, resolve only after I/O, then return every
 * unused reservation.  No staging guard or prefetched backing crosses W.
 * Successful slots can settle even if a sibling slot failed; ret is the
 * first close/I/O/settle error and freedp counts only committed payloads.
 */
static inline int mc16_writeback_batch_flush(unsigned long *freedp)
{
	unsigned int i;
	int ret;

	lockdep_assert_held(&memcomp_writeback_lock);
	ret = mc16_writeback_close_open();
	if (memcomp_writeback_plugged) {
		swap_write_unplug(memcomp_writeback_plugged);
		memcomp_writeback_plugged = NULL;
	}
	for (i = 0; i < mc16_writeback_batch_used; i++)
		folio_wait_writeback(mc16_writeback_batch[i].folio);
	for (i = 0; i < mc16_writeback_batch_used; i++) {
		struct mc16_writeback_slot *s = &mc16_writeback_batch[i];
		struct folio *folio = s->folio;
		int error;

		folio_lock(folio);
		if (folio_test_dirty(folio)) {
			unsigned int j;

			memcomp_drop_swapcache_folio(folio, true);
			memcomp_debug_inc(&memcomp_writeback_write_error);
			for (j = s->first; j < s->first + s->nr; j++)
				mc16_writeback_stage_abort(&mc16_writeback_members[j]);
			memcomp_slot_put(s->swap);
			error = -EIO;
		} else {
			error = mc16_writeback_resolve(s, freedp);
			memcomp_drop_swapcache_folio(folio, false);
		}
		if (error && !ret)
			ret = error;
		*s = (struct mc16_writeback_slot) { };
	}
	mc16_writeback_batch_used = 0;
	mc16_writeback_members_used = 0;
	mc16_writeback_open_first = 0;
	mc16_writeback_open_bytes = 0;
	memcomp_writeback_slots_return();
	return ret;
}

/*
 * Minimal KUnit seams above are folio_alloc, submit and backing_dup.  An
 * allocator stub returns a locked folio plus one owned backing reference;
 * a submit stub must unlock and complete writeback (dirty for failed I/O).
 * Real compression/zsmalloc and the existing memcomp_slot_* tally model can
 * cover raw/packed images, decoded-length boundaries, owner normalization
 * between stage and settle, allocation/dup/dirty-I/O/settle failures and
 * unused-slot refund.  Keep W and both T scopes out of fatal assertions;
 * drain staged temporaries and flush in the test task before releasing W.
 */
#endif /* CONFIG_SWAP */
#endif /* _MM_MEMCOMP_WRITEBACK_BATCH_H */
