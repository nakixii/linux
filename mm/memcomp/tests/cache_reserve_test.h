/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_CACHE_RESERVE_TEST_H
#define _MM_MEMCOMP_TESTS_CACHE_RESERVE_TEST_H

/* Include after storage_test.h; this header does not register any test cases. */
#ifdef CONFIG_MEMCG
struct mc16_test_cache_reserve {
	struct mc16_test_storage *storage;
	struct mc16_initial_payload initial[MC16_MAX_PAGES];
	bool owned[MC16_MAX_PAGES];
};

static void mc16_test_cache_drop(struct mc16_test_storage *s);

/* Prepared charges are private; stable slots use the storage cleanup action. */
static void mc16_test_cache_cleanup(void *data)
{
	struct mc16_test_cache_reserve *f = data;
	struct obj_cgroup *objcg = &f->storage->owner->model.objcg;
	unsigned int i;

	/* Teardown must not re-enter the fixture's allocation-window callback. */
	f->storage->probe.fail_at = 0;
	f->storage->probe.fail_descriptor = false;
	f->storage->probe.allocation_window = NULL;

	for (i = 0; i < MC16_MAX_PAGES; i++) {
		if (!f->owned[i])
			continue;
		if (f->initial[i].bytes) {
			memcomp_zs_free(0, f->initial[i].word);
			memcomp_objcg_uncharge(objcg, f->initial[i].bytes);
		}
	}
	mc16_test_cache_drop(f->storage);
	/* Return the independent source hold after private storage cleanup. */
	memcomp_objcg_put(objcg);
}

static struct mc16_test_cache_reserve *
mc16_test_cache_begin(struct kunit *test)
{
	struct mc16_test_cache_reserve *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->storage = mc16_test_storage_begin(test);
	if (!f->storage)
		return NULL;
	/* Explicitly model the source hold required across the allocation window. */
	memcomp_objcg_get_ref(&f->storage->owner->model.objcg);
	if (kunit_add_action_or_reset(test, mc16_test_cache_cleanup, f))
		return NULL;
	return f;
}

static int mc16_test_cache_prepare(struct mc16_test_cache_reserve *f,
				   unsigned int index, u32 bytes, u64 word)
{
	struct obj_cgroup *objcg = &f->storage->owner->model.objcg;
	unsigned long handle;
	void *mapped;
	int ret;

	if (index >= MC16_MAX_PAGES || f->owned[index] || bytes > PAGE_SIZE)
		return -EINVAL;
	if (bytes) {
		ret = memcomp_zs_malloc(memcomp_pools[0], bytes, &handle, false);
		if (ret)
			return ret;
		ret = memcomp_objcg_charge(objcg, GFP_NOWAIT | __GFP_NOWARN, bytes);
		if (ret) {
			memcomp_zs_free(0, handle);
			return ret;
		}
		mapped = zs_map_object(memcomp_pools[0], handle, ZS_MM_WO);
		memset(mapped, 0x5a, bytes);
		zs_unmap_object(memcomp_pools[0], handle);
		word = handle;
	}
	f->initial[index] = (struct mc16_initial_payload) {
		.word = word, .bytes = bytes,
	};
	f->owned[index] = true;
	return 0;
}

static int mc16_test_cache_reserve(struct mc16_test_cache_reserve *f,
				   unsigned int nr)
{
	struct mc16_test_storage *s = f->storage;
	unsigned int i;
	int ret;

	if (nr > MC16_MAX_PAGES || s->nr + nr > MC16_TEST_REFS)
		return -ENOSPC;
	ret = mc16_reserve_cache(&s->owner->model.objcg,
				 folio_nid(s->owner->folio), f->initial, nr,
				&s->refs[s->nr]);
	if (!ret) {
		for (i = 0; i < nr; i++)
			f->owned[i] = false;
		s->nr += nr;
	}
	return ret;
}

static void mc16_test_cache_drop(struct mc16_test_storage *s)
{
	unsigned int i;

	/* No pending/stat/FIFO publication occurred in this storage-only fixture. */
	for (i = 0; i < s->nr; i++) {
		struct memcomp_ref *ref = &s->refs[i];

		if (!ref->slot)
			continue;
		/* Private reservations never acquired an initial counted ref. */
		mc16_release_resources(ref);
		mc16_retire_slot(ref);
	}
	rcu_barrier();
}

