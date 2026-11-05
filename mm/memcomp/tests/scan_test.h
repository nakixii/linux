/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SCAN_TEST_H
#define _MM_MEMCOMP_TESTS_SCAN_TEST_H

/* Include after storage/refs fixtures and cache_store.h; register in main. */
#include "../scan.h"

#ifdef CONFIG_MEMCG
struct mc16_test_scan {
	struct mc16_test_refs *refs;
	struct memcomp_ref guard;
	struct memcomp_ref cache;
	u32 markers[2];
	bool active;
};

static void mc16_test_scan_cleanup(void *data)
{
	struct mc16_test_scan *f = data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(f->markers); i++)
		if (f->markers[i])
			xa_erase_irq(&mc16_clusters, f->markers[i]);
	if (f->guard.slot)
		mc16_ref_put(&f->guard);
	if (f->cache.slot) {
		if (f->active) {
			mc16_ref_put(&f->cache);
		} else {
			mc16_release_resources(&f->cache);
			mc16_retire_slot(&f->cache);
		}
	}
}

static struct mc16_test_scan *mc16_test_scan_begin(struct kunit *test)
{
	struct mc16_test_scan *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs || kunit_add_action_or_reset(test,
						  mc16_test_scan_cleanup, f))
		return NULL;
	return f;
}

/* The real reservation transaction still publishes/charges the cluster. */
static int mc16_test_scan_last_id(u32 *id)
{
	return __xa_alloc(&mc16_clusters, id, NULL,
		XA_LIMIT(MC16_MAX_CLUSTER_ID, MC16_MAX_CLUSTER_ID),
		GFP_NOWAIT | __GFP_NOWARN);
}

/* Reserve a real XArray zero entry, not a published refs-zero cache slot. */
static int mc16_test_scan_marker(struct mc16_test_scan *f,
				 unsigned int which, u32 id)
{
	unsigned long flags;
	u32 reserved;
	int ret;

	xa_lock_irqsave(&mc16_clusters, flags);
	ret = __xa_alloc(&mc16_clusters, &reserved, NULL, XA_LIMIT(id, id),
			 GFP_NOWAIT | __GFP_NOWARN);
	if (!ret)
		f->markers[which] = reserved;
	xa_unlock_irqrestore(&mc16_clusters, flags);
	return ret;
}

static int mc16_test_scan_private(struct mc16_test_scan *f)
{
	const struct mc16_initial_payload initial = { .word = 0x5a5a };
	struct mc16_test_storage *s = f->refs->storage;

	return mc16_reserve_cache(&s->owner->model.objcg,
		folio_nid(s->owner->folio), &initial, 1, &f->cache);
}

static void memcomp_test_scan_empty_budget(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	unsigned int scanned = 99;
	u64 cursor = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 0, &f->guard,
					     &scanned), MC16_SCAN_BUDGET);
	KUNIT_EXPECT_EQ(test, cursor, 0ULL);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, cursor, MC16_SCAN_END_CURSOR);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
	cursor = U64_MAX;
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 0, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, cursor, MC16_SCAN_END_CURSOR);
}

static void memcomp_test_scan_reserved_markers(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	unsigned int scanned, i;
	u64 cursor = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_scan_marker(f, 0, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_scan_marker(f, 1, 2), 0);
	/* A real later cluster distinguishes yield from hidden-marker skip. */
	kunit_activate_static_stub(test, mc16_private_id_alloc,
				   mc16_test_scan_last_id);
	ret = mc16_test_scan_private(f);
	kunit_deactivate_static_stub(test, mc16_private_id_alloc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	mc16_cache_publish(&f->cache, 0);
	f->active = true;
	for (i = 0; i < ARRAY_SIZE(f->markers); i++) {
		XA_STATE(xas, &mc16_clusters, f->markers[i]);
		void *entry;

		rcu_read_lock();
		entry = xas_load(&xas);
		rcu_read_unlock();
		KUNIT_ASSERT_TRUE(test, xa_is_zero(entry));
		KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
						     &scanned), MC16_SCAN_BUDGET);
		KUNIT_EXPECT_EQ(test, scanned, 1U);
		KUNIT_EXPECT_EQ(test, cursor,
				((u64)f->markers[i] + 1) << MC16_CLUSTER_SHIFT);
		KUNIT_EXPECT_NULL(test, f->guard.slot);
		KUNIT_EXPECT_NULL(test, f->guard.cluster);
	}
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, f->cache.slot);
	mc16_ref_put(&f->guard);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
}

