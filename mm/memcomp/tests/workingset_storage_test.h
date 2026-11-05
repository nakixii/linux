/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_WORKINGSET_STORAGE_TEST_H
#define _MM_MEMCOMP_TESTS_WORKINGSET_STORAGE_TEST_H

/* Include after storage_test.h. Registration belongs to tests/suite.h. */
static void memcomp_test_workingset_storage_marker(struct kunit *test)
{
	KUNIT_EXPECT_NE(test, MC16_SHADOW_PUBLISHING, 0ULL);
	KUNIT_EXPECT_FALSE(test, xa_is_value((void *)(unsigned long)
					  MC16_SHADOW_PUBLISHING));
	KUNIT_EXPECT_EQ(test, MC16_SEGMENT_SLOTS * sizeof(u64), (size_t)512);
	KUNIT_EXPECT_EQ(test, sizeof(struct mc16_slot), (size_t)16);
	KUNIT_EXPECT_EQ(test, sizeof(struct mc16_record), (size_t)64);
}

#ifdef CONFIG_MEMCG
static void memcomp_test_workingset_storage_words(struct kunit *test)
{
	struct mc16_test_storage *f = mc16_test_storage_begin(test);
	struct memcomp_ref guard = { };
	u64 *word, saved = (unsigned long)xa_mk_value(123);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 2), 0);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[0].token, &guard));
	mutex_lock(memcomp_lock_of(guard.token));
	word = mc16_shadow_ptr(&guard);
	KUNIT_EXPECT_NOT_NULL(test, word);
	if (word) {
		KUNIT_EXPECT_EQ(test, *word, 0ULL);
		*word = MC16_SHADOW_PUBLISHING;
		KUNIT_EXPECT_EQ(test, *mc16_shadow_ptr(&guard),
				MC16_SHADOW_PUBLISHING);
		*word = saved;
		KUNIT_EXPECT_EQ(test, *mc16_shadow_ptr(&guard), saved);
		*word = 0;
	}
	mutex_unlock(memcomp_lock_of(guard.token));
	mc16_ref_put(&guard);

	mutex_lock(memcomp_lock_of(f->refs[1].token));
	word = mc16_shadow_ptr(&f->refs[1]);
	KUNIT_EXPECT_NOT_NULL(test, word);
	if (word)
		KUNIT_EXPECT_EQ(test, *word, 0ULL);
	mutex_unlock(memcomp_lock_of(f->refs[1].token));
}

static void memcomp_test_workingset_storage_segments(struct kunit *test)
{
	struct mc16_test_storage *f = mc16_test_storage_begin(test);
	u64 *words[2], value[2] = {
		(unsigned long)xa_mk_value(11),
		(unsigned long)xa_mk_value(22),
	};
	unsigned int i, index;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 65; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	KUNIT_EXPECT_PTR_NE(test, f->refs[0].cluster->shadows[0],
			    f->refs[0].cluster->shadows[1]);
	for (i = 0; i < 2; i++) {
		index = 63 + i;
		mutex_lock(memcomp_lock_of(f->refs[index].token));
		words[i] = mc16_shadow_ptr(&f->refs[index]);
		KUNIT_EXPECT_NOT_NULL(test, words[i]);
		if (words[i]) {
			KUNIT_EXPECT_EQ(test, *words[i], 0ULL);
			*words[i] = value[i];
		}
		mutex_unlock(memcomp_lock_of(f->refs[index].token));
	}
	for (i = 0; i < 2; i++) {
		index = 63 + i;
		mutex_lock(memcomp_lock_of(f->refs[index].token));
		if (words[i])
			KUNIT_EXPECT_EQ(test, *mc16_shadow_ptr(&f->refs[index]),
					value[i]);
		mutex_unlock(memcomp_lock_of(f->refs[index].token));
	}
}

static void memcomp_test_workingset_storage_nomem(struct kunit *test)
{
	struct mc16_test_storage *f = mc16_test_storage_begin(test);
	unsigned int at;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct mc16_storage_debug before, after;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (at = 1; at <= 3; at++) {
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		mc16_test_metadata_snapshot(&before);
#endif
		/* Segment, shadow, then cluster allocation, all before publication. */
		f->probe.fail_at = f->probe.allocations + at;
		KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -ENOMEM);
		KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
		KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
		KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 1);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		mc16_test_metadata_snapshot(&after);
		KUNIT_EXPECT_EQ(test,
				after.allocated[MC16_META_SHADOW] -
				before.allocated[MC16_META_SHADOW],
				after.freed[MC16_META_SHADOW] -
				before.freed[MC16_META_SHADOW]);
