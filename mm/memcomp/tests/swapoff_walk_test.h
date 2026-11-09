/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SWAPOFF_WALK_TEST_H
#define _MM_MEMCOMP_TESTS_SWAPOFF_WALK_TEST_H

/*
 * Include after storage_test.h, refs_test.h and cache_store.h. The production
 * walker includes scan.h; no scan_test.h or legacy swapoff hooks are needed.
 * Register the cases in the main suite under CONFIG_MEMCG && CONFIG_SWAP.
 */
#include "../swapoff_walk.h"

#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)
#define MC16_WALK_TEST_FLAGS (PF_MEMALLOC_NOFS | PF_MEMALLOC)

enum mc16_test_walk_stop {
	MC16_WALK_STOP_NONE,
	MC16_WALK_STOP_BEFORE_SCAN,
	MC16_WALK_STOP_AFTER_SCAN,
	MC16_WALK_STOP_AFTER_RESTORE,
	MC16_WALK_STOP_UNLOCKED,
	MC16_WALK_STOP_EOF,
};

struct mc16_test_walk {
	struct mc16_test_refs *refs;
	struct mc16_swapoff_walk_ops ops;
	struct memcomp_ref private;
	u32 markers[MC16_SWAPOFF_SCAN_BATCH];
	unsigned int flushes, restores, yields, probes, slice, scans;
	unsigned int fail_flush, fail_restore, settled;
	u64 last_cursor, last_token;
	enum mc16_test_walk_stop stop;
	int actor_result, writer_error;
	bool interrupted, bad_scope, bad_ref, bad_budget, dirty_scan;
	bool private_active, staging, replace, retained, appended;
};

static void mc16_test_walk_cleanup(void *data)
{
	struct mc16_test_walk *f = data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(f->markers); i++)
		if (f->markers[i])
			xa_erase_irq(&mc16_clusters, f->markers[i]);
	if (f->private.slot) {
		if (f->private_active) {
			mc16_ref_put(&f->private);
		} else {
			mc16_release_resources(&f->private);
			mc16_retire_slot(&f->private);
		}
	}
}

static void mc16_test_walk_scope(struct mc16_test_walk *f)
{
	if (!mutex_is_locked(f->ops.lock) ||
	    (current->flags & MC16_WALK_TEST_FLAGS) != MC16_WALK_TEST_FLAGS)
		f->bad_scope = true;
}

/* Fixture owner refs are independent of the one borrowed by the walker. */
static void mc16_test_walk_refs(struct mc16_test_walk *f,
				const struct memcomp_ref *borrowed)
{
	struct mc16_test_storage *s = f->refs->storage;
	unsigned int i;

	for (i = 0; i < s->nr; i++) {
		struct memcomp_ref *owner = &s->refs[i];
		unsigned int expected = 1;

		if (!owner->slot)
			continue;
		if (borrowed && borrowed->slot == owner->slot)
			expected++;
		if (refcount_read(&owner->slot->refs) != expected)
			f->bad_ref = true;
	}
	if (f->private.slot) {
		unsigned int expected = f->private_active ? 1 : 0;

		if (borrowed && borrowed->slot == f->private.slot)
			expected++;
		if (refcount_read(&f->private.slot->refs) != expected)
			f->bad_ref = true;
	}
}

static int mc16_test_walk_private(struct mc16_test_walk *f)
{
	const struct mc16_initial_payload initial = { .word = 0x5a5a };
	struct mc16_test_storage *s = f->refs->storage;

	return mc16_reserve_cache(&s->owner->model.objcg,
		folio_nid(s->owner->folio), &initial, 1, &f->private);
}

static int mc16_test_walk_flush(void *data)
{
	struct mc16_test_walk *f = data;

	mc16_test_walk_scope(f);
	mc16_test_walk_refs(f, NULL);
	f->flushes++;
	if (f->flushes == f->fail_flush)
		return -EIO;
	/* Real private refs0 storage becomes scannable only at this flush. */
	if (f->staging) {
		mc16_cache_publish(&f->private, 0);
		f->private_active = true;
		f->staging = false;
		f->settled++;
	}
	return 0;
}

