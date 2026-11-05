/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_STORAGE_TEST_H
#define _MM_MEMCOMP_TESTS_STORAGE_TEST_H

/* Pressure cleanup is explicit; normal retirement keeps appendable headers. */
static void mc16_test_reclaim_retained(void)
{
	struct shrink_control sc = { .nr_to_scan = 64 };
	struct mc16_cluster *cluster;
	unsigned long flags, id;
	bool found;

	cancel_delayed_work_sync(&mc16_retained_aging);
	while (READ_ONCE(mc16_retained_empty_headers)) {
		found = false;
		xa_lock_irqsave(&mc16_clusters, flags);
		xa_for_each(&mc16_clusters, id, cluster) {
			if (xa_is_value(cluster) || xa_is_internal(cluster))
				continue;
			if (cluster->key->current_cluster != cluster ||
			    cluster->live_slots || !cluster->next ||
			    cluster->next >= MC16_CLUSTER_SLOTS ||
			    cluster->eligible_slots || !list_empty(&cluster->fifo))
				continue;
			mc16_meta_shrinker_cursor = id;
			found = true;
			break;
		}
		xa_unlock_irqrestore(&mc16_clusters, flags);
		/* An inconsistent count must not turn a failed test into a hang. */
		if (WARN_ON_ONCE(!found))
			break;
		if (WARN_ON_ONCE(!mc16_meta_shrinker_scan(NULL, &sc)))
			break;
	}
	rcu_barrier();
}

#ifdef CONFIG_MEMCG
#define MC16_TEST_REFS 514
struct mc16_test_storage {
	struct kunit *test;
	struct memcomp_test_objcg_fixture *owner;
	struct mc16_storage_fixture probe;
	struct memcomp_kunit_async_probe payload;
	struct memcomp_ref refs[MC16_TEST_REFS];
	unsigned int nr;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct mc16_storage_debug baseline;
#endif
};

/* Foundation reservations still hold their initial resident OWNER member. */
static void mc16_test_cancel_pending(struct memcomp_ref *ref)
{
	if (!ref->slot)
		return;
	if (WARN_ON_ONCE(!refcount_dec_and_test(&ref->slot->refs)))
		return;
	mc16_release_resources(ref);
	mc16_retire_slot(ref);
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static void mc16_test_metadata_snapshot(struct mc16_storage_debug *sum)
{
	struct mc16_storage_debug *debug;
	unsigned int type;
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		debug = per_cpu_ptr(&mc16_storage_debug, cpu);
		for (type = 0; type < MC16_META_CLASSES; type++) {
			sum->allocated[type] += READ_ONCE(debug->allocated[type]);
			sum->freed[type] += READ_ONCE(debug->freed[type]);
			sum->allocated_bytes[type] += READ_ONCE(debug->allocated_bytes[type]);
			sum->freed_bytes[type] += READ_ONCE(debug->freed_bytes[type]);
		}
	}
}
#endif

static void mc16_test_storage_cleanup(void *data)
{
	struct mc16_test_storage *f = data;
	unsigned int i;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct mc16_storage_debug after;
#endif

	f->probe.fail_at = 0;
	f->probe.fail_descriptor = false;
	f->probe.allocation_window = NULL;
	for (i = 0; i < f->nr; i++)
		mc16_test_cancel_pending(&f->refs[i]);
	mc16_test_drain_segments(&f->probe);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(f->test, READ_ONCE(mc16_retained_empty_headers), 0UL);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&after);
	for (i = 0; i < MC16_META_CLASSES; i++) {
		KUNIT_EXPECT_EQ(f->test, after.allocated[i] - f->baseline.allocated[i],
				after.freed[i] - f->baseline.freed[i]);
		KUNIT_EXPECT_EQ(f->test,
				after.allocated_bytes[i] - f->baseline.allocated_bytes[i],
				after.freed_bytes[i] - f->baseline.freed_bytes[i]);
	}
#endif
	if (rcu_access_pointer(memcomp_kunit_async_probe) == &f->payload) {
		rcu_assign_pointer(memcomp_kunit_async_probe, NULL);
		synchronize_rcu();
	}
	KUNIT_EXPECT_TRUE(f->test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(f->test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(f->test, atomic_long_read(&f->owner->model.charged), 0L);
	if (rcu_access_pointer(mc16_storage_fixture) == &f->probe) {
		rcu_assign_pointer(mc16_storage_fixture, NULL);
		synchronize_rcu();
	}
}

static struct mc16_test_storage *
mc16_test_storage_begin_pages(struct kunit *test, unsigned int nr_pages)
{
	struct mc16_test_storage *f;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->owner = memcomp_test_objcg_begin(test, nr_pages);
	if (!f->owner)
		return NULL;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&f->baseline);
#endif
	f->probe.task = current;
	ret = kunit_add_action_or_reset(test, mc16_test_storage_cleanup, f);
	if (ret)
		return NULL;
	rcu_assign_pointer(mc16_storage_fixture, &f->probe);
	return f;
}