static void memcomp_test_cache_reserve_payloads(struct kunit *test)
{
	const u64 fill = 0xfedcba9876543210ULL;
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	struct mc16_test_storage *s;
	struct mc16_initial_payload saved[4];
	struct mc16_view view;
	unsigned int i, state;
	u32 bytes;
	long charge;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 0, PAGE_SIZE, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 1, 0, fill), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 2, 128, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 3, 0, 0), 0);
	memcpy(saved, f->initial, sizeof(saved));
	KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 4), 0);
	for (i = 0; i < ARRAY_SIZE(saved); i++) {
		struct memcomp_ref *ref = &s->refs[i];

		state = saved[i].bytes ? MC16_STORED : MC16_SAMEFILL;
		KUNIT_EXPECT_EQ(test, ref->slot->word, saved[i].word);
		KUNIT_EXPECT_EQ(test, f->initial[i].word, saved[i].word);
		KUNIT_EXPECT_EQ(test, f->initial[i].bytes, saved[i].bytes);
		KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 0);
		KUNIT_EXPECT_EQ(test, ref->slot->flags,
				(u16)(state | (2 << MC16_ORDER_SHIFT)));
		KUNIT_EXPECT_TRUE(test, mc16_payload_length_decode(state,
								   ref->slot->length, &bytes));
		KUNIT_EXPECT_EQ(test, bytes, saved[i].bytes);
		KUNIT_ASSERT_TRUE(test, mc16_view_owned(ref, &view));
		KUNIT_EXPECT_PTR_EQ(test, view.record, NULL);
		KUNIT_EXPECT_PTR_EQ(test, view.member, NULL);
		KUNIT_EXPECT_PTR_EQ(test, ref->cluster, s->refs[0].cluster);
		KUNIT_EXPECT_EQ(test, ref->token, s->refs[0].token + i);
	}
	KUNIT_EXPECT_EQ(test, s->refs[0].token & 3U, 0U);
	KUNIT_EXPECT_EQ(test, s->refs[0].cluster->key->kind, (u8)MC16_KEY_SHMEM);
	KUNIT_EXPECT_PTR_EQ(test, s->refs[0].cluster->key->objcg,
			    &s->owner->model.objcg);
	KUNIT_EXPECT_EQ(test, s->refs[0].cluster->eligible_slots, (u16)0);
	charge = PAGE_SIZE + 128 + s->refs[0].cluster->key->metadata_bytes;
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), charge);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.get_calls), 2);
	s->payload.task = current;
	s->payload.pool = memcomp_pools[0];
	s->payload.handle = saved[0].word;
	s->payload.live = true;
	rcu_assign_pointer(memcomp_kunit_async_probe, &s->payload);
	mc16_test_cache_drop(s);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, s->payload.live);
	KUNIT_EXPECT_FALSE(test, s->payload.bad_lifetime);
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_FALSE(test, hash_empty(mc16_keys));
	/* Source and key remain independent until explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), 0L);
	/* Only the caller's explicit source hold remains. */
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.get_calls) -
			atomic_read(&s->owner->model.put_calls), 1);
}

static void memcomp_test_cache_reserve_boundaries(struct kunit *test)
{
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	struct mc16_test_storage *s;
	struct mc16_cluster *first;
	struct memcomp_ref guard;
	u32 base;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 1), 0);
	first = s->refs[0].cluster;
	base = s->refs[0].token;
	KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 2), 0);
	KUNIT_EXPECT_EQ(test, s->refs[1].token, base + 2);
	KUNIT_EXPECT_FALSE(test, mc16_get(base + 1, &guard));
	/* Segment, shadow array and cluster; the second reserve reuses them. */
	KUNIT_EXPECT_EQ(test, s->probe.allocations, 3U);
	while (first->next < MC16_SEGMENT_SLOTS)
		KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, MC16_MAX_PAGES), 0);
	KUNIT_EXPECT_EQ(test, s->refs[s->nr - MC16_MAX_PAGES].token, base + 64);
	KUNIT_EXPECT_EQ(test, s->refs[s->nr - 1].slot->flags,
			(u16)(MC16_SAMEFILL |
			      (MC16_MAX_ORDER << MC16_ORDER_SHIFT)));
	while (first->next < MC16_CLUSTER_SLOTS)
		KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 1), 0);
	KUNIT_EXPECT_PTR_NE(test, s->refs[s->nr - 1].cluster, first);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 1);
}

