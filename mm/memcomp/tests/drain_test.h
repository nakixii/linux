/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_DRAIN_TEST_H
#define _MM_MEMCOMP_TESTS_DRAIN_TEST_H

#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)
/*
 * Include after writeback_batch_test.h, with drain.h already visible.
 * FIFO, slot guards, zs payloads, packing and batch settlement are real.
 * Backing references and synchronous IO use the WB fixture's device model.
 * The pool-release seam is a no-op: these tests check pool exhaustion and
 * restoration, not a real device's free-list accounting.
 */
struct mc16_test_drain {
	struct mc16_test_wbb *wb;
	unsigned long scanned;
	unsigned int seed;
	unsigned int fail_alloc_at;
	unsigned int pool_before_alloc[2];
	unsigned int pool_after_alloc[2];
	unsigned int refs_before[2];
	unsigned int refs_after[2];
	unsigned int state[2];
	unsigned int cursor_after;
	unsigned int open_at_entry;
	u16 eligible;
	long fifo_count;
	int initial_error;
	int write_ret;
	int stopped;
	bool hot[2];
	bool valid[2];
	bool attempted;
	bool prestage;
	bool globals_restored;
};

static struct mc16_test_drain *mc16_test_drain_current;

static int mc16_test_drain_alloc(struct folio **folio, swp_entry_t *swap)
{
	struct mc16_test_drain *d = mc16_test_drain_current;
	unsigned int call;
	int saved, ret;

	if (!d || mc16_test_wbb_current != d->wb)
		return -EINVAL;
	call = d->wb->alloc_calls;
	if (call < ARRAY_SIZE(d->pool_before_alloc))
		d->pool_before_alloc[call] = memcomp_writeback_slots_free;
	saved = d->wb->alloc_error;
	if (d->fail_alloc_at == d->wb->alloc_calls + 1)
		d->wb->alloc_error = -ENOMEM;
	ret = mc16_test_wbb_alloc(folio, swap);
	if (call < ARRAY_SIZE(d->pool_after_alloc))
		d->pool_after_alloc[call] = memcomp_writeback_slots_free;
	d->wb->alloc_error = saved;
	return ret;
}

/* No assertions or borrowed view survives a T scope. */
static bool mc16_test_drain_hot(struct mc16_test_drain *d, bool hot)
{
	struct mc16_view view;
	unsigned int i;
	bool valid = true;

	for (i = 0; i < 2; i++) {
		struct memcomp_ref *ref = &d->wb->survivor[i];

		mutex_lock(memcomp_lock_of(ref->token));
		if (!mc16_view_locked(ref, &view))
			valid = false;
		else if (hot)
			*view.flags |= MC16_F_REFERENCED;
		else
			*view.flags &= ~MC16_F_REFERENCED;
		mutex_unlock(memcomp_lock_of(ref->token));
	}
	return valid;
}