static int mc16_test_walk_restore(struct memcomp_ref *ref,
				  unsigned int type, void *data)
{
	struct mc16_test_walk *f = data;

	mc16_test_walk_scope(f);
	mc16_test_walk_refs(f, ref);
	f->restores++;
	if (type != 7 || !ref->slot || !ref->cluster ||
	    (u64)ref->token <= f->last_token)
		f->bad_ref = true;
	f->last_token = ref->token;
	if (f->staging)
		f->dirty_scan = true;
	if (f->stop == MC16_WALK_STOP_AFTER_RESTORE)
		f->interrupted = true;
	if (f->restores == f->fail_restore)
		return -ENOMEM;
	/* Zero models the actor rejecting a non-target, positive a restore. */
	return f->actor_result;
}

static bool mc16_test_walk_interrupted(void *data)
{
	struct mc16_test_walk *f = data;

	mc16_test_walk_scope(f);
	return f->interrupted;
}

static void mc16_test_walk_scanned(u64 cursor, unsigned int used,
				   enum mc16_scan_result result,
				   const struct memcomp_ref *ref, void *data)
{
	struct mc16_test_walk *f = data;

	mc16_test_walk_scope(f);
	mc16_test_walk_refs(f, ref);
	f->scans++;
	f->probes += used;
	f->slice += used;
	if (f->slice > MC16_SWAPOFF_SCAN_BATCH || cursor < f->last_cursor ||
	    (result == MC16_SCAN_GUARD && (!ref->slot || !used)) ||
	    (result != MC16_SCAN_GUARD && (ref->slot || ref->cluster)))
		f->bad_budget = true;
	f->last_cursor = cursor;
	if (f->staging)
		f->dirty_scan = true;
	if ((used && f->stop == MC16_WALK_STOP_AFTER_SCAN) ||
	    (result == MC16_SCAN_END && !used && f->stop == MC16_WALK_STOP_EOF))
		f->interrupted = true;
}

static void mc16_test_walk_unlocked(void *data)
{
	struct mc16_test_walk *f = data;
	struct mc16_test_storage *s = f->refs->storage;
	unsigned int i, nr = s->nr;
	u32 base = nr ? s->refs[0].token : 0;

	f->yields++;
	if (f->slice != MC16_SWAPOFF_SCAN_BATCH)
		f->bad_budget = true;
	f->slice = 0;
	if ((current->flags & MC16_WALK_TEST_FLAGS) != MC16_WALK_TEST_FLAGS)
		f->bad_scope = true;
	/* Enter the real W as a writer at the actual unlock boundary. */
	if (!mutex_trylock(f->ops.lock)) {
		f->bad_scope = true;
		return;
	}
	mc16_test_walk_refs(f, NULL);
	if (f->replace && f->yields == 1) {
		/* Retire the old slots; append without pressure or token reuse. */
		for (i = 0; i < nr; i++)
			mc16_ref_put(&s->refs[i]);
		f->retained = !xa_empty(&mc16_clusters);
		for (i = 0; i < nr; i++) {
			f->writer_error = mc16_test_refs_reserve(f->refs, 1);
			if (f->writer_error)
				goto out_unlock;
		}
		f->appended = s->refs[nr].token == base + nr;
		f->writer_error = mc16_test_walk_private(f);
		if (!f->writer_error)
			f->staging = true;
	}
out_unlock:
	mutex_unlock(f->ops.lock);
	if (f->stop == MC16_WALK_STOP_UNLOCKED)
		f->interrupted = true;
}

static struct mc16_test_walk *mc16_test_walk_begin(struct kunit *test)
{
	struct mc16_test_walk *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs || kunit_add_action_or_reset(test,
						  mc16_test_walk_cleanup, f))
		return NULL;
	f->actor_result = 1;
	f->ops = (struct mc16_swapoff_walk_ops) {
		.lock = &memcomp_writeback_lock,
		.flush = mc16_test_walk_flush,
		.restore = mc16_test_walk_restore,
		.interrupted = mc16_test_walk_interrupted,
		.scanned = mc16_test_walk_scanned,
		.unlocked = mc16_test_walk_unlocked,
		.ctx = f,
	};
	return f;
}

static int mc16_test_walk_reserve(struct mc16_test_walk *f, unsigned int nr)
{
	unsigned int i;
	int ret;

	for (i = 0; i < nr; i++) {
		ret = mc16_test_refs_reserve(f->refs, 1);
		if (ret)
			return ret;
	}
	return 0;
}