static void memcomp_test_scan_reserved_last(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	unsigned int scanned;
	u64 cursor = U32_MAX;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_scan_marker(f, 0,
						    MC16_MAX_CLUSTER_ID), 0);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_EQ(test, cursor, MC16_SCAN_END_CURSOR);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
}

static void memcomp_test_scan_first(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	struct memcomp_ref *owner;
	enum mc16_scan_result result;
	unsigned int scanned;
	u64 cursor = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 1), 0);
	owner = &f->refs->storage->refs[0];
	KUNIT_ASSERT_EQ(test, owner->token, (u32)MC16_CLUSTER_SLOTS);
	/* Already holding T checks that scanning does not reacquire it. */
	mutex_lock(memcomp_lock_of(owner->token));
	result = mc16_scan_next(&cursor, 1, &f->guard, &scanned);
	mutex_unlock(memcomp_lock_of(owner->token));
	KUNIT_ASSERT_EQ(test, result, MC16_SCAN_GUARD);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, owner->slot);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_EQ(test, cursor, (u64)owner->token + 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&owner->slot->refs), 2U);
	mc16_ref_put(&f->guard);
	KUNIT_EXPECT_EQ(test, refcount_read(&owner->slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
}

static void memcomp_test_scan_last_guard(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	unsigned int scanned;
	u64 cursor = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	mc16_ref_put(&f->refs->storage->refs[0]);
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), 1U);
	mc16_ref_put(&f->guard);
	/* The guard is gone, but the appendable current header is retained. */
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
}

static void memcomp_test_scan_alignment(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	struct mc16_test_storage *s;
	unsigned int scanned;
	u64 cursor, hole;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 2), 0);
	hole = (u64)s->refs[0].token + 1;
	cursor = hole;
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_BUDGET);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_EQ(test, cursor, hole + 1);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 0, &f->guard,
					     &scanned), MC16_SCAN_BUDGET);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
	KUNIT_EXPECT_EQ(test, cursor, hole + 1);
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, s->refs[1].slot);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
}

static void memcomp_test_scan_refs_zero(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	struct mc16_test_storage *s;
	unsigned int scanned;
	u64 cursor;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 2), 0);
	cursor = s->refs[0].token;
	mc16_ref_put(&s->refs[0]);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_BUDGET);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, s->refs[1].slot);
}

static void memcomp_test_scan_segment_retired(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	struct mc16_test_storage *s;
	unsigned int scanned, i;
	u64 cursor, base;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	for (i = 0; i <= MC16_SEGMENT_SLOTS; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 1), 0);
	base = s->refs[0].token;
	s->probe.hold_segments = true;
	for (i = 0; i < MC16_SEGMENT_SLOTS; i++)
		mc16_ref_put(&s->refs[i]);
	KUNIT_ASSERT_NULL(test,
			  s->refs[MC16_SEGMENT_SLOTS].cluster->segments[0]);
	cursor = base;
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 3, &f->guard,
					     &scanned), MC16_SCAN_BUDGET);
	KUNIT_EXPECT_EQ(test, scanned, 3U);
	KUNIT_EXPECT_EQ(test, cursor, base + 3);
	cursor = base + MC16_SEGMENT_SLOTS - 1;
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 2, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	KUNIT_EXPECT_EQ(test, scanned, 2U);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot,
			    s->refs[MC16_SEGMENT_SLOTS].slot);
	mc16_ref_put(&f->guard);
	mc16_ref_put(&s->refs[MC16_SEGMENT_SLOTS]);
	cursor = base;
	/* The empty appendable header still consumes the scan's ID budget. */
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_BUDGET);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	mc16_test_reclaim_retained();
	cursor = base;
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
	/* Cleanup drains both real RCU retirement paths. */
}