static struct mc16_test_drain *
mc16_test_drain_begin(struct kunit *test, bool raw)
{
	struct mc16_test_drain *d;

	d = kunit_kzalloc(test, sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;
	d->wb = mc16_test_wbb_begin(test, raw);
	if (!d->wb || !mc16_test_drain_hot(d, false))
		return NULL;
	d->seed = 3;
	kunit_activate_static_stub(test, mc16_writeback_folio_alloc,
				   mc16_test_drain_alloc);
	return d;
}

/*
 * Synchronous transaction: no assertion, test action or return inherits W,
 * T, L, a folio lock, or the nofs/noreclaim scope. UINT_MAX keeps the cursor.
 * Capture drain's postconditions before a defensive flush of leaked work.
 */
static int mc16_test_drain_run(struct mc16_test_drain *d, unsigned long limit,
			       unsigned int cursor)
{
	struct mc16_test_wbb *f = d->wb;
	struct mc16_test_storage *s = f->refs->storage;
	struct mc16_cluster *cluster = f->survivor[0].cluster;
	struct memcomp_lru *fifo = mc16_fifo_of(&f->survivor[0]);
	struct memcomp_kunit_async_probe *saved_async;
	struct mc16_test_drain *saved_drain;
	struct mc16_test_wbb *saved_current;
	unsigned int nofs, noreclaim, saved_free, i, frees;
	int nid = cluster->key->nid;
	int saved_stopped, ret = 0, dups, puts, reclaims;
	unsigned long irqflags;
	struct mc16_view view;
	bool own_fifo;

	nofs = memalloc_nofs_save();
	noreclaim = memalloc_noreclaim_save();
	mutex_lock(&memcomp_writeback_lock);
	if (mc16_writeback_batch_used || mc16_writeback_members_used ||
	    mc16_writeback_open_first || mc16_writeback_open_bytes ||
	    memcomp_writeback_plugged || mc16_test_wbb_current ||
	    mc16_test_drain_current || d->seed > 3 || nid >= MAX_NUMNODES) {
		ret = -EBUSY;
		goto unlock;
	}
	spin_lock_irqsave(&fifo->lock, irqflags);
	own_fifo = cluster->eligible_slots ?
		list_is_singular(&fifo->entries) &&
		list_first_entry(&fifo->entries, struct mc16_cluster, fifo) == cluster :
		list_empty(&fifo->entries);
	if (own_fifo && cursor != UINT_MAX)
		cluster->wb_cursor = cursor;
	spin_unlock_irqrestore(&fifo->lock, irqflags);
	if (!own_fifo) {
		ret = -EBUSY;
		goto unlock;
	}

	memcpy(f->saved_pool, memcomp_writeback_slot_pool, sizeof(f->saved_pool));
	saved_free = memcomp_writeback_slots_free;
	for (i = 0; i < d->seed; i++)
		memcomp_writeback_slot_pool[i] = swp_entry(MAX_SWAPFILES - 1, 240 + i);
	memcomp_writeback_slots_free = d->seed;
	saved_stopped = atomic_read(&memcomp_writeback_stopped[nid]);
	atomic_set(&memcomp_writeback_stopped[nid], 0);
	saved_current = mc16_test_wbb_current;
	saved_drain = mc16_test_drain_current;
	mc16_test_wbb_current = f;
	mc16_test_drain_current = d;
	saved_async = rcu_access_pointer(memcomp_kunit_async_probe);
	s->payload.task = current;
	s->payload.pool = memcomp_pools[0];
	mutex_lock(memcomp_lock_of(f->token[0]));
	if (!mc16_view_locked(&f->survivor[0], &view)) {
		ret = -EIO;
	} else {
		s->payload.handle = *view.word;
		s->payload.live = mc16_flags_state(*view.flags) == MC16_STORED;
	}
	mutex_unlock(memcomp_lock_of(f->token[0]));
	rcu_assign_pointer(memcomp_kunit_async_probe, &s->payload);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	reclaims = atomic_read(&memcomp_kunit_slot_reclaims);
	frees = s->payload.frees;
	f->charged_before = atomic_long_read(&s->owner->model.charged);
	f->freed = 0;
	d->scanned = 0;
	d->attempted = false;
	d->write_ret = d->initial_error;
	for (i = 0; i < 2; i++)
		d->refs_before[i] = refcount_read(&f->survivor[i].slot->refs);
	if (!ret && d->prestage) {
		mutex_lock(memcomp_lock_of(f->token[0]));
		ret = mc16_writeback_stage_locked(&f->survivor[0], &f->staged[0]);
		mutex_unlock(memcomp_lock_of(f->token[0]));
		if (!ret)
			ret = mc16_writeback_stage(&f->staged[0]);
	}
	if (f->staged[0].ref.slot)
		mc16_writeback_stage_abort(&f->staged[0]);
	d->open_at_entry = mc16_writeback_members_used - mc16_writeback_open_first;
	f->live_before_io = s->payload.live;
	if (!ret)
		d->scanned = mc16_writeback_drain_queue(nid, fifo, limit, &f->freed,
							&d->write_ret, &d->attempted);
	f->batch_empty = !mc16_writeback_batch_used &&
		!mc16_writeback_members_used && !mc16_writeback_open_first &&
		!mc16_writeback_open_bytes && !memcomp_writeback_plugged;
	f->seed_after_flush = memcomp_writeback_slots_free;
	/* A failed assertion later must not strand global work or guards. */
	if (!f->batch_empty || memcomp_writeback_slots_free)
		mc16_writeback_batch_flush(&f->freed);
	f->dups = atomic_read(&memcomp_kunit_slot_dups) - dups;
	f->puts = atomic_read(&memcomp_kunit_slot_puts) - puts;
	f->reclaims = atomic_read(&memcomp_kunit_slot_reclaims) - reclaims;
	f->probe_frees = s->payload.frees - frees;
	f->live_after = s->payload.live;
	f->bad_lifetime = s->payload.bad_lifetime;
	for (i = 0; i < 2; i++) {
		struct memcomp_ref *ref = &f->survivor[i];

		d->refs_after[i] = refcount_read(&ref->slot->refs);
		mutex_lock(memcomp_lock_of(ref->token));
		d->valid[i] = mc16_view_locked(ref, &view);
		if (d->valid[i]) {
			d->valid[i] = mc16_flags_valid(*view.flags);
			d->state[i] = mc16_flags_state(*view.flags);
			d->hot[i] = !!(*view.flags & MC16_F_REFERENCED);
		}
		mutex_unlock(memcomp_lock_of(ref->token));
	}
	spin_lock_irqsave(&fifo->lock, irqflags);
	d->eligible = cluster->eligible_slots;
	d->fifo_count = atomic_long_read(&fifo->nr_entries);
	d->cursor_after = cluster->wb_cursor;
	spin_unlock_irqrestore(&fifo->lock, irqflags);
	d->stopped = atomic_read(&memcomp_writeback_stopped[nid]);
	atomic_set(&memcomp_writeback_stopped[nid], saved_stopped);
	memcpy(memcomp_writeback_slot_pool, f->saved_pool, sizeof(f->saved_pool));
	memcomp_writeback_slots_free = saved_free;
	f->pool_restored = !memcmp(memcomp_writeback_slot_pool, f->saved_pool,
				   sizeof(f->saved_pool)) &&
		memcomp_writeback_slots_free == saved_free;
	rcu_assign_pointer(memcomp_kunit_async_probe, saved_async);
	f->probe_restored = rcu_access_pointer(memcomp_kunit_async_probe) == saved_async;
	mc16_test_wbb_current = saved_current;
	mc16_test_drain_current = saved_drain;
	d->globals_restored = mc16_test_wbb_current == saved_current &&
		mc16_test_drain_current == saved_drain &&
		atomic_read(&memcomp_writeback_stopped[nid]) == saved_stopped;
unlock:
	mutex_unlock(&memcomp_writeback_lock);
	memalloc_noreclaim_restore(noreclaim);
	memalloc_nofs_restore(nofs);
	synchronize_rcu();
	return ret;
}

static void mc16_test_drain_done(struct kunit *test, struct mc16_test_drain *d,
				 unsigned int eligible)
{
	struct mc16_test_wbb *f = d->wb;
	unsigned int i;

	KUNIT_EXPECT_TRUE(test, f->batch_empty);
	KUNIT_EXPECT_EQ(test, f->seed_after_flush, 0U);
	KUNIT_EXPECT_TRUE(test, f->pool_restored);
	KUNIT_EXPECT_TRUE(test, f->probe_restored);
	KUNIT_EXPECT_TRUE(test, d->globals_restored);
	KUNIT_EXPECT_FALSE(test, f->bad_lifetime);
	KUNIT_EXPECT_EQ(test, d->eligible, (u16)eligible);
	KUNIT_EXPECT_EQ(test, d->fifo_count, (long)eligible);
	KUNIT_EXPECT_EQ(test, f->submits, f->completed);
	KUNIT_EXPECT_FALSE(test, folio_test_locked(f->refs->storage->owner->folio));
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_TRUE(test, d->valid[i]);
		KUNIT_EXPECT_EQ(test, d->refs_before[i], 2U);
		KUNIT_EXPECT_EQ(test, d->refs_after[i], d->refs_before[i]);
		KUNIT_EXPECT_PTR_EQ(test, f->staged[i].ref.slot, NULL);
		KUNIT_EXPECT_PTR_EQ(test, f->caller[i].slot, NULL);
	}
	if (f->submits)
		KUNIT_EXPECT_TRUE(test, f->submit_contract);
}