/* No fatal assertions while W or a modified task allocation scope is held. */
static int mc16_test_walk_run(struct kunit *test, struct mc16_test_walk *f,
			      unsigned int nested)
{
	unsigned int original = current->flags & MC16_WALK_TEST_FLAGS;
	unsigned int nofs = 0, noreclaim = 0, before, after;
	bool unlocked;
	int ret;

	if (nested & PF_MEMALLOC_NOFS)
		nofs = memalloc_nofs_save();
	if (nested & PF_MEMALLOC)
		noreclaim = memalloc_noreclaim_save();
	before = current->flags & MC16_WALK_TEST_FLAGS;
	ret = mc16_swapoff_walk(7, &f->ops);
	after = current->flags & MC16_WALK_TEST_FLAGS;
	if (nested & PF_MEMALLOC)
		memalloc_noreclaim_restore(noreclaim);
	if (nested & PF_MEMALLOC_NOFS)
		memalloc_nofs_restore(nofs);
	unlocked = mutex_trylock(f->ops.lock);
	if (unlocked) {
		mc16_test_walk_refs(f, NULL);
		mutex_unlock(f->ops.lock);
	}
	KUNIT_EXPECT_EQ(test, after, before);
	KUNIT_EXPECT_EQ(test, current->flags & MC16_WALK_TEST_FLAGS, original);
	KUNIT_EXPECT_TRUE(test, unlocked);
	KUNIT_EXPECT_FALSE(test, f->bad_scope);
	KUNIT_EXPECT_FALSE(test, f->bad_ref);
	KUNIT_EXPECT_FALSE(test, f->bad_budget);
	KUNIT_EXPECT_FALSE(test, f->dirty_scan);
	return ret;
}

static void mc16_test_walk_boundary(struct kunit *test, unsigned int nr,
				    unsigned int yields)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, nr), 0);
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->probes, nr);
	KUNIT_EXPECT_EQ(test, f->restores, nr);
	KUNIT_EXPECT_EQ(test, f->yields, yields);
	KUNIT_EXPECT_EQ(test, f->flushes, yields + 1);
	KUNIT_EXPECT_EQ(test, f->last_cursor, MC16_SCAN_END_CURSOR);
}

static void memcomp_test_swapoff_walk_empty(struct kunit *test)
{
	mc16_test_walk_boundary(test, 0, 0);
}

static void memcomp_test_swapoff_walk_31(struct kunit *test)
{
	mc16_test_walk_boundary(test, 31, 0);
}

static void memcomp_test_swapoff_walk_32(struct kunit *test)
{
	mc16_test_walk_boundary(test, 32, 1);
}

static void memcomp_test_swapoff_walk_33(struct kunit *test)
{
	mc16_test_walk_boundary(test, 33, 1);
}

static void memcomp_test_swapoff_walk_64(struct kunit *test)
{
	mc16_test_walk_boundary(test, 64, 2);
}

static void memcomp_test_swapoff_walk_non_target(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 65), 0);
	f->actor_result = 0;
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->probes, 65U);
	KUNIT_EXPECT_EQ(test, f->restores, 65U);
	KUNIT_EXPECT_EQ(test, f->yields, 2U);
	KUNIT_EXPECT_EQ(test, f->flushes, 3U);
}

static void memcomp_test_swapoff_walk_hole(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);
	struct mc16_test_storage *s;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f->refs, 2), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 29), 0);
	KUNIT_ASSERT_EQ(test, s->refs[1].token, s->refs[0].token + 2);
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->probes, 33U);
	KUNIT_EXPECT_EQ(test, f->restores, 32U);
	KUNIT_EXPECT_EQ(test, f->yields, 1U);
	KUNIT_EXPECT_EQ(test, f->flushes, 2U);
}

static void memcomp_test_swapoff_walk_retired(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);
	struct mc16_test_storage *s;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 33), 0);
	for (i = 0; i < 32; i++)
		mc16_ref_put(&s->refs[i]);
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->probes, 33U);
	KUNIT_EXPECT_EQ(test, f->restores, 1U);
	KUNIT_EXPECT_EQ(test, f->yields, 1U);
	KUNIT_EXPECT_EQ(test, f->flushes, 2U);
}