#endif
	}
	f->probe.fail_at = 0;
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), 0);
}

static void mc16_ws_test_retire(struct kunit *test, bool fail_descriptor)
{
	struct mc16_test_storage *f = mc16_test_storage_begin(test);
	struct mc16_cluster *cluster;
	struct mc16_segment_retire *retired;
	u64 *shadows;
	u64 survivor;
	unsigned short owner_id;
	unsigned int i;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct mc16_storage_debug before, after;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 65; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	cluster = f->refs[64].cluster;
	owner_id = mc16_owner_id(&f->refs[64]);
	/* Storage-only opaque words; never submit them to stock refault. */
	for (i = 0; i < 65; i++) {
		mutex_lock(memcomp_lock_of(f->refs[i].token));
		*mc16_shadow_ptr(&f->refs[i]) = (unsigned long)xa_mk_value(i + 1);
		mutex_unlock(memcomp_lock_of(f->refs[i].token));
	}
	shadows = cluster->shadows[0];
	f->probe.hold_segments = true;
	f->probe.fail_descriptor = fail_descriptor;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&before);
#endif
	for (i = 0; i < 64; i++)
		mc16_test_cancel_pending(&f->refs[i]);
	KUNIT_EXPECT_EQ(test, cluster->live_slots, 1U);
	mutex_lock(memcomp_lock_of(f->refs[64].token));
	survivor = *mc16_shadow_ptr(&f->refs[64]);
	mutex_unlock(memcomp_lock_of(f->refs[64].token));
	KUNIT_EXPECT_EQ(test, survivor, (u64)(unsigned long)xa_mk_value(65));
	KUNIT_EXPECT_EQ(test, mc16_owner_id(&f->refs[64]), owner_id);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->refs[64].slot->refs), 1U);
	if (fail_descriptor) {
		KUNIT_EXPECT_PTR_EQ(test, cluster->shadows[0], shadows);
		KUNIT_EXPECT_NOT_NULL(test, cluster->segments[0]);
		KUNIT_EXPECT_PTR_EQ(test, f->probe.delayed_segments, NULL);
	} else {
		KUNIT_EXPECT_PTR_EQ(test, cluster->shadows[0], NULL);
		KUNIT_EXPECT_PTR_EQ(test, cluster->segments[0], NULL);
		KUNIT_ASSERT_NOT_NULL(test, f->probe.delayed_segments);
		retired = container_of(f->probe.delayed_segments,
				       struct mc16_segment_retire, rcu);
		KUNIT_EXPECT_PTR_EQ(test, retired->shadows, shadows);
	}
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&after);
	KUNIT_EXPECT_EQ(test, after.freed[MC16_META_SHADOW],
			before.freed[MC16_META_SHADOW]);
#endif
	f->probe.fail_descriptor = false;
	mc16_test_drain_segments(&f->probe);
	rcu_barrier();
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&after);
	KUNIT_EXPECT_EQ(test, after.freed[MC16_META_SHADOW] -
			before.freed[MC16_META_SHADOW],
			fail_descriptor ? 0ULL : 1ULL);
#endif
	mc16_test_cancel_pending(&f->refs[64]);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
}

static void memcomp_test_workingset_storage_retire(struct kunit *test)
{
	mc16_ws_test_retire(test, false);
}

static void memcomp_test_workingset_storage_retire_nomem(struct kunit *test)
{
	mc16_ws_test_retire(test, true);
}

static void memcomp_test_workingset_storage_key_identity(struct kunit *test)
{
	struct mc16_key a = { .owner_id = 7 }, b = { .owner_id = 8 };
	struct mc16_key *found_a, *found_b, *missing;
	unsigned long flags;

	/* Same owner pointer and geometry, only the immutable source ID differs. */
	xa_lock_irqsave(&mc16_clusters, flags);
	hash_add(mc16_keys, &a.node, mc16_key_tag_id(NULL, 0, 0, false, 7));
	hash_add(mc16_keys, &b.node, mc16_key_tag_id(NULL, 0, 0, false, 8));
	found_a = mc16_key_find_id(NULL, 0, 0, false, 7);
	found_b = mc16_key_find_id(NULL, 0, 0, false, 8);
	missing = mc16_key_find_id(NULL, 0, 0, false, 9);
	hash_del(&a.node);
	hash_del(&b.node);
	xa_unlock_irqrestore(&mc16_clusters, flags);
	KUNIT_EXPECT_PTR_EQ(test, found_a, &a);
	KUNIT_EXPECT_PTR_EQ(test, found_b, &b);
	KUNIT_EXPECT_PTR_EQ(test, missing, NULL);
}

