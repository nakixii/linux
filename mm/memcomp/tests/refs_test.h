/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_REFS_TEST_H
#define _MM_MEMCOMP_TESTS_REFS_TEST_H
#ifdef CONFIG_MEMCG
struct mc16_test_refs {
	struct mc16_test_storage *storage;
	bool published[MC16_TEST_REFS];
	s64 stored_before;
	s64 compressed_before;
	s64 pending_before;
};

static void mc16_test_refs_cleanup(void *data)
{
	struct mc16_test_refs *f = data;
	struct mc16_test_storage *storage = f->storage;
	unsigned int i;

	for (i = 0; i < storage->nr; i++) {
		if (f->published[i] && storage->refs[i].slot)
			mc16_ref_put(&storage->refs[i]);
	}
	KUNIT_EXPECT_EQ(storage->test, atomic64_read(&memcomp_stored_bytes),
			f->stored_before);
	KUNIT_EXPECT_EQ(storage->test, atomic64_read(&memcomp_compressed_bytes),
			f->compressed_before);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(storage->test, atomic64_read(&memcomp_pending_pages),
			f->pending_before);
#endif
}

static struct mc16_test_refs *mc16_test_refs_begin(struct kunit *test)
{
	struct mc16_test_refs *f;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->storage = mc16_test_storage_begin(test);
	if (!f->storage)
		return NULL;
	f->stored_before = atomic64_read(&memcomp_stored_bytes);
	f->compressed_before = atomic64_read(&memcomp_compressed_bytes);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	f->pending_before = atomic64_read(&memcomp_pending_pages);
#endif
	ret = kunit_add_action_or_reset(test, mc16_test_refs_cleanup, f);
	return ret ? NULL : f;
}

static int mc16_test_refs_reserve(struct mc16_test_refs *f, unsigned int nr)
{
	unsigned int start = f->storage->nr, i;
	int ret;

	ret = mc16_test_reserve(f->storage, nr);
	if (ret)
		return ret;
	for (i = start; i < start + nr; i++) {
		mc16_account_reservation(&f->storage->refs[i]);
		f->published[i] = true;
	}
	return 0;
}

/* Publish a real payload while the slot remains physically record-backed. */
static int mc16_test_refs_payload(struct mc16_test_refs *f, unsigned int i,
				  unsigned int state)
{
	struct memcomp_ref *ref = &f->storage->refs[i];
	struct mc16_view view;
	unsigned long handle = 0;
	u32 bytes = state == MC16_STORED ? PAGE_SIZE : 0;
	int ret;

	if (bytes) {
		ret = memcomp_zs_malloc(memcomp_pools[0], bytes, &handle, false);
		if (ret)
			return ret;
		ret = memcomp_objcg_charge(&f->storage->owner->model.objcg,
					   GFP_KERNEL, bytes);
		if (ret) {
			memcomp_zs_free(0, handle);
			return ret;
		}
	}
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	mc16_account_state(ref, false);
	folio_put(view.member->owner_folio);
	*view.flags = (*view.flags & MC16_ORDER_MASK) | state | MC16_F_OWNER;
	*view.length = bytes ? bytes - 1 : 0;
	*view.word = bytes ? handle : 0x9a9a9a9a9a9a9a9aULL;
	view.member->payload_charge = bytes;
	memcomp_debug_dec(&memcomp_pending_pages);
	memcomp_compressed_bytes_add(bytes);
	mc16_account_state(ref, true);
	mc16_fifo_transition(ref, MC16_PENDING, state);
	mutex_unlock(memcomp_lock_of(ref->token));
	return 0;
}

static void memcomp_test_put_pair(struct kunit *test)
{
	struct mc16_test_refs *f;
	struct memcomp_ref guard;
	spinlock_t ptl; /* Models a paired lookup and persistent PTE put. */
	unsigned long irqflags;
	u32 token;

	f = mc16_test_refs_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 1), 0);
	token = f->storage->refs[0].token;
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &guard));
	f->storage->refs[0] = (struct memcomp_ref) { };
	spin_lock_init(&ptl);
	spin_lock_irqsave(&ptl, irqflags);
	mc16_ref_put_count(&guard, 2);
	spin_unlock_irqrestore(&ptl, irqflags);
	KUNIT_EXPECT_PTR_EQ(test, guard.slot, NULL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->storage->owner->folio), 1);
	KUNIT_EXPECT_FALSE(test, mc16_get(token, &guard));
}

