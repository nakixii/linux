/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_CACHE_STORE_TEST_H
#define _MM_MEMCOMP_TESTS_CACHE_STORE_TEST_H
#ifdef CONFIG_MEMCG
struct mc16_test_cache_producer {
	struct mc16_test_refs *refs;
	struct memcomp_ref cache;
	struct memcomp_ref private;
};

static void mc16_test_cache_producer_cleanup(void *data)
{
	struct mc16_test_cache_producer *f = data;

	if (f->private.slot) {
		mc16_release_resources(&f->private);
		mc16_retire_slot(&f->private);
	}
	if (f->cache.slot)
		mc16_ref_put(&f->cache);
}

static struct mc16_test_cache_producer *
mc16_test_cache_producer_begin(struct kunit *test)
{
	struct mc16_test_cache_producer *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs || kunit_add_action_or_reset(test,
						  mc16_test_cache_producer_cleanup, f))
		return NULL;
	return f;
}

static void mc16_test_cache_roundtrip(struct kunit *test, bool samefill)
{
	const unsigned long pattern = 0x9876543210fedcbaUL;
	struct mc16_test_cache_producer *f = mc16_test_cache_producer_begin(test);
	struct mc16_test_storage *storage;
	void *src, *dst;
	unsigned int i;
	int folio_refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	if (samefill)
		memset_l(src, pattern, PAGE_SIZE / sizeof(unsigned long));
	else
		for (i = 0; i < PAGE_SIZE; i++)
			((u8 *)src)[i] = i % 53;
	folio_refs = folio_ref_count(storage->owner->folio);
	KUNIT_ASSERT_EQ(test, mc16_cache_store(src, storage->owner->folio,
					       &f->cache), 0);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(f->cache.slot->flags),
			(unsigned int)(samefill ? MC16_SAMEFILL : MC16_STORED));
	KUNIT_EXPECT_FALSE(test, f->cache.slot->flags &
				 (MC16_F_OWNER | MC16_F_RESIDENT));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->cache.slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, f->cache.cluster->key->kind, (u8)MC16_KEY_SHMEM);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), folio_refs);
	KUNIT_EXPECT_EQ(test, f->cache.cluster->eligible_slots,
			(u16)(samefill ? 0 : 1));
	KUNIT_ASSERT_EQ(test, mc16_load_page(&f->cache, dst), 0);
	KUNIT_EXPECT_EQ(test, memcmp(src, dst, PAGE_SIZE), 0);
	mc16_ref_put(&f->cache);
	/* Payload release precedes explicit reclaim of the retained header. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged), 0L);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
}

static void memcomp_test_cache_store_samefill(struct kunit *test)
{
	mc16_test_cache_roundtrip(test, true);
}

static void memcomp_test_cache_store_compressed(struct kunit *test)
{
	mc16_test_cache_roundtrip(test, false);
}

static void mc16_test_cache_prepare_failure(struct kunit *test, bool charge)
{
	struct mc16_test_cache_producer *f = mc16_test_cache_producer_begin(test);
	struct mc16_test_storage *storage;
	u8 *src;
	unsigned int i;
	int folio_refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = i % 53;
	folio_refs = folio_ref_count(storage->owner->folio);
	if (charge)
		storage->owner->model.fail_at =
			atomic_read(&storage->owner->model.charge_calls) + 1;
	else
		storage->probe.fail_at = storage->probe.allocations + 1;
	KUNIT_EXPECT_EQ(test, mc16_cache_store(src, storage->owner->folio,
					       &f->cache), -ENOMEM);
	storage->owner->model.fail_at = 0;
	storage->probe.fail_at = 0;
	KUNIT_EXPECT_PTR_EQ(test, f->cache.slot, NULL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), folio_refs);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged), 0L);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, atomic_read(&storage->owner->model.get_calls),
			atomic_read(&storage->owner->model.put_calls));
}

static void memcomp_test_cache_store_charge_failure(struct kunit *test)
{
	mc16_test_cache_prepare_failure(test, true);
}

static void memcomp_test_cache_store_reserve_failure(struct kunit *test)
{
	mc16_test_cache_prepare_failure(test, false);
}

/* Schedule the publication-window interleaving on an already linked cluster. */
static void memcomp_test_cache_publish_window(struct kunit *test)
{
	struct mc16_test_cache_producer *f = mc16_test_cache_producer_begin(test);
	struct mc16_initial_payload initial = { .bytes = 128 };
	struct memcomp_ref guard, active;
	struct mc16_test_storage *s;
	struct memcomp_lru *fifo;
	struct mc16_view view;
	unsigned long handle;
	unsigned int budget, i;
	unsigned short owner_id;
	u8 *src;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = i % 53;
	KUNIT_ASSERT_EQ(test, mc16_cache_store(src, s->owner->folio, &f->cache), 0);
	fifo = mc16_fifo_of(&f->cache);
	KUNIT_ASSERT_EQ(test, memcomp_zs_malloc(memcomp_pools[0], initial.bytes,
						&handle, false), 0);
	initial.word = handle;
	ret = memcomp_objcg_charge(&s->owner->model.objcg,
				   GFP_NOWAIT | __GFP_NOWARN, initial.bytes);
	if (ret) {
		memcomp_zs_free(0, handle);
		KUNIT_FAIL(test, "payload charge failed: %d", ret);
		return;
	}
	ret = mc16_source_id_get(s->owner->folio, &owner_id);
	if (!ret) {
		ret = mc16_reserve_cache_id(&s->owner->model.objcg, owner_id,
					    folio_nid(s->owner->folio), &initial, 1,
					    &f->private);
		mem_cgroup_memcomp_id_put(owner_id);
	}
	if (ret) {
		memcomp_objcg_uncharge(&s->owner->model.objcg, initial.bytes);
		memcomp_zs_free(0, handle);
		KUNIT_FAIL(test, "private reserve failed: %d", ret);
		return;
	}
	KUNIT_EXPECT_PTR_EQ(test, f->private.cluster, f->cache.cluster);
	KUNIT_EXPECT_EQ(test, f->private.cluster->live_slots, 2U);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->private.slot->refs), 0U);
	KUNIT_EXPECT_FALSE(test, mc16_get(f->private.token, &guard));
	f->private.cluster->wb_cursor = f->private.token & (MC16_CLUSTER_SLOTS - 1);
	budget = 1;
	KUNIT_EXPECT_FALSE(test, mc16_fifo_select(fifo, &budget, &guard));
	KUNIT_EXPECT_EQ(test, budget, 0U);
	KUNIT_EXPECT_EQ(test, f->private.cluster->eligible_slots, (u16)1);
	/* The old last STORED may disappear before new eligibility is recorded. */
	mc16_ref_put(&f->cache);
	KUNIT_EXPECT_TRUE(test, list_empty(&fifo->entries));
	KUNIT_EXPECT_EQ(test, f->private.cluster->live_slots, 1U);
	mc16_cache_publish(&f->private, initial.bytes);
	active = f->private;
	f->private = (struct memcomp_ref) { };
	f->cache = active;
	KUNIT_EXPECT_EQ(test, active.cluster->eligible_slots, (u16)1);
	active.cluster->wb_cursor = active.token & (MC16_CLUSTER_SLOTS - 1);
	budget = 1;
	KUNIT_ASSERT_TRUE(test, mc16_fifo_select(fifo, &budget, &guard));
	KUNIT_EXPECT_PTR_EQ(test, guard.slot, active.slot);
	mutex_lock(memcomp_lock_of(guard.token));
	if (mc16_view_locked(&guard, &view)) {
		mc16_fifo_transition(&guard, MC16_STORED, MC16_WRITEBACK);
		*view.flags = MC16_WRITEBACK;
	}
	mutex_unlock(memcomp_lock_of(guard.token));
	KUNIT_EXPECT_EQ(test, active.cluster->eligible_slots, (u16)0);
	KUNIT_EXPECT_TRUE(test, list_empty(&fifo->entries));
	mc16_ref_put(&guard);
	mc16_ref_put(&f->cache);
	/* Payload release precedes explicit reclaim of the retained header. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), 0L);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_CACHE_STORE_TEST_H */