static void memcomp_test_drain_cold_packed(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, false);
	struct mc16_test_wbb *f;
	unsigned int off = 0, i;

	KUNIT_ASSERT_NOT_NULL(test, d);
	f = d->wb;
	KUNIT_ASSERT_LE(test, f->bytes[0] + f->bytes[1], (u32)PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, 0), 0);
	mc16_test_drain_done(test, d, 0);
	KUNIT_EXPECT_EQ(test, d->scanned, 2UL);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_TRUE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->stopped, 0);
	KUNIT_EXPECT_EQ(test, f->alloc_calls, 1U);
	KUNIT_EXPECT_EQ(test, d->pool_before_alloc[0], 3U);
	KUNIT_EXPECT_EQ(test, d->pool_after_alloc[0], 2U);
	KUNIT_EXPECT_EQ(test, f->submits, 1U);
	KUNIT_EXPECT_EQ(test, f->freed, 2UL);
	KUNIT_EXPECT_EQ(test, f->dups, 2);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, f->reclaims, 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 1U);
	KUNIT_EXPECT_FALSE(test, f->live_after);
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

static void memcomp_test_drain_cold_raw(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, true);
	struct mc16_test_wbb *f;

	KUNIT_ASSERT_NOT_NULL(test, d);
	f = d->wb;
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 1, 0), 0);
	mc16_test_drain_done(test, d, 1);
	KUNIT_EXPECT_EQ(test, d->scanned, 1UL);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_TRUE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, f->submits, 1U);
	KUNIT_EXPECT_EQ(test, f->freed, 1UL);
	KUNIT_EXPECT_EQ(test, f->dups, 1);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->images, f->src, PAGE_SIZE), 0);
	mc16_test_wbb_state(test, f, 0, MC16_SWAPPED, false, 0);
	mc16_test_wbb_state(test, f, 1, MC16_STORED, false, 0);
}