/* Only direct alloc/free is modeled; no synthetic ID enters an RCU callback. */
static unsigned int mc16_ws_id_get_calls;
static unsigned int mc16_ws_id_put_calls;

static bool mc16_ws_id_get_ref(unsigned short id)
{
	if (!id)
		return true;
	mc16_ws_id_get_calls++;
	return id == 7;
}

static void mc16_ws_id_put(unsigned short id)
{
	if (id)
		mc16_ws_id_put_calls++;
}

struct mc16_ws_key_hold {
	struct mc16_key *key;
};

static void mc16_ws_key_cleanup(void *data)
{
	struct mc16_ws_key_hold *hold = data;

	mc16_key_free(hold->key);
	hold->key = NULL;
}

static void memcomp_test_workingset_storage_key_ref(struct kunit *test)
{
	struct mc16_test_storage *f = mc16_test_storage_begin(test);
	struct mc16_ws_key_hold *hold;
	struct mc16_key *key;

	KUNIT_ASSERT_NOT_NULL(test, f);
	hold = kunit_kzalloc(test, sizeof(*hold), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, hold);
	mc16_ws_id_get_calls = 0;
	mc16_ws_id_put_calls = 0;
	kunit_activate_static_stub(test, mc16_owner_id_get_ref, mc16_ws_id_get_ref);
	kunit_activate_static_stub(test, mc16_owner_id_put, mc16_ws_id_put);
	/* Registered after the stubs: even an assertion exit frees while modeled. */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mc16_ws_key_cleanup,
							hold), 0);
	key = mc16_key_alloc_id(&f->owner->model.objcg, 0, MC16_KEY_ANON, false, 7);
	KUNIT_ASSERT_FALSE(test, IS_ERR(key));
	hold->key = key;
	KUNIT_EXPECT_EQ(test, key->owner_id, (unsigned short)7);
	KUNIT_EXPECT_EQ(test, mc16_ws_id_get_calls, 1U);
	KUNIT_EXPECT_EQ(test, mc16_ws_id_put_calls, 0U);
	mc16_ws_key_cleanup(hold);
	KUNIT_EXPECT_EQ(test, mc16_ws_id_put_calls, 1U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
}

static void memcomp_test_workingset_storage_key_ref_failure(struct kunit *test)
{
	struct mc16_test_storage *f = mc16_test_storage_begin(test);
	const struct mc16_initial_payload payload = { .word = 0x55 };
	struct memcomp_ref ref = { };
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_ws_id_get_calls = 0;
	kunit_activate_static_stub(test, mc16_owner_id_get_ref, mc16_ws_id_get_ref);
	/* ID 8 cannot acquire a key reference; nothing may become visible. */
	ret = mc16_reserve_cache_id(&f->owner->model.objcg, 8, 0, &payload, 1,
				    &ref);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
	KUNIT_EXPECT_PTR_EQ(test, ref.slot, NULL);
	KUNIT_EXPECT_EQ(test, mc16_ws_id_get_calls, 1U);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
}

static int mc16_ws_source_id_failure(struct folio *folio, unsigned short *id)
{
	*id = 0;
	return -EAGAIN;
}

static void memcomp_test_workingset_storage_source_id_failure(struct kunit *test)
{
	struct mc16_test_storage *f = mc16_test_storage_begin(test);
	unsigned int allocations;

	KUNIT_ASSERT_NOT_NULL(test, f);
	allocations = f->probe.allocations;
	kunit_activate_static_stub(test, mc16_source_id_get,
				   mc16_ws_source_id_failure);
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->nr, 0U);
	KUNIT_EXPECT_EQ(test, f->probe.allocations, allocations);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
}

struct mc16_ws_charge_hold {
	struct folio *folio;
	unsigned short id;
};

static void mc16_ws_charge_cleanup(void *data)
{
	struct mc16_ws_charge_hold *hold = data;

	mem_cgroup_memcomp_id_put(hold->id);
	if (hold->folio) {
		folio_unlock(hold->folio);
		folio_put(hold->folio);
	}
}