static void memcomp_test_scan_private(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	unsigned int scanned;
	u64 cursor;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_scan_private(f), 0);
	cursor = f->cache.token;
	KUNIT_EXPECT_EQ(test, refcount_read(&f->cache.slot->refs), 0U);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_BUDGET);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	mc16_cache_publish(&f->cache, 0);
	f->active = true;
	cursor = f->cache.token;
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, f->cache.slot);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->cache.slot->refs), 2U);
}

static void memcomp_test_scan_sparse(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	unsigned int scanned;
	u64 cursor;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 1), 0);
	kunit_activate_static_stub(test, mc16_private_id_alloc,
				   mc16_test_scan_last_id);
	ret = mc16_test_scan_private(f);
	kunit_deactivate_static_stub(test, mc16_private_id_alloc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	mc16_cache_publish(&f->cache, 0);
	f->active = true;
	KUNIT_ASSERT_EQ(test, f->cache.cluster->id, (u32)MC16_MAX_CLUSTER_ID);
	cursor = (u64)f->refs->storage->refs[0].token + 1;
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, f->cache.slot);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
}

static void memcomp_test_scan_u32_max(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	struct mc16_test_storage *s;
	unsigned int scanned, i;
	u64 cursor = U32_MAX;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	kunit_activate_static_stub(test, mc16_private_id_alloc,
				   mc16_test_scan_last_id);
	ret = mc16_test_refs_reserve(f->refs, 1);
	kunit_deactivate_static_stub(test, mc16_private_id_alloc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 1; i < MC16_CLUSTER_SLOTS; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 1), 0);
	KUNIT_ASSERT_EQ(test, s->refs[MC16_CLUSTER_SLOTS - 1].token, U32_MAX);
	KUNIT_ASSERT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_GUARD);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
	KUNIT_EXPECT_EQ(test, cursor, MC16_SCAN_END_CURSOR);
	KUNIT_EXPECT_EQ(test, f->guard.token, U32_MAX);
	mc16_ref_put(&f->guard);
	KUNIT_EXPECT_EQ(test, mc16_scan_next(&cursor, 1, &f->guard,
					     &scanned), MC16_SCAN_END);
	KUNIT_EXPECT_EQ(test, scanned, 0U);
}

static void memcomp_test_scan_w_relookup(struct kunit *test)
{
	struct mc16_test_scan *f = mc16_test_scan_begin(test);
	struct mc16_test_storage *s;
	enum mc16_scan_result result;
	unsigned int scanned;
	u64 cursor = 0;
	u32 token;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 2), 0);
	token = s->refs[0].token;
	mutex_lock(&memcomp_writeback_lock);
	result = mc16_scan_next(&cursor, 1, &f->guard, &scanned);
	if (f->guard.slot)
		mc16_ref_put(&f->guard);
	mutex_unlock(&memcomp_writeback_lock);
	KUNIT_ASSERT_EQ(test, result, MC16_SCAN_GUARD);
	KUNIT_ASSERT_EQ(test, cursor, (u64)token + 1);
	mc16_ref_put(&s->refs[0]);
	mc16_ref_put(&s->refs[1]);
	/* Explicit pressure enables numeric ID reuse for this relookup case. */
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	mc16_test_reclaim_retained();
	KUNIT_ASSERT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 2), 0);
	KUNIT_ASSERT_EQ(test, s->refs[2].token, token);
	mutex_lock(&memcomp_writeback_lock);
	result = mc16_scan_next(&cursor, 1, &f->guard, &scanned);
	mutex_unlock(&memcomp_writeback_lock);
	KUNIT_ASSERT_EQ(test, result, MC16_SCAN_GUARD);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, s->refs[3].slot);
	KUNIT_EXPECT_EQ(test, scanned, 1U);
}

#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_SCAN_TEST_H */