static void memcomp_test_fifo_eligibility(struct kunit *test)
{
	struct mc16_test_refs *f;
	struct mc16_test_storage *storage;
	struct memcomp_lru *fifo;
	struct memcomp_ref guard;
	unsigned int budget;
	long before;

	f = mc16_test_refs_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 2), 0);
	fifo = mc16_fifo_of(&storage->refs[0]);
	before = atomic_long_read(&fifo->nr_entries);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 0, MC16_STORED), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 1, MC16_SAMEFILL), 0);
	KUNIT_EXPECT_EQ(test, storage->refs[0].cluster->eligible_slots, (u16)1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo->nr_entries), before + 1);
	budget = 2;
	KUNIT_ASSERT_TRUE(test, mc16_fifo_select(fifo, &budget, &guard));
	KUNIT_EXPECT_EQ(test, guard.token, storage->refs[0].token);
	mc16_ref_put(&guard);
	mc16_ref_put(&storage->refs[0]);
	KUNIT_EXPECT_EQ(test, storage->refs[1].cluster->eligible_slots, (u16)0);
	KUNIT_EXPECT_TRUE(test, list_empty(&storage->refs[1].cluster->fifo));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo->nr_entries), before);
	/* SAMEFILL may still live but can no longer keep a cluster on FIFO. */
	budget = 10;
	KUNIT_EXPECT_FALSE(test, mc16_fifo_select(fifo, &budget, &guard));
	KUNIT_EXPECT_EQ(test, budget, 10U);
}

static void memcomp_test_fifo_scan_budget(struct kunit *test)
{
	struct mc16_test_refs *f;
	struct mc16_test_storage *storage;
	struct memcomp_ref guard;
	struct mc16_view view;
	struct memcomp_lru *fifo;
	unsigned int i, budget = 65, state, found = 0;

	f = mc16_test_refs_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->storage;
	for (i = 0; i < 65; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 64, MC16_STORED), 0);
	fifo = mc16_fifo_of(&storage->refs[64]);
	while (mc16_fifo_select(fifo, &budget, &guard)) {
		mutex_lock(memcomp_lock_of(guard.token));
		mc16_view_locked(&guard, &view);
		state = mc16_flags_state(*view.flags);
		mutex_unlock(memcomp_lock_of(guard.token));
		if (state == MC16_STORED)
			found++;
		mc16_ref_put(&guard);
	}
	KUNIT_EXPECT_EQ(test, found, 1U);
	KUNIT_EXPECT_EQ(test, budget, 0U);
}

static void memcomp_test_writeback_last_guard(struct kunit *test)
{
	struct mc16_test_refs *f;
	struct mc16_test_storage *storage;
	struct memcomp_ref writer;
	struct mc16_view view;
	struct memcomp_lru *fifo;
	unsigned int budget = 1;
	bool hot;

	f = mc16_test_refs_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 0, MC16_STORED), 0);
	fifo = mc16_fifo_of(&storage->refs[0]);
	KUNIT_ASSERT_TRUE(test, mc16_fifo_select(fifo, &budget, &writer));
	mutex_lock(memcomp_lock_of(writer.token));
	mc16_view_locked(&writer, &view);
	*view.flags |= MC16_F_REFERENCED;
	hot = mc16_fifo_second_chance_locked(&writer);
	KUNIT_EXPECT_TRUE(test, hot);
	KUNIT_EXPECT_FALSE(test, mc16_fifo_second_chance_locked(&writer));
	mc16_fifo_transition(&writer, MC16_STORED, MC16_WRITEBACK);
	*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_WRITEBACK;
	mutex_unlock(memcomp_lock_of(writer.token));
	/* PTE/owner release cannot destroy a payload still held by the writer. */
	mc16_ref_put(&storage->refs[0]);
	KUNIT_EXPECT_EQ(test, refcount_read(&writer.slot->refs), 1U);
	KUNIT_EXPECT_TRUE(test, list_empty(&fifo->entries));
	mc16_ref_put(&writer);
	KUNIT_EXPECT_PTR_EQ(test, writer.slot, NULL);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_REFS_TEST_H */