static void memcomp_test_workingset_storage_source_id(struct kunit *test)
{
	struct mc16_ws_charge_hold *hold;
	struct mem_cgroup *memcg;
	unsigned short expected_id;
	bool got;
	int ret;

	hold = kunit_kzalloc(test, sizeof(*hold), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, hold);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mc16_ws_charge_cleanup,
							hold), 0);
	hold->folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, hold->folio);
	folio_lock(hold->folio);
	ret = mem_cgroup_memcomp_charge(hold->folio, &init_mm, GFP_KERNEL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	if (mem_cgroup_disabled()) {
		KUNIT_EXPECT_EQ(test, mem_cgroup_memcomp_id_get(hold->folio),
				(unsigned short)0);
		return;
	}
	memcg = folio_memcg(hold->folio);
	KUNIT_ASSERT_NOT_NULL(test, memcg);
	expected_id = mem_cgroup_id(memcg);
	KUNIT_EXPECT_NE(test, expected_id, (unsigned short)0);
	KUNIT_EXPECT_PTR_EQ(test, memcg, root_mem_cgroup);
	hold->id = mem_cgroup_memcomp_id_get(hold->folio);
	KUNIT_EXPECT_EQ(test, hold->id, expected_id);
	got = mem_cgroup_memcomp_id_get_ref(hold->id);
	KUNIT_EXPECT_TRUE(test, got);
	if (got)
		mem_cgroup_memcomp_id_put(hold->id);
	if (hold->id) {
		rcu_read_lock();
		KUNIT_EXPECT_PTR_EQ(test, mem_cgroup_from_id(hold->id), memcg);
		rcu_read_unlock();
	}
}

static void memcomp_test_workingset_storage_root_source(struct kunit *test)
{
	struct mc16_ws_charge_hold *hold;
	unsigned short root_id;
	bool got;

	hold = kunit_kzalloc(test, sizeof(*hold), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, hold);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mc16_ws_charge_cleanup,
							hold), 0);
	hold->folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, hold->folio);
	folio_lock(hold->folio);
	/* An uncharged source still has root identity, not the later reader's. */
	KUNIT_EXPECT_PTR_EQ(test, folio_memcg(hold->folio), NULL);
	KUNIT_ASSERT_EQ(test, mem_cgroup_memcomp_id_get_checked(hold->folio,
								&hold->id), 0);
	if (mem_cgroup_disabled()) {
		KUNIT_EXPECT_EQ(test, hold->id, (unsigned short)0);
		return;
	}
	root_id = mem_cgroup_id(root_mem_cgroup);
	KUNIT_EXPECT_NE(test, root_id, (unsigned short)0);
	KUNIT_EXPECT_EQ(test, hold->id, root_id);
	rcu_read_lock();
	KUNIT_EXPECT_PTR_EQ(test, mem_cgroup_from_id(hold->id), root_mem_cgroup);
	rcu_read_unlock();
	got = mem_cgroup_memcomp_id_get_ref(hold->id);
	KUNIT_EXPECT_TRUE(test, got);
	if (got)
		mem_cgroup_memcomp_id_put(hold->id);
	KUNIT_ASSERT_EQ(test, mem_cgroup_memcomp_charge(hold->folio, &init_mm,
							GFP_KERNEL, hold->id), 0);
	KUNIT_EXPECT_PTR_EQ(test, folio_memcg(hold->folio), root_mem_cgroup);
}

static void memcomp_test_workingset_storage_charge_fallback(struct kunit *test)
{
	struct mc16_ws_charge_hold *hold;
	unsigned int id;

	hold = kunit_kzalloc(test, sizeof(*hold), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, hold);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mc16_ws_charge_cleanup,
							hold), 0);
	hold->folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, hold->folio);
	folio_lock(hold->folio);
	rcu_read_lock();
	for (id = 1; id <= U16_MAX; id++) {
		if (!mem_cgroup_from_id(id))
			break;
	}
	rcu_read_unlock();
	if (id > U16_MAX) {
		kunit_skip(test, "no unassigned memcg ID for fallback check");
		return;
	}
	KUNIT_EXPECT_FALSE(test, mem_cgroup_memcomp_id_get_ref(id));
	KUNIT_ASSERT_EQ(test, mem_cgroup_memcomp_charge(hold->folio, &init_mm,
							GFP_KERNEL, id), 0);
	if (!mem_cgroup_disabled())
		KUNIT_EXPECT_PTR_EQ(test, folio_memcg(hold->folio), root_mem_cgroup);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_WORKINGSET_STORAGE_TEST_H */