static void memcomp_test_drain_hot_second_chance(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, false);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, d);
	KUNIT_ASSERT_TRUE(test, mc16_test_drain_hot(d, true));
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, 0), 0);
	mc16_test_drain_done(test, d, 2);
	KUNIT_EXPECT_EQ(test, d->scanned, 2UL);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_FALSE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->wb->alloc_calls, 0U);
	KUNIT_EXPECT_EQ(test, d->wb->freed, 0UL);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_FALSE(test, d->hot[i]);
		KUNIT_EXPECT_EQ(test, d->state[i], (unsigned int)MC16_STORED);
	}
	/* Preserve the real FIFO cursor: only a second pass may claim them. */
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, UINT_MAX), 0);
	mc16_test_drain_done(test, d, 0);
	KUNIT_EXPECT_EQ(test, d->scanned, 2UL);
	KUNIT_EXPECT_TRUE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_EQ(test, d->wb->freed, 2UL);
	KUNIT_EXPECT_EQ(test, d->wb->submits, 1U);
}

static void memcomp_test_drain_zero_limit(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, false);

	KUNIT_ASSERT_NOT_NULL(test, d);
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 0, 0), 0);
	mc16_test_drain_done(test, d, 2);
	KUNIT_EXPECT_EQ(test, d->scanned, 0UL);
	KUNIT_EXPECT_EQ(test, d->cursor_after, 0U);
	KUNIT_EXPECT_FALSE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_EQ(test, d->wb->alloc_calls, 0U);
	KUNIT_EXPECT_EQ(test, d->wb->freed, 0UL);
}

static void memcomp_test_drain_empty_fifo(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, false);
	unsigned int allocs;

	KUNIT_ASSERT_NOT_NULL(test, d);
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, 0), 0);
	KUNIT_ASSERT_EQ(test, d->wb->freed, 2UL);
	allocs = d->wb->alloc_calls;
	/* This is the same actual FIFO after both entries became SWAPPED. */
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, ULONG_MAX, UINT_MAX), 0);
	mc16_test_drain_done(test, d, 0);
	KUNIT_EXPECT_EQ(test, d->scanned, 0UL);
	KUNIT_EXPECT_FALSE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_EQ(test, d->wb->alloc_calls, allocs);
	KUNIT_EXPECT_EQ(test, d->wb->freed, 0UL);
	KUNIT_EXPECT_EQ(test, d->wb->dups, 0);
	KUNIT_EXPECT_EQ(test, d->wb->puts, 0);
}

