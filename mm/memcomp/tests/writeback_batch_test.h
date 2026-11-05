/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_WRITEBACK_BATCH_TEST_H
#define _MM_MEMCOMP_TESTS_WRITEBACK_BATCH_TEST_H

#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)
#include <kunit/static_stub.h>

/*
 * Include after refs_test.h (and its storage/objcg fixtures),
 * with writeback_batch.h already visible in memcomp.c.
 * These are model-I/O tests: real zs payloads, copy/decompression, folio
 * writeback completion and slot transitions, but no device or swapcache
 * insertion.  All fake backing operations use the existing slot tally model.
 */
struct mc16_test_wbb {
	struct mc16_test_refs *refs;
	struct mc16_storage_fixture *saved_probe;
	struct mc16_payload prepared[2];
	struct mc16_payload_tail tail[2];
	struct memcomp_ref survivor[2];
	struct memcomp_ref caller[2];
	struct mc16_writeback_member staged[2];
	swp_entry_t saved_pool[MEMCOMP_WRITEBACK_BATCH];
	swp_entry_t used_swap;
	u32 token[2];
	u32 bytes[2];
	unsigned int before_claim[2];
	unsigned int after_claim[2];
	unsigned int after_caller_put[2];
	unsigned int after_detach[2];
	unsigned int alloc_calls;
	unsigned int dup_calls;
	unsigned int submits;
	unsigned int completed;
	unsigned int seed_after_take;
	unsigned int seed_after_flush;
	unsigned int probe_frees;
	unsigned long freed;
	long charged_before;
	int alloc_error;
	int dup_error;
	int flush_ret;
	int dups;
	int puts;
	int reclaims;
	bool dirty_io;
	bool reject_first;
	bool live_before_io;
	bool live_at_submit;
	bool live_after;
	bool bad_lifetime;
	bool submit_contract;
	bool batch_empty;
	bool pool_restored;
	bool probe_restored;
	u8 *src;
	u8 *encoded[2];
	u8 *images;
	u8 *dst;
};

/* Set only inside the synchronous test task's W scope, restored before exit. */
static struct mc16_test_wbb *mc16_test_wbb_current;

/* The inherited objcg fixture normally keeps its folio locked across tests. */
static void mc16_test_wbb_objcg_cleanup(void *data)
{
	struct memcomp_test_objcg_fixture *owner = data;

	/* Acquire here, release in the same call; never inherit a task's lock. */
	if (owner->folio)
		folio_lock(owner->folio);
	memcomp_test_objcg_cleanup(owner);
}

static void mc16_test_wbb_storage_cleanup(void *data)
{
	struct mc16_test_wbb *f = data;

	mc16_test_storage_cleanup(f->refs->storage);
	rcu_assign_pointer(mc16_storage_fixture, f->saved_probe);
	synchronize_rcu();
}

static void mc16_test_wbb_cleanup(void *data)
{
	struct mc16_test_wbb *f = data;
	struct mc16_test_storage *s = f->refs->storage;
	unsigned int i;

	/* The test-task transaction must already have drained its locked work. */
	KUNIT_EXPECT_PTR_NE(s->test, mc16_test_wbb_current, f);
	for (i = 0; i < ARRAY_SIZE(f->survivor); i++) {
		KUNIT_EXPECT_PTR_EQ(s->test, f->staged[i].ref.slot, NULL);
		KUNIT_EXPECT_PTR_EQ(s->test, f->caller[i].slot, NULL);
		if (s->refs[i].slot) {
			mc16_payload_finish(&s->refs[i], &f->tail[i]);
			mc16_payload_abort(&s->refs[i], &f->prepared[i]);
		}
		if (f->survivor[i].slot)
			mc16_ref_put(&f->survivor[i]);
	}
}