static void memcomp_test_cache_reserve_invalid(struct kunit *test)
{
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	const unsigned int bad_nr[] = { 0, 3, MC16_MAX_PAGES + 1, 65537, UINT_MAX };
	const unsigned int bad_nid[] = { MAX_NUMNODES, 65536, UINT_MAX };
	struct memcomp_ref refs[MC16_MAX_PAGES];
	struct mc16_test_storage *s;
	struct obj_cgroup *objcg;
	unsigned int i;
	u64 handle;
	long charge;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->storage;
	objcg = &s->owner->model.objcg;
	for (i = 0; i < ARRAY_SIZE(refs); i++)
		refs[i] = (struct memcomp_ref) { .token = U32_MAX };
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 0, 128, 0), 0);
	handle = f->initial[0].word;
	charge = atomic_long_read(&s->owner->model.charged);
	for (i = 0; i < ARRAY_SIZE(bad_nr); i++)
		KUNIT_EXPECT_EQ(test, mc16_reserve_cache(objcg, 0, f->initial,
							 bad_nr[i], refs), -EINVAL);
	for (i = 0; i < ARRAY_SIZE(bad_nid); i++)
		KUNIT_EXPECT_EQ(test, mc16_reserve_cache(objcg, bad_nid[i],
							 f->initial, 1, refs), -EINVAL);
	KUNIT_EXPECT_EQ(test, mc16_reserve_cache(objcg, 0, NULL, 1, refs), -EINVAL);
	KUNIT_EXPECT_EQ(test, mc16_reserve_cache(objcg, 0, f->initial, 1, NULL),
			-EINVAL);
	/* The last bad element must prevent publication of every earlier one. */
	f->initial[MC16_MAX_PAGES - 1].bytes = PAGE_SIZE + 1;
	f->initial[MC16_MAX_PAGES - 1].word = 1;
	KUNIT_EXPECT_EQ(test, mc16_reserve_cache(objcg, 0, f->initial,
						 MC16_MAX_PAGES, refs), -EINVAL);
	f->initial[MC16_MAX_PAGES - 1].bytes = U32_MAX;
	KUNIT_EXPECT_EQ(test, mc16_reserve_cache(objcg, 0, f->initial,
						 MC16_MAX_PAGES, refs), -EINVAL);
	f->initial[MC16_MAX_PAGES - 1].bytes = 1;
	f->initial[MC16_MAX_PAGES - 1].word = 0;
	KUNIT_EXPECT_EQ(test, mc16_reserve_cache(objcg, 0, f->initial,
						 MC16_MAX_PAGES, refs), -EINVAL);
	for (i = 0; i < ARRAY_SIZE(refs); i++) {
		KUNIT_EXPECT_EQ(test, refs[i].token, U32_MAX);
		KUNIT_EXPECT_PTR_EQ(test, refs[i].slot, NULL);
	}
	KUNIT_EXPECT_EQ(test, f->initial[0].word, handle);
	KUNIT_EXPECT_TRUE(test, f->owned[0]);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), charge);
	KUNIT_EXPECT_EQ(test, s->probe.allocations, 0U);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
}

static int mc16_test_cache_id_fail(u32 *id)
{
	return -ENOSPC;
}

static struct mc16_key *mc16_test_cache_key_fail(void)
{
	return NULL;
}

static void memcomp_test_cache_reserve_nomem(struct kunit *test)
{
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	struct mc16_test_storage *s;
	unsigned int at;
	u64 handle;
	long charge;
	u8 *mapped, first, last;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 0, 128, 0), 0);
	handle = f->initial[0].word;
	charge = atomic_long_read(&s->owner->model.charged);
	/* Keep header failure coverage after adding the shadow allocation. */
	for (at = 1; at <= 3; at++) {
		s->probe.fail_at = s->probe.allocations + at;
		KUNIT_EXPECT_EQ(test, mc16_test_cache_reserve(f, 1), -ENOMEM);
		s->probe.fail_at = 0;
		KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
		KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
		KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
				charge);
	}
	kunit_activate_static_stub(test, mc16_key_zalloc, mc16_test_cache_key_fail);
	KUNIT_EXPECT_EQ(test, mc16_test_cache_reserve(f, 1), -ENOMEM);
	kunit_deactivate_static_stub(test, mc16_key_zalloc);
	s->owner->model.fail_at = atomic_read(&s->owner->model.charge_calls) + 1;
	KUNIT_EXPECT_EQ(test, mc16_test_cache_reserve(f, 1), -ENOMEM);
	s->owner->model.fail_at = 0;
	kunit_activate_static_stub(test, mc16_private_id_alloc, mc16_test_cache_id_fail);
	KUNIT_EXPECT_EQ(test, mc16_test_cache_reserve(f, 1), -ENOSPC);
	kunit_deactivate_static_stub(test, mc16_private_id_alloc);
	KUNIT_EXPECT_EQ(test, f->initial[0].word, handle);
	KUNIT_EXPECT_EQ(test, f->initial[0].bytes, 128U);
	KUNIT_EXPECT_TRUE(test, f->owned[0]);
	KUNIT_EXPECT_EQ(test, s->nr, 0U);
	KUNIT_EXPECT_PTR_EQ(test, s->refs[0].slot, NULL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), charge);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 1);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	/* A rejected reserve must leave the real zs object mapped and intact. */
	mapped = zs_map_object(memcomp_pools[0], handle, ZS_MM_RO);
	first = mapped[0];
	last = mapped[127];
	zs_unmap_object(memcomp_pools[0], handle);
	KUNIT_EXPECT_EQ(test, first, (u8)0x5a);
	KUNIT_EXPECT_EQ(test, last, (u8)0x5a);
}