/* Add adjacent resident PENDING, SAMEFILL, and retired refs-zero positions. */
static int mc16_test_drain_ineligible(struct mc16_test_drain *d)
{
	struct mc16_test_storage *s = d->wb->refs->storage;
	unsigned int i;
	int ret = 0;

	folio_lock(s->owner->folio);
	for (i = 0; !ret && i < 3; i++)
		ret = mc16_test_refs_reserve(d->wb->refs, 1);
	if (!ret)
		ret = mc16_test_refs_payload(d->wb->refs, 3, MC16_SAMEFILL);
	folio_unlock(s->owner->folio);
	if (ret)
		return ret;
	mc16_ref_put(&s->refs[4]);
	return 0;
}

static void memcomp_test_drain_ineligible_budget(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, false);
	struct mc16_test_storage *s;
	struct memcomp_ref gone;

	KUNIT_ASSERT_NOT_NULL(test, d);
	KUNIT_ASSERT_EQ(test, mc16_test_drain_ineligible(d), 0);
	s = d->wb->refs->storage;
	KUNIT_ASSERT_PTR_EQ(test, s->refs[2].cluster, s->refs[0].cluster);
	KUNIT_ASSERT_PTR_EQ(test, s->refs[3].cluster, s->refs[0].cluster);
	KUNIT_ASSERT_EQ(test, s->refs[3].token, s->refs[0].token + 3);
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 3, 2), 0);
	mc16_test_drain_done(test, d, 2);
	KUNIT_EXPECT_EQ(test, d->scanned, 3UL);
	KUNIT_EXPECT_EQ(test, d->cursor_after, 5U);
	KUNIT_EXPECT_FALSE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_EQ(test, d->wb->alloc_calls, 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&s->refs[2].slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, refcount_read(&s->refs[3].slot->refs), 1U);
	if (mc16_get(s->refs[0].token + 4, &gone)) {
		mc16_ref_put(&gone);
		KUNIT_FAIL(test, "retired position became visible again");
	}
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, UINT_MAX), 0);
	mc16_test_drain_done(test, d, 0);
	KUNIT_EXPECT_EQ(test, d->scanned, 2UL);
	KUNIT_EXPECT_EQ(test, d->wb->freed, 2UL);
}

static void mc16_test_drain_failed(struct kunit *test, bool raw, bool dirty,
				   bool enospc)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, raw);
	struct mc16_test_wbb *f;
	unsigned int i;
	int error = enospc ? -ENOSPC : dirty ? -EIO : -ENOMEM;

	KUNIT_ASSERT_NOT_NULL(test, d);
	f = d->wb;
	f->dirty_io = dirty;
	if (enospc)
		d->seed = 0; /* The model allocator must not refill from a device. */
	else if (!dirty)
		f->alloc_error = -ENOMEM;
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, 0), 0);
	mc16_test_drain_done(test, d, 2);
	KUNIT_EXPECT_EQ(test, d->scanned, raw ? 1UL : 2UL);
	KUNIT_EXPECT_EQ(test, d->write_ret, error);
	KUNIT_EXPECT_TRUE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->stopped, enospc ? 1 : 0);
	KUNIT_EXPECT_EQ(test, f->alloc_calls, 1U);
	KUNIT_EXPECT_EQ(test, d->pool_before_alloc[0], d->seed);
	KUNIT_EXPECT_EQ(test, d->pool_after_alloc[0], dirty ? d->seed - 1 : d->seed);
	KUNIT_EXPECT_EQ(test, f->freed, 0UL);
	KUNIT_EXPECT_EQ(test, f->submits, dirty ? 1U : 0U);
	KUNIT_EXPECT_EQ(test, f->dups, dirty ? 1 : 0);
	KUNIT_EXPECT_EQ(test, f->puts, dirty ? 1 : 0);
	KUNIT_EXPECT_EQ(test, f->reclaims, 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 0U);
	KUNIT_EXPECT_TRUE(test, f->live_after);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before);
	for (i = 0; i < 2; i++)
		mc16_test_wbb_state(test, f, i, MC16_STORED, false, 0);
}

static void memcomp_test_drain_alloc_failure(struct kunit *test)
{
	mc16_test_drain_failed(test, false, false, false);
}

static void memcomp_test_drain_stage_alloc_failure(struct kunit *test)
{
	mc16_test_drain_failed(test, true, false, false);
}

static void memcomp_test_drain_dirty_io(struct kunit *test)
{
	mc16_test_drain_failed(test, false, true, false);
}