/* Reuse shared cleanup/reserve logic, with a source folio unlocked on return. */
static int mc16_test_wbb_fixtures(struct kunit *test, struct mc16_test_wbb *f)
{
	struct memcomp_test_objcg_fixture *owner;
	struct mc16_test_storage *s;
	int ret;

	f->refs = kunit_kzalloc(test, sizeof(*f->refs), GFP_KERNEL);
	s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	if (!f->refs || !s)
		return -ENOMEM;
	f->refs->storage = s;
	owner = memcomp_test_objcg_begin(test, 2);
	if (!owner)
		return -ENOMEM;
	folio_unlock(owner->folio);
	kunit_remove_action(test, memcomp_test_objcg_cleanup, owner);
	ret = kunit_add_action_or_reset(test, mc16_test_wbb_objcg_cleanup, owner);
	if (ret)
		return ret;
	s->owner = owner;
	s->test = test;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&s->baseline);
#endif
	f->saved_probe = rcu_access_pointer(mc16_storage_fixture);
	s->probe.task = current;
	ret = kunit_add_action_or_reset(test, mc16_test_wbb_storage_cleanup, f);
	if (ret)
		return ret;
	rcu_assign_pointer(mc16_storage_fixture, &s->probe);
	f->refs->stored_before = atomic64_read(&memcomp_stored_bytes);
	f->refs->compressed_before = atomic64_read(&memcomp_compressed_bytes);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	f->refs->pending_before = atomic64_read(&memcomp_pending_pages);
#endif
	ret = kunit_add_action_or_reset(test, mc16_test_refs_cleanup, f->refs);
	if (ret)
		return ret;
	ret = kunit_add_action_or_reset(test, mc16_test_wbb_cleanup, f);
	if (ret)
		return ret;
	folio_lock(owner->folio);
	ret = mc16_test_refs_reserve(f->refs, 2);
	folio_unlock(owner->folio);
	return ret;
}

static int mc16_test_wbb_prepare(struct mc16_test_wbb *f, unsigned int i,
				 bool raw)
{
	struct mc16_test_storage *s = f->refs->storage;
	struct memcomp_ref *ref = &s->refs[i];
	unsigned long handle = 0;
	unsigned int bytes = 0;
	void *payload;
	u8 pool = 0;
	int ret;

	if (raw)
		ret = memcomp_alloc_raw_payload(0, f->src + i * PAGE_SIZE,
						PAGE_SIZE, &bytes, &handle, false, NULL);
	else
		ret = memcomp_prepare_payload(f->src + i * PAGE_SIZE, PAGE_SIZE,
					      &bytes, &handle, &pool,
					      MEMCOMP_PAYLOAD_MUST_SUCCEED, false);
	if (ret)
		return ret;
	if (pool) {
		memcomp_zs_free(pool, handle);
		return -EINVAL;
	}
	f->prepared[i] = (struct mc16_payload) { .word = handle, .bytes = bytes };
	if (!bytes || (!raw && bytes >= PAGE_SIZE))
		return -E2BIG;
	f->bytes[i] = bytes;
	payload = zs_map_object(memcomp_pools[0], handle, ZS_MM_RO);
	if (!payload)
		return -EIO;
	memcpy(f->encoded[i], payload, bytes);
	zs_unmap_object(memcomp_pools[0], handle);
	ret = mc16_payload_charge(ref, &f->prepared[i]);
	if (ret)
		return ret;
	folio_lock(s->owner->folio);
	mutex_lock(memcomp_lock_of(ref->token));
	ret = mc16_payload_commit_locked(ref, s->owner->folio, 2, i,
					 &f->prepared[i], &f->tail[i]);
	mutex_unlock(memcomp_lock_of(ref->token));
	mc16_payload_finish(ref, &f->tail[i]);
	folio_unlock(s->owner->folio);
	if (ret)
		return ret;
	f->token[i] = ref->token;
	return mc16_get(ref->token, &f->survivor[i]) ? 0 : -ENOENT;
}