struct mc16_test_cache_window {
	struct mc16_test_storage *storage;
	int result;
	int source_holds;
};

static void mc16_test_cache_compete(void *data)
{
	struct mc16_test_cache_window *window = data;
	struct mc16_test_storage *s = window->storage;
	struct mc16_initial_payload initial = { .word = U64_MAX };

	window->source_holds = atomic_read(&s->owner->model.get_calls) -
		atomic_read(&s->owner->model.put_calls);
	window->result = mc16_reserve_cache(&s->owner->model.objcg,
					    folio_nid(s->owner->folio), &initial, 1,
					    &s->refs[s->nr]);
	if (!window->result)
		s->nr++;
}

static void memcomp_test_cache_reserve_race(struct kunit *test)
{
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	struct mc16_test_cache_window window;
	struct mc16_test_storage *s;
	long charge;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 0, 128, 0), 0);
	window = (struct mc16_test_cache_window) { .storage = s };
	s->probe.allocation_window = mc16_test_cache_compete;
	s->probe.window_data = &window;
	KUNIT_EXPECT_EQ(test, mc16_test_cache_reserve(f, 1), -EAGAIN);
	KUNIT_EXPECT_EQ(test, window.result, 0);
	KUNIT_EXPECT_EQ(test, window.source_holds, 1);
	KUNIT_EXPECT_EQ(test, s->nr, 1U);
	KUNIT_EXPECT_EQ(test, s->refs[0].slot->word, U64_MAX);
	KUNIT_EXPECT_TRUE(test, f->owned[0]);
	charge = 128 + s->refs[0].cluster->key->metadata_bytes;
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), charge);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 1);
}

static void mc16_test_cache_retire_window(void *data)
{
	struct mc16_test_cache_window *window = data;
	struct mc16_test_storage *s = window->storage;

	mc16_test_cache_drop(s);
	window->source_holds = atomic_read(&s->owner->model.get_calls) -
		atomic_read(&s->owner->model.put_calls);
}

static void memcomp_test_cache_reserve_retire(struct kunit *test)
{
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	struct mc16_test_cache_window window;
	struct mc16_test_storage *s;
	struct mc16_cluster *cluster;
	unsigned int i;
	u32 base;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->storage;
	for (i = 0; i < MC16_SEGMENT_SLOTS; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 0, 128, 0), 0);
	cluster = s->refs[0].cluster;
	base = s->refs[0].token;
	window = (struct mc16_test_cache_window) { .storage = s };
	s->probe.allocation_window = mc16_test_cache_retire_window;
	s->probe.window_data = &window;
	/* Last-live retirement preserves X1 geometry, so X2 can append. */
	KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 1), 0);
	KUNIT_EXPECT_EQ(test, window.source_holds, 2);
	KUNIT_EXPECT_FALSE(test, f->owned[0]);
	KUNIT_EXPECT_EQ(test, s->nr, MC16_SEGMENT_SLOTS + 1U);
	KUNIT_EXPECT_PTR_EQ(test, s->refs[MC16_SEGMENT_SLOTS].cluster, cluster);
	KUNIT_EXPECT_EQ(test, s->refs[MC16_SEGMENT_SLOTS].token,
			base + MC16_SEGMENT_SLOTS);
	KUNIT_EXPECT_EQ(test, cluster->live_slots, 1U);
	KUNIT_EXPECT_EQ(test, cluster->next, MC16_SEGMENT_SLOTS + 1U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			128L + cluster->key->metadata_bytes);
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_FALSE(test, hash_empty(mc16_keys));
}

static void memcomp_test_cache_reserve_root(struct kunit *test)
{
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	struct mc16_initial_payload initial = { .word = U64_MAX };
	struct mc16_test_storage *s;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_reserve_cache(NULL, folio_nid(s->owner->folio),
						 &initial, 1, s->refs), 0);
	s->nr = 1;
	KUNIT_EXPECT_PTR_EQ(test, s->refs[0].cluster->key->objcg, NULL);
			    KUNIT_EXPECT_EQ(test, s->refs[0].slot->word, U64_MAX);
	KUNIT_EXPECT_EQ(test, s->refs[0].slot->length, (u16)0);
	KUNIT_EXPECT_EQ(test, s->refs[0].slot->flags, (u16)MC16_SAMEFILL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), 0L);
	mc16_test_cache_drop(s);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_CACHE_RESERVE_TEST_H */