static struct mc16_test_storage *mc16_test_storage_begin(struct kunit *test)
{
	return mc16_test_storage_begin_pages(test, 2);
}

static int mc16_test_reserve(struct mc16_test_storage *f, unsigned int nr)
{
	struct mc16_record *record;
	int ret;

	if (f->nr + nr > MC16_TEST_REFS)
		return -ENOSPC;
	record = mc16_record_alloc(f->owner->folio, nr, false);
	if (IS_ERR(record))
		return PTR_ERR(record);
	ret = mc16_reserve(record, nr, MC16_KEY_ANON, &f->refs[f->nr]);
	if (ret) {
		mc16_record_abort(record);
		return ret;
	}
	f->nr += nr;
	return 0;
}

static void memcomp_test_reserve_boundaries(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct memcomp_ref guard;
	unsigned int i;
	u32 base;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 66; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	base = f->refs[0].token;
	KUNIT_EXPECT_EQ(test, base & (MC16_CLUSTER_SLOTS - 1), 0U);
	for (i = 0; i < 66; i++) {
		KUNIT_EXPECT_EQ(test, f->refs[i].token, base + i);
		KUNIT_EXPECT_PTR_EQ(test, f->refs[i].cluster, f->refs[0].cluster);
		KUNIT_ASSERT_TRUE(test, mc16_get(base + i, &guard));
		KUNIT_EXPECT_PTR_EQ(test, guard.slot, f->refs[i].slot);
		KUNIT_EXPECT_FALSE(test, refcount_dec_and_test(&guard.slot->refs));
	}
	KUNIT_EXPECT_FALSE(test, mc16_get(base + 66, &guard));
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 67);
}

static void memcomp_test_reserve_holes(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct memcomp_ref guard;
	struct mc16_slot *old;
	u32 base;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	base = f->refs[0].token;
	old = f->refs[0].slot;
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 2), 0);
	KUNIT_EXPECT_EQ(test, f->refs[1].token, base + 2);
	KUNIT_EXPECT_FALSE(test, mc16_get(base + 1, &guard));
	mc16_test_cancel_pending(&f->refs[0]);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	KUNIT_EXPECT_EQ(test, f->refs[3].token, base + 4);
	KUNIT_EXPECT_PTR_NE(test, f->refs[3].slot, old);
	KUNIT_EXPECT_FALSE(test, mc16_get(base, &guard));
}

static void mc16_test_segment_retire(struct kunit *test, bool fail_descriptor)
{
	struct mc16_test_storage *f;
	struct mc16_cluster *cluster;
	struct mc16_slot *old, *published;
	unsigned int i, bad_refs = 0, live;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 65; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	cluster = f->refs[64].cluster;
	old = cluster->segments[0];
	f->probe.fail_descriptor = fail_descriptor;
	rcu_read_lock();
	for (i = 0; i < 64; i++)
		mc16_test_cancel_pending(&f->refs[i]);
	live = cluster->live_slots;
	published = cluster->segments[0];
	for (i = 0; i < 64; i++)
		bad_refs += !!refcount_read(&old[i].refs);
	rcu_read_unlock();
	KUNIT_EXPECT_EQ(test, live, 1U);
	KUNIT_EXPECT_PTR_EQ(test, published, fail_descriptor ? old : NULL);
	KUNIT_EXPECT_EQ(test, bad_refs, 0U);
	rcu_barrier();
	mc16_test_cancel_pending(&f->refs[64]);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
}

static void memcomp_test_segment_retire(struct kunit *test)
{
	mc16_test_segment_retire(test, false);
}

static void memcomp_test_segment_retire_nomem(struct kunit *test)
{
	mc16_test_segment_retire(test, true);
}

static void memcomp_test_reserve_nomem(struct kunit *test)
{
	struct mc16_test_storage *f;
	unsigned int at;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (at = 1; at <= 2; at++) {
		f->probe.fail_at = f->probe.allocations + at;
		KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -ENOMEM);
		KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
		KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
		KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 1);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
	}
	f->probe.fail_at = 0;
	/* Record charge succeeds; the independent key charge fails. */
	f->owner->model.fail_at = atomic_read(&f->owner->model.charge_calls) + 2;
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -ENOMEM);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
	f->owner->model.fail_at = 0;
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	KUNIT_EXPECT_EQ(test, f->refs[0].token,
			(u32)BIT(MC16_CLUSTER_SHIFT));
}

struct mc16_test_rival {
	struct mc16_test_storage *f;
	int result;
};

static void mc16_test_rival_reserve(void *data)
{
	struct mc16_test_rival *rival = data;

	rival->result = mc16_test_reserve(rival->f, 1);
}