/* Seeded reservations only: an empty pool must NEVER refill from a device. */
static int mc16_test_wbb_alloc(struct folio **foliop, swp_entry_t *swapp)
{
	struct mc16_test_wbb *f = mc16_test_wbb_current;
	struct folio *folio;
	swp_entry_t swap;
	void *dst;
	int ret;

	if (!f)
		return -EINVAL;
	f->alloc_calls++;
	if (f->alloc_error)
		return f->alloc_error;
	if (!memcomp_writeback_slots_free)
		return -ENOSPC;
	folio = folio_alloc(GFP_HIGHUSER_MOVABLE | __GFP_NOWARN |
			    __GFP_NOMEMALLOC, 0);
	if (!folio)
		return -ENOMEM;
	ret = memcomp_writeback_slot_take(&swap);
	if (ret) {
		folio_put(folio);
		return ret;
	}
	ret = memcomp_slot_dup(swap);
	if (ret) {
		memcomp_writeback_slots_release(&swap, 1);
		folio_put(folio);
		return ret;
	}
	folio_lock(folio);
	__folio_set_swapbacked(folio);
	folio->swap = swap;
	dst = kmap_local_folio(folio, 0);
	memset(dst, 0xa7, PAGE_SIZE);
	kunmap_local(dst);
	f->seed_after_take = memcomp_writeback_slots_free;
	f->used_swap = swap;
	*foliop = folio;
	*swapp = swap;
	/* Locked by allocator contract; batch consumes it before any assertion. */
	return 0;
}

/* Synchronous model completion; the actual batch builds every byte first. */
static void mc16_test_wbb_submit(struct folio *folio,
				 struct writeback_control *wbc)
{
	struct mc16_test_wbb *f = mc16_test_wbb_current;
	void *src;

	if (f) {
		f->submit_contract = folio_test_locked(folio) &&
			folio_test_uptodate(folio) && wbc->sync_mode == WB_SYNC_NONE &&
			wbc->swap_plug == &memcomp_writeback_plugged;
		f->live_at_submit = f->refs->storage->payload.live;
		if (f->submits < 2) {
			src = kmap_local_folio(folio, 0);
			memcpy(f->images + f->submits * PAGE_SIZE, src, PAGE_SIZE);
			kunmap_local(src);
		}
		f->submits++;
		/* Stale attempt identity; leave the real live payload untouched. */
		if (f->reject_first)
			mc16_writeback_members[0].snapshot.word ^= 1;
	}
	folio_start_writeback(folio);
	if (f && f->dirty_io)
		folio_mark_dirty(folio);
	folio_unlock(folio);
	folio_end_writeback(folio);
	if (f)
		f->completed++;
}

static int mc16_test_wbb_dup(swp_entry_t swap)
{
	struct mc16_test_wbb *f = mc16_test_wbb_current;

	if (!f)
		return -EINVAL;
	f->dup_calls++;
	if (f->dup_error)
		return f->dup_error;
	return memcomp_slot_dup(swap);
}

static struct mc16_test_wbb *mc16_test_wbb_begin(struct kunit *test, bool raw)
{
	struct mc16_test_wbb *f;
	unsigned int i;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f || mc16_test_wbb_fixtures(test, f))
		return NULL;
	f->src = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	f->images = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	f->dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	for (i = 0; i < ARRAY_SIZE(f->encoded); i++)
		f->encoded[i] = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->src || !f->images || !f->dst || !f->encoded[0] || !f->encoded[1])
		return NULL;
	for (i = 0; i < PAGE_SIZE; i++) {
		f->src[i] = raw ? i * 37 + (i >> 5) : i % 53;
		f->src[PAGE_SIZE + i] = (i * 3) % 71;
	}
	for (i = 0; i < ARRAY_SIZE(f->survivor); i++) {
		if (mc16_test_wbb_prepare(f, i, raw && !i))
			return NULL;
	}
	kunit_activate_static_stub(test, mc16_writeback_folio_alloc,
				   mc16_test_wbb_alloc);
	kunit_activate_static_stub(test, mc16_writeback_submit,
				   mc16_test_wbb_submit);
	kunit_activate_static_stub(test, mc16_writeback_backing_dup,
				   mc16_test_wbb_dup);
	return f;
}