static void memcomp_test_swapoff_walk_segment_hole(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);
	struct mc16_test_storage *s;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	s->probe.hold_segments = true;
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 65), 0);
	for (i = 0; i < 64; i++)
		mc16_ref_put(&s->refs[i]);
	KUNIT_ASSERT_NULL(test, s->refs[64].cluster->segments[0]);
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->probes, 65U);
	KUNIT_EXPECT_EQ(test, f->restores, 1U);
	KUNIT_EXPECT_EQ(test, f->yields, 2U);
	KUNIT_EXPECT_EQ(test, f->flushes, 3U);
}

static void memcomp_test_swapoff_walk_private(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 31), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_walk_private(f), 0);
	KUNIT_ASSERT_EQ(test, refcount_read(&f->private.slot->refs), 0U);
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->probes, 32U);
	KUNIT_EXPECT_EQ(test, f->restores, 31U);
	KUNIT_EXPECT_EQ(test, f->yields, 1U);
	KUNIT_EXPECT_EQ(test, f->flushes, 2U);
	KUNIT_EXPECT_FALSE(test, f->private_active);
}

static void memcomp_test_swapoff_walk_markers(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);
	unsigned long flags;
	unsigned int i;
	u32 reserved;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < ARRAY_SIZE(f->markers); i++) {
		XA_STATE(xas, &mc16_clusters, i + 1);
		void *entry;

		xa_lock_irqsave(&mc16_clusters, flags);
		ret = __xa_alloc(&mc16_clusters, &reserved, NULL,
				 XA_LIMIT(i + 1, i + 1), GFP_NOWAIT | __GFP_NOWARN);
		if (!ret)
			f->markers[i] = reserved;
		xa_unlock_irqrestore(&mc16_clusters, flags);
		KUNIT_ASSERT_EQ(test, ret, 0);
		rcu_read_lock();
		entry = xas_load(&xas);
		rcu_read_unlock();
		KUNIT_ASSERT_TRUE(test, xa_is_zero(entry));
	}
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 1), 0);
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->probes, 33U);
	KUNIT_EXPECT_EQ(test, f->restores, 1U);
	KUNIT_EXPECT_EQ(test, f->yields, 1U);
	KUNIT_EXPECT_EQ(test, f->flushes, 2U);
}

static void memcomp_test_swapoff_walk_reflush(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 33), 0);
	f->replace = true;
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->writer_error, 0);
	KUNIT_EXPECT_TRUE(test, f->retained);
	KUNIT_EXPECT_TRUE(test, f->appended);
	KUNIT_EXPECT_TRUE(test, f->private_active);
	KUNIT_EXPECT_EQ(test, f->settled, 1U);
	/* 32 old guards, one retired position, 33 appended, one private. */
	KUNIT_EXPECT_EQ(test, f->probes, 67U);
	KUNIT_EXPECT_EQ(test, f->restores, 66U);
	KUNIT_EXPECT_EQ(test, f->yields, 2U);
	KUNIT_EXPECT_EQ(test, f->flushes, 3U);
}

/* Force the real allocator to publish a cluster at the last numeric ID. */
static int mc16_test_walk_last_id(u32 *id)
{
	return __xa_alloc(&mc16_clusters, id, NULL,
		XA_LIMIT(MC16_MAX_CLUSTER_ID, MC16_MAX_CLUSTER_ID),
		GFP_NOWAIT | __GFP_NOWARN);
}