static void memcomp_test_reserve_race(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_test_rival rival;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	rival = (struct mc16_test_rival) { .f = f, .result = -EINPROGRESS };
	f->probe.allocation_window = mc16_test_rival_reserve;
	f->probe.window_data = &rival;
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -EAGAIN);
	KUNIT_EXPECT_EQ(test, rival.result, 0);
	KUNIT_EXPECT_EQ(test, f->nr, 1U);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 2);
}

static void memcomp_test_cluster_key_lifetime(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_key *key;
	unsigned int i;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 513; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	KUNIT_EXPECT_PTR_NE(test, f->refs[0].cluster, f->refs[512].cluster);
	key = f->refs[512].cluster->key;
	KUNIT_EXPECT_EQ(test, key->clusters, 2U);
	rcu_read_lock();
	for (i = 0; i < 512; i++)
		mc16_test_cancel_pending(&f->refs[i]);
	/* The queued old header still owns its key through the callback. */
	KUNIT_EXPECT_EQ(test, key->clusters, 2U);
	KUNIT_EXPECT_PTR_EQ(test, key->current_cluster, f->refs[512].cluster);
	rcu_read_unlock();
	rcu_barrier();
	KUNIT_EXPECT_EQ(test, key->clusters, 1U);
	mc16_test_cancel_pending(&f->refs[512]);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_read(&f->owner->model.get_calls), 514);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->owner->model.put_calls), 514);
}

static void memcomp_test_segment_reserve_race(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_test_rival rival;
	unsigned int i;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 64; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	rival = (struct mc16_test_rival) { .f = f, .result = -EINPROGRESS };
	f->probe.allocation_window = mc16_test_rival_reserve;
	f->probe.window_data = &rival;
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -EAGAIN);
	KUNIT_EXPECT_EQ(test, rival.result, 0);
	KUNIT_EXPECT_EQ(test, f->nr, 65U);
}

static void mc16_test_retire_and_reuse(void *data)
{
	struct mc16_test_rival *rival = data;
	struct mc16_test_storage *f = rival->f;
	unsigned int i;

	for (i = 0; i < f->nr; i++)
		mc16_test_cancel_pending(&f->refs[i]);
	rcu_barrier(); /* old key/header really disappear during preallocation */
	f->nr = 0;
	rival->result = mc16_test_reserve(f, 1);
}

static void memcomp_test_rollover_retire_race(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_test_rival rival;
	unsigned int i;
	u32 old_id;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 512; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	old_id = f->refs[0].cluster->id;
	rival = (struct mc16_test_rival) { .f = f, .result = -EINPROGRESS };
	f->probe.allocation_window = mc16_test_retire_and_reuse;
	f->probe.window_data = &rival;
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -EAGAIN);
	KUNIT_EXPECT_EQ(test, rival.result, 0);
	KUNIT_EXPECT_EQ(test, f->nr, 1U);
	KUNIT_EXPECT_EQ(test, f->refs[0].cluster->id, old_id);
	KUNIT_EXPECT_EQ(test, f->refs[0].cluster->next, (u16)1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 2);
}

static void memcomp_test_header_before_segment_callback(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_slot *old;
	unsigned int i;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 64; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	old = f->refs[0].cluster->segments[0];
	f->probe.hold_segments = true;
	for (i = 0; i < 64; i++)
		mc16_test_cancel_pending(&f->refs[i]);
	/* Pressure queues the retained header; segment callbacks stay delayed. */
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_NOT_NULL(test, f->probe.delayed_segments);
	for (i = 0; i < 64; i++)
		KUNIT_EXPECT_EQ(test, refcount_read(&old[i].refs), 0U);
	/* Actual callback and real grace, now after both header and key free. */
	mc16_test_drain_segments(&f->probe);
	rcu_barrier();
	KUNIT_EXPECT_PTR_EQ(test, f->probe.delayed_segments, NULL);
}

static int mc16_test_id_alloc_fail(u32 *id)
{
	return -ENOMEM;
}

static struct mc16_key *mc16_test_key_zalloc_fail(void)
{
	return NULL;
}

static void memcomp_test_reserve_id_failure(struct kunit *test)
{
	struct mc16_test_storage *f;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	kunit_activate_static_stub(test, mc16_private_id_alloc, mc16_test_id_alloc_fail);
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -ENOMEM);
	kunit_deactivate_static_stub(test, mc16_private_id_alloc);
	KUNIT_EXPECT_EQ(test, f->probe.allocations, 0U);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
}

static void memcomp_test_reserve_key_failure(struct kunit *test)
{
	struct mc16_test_storage *f;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	kunit_activate_static_stub(test, mc16_key_zalloc, mc16_test_key_zalloc_fail);
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(f, 1), -ENOMEM);
	kunit_deactivate_static_stub(test, mc16_key_zalloc);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 1);
}

#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_STORAGE_TEST_H */