/* No assertions here: every return restores W, T, folios, flags and globals. */
static int mc16_test_wbb_run(struct mc16_test_wbb *f, unsigned int nr,
			     bool detach)
{
	struct mc16_test_storage *s = f->refs->storage;
	struct memcomp_kunit_async_probe *saved_async;
	struct mc16_test_wbb *saved_current;
	unsigned int nofs, noreclaim, saved_free, i;
	struct mc16_view view;
	int ret = 0, dups, puts, reclaims;

	nofs = memalloc_nofs_save();
	noreclaim = memalloc_noreclaim_save();
	mutex_lock(&memcomp_writeback_lock);
	if (mc16_writeback_batch_used || mc16_writeback_members_used ||
	    memcomp_writeback_plugged || mc16_test_wbb_current || !nr || nr > 2) {
		ret = -EBUSY;
		goto unlock;
	}
	memcpy(f->saved_pool, memcomp_writeback_slot_pool, sizeof(f->saved_pool));
	saved_free = memcomp_writeback_slots_free;
	for (i = 0; i < 3; i++)
		memcomp_writeback_slot_pool[i] = swp_entry(MAX_SWAPFILES - 1, 140 + i);
	memcomp_writeback_slots_free = 3;
	saved_current = mc16_test_wbb_current;
	mc16_test_wbb_current = f;
	saved_async = rcu_access_pointer(memcomp_kunit_async_probe);
	s->payload.task = current;
	s->payload.pool = memcomp_pools[0];
	mutex_lock(memcomp_lock_of(f->token[0]));
	if (!mc16_view_locked(&f->survivor[0], &view)) {
		ret = -EIO;
	} else {
		s->payload.handle = *view.word;
		s->payload.live = true;
	}
	mutex_unlock(memcomp_lock_of(f->token[0]));
	rcu_assign_pointer(memcomp_kunit_async_probe, &s->payload);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	reclaims = atomic_read(&memcomp_kunit_slot_reclaims);
	f->charged_before = atomic_long_read(&s->owner->model.charged);
	for (i = 0; !ret && i < nr; i++) {
		if (!mc16_get(f->token[i], &f->caller[i])) {
			ret = -ENOENT;
			break;
		}
		f->before_claim[i] = refcount_read(&f->caller[i].slot->refs);
		mutex_lock(memcomp_lock_of(f->caller[i].token));
		ret = mc16_writeback_stage_locked(&f->caller[i], &f->staged[i]);
		mutex_unlock(memcomp_lock_of(f->caller[i].token));
		f->after_claim[i] = refcount_read(&f->caller[i].slot->refs);
		mc16_ref_put(&f->caller[i]);
		if (!ret)
			f->after_caller_put[i] = refcount_read(&f->staged[i].ref.slot->refs);
	}
	if (!ret && detach) {
		/* Drop siblings before head, then leave only each batch guard. */
		folio_lock(s->owner->folio);
		for (i = nr; i; ) {
			struct memcomp_ref lookup;
			bool dropped;

			i--;
			if (!mc16_get(f->token[i], &lookup)) {
				ret = -ENOENT;
				break;
			}
			dropped = mc16_owner_drop_folio_held(&lookup,
							     s->owner->folio, 2, i);
			if (!dropped) {
				ret = -ESTALE;
				break;
			}
			s->refs[i] = (struct memcomp_ref) { };
		}
		folio_unlock(s->owner->folio);
		if (!ret) {
			for (i = 0; i < nr; i++) {
				mc16_ref_put(&f->survivor[i]);
				f->after_detach[i] = refcount_read(&f->staged[i].ref.slot->refs);
			}
		}
	}
	f->live_before_io = s->payload.live;
	for (i = 0; !ret && i < nr; i++)
		ret = mc16_writeback_stage(&f->staged[i]);
	/* Include temporaries not consumed when an earlier member failed. */
	for (i = 0; i < nr; i++) {
		if (f->staged[i].ref.slot)
			mc16_writeback_stage_abort(&f->staged[i]);
		if (f->caller[i].slot)
			mc16_ref_put(&f->caller[i]);
	}
	f->flush_ret = mc16_writeback_batch_flush(&f->freed);
	if (!ret)
		ret = f->flush_ret;
	f->seed_after_flush = memcomp_writeback_slots_free;
	f->dups = atomic_read(&memcomp_kunit_slot_dups) - dups;
	f->puts = atomic_read(&memcomp_kunit_slot_puts) - puts;
	f->reclaims = atomic_read(&memcomp_kunit_slot_reclaims) - reclaims;
	f->probe_frees = s->payload.frees;
	f->live_after = s->payload.live;
	f->bad_lifetime = s->payload.bad_lifetime;
	f->batch_empty = !mc16_writeback_batch_used &&
		!mc16_writeback_members_used && !mc16_writeback_open_first &&
		!mc16_writeback_open_bytes && !memcomp_writeback_plugged;
	memcpy(memcomp_writeback_slot_pool, f->saved_pool, sizeof(f->saved_pool));
	memcomp_writeback_slots_free = saved_free;
	f->pool_restored = !memcmp(memcomp_writeback_slot_pool, f->saved_pool,
				   sizeof(f->saved_pool)) &&
		memcomp_writeback_slots_free == saved_free;
	rcu_assign_pointer(memcomp_kunit_async_probe, saved_async);
	f->probe_restored = rcu_access_pointer(memcomp_kunit_async_probe) == saved_async;
	mc16_test_wbb_current = saved_current;
unlock:
	mutex_unlock(&memcomp_writeback_lock);
	memalloc_noreclaim_restore(noreclaim);
	memalloc_nofs_restore(nofs);
	synchronize_rcu();
	return ret;
}