static void memcomp_test_drain_enospc(struct kunit *test)
{
	mc16_test_drain_failed(test, false, false, true);
}

static void memcomp_test_drain_stage_enospc(struct kunit *test)
{
	mc16_test_drain_failed(test, true, false, true);
}

static void memcomp_test_drain_zero_limit_flush(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, false);
	struct mc16_test_wbb *f;

	KUNIT_ASSERT_NOT_NULL(test, d);
	f = d->wb;
	d->prestage = true;
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 0, 0), 0);
	mc16_test_drain_done(test, d, 1);
	KUNIT_EXPECT_EQ(test, d->open_at_entry, 1U);
	KUNIT_EXPECT_EQ(test, d->scanned, 0UL);
	KUNIT_EXPECT_FALSE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->write_ret, 0);
	KUNIT_EXPECT_EQ(test, f->alloc_calls, 1U);
	KUNIT_EXPECT_EQ(test, d->pool_before_alloc[0], 3U);
	KUNIT_EXPECT_EQ(test, d->pool_after_alloc[0], 2U);
	KUNIT_EXPECT_EQ(test, f->submits, 1U);
	KUNIT_EXPECT_EQ(test, f->freed, 1UL);
	KUNIT_EXPECT_EQ(test, f->dups, 1);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->images, f->src, PAGE_SIZE), 0);
	mc16_test_wbb_state(test, f, 0, MC16_SWAPPED, false, 0);
	mc16_test_wbb_state(test, f, 1, MC16_STORED, false, 0);
}

static void memcomp_test_drain_partial_flush_failure(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, true);
	struct mc16_test_wbb *f;

	KUNIT_ASSERT_NOT_NULL(test, d);
	f = d->wb;
	d->fail_alloc_at = 2;
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, 0), 0);
	mc16_test_drain_done(test, d, 1);
	KUNIT_EXPECT_EQ(test, d->scanned, 2UL);
	KUNIT_EXPECT_EQ(test, d->write_ret, -ENOMEM);
	KUNIT_EXPECT_TRUE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->stopped, 0);
	KUNIT_EXPECT_EQ(test, f->alloc_calls, 2U);
	KUNIT_EXPECT_EQ(test, d->pool_before_alloc[0], 3U);
	KUNIT_EXPECT_EQ(test, d->pool_after_alloc[0], 2U);
	KUNIT_EXPECT_EQ(test, d->pool_before_alloc[1], 2U);
	KUNIT_EXPECT_EQ(test, d->pool_after_alloc[1], 2U);
	KUNIT_EXPECT_EQ(test, f->submits, 1U);
	KUNIT_EXPECT_EQ(test, f->freed, 1UL);
	KUNIT_EXPECT_EQ(test, f->dups, 1);
	KUNIT_EXPECT_EQ(test, f->puts, 0);
	KUNIT_EXPECT_EQ(test, f->probe_frees, 1U);
	KUNIT_EXPECT_EQ(test, memcmp(f->images, f->src, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->refs->storage->owner->model.charged),
			f->charged_before - f->bytes[0]);
	mc16_test_wbb_state(test, f, 0, MC16_SWAPPED, false, 0);
	mc16_test_wbb_state(test, f, 1, MC16_STORED, false, 0);
}

static void memcomp_test_drain_first_error(struct kunit *test)
{
	struct mc16_test_drain *d = mc16_test_drain_begin(test, false);

	KUNIT_ASSERT_NOT_NULL(test, d);
	d->initial_error = -EINTR;
	d->wb->alloc_error = -ENOSPC;
	KUNIT_ASSERT_EQ(test, mc16_test_drain_run(d, 2, 0), 0);
	mc16_test_drain_done(test, d, 2);
	KUNIT_EXPECT_EQ(test, d->scanned, 2UL);
	KUNIT_EXPECT_TRUE(test, d->attempted);
	KUNIT_EXPECT_EQ(test, d->write_ret, -EINTR);
	KUNIT_EXPECT_EQ(test, d->stopped, 1);
	KUNIT_EXPECT_EQ(test, d->wb->freed, 0UL);
	KUNIT_EXPECT_EQ(test, d->wb->alloc_calls, 1U);
}
#endif /* CONFIG_MEMCG && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_DRAIN_TEST_H */