static void memcomp_test_swapoff_walk_u32_max(struct kunit *test)
{
	struct mc16_test_walk *f = mc16_test_walk_begin(test);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	kunit_activate_static_stub(test, mc16_private_id_alloc,
				   mc16_test_walk_last_id);
	ret = mc16_test_walk_reserve(f, MC16_CLUSTER_SLOTS);
	kunit_deactivate_static_stub(test, mc16_private_id_alloc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test,
			f->refs->storage->refs[MC16_CLUSTER_SLOTS - 1].token, U32_MAX);
	KUNIT_EXPECT_EQ(test, mc16_test_walk_run(test, f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->last_cursor, MC16_SCAN_END_CURSOR);
	KUNIT_EXPECT_EQ(test, f->last_token, (u64)U32_MAX);
	KUNIT_EXPECT_EQ(test, f->probes, (unsigned int)MC16_CLUSTER_SLOTS);
	KUNIT_EXPECT_EQ(test, f->restores, (unsigned int)MC16_CLUSTER_SLOTS);
	/* Known EOF at the final token does not reflush an empty batch. */
	KUNIT_EXPECT_EQ(test, f->yields,
			(unsigned int)MC16_CLUSTER_SLOTS / MC16_SWAPOFF_SCAN_BATCH - 1);
	KUNIT_EXPECT_EQ(test, f->flushes, f->yields + 1);
}

/* Reuse the real slots, resetting observers only, for each exit/flag pair. */
static void mc16_test_walk_reset(struct mc16_test_walk *f)
{
	f->flushes = 0;
	f->restores = 0;
	f->yields = 0;
	f->probes = 0;
	f->slice = 0;
	f->scans = 0;
	f->last_cursor = 0;
	f->last_token = 0;
	f->bad_scope = false;
	f->bad_ref = false;
	f->bad_budget = false;
	f->dirty_scan = false;
	f->interrupted = false;
	f->stop = MC16_WALK_STOP_NONE;
	f->fail_flush = 0;
	f->fail_restore = 0;
}

static void memcomp_test_swapoff_walk_exits(struct kunit *test)
{
	static const struct {
		unsigned int flush, restore;
		enum mc16_test_walk_stop stop;
		int result;
		unsigned int probes, restores, flushes, yields;
	} cases[] = {
		{ 1, 0, MC16_WALK_STOP_NONE, -EIO, 0, 0, 1, 0 },
		{ 2, 0, MC16_WALK_STOP_NONE, -EIO, 32, 32, 2, 1 },
		{ 0, 3, MC16_WALK_STOP_NONE, -ENOMEM, 3, 3, 1, 0 },
		{ 1, 0, MC16_WALK_STOP_BEFORE_SCAN, -EIO, 0, 0, 1, 0 },
		{ 0, 1, MC16_WALK_STOP_AFTER_RESTORE, -ENOMEM, 1, 1, 1, 0 },
		{ 0, 0, MC16_WALK_STOP_BEFORE_SCAN, -EINTR, 0, 0, 1, 0 },
		{ 0, 0, MC16_WALK_STOP_AFTER_SCAN, -EINTR, 1, 0, 1, 0 },
		{ 0, 0, MC16_WALK_STOP_AFTER_RESTORE, -EINTR, 1, 1, 1, 0 },
		{ 0, 0, MC16_WALK_STOP_UNLOCKED, -EINTR, 32, 32, 2, 1 },
		{ 0, 0, MC16_WALK_STOP_NONE, 0, 33, 33, 2, 1 },
		{ 0, 0, MC16_WALK_STOP_EOF, 0, 33, 33, 2, 1 },
	};
	static const unsigned int nested[] = {
		0, PF_MEMALLOC_NOFS, PF_MEMALLOC, MC16_WALK_TEST_FLAGS,
	};
	struct mc16_test_walk *f = mc16_test_walk_begin(test);
	unsigned int i, j;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_walk_reserve(f, 33), 0);
	for (i = 0; i < ARRAY_SIZE(nested); i++) {
		for (j = 0; j < ARRAY_SIZE(cases); j++) {
			mc16_test_walk_reset(f);
			f->fail_flush = cases[j].flush;
			f->fail_restore = cases[j].restore;
			f->stop = cases[j].stop;
			f->interrupted = f->stop == MC16_WALK_STOP_BEFORE_SCAN;
			ret = mc16_test_walk_run(test, f, nested[i]);
			KUNIT_EXPECT_EQ(test, ret, cases[j].result);
			KUNIT_EXPECT_EQ(test, f->probes, cases[j].probes);
			KUNIT_EXPECT_EQ(test, f->restores, cases[j].restores);
			KUNIT_EXPECT_EQ(test, f->flushes, cases[j].flushes);
			KUNIT_EXPECT_EQ(test, f->yields, cases[j].yields);
		}
	}
}
#endif /* CONFIG_MEMCG && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_SWAPOFF_WALK_TEST_H */