static void mc16_test_wbb_drained(struct kunit *test, struct mc16_test_wbb *f,
				  unsigned int nr)
{
	unsigned int i;

	KUNIT_EXPECT_TRUE(test, f->batch_empty);
	KUNIT_EXPECT_TRUE(test, f->pool_restored);
	KUNIT_EXPECT_TRUE(test, f->probe_restored);
	KUNIT_EXPECT_EQ(test, f->seed_after_flush, 0U);
	KUNIT_EXPECT_FALSE(test, f->bad_lifetime);
	KUNIT_EXPECT_TRUE(test, f->live_before_io);
	KUNIT_EXPECT_EQ(test, f->submits, f->completed);
	for (i = 0; i < nr; i++) {
		KUNIT_EXPECT_EQ(test, f->after_claim[i], f->before_claim[i] + 1);
		KUNIT_EXPECT_EQ(test, f->after_caller_put[i], f->before_claim[i]);
		KUNIT_EXPECT_PTR_EQ(test, f->staged[i].ref.slot, NULL);
		KUNIT_EXPECT_PTR_EQ(test, f->caller[i].slot, NULL);
	}
	if (f->submits) {
		KUNIT_EXPECT_TRUE(test, f->submit_contract);
		KUNIT_EXPECT_TRUE(test, f->live_at_submit);
		KUNIT_EXPECT_EQ(test, f->seed_after_take, 2U);
	}
	KUNIT_EXPECT_FALSE(test, folio_test_locked(f->refs->storage->owner->folio));
}

static void mc16_test_wbb_state(struct kunit *test, struct mc16_test_wbb *f,
				unsigned int i, unsigned int expected,
				bool packed, unsigned int offset)
{
	struct memcomp_ref *ref = &f->survivor[i];
	struct mc16_swap_fields swap = { };
	struct mc16_view view;
	unsigned int state = MC16_EMPTY;
	bool valid, decoded = false;

	KUNIT_ASSERT_NOT_NULL(test, ref->slot);
	mutex_lock(memcomp_lock_of(ref->token));
	valid = mc16_view_locked(ref, &view);
	if (valid) {
		state = mc16_flags_state(*view.flags);
		if (state == MC16_SWAPPED)
			decoded = mc16_swap_decode(*view.word, *view.length, &swap);
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_ASSERT_TRUE(test, valid);
	KUNIT_EXPECT_EQ(test, state, expected);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 2U);
	if (expected == MC16_SWAPPED) {
		KUNIT_EXPECT_TRUE(test, decoded);
		KUNIT_EXPECT_EQ(test, swap.type, (u8)swp_type(f->used_swap));
		KUNIT_EXPECT_EQ(test, swap.swap_offset, (u64)swp_offset(f->used_swap));
		KUNIT_EXPECT_EQ(test, swap.byte_offset, offset);
		KUNIT_EXPECT_EQ(test, swap.length, f->bytes[i]);
		KUNIT_EXPECT_EQ(test, swap.packed, packed);
	} else {
		KUNIT_ASSERT_EQ(test, mc16_load_page(ref, f->dst), 0);
		KUNIT_EXPECT_EQ(test, memcmp(f->src + i * PAGE_SIZE, f->dst, PAGE_SIZE), 0);
	}
}

static void mc16_test_wbb_single(struct kunit *test, bool raw)
{
	struct mc16_test_wbb *f = mc16_test_wbb_begin(test, raw);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ret = mc16_test_wbb_run(f, 1, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	mc16_test_wbb_drained(test, f, 1);
	KUNIT_EXPECT_EQ(test, f->submits, 1U);
	KUNIT_EXPECT_EQ(test, f->freed, 1UL);
	KUNIT_EXPECT_EQ(test, memcmp(f->images, f->src, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 1U);
	KUNIT_EXPECT_FALSE(test, f->live_after);
	KUNIT_EXPECT_EQ(test, f->dups, 1);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before - f->bytes[0]);
	mc16_test_wbb_state(test, f, 0, MC16_SWAPPED, false, 0);
}

static void memcomp_test_wbb_raw(struct kunit *test)
{
	mc16_test_wbb_single(test, true);
}

static void memcomp_test_wbb_unpacked_compressed(struct kunit *test)
{
	mc16_test_wbb_single(test, false);
}

static void memcomp_test_wbb_packed(struct kunit *test)
{
	struct mc16_test_wbb *f = mc16_test_wbb_begin(test, false);
	unsigned int off, i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_LT(test, f->bytes[0] + f->bytes[1], (u32)PAGE_SIZE);
	ret = mc16_test_wbb_run(f, 2, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	mc16_test_wbb_drained(test, f, 2);
	KUNIT_EXPECT_EQ(test, f->submits, 1U);
	KUNIT_EXPECT_EQ(test, f->freed, 2UL);
	KUNIT_EXPECT_EQ(test, f->dups, 2);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 1U);
	KUNIT_EXPECT_FALSE(test, f->live_after);
	off = 0;
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_EQ(test, memcmp(f->images + off, f->encoded[i], f->bytes[i]), 0);
		KUNIT_ASSERT_EQ(test,
				memcomp_decode_slot(f->images, true, off, f->bytes[i], f->dst), 0);
		KUNIT_EXPECT_EQ(test, memcmp(f->dst, f->src + i * PAGE_SIZE, PAGE_SIZE), 0);
		mc16_test_wbb_state(test, f, i, MC16_SWAPPED, true, off);
		off += f->bytes[i];
	}
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(f->images + off, 0, PAGE_SIZE - off), NULL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before - f->bytes[0] - f->bytes[1]);
}

static void memcomp_test_wbb_owner_detach(struct kunit *test)
{
	struct mc16_test_wbb *f = mc16_test_wbb_begin(test, false);
	struct memcomp_ref gone;
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ret = mc16_test_wbb_run(f, 2, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	mc16_test_wbb_drained(test, f, 2);
	KUNIT_EXPECT_EQ(test, f->freed, 2UL);
	KUNIT_EXPECT_EQ(test, f->reclaims, 2);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 1U);
	KUNIT_EXPECT_FALSE(test, f->live_after);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_EQ(test, f->after_detach[i], 1U);
		if (mc16_get(f->token[i], &gone)) {
			mc16_ref_put(&gone);
			KUNIT_FAIL(test, "batch final put did not retire member %u", i);
		}
	}
	/* All batch guards are gone; pressure may now reclaim metadata. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->refs->storage->owner->folio), 1);
}

static void memcomp_test_wbb_alloc_failure(struct kunit *test)
{
	struct mc16_test_wbb *f = mc16_test_wbb_begin(test, false);
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->alloc_error = -ENOMEM;
	ret = mc16_test_wbb_run(f, 2, false);
	KUNIT_ASSERT_EQ(test, ret, -ENOMEM);
	mc16_test_wbb_drained(test, f, 2);
	KUNIT_EXPECT_EQ(test, f->alloc_calls, 1U);
	KUNIT_EXPECT_EQ(test, f->submits, 0U);
	KUNIT_EXPECT_EQ(test, f->freed, 0UL);
	KUNIT_EXPECT_EQ(test, f->dups, 0);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 0U);
	KUNIT_EXPECT_TRUE(test, f->live_after);
	for (i = 0; i < 2; i++)
		mc16_test_wbb_state(test, f, i, MC16_STORED, false, 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before);
}

static void memcomp_test_wbb_dup_failure(struct kunit *test)
{
	struct mc16_test_wbb *f = mc16_test_wbb_begin(test, false);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->dup_error = -ENOMEM;
	ret = mc16_test_wbb_run(f, 2, false);
	KUNIT_ASSERT_EQ(test, ret, -ENOMEM);
	mc16_test_wbb_drained(test, f, 2);
	KUNIT_EXPECT_EQ(test, f->freed, 1UL);
	KUNIT_EXPECT_EQ(test, f->dup_calls, 1U);
	KUNIT_EXPECT_EQ(test, f->dups, 1);
	/* A failed dup owns nothing: do not refund a reference it never got. */
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 1U);
	KUNIT_EXPECT_FALSE(test, f->live_after);
	mc16_test_wbb_state(test, f, 0, MC16_SWAPPED, true, 0);
	mc16_test_wbb_state(test, f, 1, MC16_STORED, false, 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before - f->bytes[0]);
}

static void memcomp_test_wbb_dirty_io(struct kunit *test)
{
	struct mc16_test_wbb *f = mc16_test_wbb_begin(test, false);
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->dirty_io = true;
	ret = mc16_test_wbb_run(f, 2, false);
	KUNIT_ASSERT_EQ(test, ret, -EIO);
	mc16_test_wbb_drained(test, f, 2);
	KUNIT_EXPECT_EQ(test, f->submits, 1U);
	KUNIT_EXPECT_EQ(test, f->freed, 0UL);
	KUNIT_EXPECT_EQ(test, f->dups, 1);
	KUNIT_EXPECT_EQ(test, f->puts, 1);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 0U);
	KUNIT_EXPECT_TRUE(test, f->live_after);
	for (i = 0; i < 2; i++)
		mc16_test_wbb_state(test, f, i, MC16_STORED, false, 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before);
}

static void memcomp_test_wbb_settle_reject(struct kunit *test)
{
	struct mc16_test_wbb *f = mc16_test_wbb_begin(test, false);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->reject_first = true;
	ret = mc16_test_wbb_run(f, 2, false);
	KUNIT_ASSERT_EQ(test, ret, -ESTALE);
	mc16_test_wbb_drained(test, f, 2);
	KUNIT_EXPECT_EQ(test, f->freed, 1UL);
	KUNIT_EXPECT_EQ(test, f->dups, 2);
	KUNIT_EXPECT_EQ(test, f->puts, 1);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 0U);
	KUNIT_EXPECT_TRUE(test, f->live_after);
	mc16_test_wbb_state(test, f, 0, MC16_STORED, false, 0);
	mc16_test_wbb_state(test, f, 1, MC16_SWAPPED, true, f->bytes[0]);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before - f->bytes[1]);
}
#endif /* CONFIG_MEMCG && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_WRITEBACK_BATCH_TEST_H */
