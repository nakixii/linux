/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SHARED_TEST_H
#define _MM_MEMCOMP_TESTS_SHARED_TEST_H

#include <linux/sched/mm.h>

/* Cleanup runs in a kthread with no mm; madvise needs the target mm. */
#ifdef CONFIG_ADVISE_SYSCALLS
static int mc16_test_dontneed(struct mm_struct *mm, unsigned long start,
			      size_t len)
{
	bool attach = current->mm != mm;
	int ret;

	if (attach)
		kthread_use_mm(mm);
	ret = do_madvise(mm, start, len, MADV_DONTNEED);
	if (attach)
		kthread_unuse_mm(mm);
	return ret;
}
#endif

/* Test-only shared services. No entry80 registry, object or fallback. */
#define MEMCOMP_TEST_HUF_CACHE_PAGES 130
#define MEMCOMP_TEST_USER_PATTERN 0x6d656d636f6d7072UL

struct memcomp_test_real_page_record {
	u64 seq;
	u64 jiffies_snapshot;
	unsigned long totalram;
	unsigned int online_cpus;
	u32 mix;
	char comm[TASK_COMM_LEN];
	struct new_utsname uts;
	char banner[128];
};

struct memcomp_test_zstd_page_case {
	const char *name;
	void (*fill)(u8 *buf);
	unsigned int max_length;
	bool allow_incompressible;
};

/* Defined in storage_test.h; async teardown is included before that header. */
static void mc16_test_reclaim_retained(void);

#ifdef CONFIG_MEMCG
/* Identity-only objcg model. Payload/storage allocations remain real.
 * Descendant slot actions release all slots before this parent action.
 */
struct memcomp_test_objcg_fixture {
	struct memcomp_kunit_objcg model;
	struct kunit *test;
	struct folio *folio;
	unsigned int nr_pages;
};

static void memcomp_test_objcg_cleanup(void *data)
{
	struct memcomp_test_objcg_fixture *f = data;

	/* All descendant slots are gone; reclaim before unregistering objcg. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(f->test, atomic_long_read(&f->model.charged), 0L);
	KUNIT_EXPECT_EQ(f->test, atomic_read(&f->model.put_calls),
			atomic_read(&f->model.get_calls));
	if (rcu_access_pointer(memcomp_kunit_objcg) == &f->model) {
		rcu_assign_pointer(memcomp_kunit_objcg, NULL);
		synchronize_rcu();
	}
	if (f->folio) {
		folio_unlock(f->folio);
		folio_put(f->folio);
		f->folio = NULL;
	}
}

static struct memcomp_test_objcg_fixture *
memcomp_test_objcg_begin(struct kunit *test, unsigned int nr_pages)
{
	struct memcomp_test_objcg_fixture *f;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return NULL;
	}
	rcu_barrier();
	if (rcu_access_pointer(memcomp_kunit_objcg)) {
		KUNIT_FAIL(test, "objcg model already registered");
		return NULL;
	}
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->nr_pages = nr_pages;
	f->model.task = current;
	f->model.error = -ENOMEM;
	if (kunit_add_action_or_reset(test, memcomp_test_objcg_cleanup, f))
		return NULL;
	f->folio = folio_alloc(GFP_KERNEL, nr_pages == 2 ? 1 : 0);
	if (!f->folio)
		return NULL;
	folio_lock(f->folio);
	f->model.folio = f->folio;
	rcu_assign_pointer(memcomp_kunit_objcg, &f->model);
	return f;
}
#endif /* CONFIG_MEMCG */

/* Guard owns the slot; only T permits inspecting its current record. */
static bool memcomp_test_head_holds_anon_vma(unsigned long token)
{
	struct memcomp_ref ref = { };
	struct mc16_view view;
	bool held = false;

	if (!memcomp_entry_get_ref(token, &ref))
		return false;
	mutex_lock(memcomp_lock_of(ref.token));
	if (mc16_view_locked(&ref, &view) && view.record)
		held = view.record->record_flags & MC16_RECORD_ANON_VMA;
	mutex_unlock(memcomp_lock_of(ref.token));
	memcomp_entry_put_ref(&ref);
	return held;
}

static bool memcomp_test_store_after_unmap(struct folio *folio)
{
	struct memcomp_reclaim_ctx *ctx;
	bool ret;

	ctx = memcomp_get_comp_ctx(MEMCOMP_PAYLOAD_MUST_SUCCEED, NULL,
				   MEMCOMP_RECLAIM_DIRECT);
	if (!ctx)
		return false;
	ret = memcomp_store_after_unmap(folio, &ctx);
	WARN_ON_ONCE(ctx);
	return ret;
}

static void memcomp_test_initial_compression_placement(struct kunit *test)
{
	const enum memcomp_placement_stat compress_stat =
		MEMCOMP_PLACEMENT_STAT_COMPRESS;
	const enum memcomp_placement_stat fallback_stat =
		MEMCOMP_PLACEMENT_STAT_NORMAL_SWAP_FALLBACK;
	u64 compress;
	u64 fallback;
	u64 observed;

	compress = memcomp_placement_stat_read(compress_stat);
	fallback = memcomp_placement_stat_read(fallback_stat);

	memcomp_count_initial_compress();
	memcomp_count_normal_swap_fallback();

	observed = memcomp_placement_stat_read(compress_stat);
	KUNIT_EXPECT_EQ(test, observed, compress + 1);
	observed = memcomp_placement_stat_read(fallback_stat);
	KUNIT_EXPECT_EQ(test, observed, fallback + 1);
}

static void memcomp_test_writeback_pressure_scope(struct kunit *test)
{
	const int nid = 0;
	int scopes;
	int observed;

	scopes = atomic_read(&memcomp_writeback_scopes[nid]);
	memcomp_reclaim_writeback_begin(-1, 0);
	memcomp_reclaim_writeback_end(-1);
	observed = atomic_read(&memcomp_writeback_scopes[nid]);
	KUNIT_EXPECT_EQ(test, observed, scopes);

	memcomp_reclaim_writeback_begin(nid, 0);
	observed = atomic_read(&memcomp_writeback_scopes[nid]);
	KUNIT_EXPECT_EQ(test, observed, scopes + 1);
	KUNIT_EXPECT_TRUE(test, memcomp_writeback_scope_active(nid));

	memcomp_reclaim_writeback_begin(nid, 0);
	observed = atomic_read(&memcomp_writeback_scopes[nid]);
	KUNIT_EXPECT_EQ(test, observed, scopes + 2);
	memcomp_reclaim_writeback_end(nid);
	observed = atomic_read(&memcomp_writeback_scopes[nid]);
	KUNIT_EXPECT_EQ(test, observed, scopes + 1);
	memcomp_reclaim_writeback_end(nid);
	observed = atomic_read(&memcomp_writeback_scopes[nid]);
	KUNIT_EXPECT_EQ(test, observed, scopes);
}

static void memcomp_test_writeback_drain_recovery_applied(struct kunit *test)
{
	const int nid = 0;
	unsigned int before;
	unsigned int seeded;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	/*
	 * The decay only fires for a node that has climbed back above its low
	 * watermark.  A test host under real pressure would take the other
	 * branch, so check rather than assume.
	 */
	if (memcomp_writeback_node_below_low(nid, 0)) {
		kunit_skip(test, "node 0 is below its low watermark");
		return;
	}
	KUNIT_ASSERT_FALSE(test, memcomp_writeback_scope_active(nid));

	before = READ_ONCE(memcomp_writeback_drain[nid]);
	seeded = MEMCOMP_WRITEBACK_DRAIN_MAX;

	memcomp_reclaim_writeback_begin(nid, 0);
	WRITE_ONCE(memcomp_writeback_drain[nid], seeded);
	WRITE_ONCE(memcomp_writeback_feedback_valid[nid], true);
	memcomp_reclaim_writeback_end(nid);

	/* The last scope closing is what applies it, and exactly once. */
	KUNIT_EXPECT_EQ(test, READ_ONCE(memcomp_writeback_drain[nid]),
			memcomp_writeback_drain_recover(seeded));
	/* A decayed share is not a measured one, so the sample is dropped. */
	KUNIT_EXPECT_FALSE(test,
			   READ_ONCE(memcomp_writeback_feedback_valid[nid]));

	/*
	 * And a nested scope leaves it alone until the outermost one ends.
	 * Seeded inside the outer scope: its grant runs on a pool below the
	 * level gate, which drops the share by design.
	 */
	memcomp_reclaim_writeback_begin(nid, 0);
	WRITE_ONCE(memcomp_writeback_drain[nid], seeded);
	memcomp_reclaim_writeback_begin(nid, 0);
	memcomp_reclaim_writeback_end(nid);
	KUNIT_EXPECT_EQ(test, READ_ONCE(memcomp_writeback_drain[nid]),
			seeded);
	memcomp_reclaim_writeback_end(nid);
	KUNIT_EXPECT_EQ(test, READ_ONCE(memcomp_writeback_drain[nid]),
			memcomp_writeback_drain_recover(seeded));

	WRITE_ONCE(memcomp_writeback_drain[nid], before);
	WRITE_ONCE(memcomp_writeback_feedback_valid[nid], false);
}

static void memcomp_test_writeback_feedback(struct kunit *test)
{
	enum memcomp_writeback_feedback feedback;
	const u64 sample = MEMCOMP_WRITEBACK_FEEDBACK_MIN_BYTES;
	const u64 half_sample = sample / 2;

	/* No source sample is never converted into a backing-I/O request. */
	feedback = memcomp_writeback_feedback_values(0, 0, 0, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, feedback, MEMCOMP_WRITEBACK_PENDING);

	feedback = memcomp_writeback_feedback_values(0, 0, PAGE_SIZE, 0);
	KUNIT_EXPECT_EQ(test, feedback, MEMCOMP_WRITEBACK_PENDING);

	feedback = memcomp_writeback_feedback_values(0, 0, sample, 0);
	KUNIT_EXPECT_EQ(test, feedback, MEMCOMP_WRITEBACK_EFFECTIVE);

	feedback = memcomp_writeback_feedback_values(0, 0, sample, half_sample);
	KUNIT_EXPECT_EQ(test, feedback, MEMCOMP_WRITEBACK_EFFECTIVE);

	feedback = memcomp_writeback_feedback_values(0, 0, sample, sample);
	KUNIT_EXPECT_EQ(test, feedback, MEMCOMP_WRITEBACK_INEFFICIENT);
}

static void memcomp_test_writeback_budget_values(struct kunit *test)
{
	const unsigned int over = MEMCOMP_WRITEBACK_DRAIN_DEADBAND +
				  MEMCOMP_WRITEBACK_FRAC_ONE / 64;
	const unsigned long cold = 4096UL;

	/* No share, no writeback, however many payloads are cold. */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_budget_values(cold, 0), 0UL);

	/*
	 * Nothing is granted until the share clears the deadband, so the few
	 * scopes a boot or a short allocation burst opens write nothing back.
	 */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(cold,
							MEMCOMP_WRITEBACK_DRAIN_DEADBAND),
			0UL);
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(cold,
							MEMCOMP_WRITEBACK_DRAIN_DEADBAND - 1),
			0UL);
	KUNIT_EXPECT_GT(test,
			memcomp_writeback_budget_values(cold,
							MEMCOMP_WRITEBACK_DRAIN_DEADBAND + 1),
			0UL);

	/*
	 * The ceiling sits a deadband above half so the top of the ramp still
	 * grants half of the cold payloads.
	 */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(cold,
							MEMCOMP_WRITEBACK_DRAIN_MAX),
			cold / 2);

	/* Above the deadband the share is a fraction of the cold payloads. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(cold, over),
			cold / 64);

	/*
	 * Past the deadband a small share must not round down to "never write
	 * back" while a whole batch of cold payloads is still in the pool.
	 */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(MEMCOMP_WRITEBACK_BATCH,
							over),
			1UL);

	/* Below one batch there is nothing worth draining. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(MEMCOMP_WRITEBACK_BATCH - 1,
							over),
			0UL);
}

static void memcomp_test_writeback_level_floor(struct kunit *test)
{
	const u64 total = 15ULL << 30;
	const u64 target = total * MEMCOMP_WRITEBACK_LEVEL_PERMILLE / 1000;
	const unsigned long cold = 4096UL;
	unsigned int floor;
	u64 pool;
	int i;

	/* At or under the level, the pool keeps everything. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_level_floor_values(target, total),
			0U);
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_level_floor_values(target / 2, total),
			0U);
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_level_floor_values(0, total), 0U);

	/* Just over the line asks for a little, not for half. */
	floor = memcomp_writeback_level_floor_values(target + target / 64,
						     total);
	KUNIT_EXPECT_GT(test, memcomp_writeback_budget_values(cold, floor),
			0UL);
	KUNIT_EXPECT_LT(test, memcomp_writeback_budget_values(cold, floor),
			cold / 16);

	/* Far over the line is capped at half, so one scope cannot empty it. */
	floor = memcomp_writeback_level_floor_values(target * 100, total);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_budget_values(cold, floor),
			cold / 2);

	/*
	 * Monotone in the footprint below the cap.  The excess fraction
	 * reaches half at twice the target, so anything from there up is
	 * saturated and equal by design.
	 */
	KUNIT_EXPECT_LT(test,
			memcomp_writeback_level_floor_values(target + target / 8,
							     total),
			memcomp_writeback_level_floor_values(target + target / 2,
							     total));
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_level_floor_values(target * 2, total),
			memcomp_writeback_level_floor_values(target * 4, total));

	/*
	 * The error closes.  Draining the asked-for share removes about that
	 * share of the footprint, so iterating has to converge on the target
	 * and then stop - this is the property whose absence drained 99% of
	 * the pool into swap.
	 */
	pool = target * 8;
	for (i = 0; i < 64; i++) {
		floor = memcomp_writeback_level_floor_values(pool, total);
		if (!floor)
			break;
		pool -= div64_u64(pool * (floor - MEMCOMP_WRITEBACK_DRAIN_DEADBAND),
				  MEMCOMP_WRITEBACK_FRAC_ONE);
	}
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_level_floor_values(pool, total), 0U);
	KUNIT_EXPECT_GT(test, pool, target / 2);
	KUNIT_EXPECT_LE(test, pool, target);

	/* Unknown total RAM leaves the loop in sole charge rather than guessing. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_level_floor_values(target * 4, 0),
			0U);

	/* Scale-free: the same ratio gives the same share on any machine. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_level_floor_values(target * 4, total),
			memcomp_writeback_level_floor_values(target, total / 4));
}

static void memcomp_test_writeback_gate_values(struct kunit *test)
{
	const unsigned long total = (15UL << 30) >> PAGE_SHIFT;
	const unsigned long start = total * MEMCOMP_WRITEBACK_GATE_START_PERMILLE / 1000;
	const unsigned long stop = total * MEMCOMP_WRITEBACK_GATE_STOP_PERMILLE / 1000;
	const u64 total_bytes = (u64)total << PAGE_SHIFT;
	const u64 fp_start = total_bytes * MEMCOMP_WRITEBACK_GATE_FP_START_PERMILLE / 1000;
	const u64 fp_stop = total_bytes * MEMCOMP_WRITEBACK_GATE_FP_STOP_PERMILLE / 1000;
	/* A 3.5:1 pool, whose footprint never reaches the backstop. */
	u64 fp = ((u64)start << PAGE_SHIFT) * 2 / 7;
	bool open = false;
	unsigned long excess;

	KUNIT_ASSERT_LT(test, fp, fp_stop);

	/* Just under the start point writes nothing, however often asked. */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_gate_values(&open, start - 1,
							    fp, total), 0UL);
	KUNIT_EXPECT_FALSE(test, open);

	/* At the start point it opens, and owes what is above the close point. */
	excess = memcomp_writeback_gate_values(&open, start, fp, total);
	KUNIT_EXPECT_TRUE(test, open);
	KUNIT_EXPECT_EQ(test, excess, start - stop);

	/* Inside the band it stays open: that is the hysteresis. */
	excess = memcomp_writeback_gate_values(&open, stop + 1, fp, total);
	KUNIT_EXPECT_TRUE(test, open);
	KUNIT_EXPECT_EQ(test, excess, 1UL);

	/* At the close point it closes and owes nothing. */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_gate_values(&open, stop, fp,
							    total), 0UL);
	KUNIT_EXPECT_FALSE(test, open);

	/* And a closed gate inside the band stays closed. */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_gate_values(&open, start - 1,
							    fp, total), 0UL);
	KUNIT_EXPECT_FALSE(test, open);

	/*
	 * Incompressible data: the footprint reaches a quarter of RAM long
	 * before the entries reach two thirds, and the backstop opens it.
	 * What it owes is the footprint's excess share of the entries.
	 */
	excess = memcomp_writeback_gate_values(&open, stop / 2, fp_start,
					       total);
	KUNIT_EXPECT_TRUE(test, open);
	KUNIT_EXPECT_EQ(test, excess,
			(unsigned long)div64_u64((u64)(stop / 2) *
						 (fp_start - fp_stop), fp_start));
	/* It closes once both terms are back under their close points. */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_gate_values(&open, stop / 2,
							    fp_stop, total), 0UL);
	KUNIT_EXPECT_FALSE(test, open);

	/* No RAM and no pool are both no reason to write anything. */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_gate_values(&open, start, fp,
							    0), 0UL);
	KUNIT_EXPECT_FALSE(test, open);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_gate_values(&open, 0,
							    fp_start, total), 0UL);
	KUNIT_EXPECT_FALSE(test, open);

	/* Scale-free: the same fractions on any machine. */
	open = false;
	excess = memcomp_writeback_gate_values(&open, start / 4 - 1, fp / 4,
					       total / 4);
	KUNIT_EXPECT_EQ(test, excess, 0UL);
	KUNIT_EXPECT_FALSE(test, open);
}

static void memcomp_test_writeback_drain_loop(struct kunit *test)
{
	unsigned int drain;
	unsigned int prev;
	unsigned int i;

	/*
	 * A pool that keeps growing under pressure raises the share one step
	 * per scope, monotonically, until it reaches the ceiling and stops.
	 */
	drain = 0;
	for (i = 0; i < 256; i++) {
		prev = drain;
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_INEFFICIENT,
							 false);
		KUNIT_EXPECT_GE(test, drain, prev);
		KUNIT_EXPECT_LE(test, drain, MEMCOMP_WRITEBACK_DRAIN_MAX);
		if (i == 0)
			KUNIT_EXPECT_EQ(test, drain,
					MEMCOMP_WRITEBACK_DRAIN_STEP);
	}
	KUNIT_EXPECT_EQ(test, drain, MEMCOMP_WRITEBACK_DRAIN_MAX);

	/* Nothing raises it past the ceiling. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_drain_next(MEMCOMP_WRITEBACK_DRAIN_MAX,
						     MEMCOMP_WRITEBACK_INEFFICIENT,
							 false),
			MEMCOMP_WRITEBACK_DRAIN_MAX);

	/*
	 * A shrinking pool is the loop's evidence that the share was enough,
	 * and gives a quarter of it back - reaching zero, so a pool that stays
	 * under control stops being drained entirely.
	 */
	drain = MEMCOMP_WRITEBACK_DRAIN_MAX;
	for (i = 0; i < 128 && drain; i++) {
		prev = drain;
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_PENDING,
							 true);
		KUNIT_EXPECT_LT(test, drain, prev);
	}
	KUNIT_EXPECT_EQ(test, drain, 0U);

	/* Having shrunk the pool gives back a quarter, not a half. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_drain_next(MEMCOMP_WRITEBACK_DRAIN_MAX,
						     MEMCOMP_WRITEBACK_PENDING,
							 true),
			MEMCOMP_WRITEBACK_DRAIN_MAX -
			MEMCOMP_WRITEBACK_DRAIN_MAX / 4);

	/* The pool yielding is what lowers the share, whatever the sample. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_drain_next(MEMCOMP_WRITEBACK_DRAIN_MAX,
						     MEMCOMP_WRITEBACK_EFFECTIVE,
							 true),
			memcomp_writeback_drain_next(MEMCOMP_WRITEBACK_DRAIN_MAX,
						     MEMCOMP_WRITEBACK_PENDING,
							 true));

	/*
	 * Net-positive compression only slows the rise.  It reports so at any
	 * ratio better than 2:1 - the normal case for a compressed pool - so
	 * letting it decay the share pinned the loop at zero for exactly the
	 * workloads that grow the pool.
	 */
	drain = MEMCOMP_WRITEBACK_DRAIN_STEP;
	KUNIT_EXPECT_GT(test,
			memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_EFFECTIVE,
							 false),
			drain);
	KUNIT_EXPECT_LT(test,
			memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_EFFECTIVE,
							 false),
			memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_INEFFICIENT,
							 false));

	/*
	 * A workload storing pages at a healthy ratio while the pool grows is
	 * the case that used to pin the share at zero.  It must wind up.
	 */
	drain = 0;
	for (i = 0; i < 16; i++)
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_EFFECTIVE,
							 false);
	KUNIT_EXPECT_GT(test, drain, 4 * MEMCOMP_WRITEBACK_DRAIN_STEP);
	KUNIT_EXPECT_GT(test,
			memcomp_writeback_budget_values(4096UL, drain), 0UL);

	/*
	 * The loop cannot latch at zero: while the pool keeps growing under
	 * pressure, every scope raises the share again.  This is what keeps
	 * "conservative" from becoming "never writes back".  One scope still
	 * grants nothing - it has not cleared the deadband - but sustained
	 * pressure does.
	 */
	drain = 0;
	drain = memcomp_writeback_drain_next(drain,
					     MEMCOMP_WRITEBACK_INEFFICIENT,
						 false);
	KUNIT_EXPECT_GT(test, drain, 0U);
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(4096UL, drain), 0UL);
	for (i = 0; i < 8; i++)
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_INEFFICIENT,
							 false);
	KUNIT_EXPECT_GT(test,
			memcomp_writeback_budget_values(4096UL, drain), 0UL);

	/*
	 * And it cannot run away either: a pool that yields to every share it
	 * is given settles into a bounded cycle instead of climbing to the
	 * ceiling.  Rising by a step and giving a quarter back puts that
	 * steady state at about four steps - twice where halving left it,
	 * which was low enough to barely drain a pool that keeps refilling.
	 */
	drain = 0;
	for (i = 0; i < 128; i++) {
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_INEFFICIENT,
							 false);
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_PENDING,
							 true);
	}
	KUNIT_EXPECT_GT(test, drain, 2 * MEMCOMP_WRITEBACK_DRAIN_STEP);
	KUNIT_EXPECT_LT(test, drain, 5 * MEMCOMP_WRITEBACK_DRAIN_STEP);
	KUNIT_EXPECT_LT(test, drain, MEMCOMP_WRITEBACK_DRAIN_MAX / 2);

	/*
	 * A pool that merely holds steady is not evidence the share was
	 * enough - the node is below its watermark and the pool is not
	 * yielding - so the share rises.  One scope of draining then shrinks
	 * it and the decay takes over.
	 */
	drain = MEMCOMP_WRITEBACK_DRAIN_STEP;
	KUNIT_EXPECT_GT(test,
			memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_PENDING,
							 false),
			drain);
	KUNIT_EXPECT_LT(test,
			memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_PENDING,
							 true),
			drain);
}

static void memcomp_test_writeback_drain_transient(struct kunit *test)
{
	unsigned int settled[11] = {};
	unsigned int burst;

	for (burst = 1; burst <= 10; burst++) {
		unsigned int drain = 0;
		unsigned int cycle;

		for (cycle = 0; cycle < 64; cycle++) {
			unsigned int i;

			/* A burst of scopes while the pool is growing... */
			for (i = 0; i < burst; i++)
				drain = memcomp_writeback_drain_next(drain,
								     MEMCOMP_WRITEBACK_EFFECTIVE,
									 false);
			/* ...then the node climbs back on its own. */
			drain = memcomp_writeback_drain_recover(drain);
		}
		settled[burst] = drain;

		/* Never anywhere near the ceiling, however long the uptime. */
		KUNIT_EXPECT_LT(test, drain,
				MEMCOMP_WRITEBACK_DRAIN_MAX / 4);
		/* Monotone in burst length, so the signal means something. */
		if (burst > 1)
			KUNIT_EXPECT_GT(test, settled[burst],
					settled[burst - 1]);
	}

	/* The shortest bursts - a boot, a single spike - grant nothing. */
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(1UL << 20,
							settled[1]),
			0UL);
	KUNIT_EXPECT_EQ(test,
			memcomp_writeback_budget_values(1UL << 20,
							settled[2]),
			0UL);

	/*
	 * Pressure that is never relieved is the case that must still reach
	 * the ceiling, and grant half of the cold payloads there.
	 */
	{
		unsigned int drain = 0;
		unsigned int i;

		for (i = 0; i < 256; i++)
			drain = memcomp_writeback_drain_next(drain,
							     MEMCOMP_WRITEBACK_EFFECTIVE,
								 false);
		KUNIT_EXPECT_EQ(test, drain, MEMCOMP_WRITEBACK_DRAIN_MAX);
		KUNIT_EXPECT_EQ(test,
				memcomp_writeback_budget_values(4096UL, drain),
				4096UL / 2);
	}
}

static void memcomp_test_writeback_drain_handoff(struct kunit *test)
{
	unsigned int drain;
	unsigned int i;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	/* Wind the loop up as a sustained pressure period would. */
	drain = 0;
	for (i = 0; i < 256; i++)
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_INEFFICIENT,
							 false);
	KUNIT_ASSERT_EQ(test, drain, MEMCOMP_WRITEBACK_DRAIN_MAX);

	/*
	 * A grant with no sample - the first scope of a period that follows a
	 * recovery - carries the share over instead of restarting from zero,
	 * because the drain has been running the same loop against it.
	 */
	memcomp_writeback_drain[0] = drain;
	WRITE_ONCE(memcomp_writeback_feedback_valid[0], false);
	memcomp_writeback_budget_grant_locked(0, 0, 0, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_drain[0], drain);

	/*
	 * Unless the gate is closed: then the grant is nothing and the share
	 * is dropped, so a later opening does not inherit it.
	 */
	memcomp_writeback_budget_grant_locked(0, 0, 0, 0);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_drain[0], 0U);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_budget[0], 0UL);
	memcomp_writeback_drain[0] = drain;

	/*
	 * The drain applies the same decay the scope path does, so a pool
	 * that yields is worked down to nothing off the reclaim path - it
	 * does not sit frozen waiting for the next workload.
	 */
	for (i = 0; i < 128 && drain; i++)
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_PENDING,
							 true);
	KUNIT_EXPECT_EQ(test, drain, 0U);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_budget_values(4096UL, drain),
			0UL);

	/*
	 * The handoff itself.  Everything above is the loop's arithmetic; none
	 * of it reaches memcomp_reclaim_writeback_end(), which is what
	 * hands a period's remaining share to the off-reclaim drain.  Making
	 * that call never fire leaves every case above green, so drive a real
	 * scope and require the quota to appear.
	 */
	{
		struct memcomp_lru *lru = memcomp_lru_nid(0);
		long saved_total = total_swap_pages;
		long saved_entries;
		unsigned long entries;
		unsigned long expected;
		unsigned long total;
		unsigned long stop;

		KUNIT_ASSERT_NOT_NULL(test, lru);
		/*
		 * The drain needs somewhere to write to, and a UML test host
		 * has no swap device.  Without this the arm below is skipped
		 * and the case is green whether or not the handoff exists.
		 */
		total_swap_pages = 1024;
		WRITE_ONCE(memcomp_writeback_carry[0], 0);
		memcomp_writeback_drain[0] = MEMCOMP_WRITEBACK_DRAIN_MAX;

		/*
		 * And cold payloads for the share to be a share of, enough
		 * of them to open the level gate.  With an empty list the
		 * quota is zero and the schedule returns before it stores
		 * anything, so the assertion below would hold whether or not
		 * the handoff ran; below the gate it is zero by design.
		 */
		saved_entries = atomic_long_read(&lru->nr_entries);
		total = totalram_pages();
		stop = (unsigned long)div64_u64((u64)total *
				MEMCOMP_WRITEBACK_GATE_STOP_PERMILLE, 1000);
		entries = (unsigned long)div64_u64((u64)total *
				MEMCOMP_WRITEBACK_GATE_START_PERMILLE, 1000) + 1;
		atomic_long_set(&lru->nr_entries, entries);
		/*
		 * The share asks for half of the cold payloads, but the gate
		 * lets only what is above its close point go.
		 */
		expected = memcomp_writeback_budget_values(entries,
							   MEMCOMP_WRITEBACK_DRAIN_MAX);
		KUNIT_ASSERT_GT(test, expected, entries - stop);
		expected = entries - stop;

		memcomp_reclaim_writeback_begin(0, 0);
		KUNIT_EXPECT_EQ(test,
				atomic_read(&memcomp_writeback_gate_open), 1);
		KUNIT_EXPECT_LE(test, READ_ONCE(memcomp_writeback_budget[0]),
				expected);
		memcomp_writeback_drain[0] = MEMCOMP_WRITEBACK_DRAIN_MAX;
		memcomp_reclaim_writeback_end(0);

		/*
		 * The quota is what the share asks of the cold payloads, and
		 * it is settled once - a second period must not add to it.
		 */
		KUNIT_EXPECT_EQ(test, READ_ONCE(memcomp_writeback_carry[0]),
				expected);
		if (expected) {
			/* And one pass of it stays inside the slice bound. */
			KUNIT_EXPECT_LE(test,
					memcomp_writeback_drain_slice(expected),
					(unsigned long)MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX);
		}

		/*
		 * Back at the close point the gate shuts, and the period hands
		 * nothing over - not even what an earlier period left behind,
		 * however large a share the loop still holds.
		 */
		atomic_long_set(&lru->nr_entries, stop);
		WRITE_ONCE(memcomp_writeback_carry[0], 77);
		memcomp_reclaim_writeback_begin(0, 0);
		KUNIT_EXPECT_EQ(test,
				atomic_read(&memcomp_writeback_gate_open), 0);
		KUNIT_EXPECT_EQ(test, READ_ONCE(memcomp_writeback_budget[0]),
				0UL);
		KUNIT_EXPECT_EQ(test, READ_ONCE(memcomp_writeback_drain[0]),
				0U);
		memcomp_writeback_drain[0] = MEMCOMP_WRITEBACK_DRAIN_MAX;
		memcomp_reclaim_writeback_end(0);
		KUNIT_EXPECT_EQ(test, READ_ONCE(memcomp_writeback_carry[0]),
				0UL);

		WRITE_ONCE(memcomp_writeback_carry[0], 0);
		atomic_long_set(&lru->nr_entries, saved_entries);
		/* Leave the latch as the real pool has it. */
		memcomp_writeback_gate_excess(0, memcomp_pool_total_size());
		total_swap_pages = saved_total;
	}

	memcomp_writeback_drain[0] = 0;
	memcomp_writeback_budget[0] = 0;
	WRITE_ONCE(memcomp_writeback_feedback_valid[0], false);
}

static void memcomp_test_writeback_drain_slice(struct kunit *test)
{
	static const unsigned long quotas[] = {
		1, 32, 1000, 100000, 10000000,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(quotas); i++) {
		unsigned long quota = quotas[i];
		unsigned long limit = memcomp_writeback_drain_slice(quota);

		/* Never more than the cap, and never more than is owed. */
		KUNIT_EXPECT_LE(test, limit,
				(unsigned long)MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX);
		KUNIT_EXPECT_LE(test, limit, quota);
		/* And always forward progress, so the quota cannot stall. */
		KUNIT_EXPECT_GT(test, limit, 0UL);
	}

	/*
	 * A huge quota must not scale the slice with it.  Asserting the cap is
	 * reached, rather than only that it is not exceeded, is what catches a
	 * slice that regressed to a plain fraction of the quota.
	 */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_drain_slice(10000000UL),
			(unsigned long)MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX);

	/*
	 * Below the cap the slice still has to grow with the quota, or the
	 * drain would crawl through a large backlog a batch at a time.
	 */
	KUNIT_EXPECT_GT(test,
			memcomp_writeback_drain_slice(MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX *
							  MEMCOMP_WRITEBACK_DRAIN_SLICES / 2),
			(unsigned long)MEMCOMP_WRITEBACK_BATCH);
}

static void memcomp_test_writeback_pass_bounded(struct kunit *test)
{
	static const unsigned long budgets[] = {
		1, 32, MEMCOMP_WRITEBACK_BATCH, 4096, 500000, 10000000,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(budgets); i++) {
		unsigned long budget = budgets[i];
		unsigned long pass = memcomp_writeback_drain_slice(budget);

		/* One pass is bounded however large the budget grew. */
		KUNIT_EXPECT_LE(test, pass,
				(unsigned long)MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX);
		KUNIT_EXPECT_LE(test, pass, budget);
		KUNIT_EXPECT_GT(test, pass, 0UL);

		/*
		 * And what a pass does not spend survives: the consume step
		 * subtracts only what was scanned, so the scope keeps asking
		 * until the whole budget is gone.
		 */
		if (pass < budget) {
			unsigned long left = budget - pass;

			KUNIT_EXPECT_GT(test, left, 0UL);
			KUNIT_EXPECT_GT(test,
					memcomp_writeback_drain_slice(left),
					0UL);
		}
	}

	/* A saturated budget must not become a single pass. */
	KUNIT_EXPECT_LT(test, memcomp_writeback_drain_slice(500000UL),
			500000UL);

	/*
	 * And the reclaim path takes its limit from the same place.  This
	 * covers the policy, not the call: nothing here can invoke
	 * memcomp_shrinker_scan(), so a future change that read the raw
	 * budget again would still have to be caught by review.  Sharing the
	 * helper is what makes that change visible.
	 */
	{
		unsigned long saved = READ_ONCE(memcomp_writeback_budget[0]);

		WRITE_ONCE(memcomp_writeback_budget[0], 500000UL);
		KUNIT_EXPECT_EQ(test, memcomp_writeback_pass_limit(0),
				memcomp_writeback_drain_slice(500000UL));
		KUNIT_EXPECT_LE(test, memcomp_writeback_pass_limit(0),
				(unsigned long)MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX);
		WRITE_ONCE(memcomp_writeback_budget[0], saved);
	}
	/* An invalid node asks for nothing rather than reading past the array. */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_pass_limit(-1), 0UL);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_pass_limit(MAX_NUMNODES), 0UL);
}

static unsigned long
memcomp_test_writeback_budget_legacy(unsigned long cold_entries,
				     enum memcomp_writeback_feedback feedback,
					 unsigned int streak)
{
	unsigned int shift = 6;

	if (streak < shift - 2)
		shift -= streak;
	else
		shift = 2;
	if (feedback == MEMCOMP_WRITEBACK_EFFECTIVE)
		shift++;
	else if (feedback == MEMCOMP_WRITEBACK_INEFFICIENT && shift > 2)
		shift--;

	return cold_entries >> shift;
}

static void memcomp_test_writeback_budget_vs_legacy(struct kunit *test)
{
	const unsigned long cold = 4096UL;
	unsigned int drain;
	unsigned int i;

	/*
	 * The old share depended only on how many scopes had passed, so a
	 * first scope drained 1/64 of the pool whether or not the pool was in
	 * any trouble, and four consecutive scopes reached its 1/4 ceiling
	 * whether or not the previous three had achieved anything.
	 */
	KUNIT_EXPECT_EQ(test,
			memcomp_test_writeback_budget_legacy(cold,
							     MEMCOMP_WRITEBACK_PENDING,
								 0),
			cold / 64);
	KUNIT_EXPECT_EQ(test,
			memcomp_test_writeback_budget_legacy(cold,
							     MEMCOMP_WRITEBACK_PENDING,
								 100),
			cold / 4);

	/*
	 * The loop starts from nothing instead, and a pool that yields to the
	 * share it was given goes back to nothing - the state a device spends
	 * most of its life in, where the old policy still drained.
	 */
	KUNIT_EXPECT_EQ(test, memcomp_writeback_budget_values(cold, 0), 0UL);
	drain = MEMCOMP_WRITEBACK_DRAIN_MAX;
	for (i = 0; i < 128; i++)
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_PENDING,
							 true);
	KUNIT_EXPECT_EQ(test, memcomp_writeback_budget_values(cold, drain),
			0UL);
	KUNIT_EXPECT_GT(test,
			memcomp_test_writeback_budget_legacy(cold,
							     MEMCOMP_WRITEBACK_EFFECTIVE,
								 0),
			0UL);

	/*
	 * Sustained resistance does eventually drain more than the old
	 * ceiling allowed - the loop is not merely a gentler constant.
	 */
	drain = 0;
	for (i = 0; i < 256; i++)
		drain = memcomp_writeback_drain_next(drain,
						     MEMCOMP_WRITEBACK_INEFFICIENT,
							 false);
	KUNIT_EXPECT_GT(test, memcomp_writeback_budget_values(cold, drain),
			memcomp_test_writeback_budget_legacy(cold,
							     MEMCOMP_WRITEBACK_INEFFICIENT,
								 100));
}

static void memcomp_test_comp_ctx_bitmap(struct kunit *test)
{
	unsigned long all_mask = BIT(0) | BIT(1);
	unsigned long free_mask = BIT(2) | BIT(5);
	enum memcomp_comp_ctx_miss miss;
	s64 all_busy_pages;
	s64 borrow_limit_pages;
	s64 borrow_waiter_pages;
	s64 budget_pages;
	s64 busy_pages;
	s64 reserved_free_pages;
	int idx;

	idx = memcomp_comp_ctx_claim(&free_mask, BIT(5));
	KUNIT_EXPECT_EQ(test, idx, 5);
	KUNIT_EXPECT_EQ(test, free_mask, BIT(2));
	idx = memcomp_comp_ctx_claim(&free_mask, ~0UL);
	KUNIT_EXPECT_EQ(test, idx, 2);
	KUNIT_EXPECT_EQ(test, free_mask, 0UL);
	KUNIT_EXPECT_EQ(test,
			memcomp_comp_ctx_claim(&free_mask, ~0UL), -EBUSY);
	KUNIT_EXPECT_TRUE(test, memcomp_comp_ctx_release(&free_mask, 2));
	KUNIT_EXPECT_EQ(test, free_mask, BIT(2));
	KUNIT_EXPECT_FALSE(test, memcomp_comp_ctx_release(&free_mask, 2));

	miss = memcomp_comp_ctx_classify(BIT(0), BIT(1), BIT(0) | BIT(1));
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_RESERVED_FREE);
	miss = memcomp_comp_ctx_classify(0, BIT(1), BIT(0) | BIT(1));
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_ALL_BUSY);
	miss = memcomp_comp_ctx_classify(BIT(1), BIT(1), BIT(0) | BIT(1));
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_AVAILABLE);
	miss = memcomp_comp_ctx_borrow_decision(0, 1, 0);
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_AVAILABLE);
	miss = memcomp_comp_ctx_borrow_decision(1, 1, 0);
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_BORROW_LIMIT);
	miss = memcomp_comp_ctx_borrow_decision(0, 1, 1);
	KUNIT_EXPECT_EQ(test, miss,
			MEMCOMP_COMP_CTX_BACKGROUND_WAITERS);
	miss = memcomp_comp_ctx_borrow_decision(1, 1, 1);
	KUNIT_EXPECT_EQ(test, miss,
			MEMCOMP_COMP_CTX_BACKGROUND_WAITERS);
	miss = memcomp_comp_ctx_foreground_status(BIT(1), BIT(1),
						  all_mask, 1, 1, 1);
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_AVAILABLE);
	miss = memcomp_comp_ctx_foreground_status(BIT(0), BIT(1),
						  all_mask, 0, 1, 0);
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_AVAILABLE);
	miss = memcomp_comp_ctx_foreground_status(BIT(0), BIT(1),
						  all_mask, 1, 1, 0);
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_BORROW_LIMIT);
	miss = memcomp_comp_ctx_foreground_status(BIT(0), BIT(1),
						  all_mask, 0, 1, 1);
	KUNIT_EXPECT_EQ(test, miss,
			MEMCOMP_COMP_CTX_BACKGROUND_WAITERS);
	miss = memcomp_comp_ctx_foreground_status(0, BIT(1), all_mask,
						  0, 1, 0);
	KUNIT_EXPECT_EQ(test, miss, MEMCOMP_COMP_CTX_ALL_BUSY);

	reserved_free_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages);
	all_busy_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages);
	borrow_limit_pages =
		atomic64_read(&memcomp_reclaim_ctx_borrow_limit_pages);
	borrow_waiter_pages =
		atomic64_read(&memcomp_reclaim_ctx_borrow_waiter_pages);
	budget_pages = atomic64_read(&memcomp_reclaim_ctx_budget_pages);
	busy_pages = reserved_free_pages + all_busy_pages;
	memcomp_account_reclaim_ctx_miss(MEMCOMP_COMP_CTX_RESERVED_FREE,
					 2);
	memcomp_account_reclaim_ctx_miss(MEMCOMP_COMP_CTX_ALL_BUSY, 3);
	memcomp_account_reclaim_ctx_miss(MEMCOMP_COMP_CTX_BORROW_LIMIT,
					 5);
	miss = MEMCOMP_COMP_CTX_BACKGROUND_WAITERS;
	memcomp_account_reclaim_ctx_miss(miss, 7);
	memcomp_account_reclaim_ctx_miss(MEMCOMP_COMP_CTX_BUDGET, 11);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages) +
			atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages),
			busy_pages + 17);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages),
			reserved_free_pages + 14);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages),
			all_busy_pages + 3);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_borrow_limit_pages),
			borrow_limit_pages + 5);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_borrow_waiter_pages),
			borrow_waiter_pages + 7);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_budget_pages),
			budget_pages + 11);
}

static void memcomp_test_comp_budget(struct kunit *test)
{
	struct memcomp_comp_budget budget = {};
	enum memcomp_payload_mode mode;
	enum memcomp_reclaim_source source;
	bool granted;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, memcomp_comp_budget_limit(0, 0), 0U);
	KUNIT_EXPECT_EQ(test, memcomp_comp_budget_limit(1, 32), 1U);
	KUNIT_EXPECT_EQ(test, memcomp_comp_budget_limit(2, 32), 1U);
	KUNIT_EXPECT_EQ(test, memcomp_comp_budget_limit(8, 32), 7U);
	KUNIT_EXPECT_EQ(test, memcomp_comp_budget_limit(8, 4), 4U);
	mode = MEMCOMP_PAYLOAD_BEST_EFFORT;
	source = MEMCOMP_RECLAIM_KSWAPD;
	granted = memcomp_comp_ctx_background(mode, source, true);
	KUNIT_EXPECT_FALSE(test, granted);
	mode = MEMCOMP_PAYLOAD_MUST_SUCCEED;
	source = MEMCOMP_RECLAIM_DIRECT;
	granted = memcomp_comp_ctx_background(mode, source, true);
	KUNIT_EXPECT_FALSE(test, granted);
	source = MEMCOMP_RECLAIM_KSWAPD;
	granted = memcomp_comp_ctx_background(mode, source, true);
	KUNIT_EXPECT_TRUE(test, granted);
	granted = memcomp_comp_ctx_background(mode, source, false);
	KUNIT_EXPECT_FALSE(test, granted);

	for (i = 0; i < 7; i++) {
		granted = memcomp_comp_budget_try_acquire(&budget, 7);
		KUNIT_ASSERT_TRUE(test, granted);
	}
	KUNIT_EXPECT_EQ(test, budget.active, 7U);
	KUNIT_EXPECT_EQ(test, budget.background_active, 7U);
	KUNIT_EXPECT_EQ(test, budget.peak, 7U);
	KUNIT_EXPECT_EQ(test, budget.background_peak, 7U);
	granted = memcomp_comp_budget_try_acquire(&budget, 7);
	KUNIT_EXPECT_FALSE(test, granted);
	granted = memcomp_comp_budget_available(&budget, 4);
	KUNIT_EXPECT_FALSE(test, granted);
	KUNIT_EXPECT_EQ(test, budget.active, 7U);

	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_TRUE(test,
				  memcomp_comp_budget_release(&budget));
	KUNIT_EXPECT_TRUE(test,
			  memcomp_comp_budget_available(&budget, 4));
	for (; i < 7; i++)
		KUNIT_ASSERT_TRUE(test,
				  memcomp_comp_budget_release(&budget));
	KUNIT_EXPECT_EQ(test, budget.active, 0U);
	KUNIT_EXPECT_EQ(test, budget.background_active, 0U);
	KUNIT_EXPECT_FALSE(test,
			   memcomp_comp_budget_release(&budget));
}

static void memcomp_test_direct_staging_policy(struct kunit *test)
{
	static const struct {
		enum memcomp_payload_mode mode;
		enum memcomp_reclaim_source source;
		unsigned int nr_pages;
		const char *driver;
		bool eligible;
	} cases[] = {
		{ MEMCOMP_PAYLOAD_MUST_SUCCEED,
		  MEMCOMP_RECLAIM_DIRECT, 1, "zstd-generic", true },
		{ MEMCOMP_PAYLOAD_MUST_SUCCEED,
		  MEMCOMP_RECLAIM_DIRECT, 2, "zstd-generic", false },
		{ MEMCOMP_PAYLOAD_BEST_EFFORT,
		  MEMCOMP_RECLAIM_DIRECT, 1, "zstd-generic", false },
		{ MEMCOMP_PAYLOAD_MUST_SUCCEED,
		  MEMCOMP_RECLAIM_KSWAPD, 1, "zstd-generic", false },
		{ MEMCOMP_PAYLOAD_MUST_SUCCEED,
		  MEMCOMP_RECLAIM_DIRECT, 1, "lz4-generic", false },
	};
	struct memcomp_reclaim_ctx *ctx;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const typeof(cases[0]) *c = &cases[i];
		bool actual;
		bool expected = c->eligible &&
				!IS_ENABLED(CONFIG_PREEMPT_RT);

		actual = memcomp_staging_policy(c->mode, c->source,
						c->nr_pages, c->driver);
		KUNIT_EXPECT_EQ(test, actual, expected);
	}

	if (!memcomp_is_enabled())
		return;
	ctx = memcomp_reclaim_ctx_get(2, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_EXPECT_NOT_NULL(test, ctx);
	if (ctx) {
		KUNIT_EXPECT_FALSE(test, ctx->direct_staging);
		memcomp_reclaim_ctx_put(ctx);
	}
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_EXPECT_NOT_NULL(test, ctx);
	if (ctx) {
		const char *driver =
			crypto_tfm_alg_driver_name(crypto_comp_tfm(ctx->tfm));

		KUNIT_EXPECT_EQ(test, ctx->direct_staging,
				!IS_ENABLED(CONFIG_PREEMPT_RT) &&
				memcomp_driver_is_zstd(driver));
		memcomp_reclaim_ctx_put(ctx);
	}
	ctx = memcomp_get_comp_ctx(MEMCOMP_PAYLOAD_BEST_EFFORT,
				   NULL, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_EXPECT_NOT_NULL(test, ctx);
	if (ctx) {
		KUNIT_EXPECT_FALSE(test, ctx->direct_staging);
		memcomp_put_comp_ctx(ctx);
	}
}

static void memcomp_test_direct_staging_pool(struct kunit *test)
{
	struct memcomp_direct_staging *claimed[MEMCOMP_MAX_COMP_CTXS] = {};
	struct memcomp_direct_staging *extra;
	unsigned int nr_staging = READ_ONCE(memcomp_nr_direct_staging);
	unsigned int claimed_count = 0;
	unsigned long flags;
	unsigned int active;
	unsigned int free;
	unsigned int i;
	unsigned int j;
	unsigned int peak;

	if (!nr_staging) {
		kunit_skip(test, "no direct staging buffers");
		return;
	}
	spin_lock_irqsave(&memcomp_direct_staging_lock, flags);
	active = memcomp_direct_staging_active;
	free = bitmap_weight(&memcomp_direct_staging_free, nr_staging);
	spin_unlock_irqrestore(&memcomp_direct_staging_lock, flags);
	if (active || free != nr_staging) {
		kunit_skip(test, "direct staging pool is active");
		return;
	}

	for (i = 0; i < nr_staging; i++) {
		claimed[i] = memcomp_direct_staging_try_get();
		if (!claimed[i])
			goto out_skip;
		claimed_count++;
	}
	for (i = 0; i < claimed_count; i++) {
		for (j = 0; j < i; j++)
			KUNIT_EXPECT_PTR_NE(test, claimed[i], claimed[j]);
	}
	extra = memcomp_direct_staging_try_get();
	KUNIT_EXPECT_PTR_EQ(test, extra, NULL);
	if (extra)
		memcomp_direct_staging_put(extra);
	spin_lock_irqsave(&memcomp_direct_staging_lock, flags);
	active = memcomp_direct_staging_active;
	peak = memcomp_direct_staging_peak;
	spin_unlock_irqrestore(&memcomp_direct_staging_lock, flags);
	KUNIT_EXPECT_EQ(test, active, claimed_count);
	KUNIT_EXPECT_GE(test, peak, claimed_count);

	for (i = 0; i < claimed_count; i++)
		memcomp_direct_staging_put(claimed[i]);
	spin_lock_irqsave(&memcomp_direct_staging_lock, flags);
	active = memcomp_direct_staging_active;
	free = bitmap_weight(&memcomp_direct_staging_free, nr_staging);
	spin_unlock_irqrestore(&memcomp_direct_staging_lock, flags);
	KUNIT_EXPECT_EQ(test, active, 0U);
	KUNIT_EXPECT_EQ(test, free, nr_staging);
	return;

out_skip:
	for (i = 0; i < claimed_count; i++)
		memcomp_direct_staging_put(claimed[i]);
	kunit_skip(test, "direct staging pool became active");
}

static void
memcomp_test_release_prepared(struct memcomp_reclaim_ctx *ctx,
			      struct memcomp_store_ctx *store,
				  unsigned int length,
				  unsigned long handle, u8 pool_idx)
{
	if (length)
		memcomp_zs_free(pool_idx, handle);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	if (store->staging) {
		memcomp_ctx_timing_account(&store->staging->timing);
		memcomp_direct_staging_put(store->staging);
		store->staging = NULL;
	}
}

static void memcomp_test_fill_real_memory_page(u8 *buf)
{
	struct memcomp_test_real_page_record record = {};
	size_t pos = 0;
	u32 mix;

	memcpy(&record.uts, init_utsname(), sizeof(record.uts));
	strscpy(record.banner, linux_banner, sizeof(record.banner));
	strscpy(record.comm, current->comm, sizeof(record.comm));
	record.totalram = totalram_pages();
	record.online_cpus = num_online_cpus();
	mix = (u32)jiffies ^ (u32)record.totalram;

	while (pos < PAGE_SIZE) {
		size_t copy = min(sizeof(record), PAGE_SIZE - pos);

		record.seq++;
		record.jiffies_snapshot = get_jiffies_64();
		record.mix = mix;
		memcpy(buf + pos, &record, copy);
		mix = mix * 1664525U + 1013904223U + record.seq;
		pos += copy;
	}
}

static void memcomp_test_fill_zstd_ratio_page(u8 *buf)
{
	u32 state = 0x9e3779b9;
	size_t pos;

	for (pos = 0; pos < PAGE_SIZE; pos += 64) {
		u8 *dst = buf + pos;
		unsigned int i;

		memset(dst, 0, 49);
		memcpy(dst, "memcomp-zstd-page",
		       sizeof("memcomp-zstd-page"));
		for (i = 49; i < 64; i++) {
			state = state * 1664525U + 1013904223U;
			dst[i] = (state >> 16) & 0x7f;
		}
	}
}

static void memcomp_test_fill_zstd_shift_page(u8 *buf, unsigned int page)
{
	u32 state = 0x9e3779b9 ^ (page * 2654435761U);
	size_t pos;

	if (page < MEMCOMP_TEST_HUF_CACHE_PAGES / 2) {
		memcomp_test_fill_zstd_ratio_page(buf);
		buf[(page * 131U) % PAGE_SIZE] = page & 0x7f;
		return;
	}

	for (pos = 0; pos < PAGE_SIZE; pos += 64) {
		u8 *dst = buf + pos;
		unsigned int i;

		/*
		 * A different restricted alphabet, not a wider one.  Uniform
		 * bytes would make zstd give up on the literals entirely
		 * (HUF_compress_internal() returns 0 below its largest-symbol
		 * threshold), emit a set_basic block, and code no table at all
		 * - which exercises neither the encoder cache nor the decoder
		 * one.  Staying compressible with a different distribution is
		 * what produces a second Huffman table.
		 */
		memset(dst, 0xa0, 49);
		memcpy(dst, "\xb1\xb2\xb3-shift-page", sizeof("\xb1\xb2\xb3-shift-page"));
		for (i = 49; i < 64; i++) {
			state = state * 1664525U + 1013904223U;
			dst[i] = 0x80 | ((state >> 16) & 0x3f);
		}
	}
}

static void memcomp_test_fill_zstd_cache_page(u8 *buf,
					      unsigned int page)
{
	memcomp_test_fill_zstd_ratio_page(buf);
	buf[(page * 131U) % PAGE_SIZE] = page & 0x7f;

	/* Exercise checked table misses before and after a cache refresh. */
	if (page == 0 || page == MEMCOMP_TEST_HUF_CACHE_PAGES - 1)
		buf[PAGE_SIZE - 1] = 0x81;
	if (page == 63)
		buf[PAGE_SIZE - 2] = 0xff;
}

static void
memcomp_test_zstd_page_roundtrip(struct kunit *test,
				 const struct memcomp_test_zstd_page_case *page_case)
{
	struct memcomp_store_ctx store = {};
	struct memcomp_reclaim_ctx *ctx;
	const char *driver;
	void *mapped;
	unsigned int length = 0;
	unsigned long handle = 0;
	u8 pool_idx = 0;
	u8 *dst;
	u8 *src;
	int ret;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	page_case->fill(src);

	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	driver = crypto_tfm_alg_driver_name(crypto_comp_tfm(ctx->tfm));
	if (!memcomp_driver_is_zstd(driver)) {
		memcomp_reclaim_ctx_put(ctx);
		kunit_skip(test, "test requires zstd-generic");
		return;
	}
	ret = memcomp_prepare_payload_locked(&ctx, src, PAGE_SIZE,
					     &length, &handle, &pool_idx,
						 false, &store, NULL);
	if (ret == -E2BIG && page_case->allow_incompressible) {
		memcomp_test_release_prepared(ctx, &store, length, handle,
					      pool_idx);
		kunit_info(test, "%s was rejected as incompressible",
			   page_case->name);
		return;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out;

	KUNIT_EXPECT_GT(test, length, 0U);
	KUNIT_EXPECT_LE(test, length, PAGE_SIZE);
	KUNIT_EXPECT_LE(test, length, page_case->max_length);
	kunit_info(test, "%s compressed to %u bytes", page_case->name, length);
	mapped = zs_map_object(memcomp_pools[pool_idx], handle, ZS_MM_RO);
	if (!mapped) {
		KUNIT_FAIL(test, "payload map failed");
		goto out;
	}
	ret = memcomp_decompress_payload(mapped, length, dst);
	zs_unmap_object(memcomp_pools[pool_idx], handle);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, src, dst, PAGE_SIZE);
out:
	memcomp_test_release_prepared(ctx, &store, length, handle, pool_idx);
}

static void memcomp_test_zstd_real_page_roundtrip(struct kunit *test)
{
	static const struct memcomp_test_zstd_page_case page_case = {
		.name = "real memory page",
		.fill = memcomp_test_fill_real_memory_page,
		.max_length = PAGE_SIZE * 28 / 100,
		.allow_incompressible = true,
	};

	memcomp_test_zstd_page_roundtrip(test, &page_case);
}

static void memcomp_test_zstd_page_ratio_bench(struct kunit *test)
{
	static const struct memcomp_test_zstd_page_case page_case = {
		.name = "near-budget page",
		.fill = memcomp_test_fill_zstd_ratio_page,
		.max_length = PAGE_SIZE * 28 / 100,
	};

	memcomp_test_zstd_page_roundtrip(test, &page_case);
}

static void memcomp_test_zstd_huf_cache_frames(struct kunit *test)
{
	struct memcomp_reclaim_ctx *ctx;
	const char *driver;
	u8 *src;
	u8 *compressed;
	u8 *dst;
	unsigned int page;
	unsigned int worst = 0;
	u64 total = 0;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	compressed = kunit_kmalloc(test, MEMCOMP_MAX_COMPRESSED_SIZE,
				   GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, compressed);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	driver = crypto_tfm_alg_driver_name(crypto_comp_tfm(ctx->tfm));
	if (!memcomp_driver_is_zstd(driver)) {
		memcomp_reclaim_ctx_put(ctx);
		kunit_skip(test, "test requires zstd-generic");
		return;
	}

	for (page = 0; page < MEMCOMP_TEST_HUF_CACHE_PAGES; ++page) {
		struct crypto_comp *decomp;
		unsigned int compressed_len = MEMCOMP_MAX_COMPRESSED_SIZE;
		unsigned int decompressed_len = PAGE_SIZE;
		int ret;

		memcomp_test_fill_zstd_cache_page(src, page);
		ret = crypto_comp_compress(ctx->tfm, src, PAGE_SIZE, compressed,
					   &compressed_len);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out;
		KUNIT_EXPECT_LT(test, compressed_len, PAGE_SIZE);
		if (compressed_len >= PAGE_SIZE)
			goto out;

		/* A fresh decoder proves this frame carries its own Huffman table. */
		decomp = crypto_alloc_comp("zstd-generic", 0, 0);
		KUNIT_EXPECT_FALSE(test, IS_ERR(decomp));
		if (IS_ERR(decomp))
			goto out;
		ret = crypto_comp_decompress(decomp, compressed, compressed_len,
					     dst, &decompressed_len);
		crypto_free_comp(decomp);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out;
		KUNIT_EXPECT_EQ(test, decompressed_len, PAGE_SIZE);
		KUNIT_EXPECT_MEMEQ(test, src, dst, PAGE_SIZE);
		total += compressed_len;
		worst = max(worst, compressed_len);
	}

	/*
	 * Report what the cache costs in ratio across its whole life, not just
	 * that each frame decoded.  The selector decides per block whether the
	 * standing table is still worth using, and a block that keeps a table
	 * it should have rebuilt shows up here and nowhere else - the
	 * per-frame check above only requires beating PAGE_SIZE.  The bound is
	 * the huge-class boundary, since a payload past it costs a whole page
	 * of backing store.
	 */
	kunit_info(test, "%u pages through one cache: %llu bytes total, %u worst, %llu mean",
		   MEMCOMP_TEST_HUF_CACHE_PAGES, total, worst,
		   total / MEMCOMP_TEST_HUF_CACHE_PAGES);
	KUNIT_EXPECT_LT(test, worst, (unsigned int)PAGE_SIZE);

out:
	memcomp_reclaim_ctx_put(ctx);
}

static void memcomp_test_zstd_huf_cache_shift(struct kunit *test)
{
	struct memcomp_reclaim_ctx *ctx;
	const char *driver;
	struct crypto_comp *shared = NULL;
	unsigned int worst = 0;
	unsigned int page;
	u8 *compressed;
	u8 *dst;
	u8 *src;
	u64 total = 0;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	compressed = kunit_kmalloc(test, MEMCOMP_MAX_COMPRESSED_SIZE,
				   GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, compressed);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	driver = crypto_tfm_alg_driver_name(crypto_comp_tfm(ctx->tfm));
	if (!memcomp_driver_is_zstd(driver)) {
		memcomp_reclaim_ctx_put(ctx);
		kunit_skip(test, "test requires zstd-generic");
		return;
	}

	/*
	 * One decoder for every frame, which is what the fault path uses: the
	 * decode-side HUF_DTableCache only ever hits on a DCtx that has seen a
	 * frame before.  A fresh decoder per frame - what the sibling case
	 * does, to prove frames are self-contained - leaves that cache empty on
	 * every lookup, so nothing there exercises it.
	 *
	 * The shift is what makes this a test of the cache rather than of the
	 * table: the two halves carry different Huffman descriptions, so a
	 * lookup that skipped HUF_dtableCacheLookup()'s memcmp would serve the
	 * first half's table to the second half and hand back wrong bytes.
	 */
	shared = crypto_alloc_comp("zstd-generic", 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(shared));

	for (page = 0; page < MEMCOMP_TEST_HUF_CACHE_PAGES; ++page) {
		unsigned int compressed_len = MEMCOMP_MAX_COMPRESSED_SIZE;
		unsigned int decompressed_len = PAGE_SIZE;
		unsigned int shared_len = PAGE_SIZE;
		struct crypto_comp *decomp;
		int ret;

		memcomp_test_fill_zstd_shift_page(src, page);
		ret = crypto_comp_compress(ctx->tfm, src, PAGE_SIZE, compressed,
					   &compressed_len);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out_shift;

		/* Every frame stays independently decodable across the shift. */
		decomp = crypto_alloc_comp("zstd-generic", 0, 0);
		KUNIT_EXPECT_FALSE(test, IS_ERR(decomp));
		if (IS_ERR(decomp))
			goto out_shift;
		ret = crypto_comp_decompress(decomp, compressed, compressed_len,
					     dst, &decompressed_len);
		crypto_free_comp(decomp);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out_shift;
		KUNIT_EXPECT_EQ(test, decompressed_len, PAGE_SIZE);
		KUNIT_EXPECT_MEMEQ(test, src, dst, PAGE_SIZE);

		/* And the same bytes back through the decoder that remembers. */
		memset(dst, 0, PAGE_SIZE);
		ret = crypto_comp_decompress(shared, compressed, compressed_len,
					     dst, &shared_len);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out_shift;
		KUNIT_EXPECT_EQ(test, shared_len, PAGE_SIZE);
		KUNIT_EXPECT_MEMEQ(test, src, dst, PAGE_SIZE);

		total += compressed_len;
		worst = max(worst, compressed_len);
	}

	kunit_info(test,
		   "%u pages across a distribution shift: %llu bytes total, %u worst, %llu mean",
		   MEMCOMP_TEST_HUF_CACHE_PAGES, total, worst,
		   total / MEMCOMP_TEST_HUF_CACHE_PAGES);
	KUNIT_EXPECT_LT(test, worst, (unsigned int)PAGE_SIZE);

	/*
	 * A truncated frame must be rejected, not read past, by a decoder that
	 * is holding a remembered description.
	 *
	 * This does not reach HUF_dtableCacheLookup()'s descSize > srcSize
	 * guard: frame and block header validation rejects a truncated buffer
	 * before any literal block is decoded, so the lookup never sees a
	 * short cSrcSize.  Covering that guard needs a hand-built frame
	 * declaring a litCSize smaller than the cached description, which
	 * zstd's own encoder cannot emit - fuzzer work, not a unit test.
	 * What this does cover is that a remembered table does not make the
	 * ordinary truncation paths misbehave, which is worth having under
	 * KASAN.
	 */
	{
		unsigned int last_len = 0;
		unsigned int cut;

		memcomp_test_fill_zstd_shift_page(src,
						  MEMCOMP_TEST_HUF_CACHE_PAGES - 1);
		last_len = MEMCOMP_MAX_COMPRESSED_SIZE;
		if (crypto_comp_compress(ctx->tfm, src, PAGE_SIZE, compressed,
					 &last_len))
			goto out_shift;

		for (cut = 1; cut < last_len && cut <= 96; cut++) {
			u8 *cropped = kunit_kmalloc(test, cut, GFP_KERNEL);
			unsigned int out_len = PAGE_SIZE;

			KUNIT_ASSERT_NOT_NULL(test, cropped);
			memcpy(cropped, compressed, cut);
			/* Any answer but a read past the end is acceptable. */
			(void)crypto_comp_decompress(shared, cropped, cut,
						     dst, &out_len);
			kunit_kfree(test, cropped);
		}
	}

out_shift:
	if (!IS_ERR_OR_NULL(shared))
		crypto_free_comp(shared);
	memcomp_reclaim_ctx_put(ctx);
}

static void memcomp_test_direct_staging_execution(struct kunit *test)
{
	struct memcomp_direct_staging *held[MEMCOMP_MAX_COMP_CTXS] = {};
	struct memcomp_reclaim_ctx *ctx;
	struct memcomp_store_ctx store = {};
	unsigned int nr_staging = READ_ONCE(memcomp_nr_direct_staging);
	unsigned int length = 0;
	unsigned int held_count = 0;
	unsigned long handle = 0;
	unsigned int expected;
	unsigned int i;
	u64 lease_end;
	u64 lease_start;
	void *ctx_dst;
	int before;
	u8 pool_idx = 0;
	u8 *src;
	int ret;

	if (!memcomp_is_enabled() || !nr_staging ||
	    IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "direct staging is unavailable");
		return;
	}
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = (i >> 5) ^ (i >> 9);

	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	if (!ctx->direct_staging) {
		memcomp_reclaim_ctx_put(ctx);
		kunit_skip(test, "current compressor is not staging-safe");
		return;
	}
	before = preempt_count();
	ret = memcomp_prepare_payload_locked(&ctx, src, PAGE_SIZE,
					     &length, &handle, &pool_idx,
						 false, &store, NULL);
	expected = before + PREEMPT_DISABLE_OFFSET;
	if (!store.staging) {
		memcomp_test_release_prepared(ctx, &store, length, handle,
					      pool_idx);
		kunit_skip(test, "direct staging slot was unavailable");
		return;
	}
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	KUNIT_EXPECT_NOT_NULL(test, store.staging);
	KUNIT_EXPECT_EQ(test, preempt_count(), before);
	if (store.staging) {
		KUNIT_EXPECT_EQ(test,
				store.staging->kunit_compress_preempt_count,
				(int)expected);
		KUNIT_EXPECT_EQ(test,
				store.staging->kunit_detach_preempt_count,
				(int)expected);
		KUNIT_EXPECT_PTR_EQ(test,
				    store.staging->kunit_compress_dst,
				    store.staging->dst);
		lease_end = store.staging->timing.timing_lease_end_ns;
		lease_start = store.staging->timing.timing_start_ns;
		if (store.staging->timing.timing_active)
			KUNIT_EXPECT_GT(test, lease_end, lease_start);
	}
	memcomp_test_release_prepared(ctx, &store, length, handle,
				      pool_idx);

	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	length = 0;
	handle = 0;
	ctx->kunit_compress_error = -EIO;
	before = preempt_count();
	ret = memcomp_prepare_payload_locked(&ctx, src, PAGE_SIZE,
					     &length, &handle, &pool_idx,
						 false, &store, NULL);
	if (!store.staging) {
		memcomp_test_release_prepared(ctx, &store, length, handle,
					      pool_idx);
		kunit_skip(test, "direct staging slot was unavailable");
		return;
	}
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	KUNIT_EXPECT_NOT_NULL(test, store.staging);
	KUNIT_EXPECT_EQ(test, preempt_count(), before);
	memcomp_test_release_prepared(ctx, &store, length, handle,
				      pool_idx);

	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	length = 0;
	handle = 0;
	store.kunit_zs_failures = 2;
	before = preempt_count();
	ret = memcomp_prepare_payload_locked(&ctx, src, PAGE_SIZE,
					     &length, &handle, &pool_idx,
						 false, &store, NULL);
	store.kunit_zs_failures = 0;
	if (!store.staging) {
		memcomp_test_release_prepared(ctx, &store, length, handle,
					      pool_idx);
		kunit_skip(test, "direct staging slot was unavailable");
		return;
	}
	KUNIT_EXPECT_LT(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	KUNIT_EXPECT_NOT_NULL(test, store.staging);
	KUNIT_EXPECT_EQ(test, preempt_count(), before);
	memcomp_test_release_prepared(ctx, &store, length, handle,
				      pool_idx);

	for (i = 0; i < nr_staging; i++) {
		held[i] = memcomp_direct_staging_try_get();
		if (!held[i])
			break;
		held_count++;
	}
	if (held_count != nr_staging)
		goto out_held;
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_EXPECT_NOT_NULL(test, ctx);
	if (!ctx)
		goto out_held;
	ctx_dst = ctx->dst;
	length = 0;
	handle = 0;
	before = preempt_count();
	ret = memcomp_prepare_payload_locked(&ctx, src, PAGE_SIZE,
					     &length, &handle, &pool_idx,
						 false, &store, NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_NOT_NULL(test, ctx);
	KUNIT_EXPECT_PTR_EQ(test, store.staging, NULL);
	KUNIT_EXPECT_EQ(test, preempt_count(), before);
	KUNIT_EXPECT_EQ(test, ctx->kunit_compress_preempt_count, before);
	KUNIT_EXPECT_PTR_EQ(test, ctx->kunit_compress_dst, ctx_dst);
	memcomp_test_release_prepared(ctx, &store, length, handle,
				      pool_idx);
out_held:
	for (i = 0; i < held_count; i++)
		memcomp_direct_staging_put(held[i]);
}

static void memcomp_test_store_consumes_context(struct kunit *test)
{
	struct memcomp_reclaim_ctx *ctx;
	unsigned int i;
	bool detached = false;
	bool success;
	void *dst;
	struct folio *folio;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_EXPECT_FALSE(test,
			   memcomp_store_after_unmap(folio, &ctx));
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	folio_put(folio);

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_lock(folio);
	folio->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(folio);
	KUNIT_EXPECT_TRUE(test, memcomp_reserve(folio));
	if (!is_memcomp_entry(folio->swap))
		goto out_success_folio;
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_EXPECT_NOT_NULL(test, ctx);
	if (!ctx)
		goto out_success_rollback;
	success = memcomp_store_after_unmap(folio, &ctx);
	KUNIT_EXPECT_TRUE(test, success);
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	if (success)
		memcomp_invalidate_folio(folio);
	else
		memcomp_rollback_folio(folio);
	goto out_success_folio;

out_success_rollback:
	memcomp_rollback_folio(folio);
out_success_folio:
	folio_unlock(folio);
	folio_put(folio);

	if (IS_ENABLED(CONFIG_PREEMPT_RT) ||
	    !READ_ONCE(memcomp_nr_direct_staging))
		return;
	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_lock(folio);
	folio->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(folio);
	dst = kmap_local_folio(folio, 0);
	for (i = 0; i < PAGE_SIZE; i++)
		((u8 *)dst)[i] = (i >> 5) ^ (i >> 9);
	kunmap_local(dst);
	KUNIT_EXPECT_TRUE(test, memcomp_reserve(folio));
	if (!is_memcomp_entry(folio->swap))
		goto out_failure_folio;
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	KUNIT_EXPECT_NOT_NULL(test, ctx);
	if (!ctx)
		goto out_failure_rollback;
	ctx->kunit_refresh_commit_error = -EIO;
	ctx->kunit_detached = &detached;
	success = memcomp_store_after_unmap(folio, &ctx);
	if (!detached) {
		if (ctx)
			memcomp_reclaim_ctx_put(ctx);
		memcomp_rollback_folio(folio);
		kunit_skip(test, "direct staging slot was unavailable");
		goto out_failure_folio;
	}
	KUNIT_EXPECT_FALSE(test, success);
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	memcomp_rollback_folio(folio);
	goto out_failure_folio;

out_failure_rollback:
	memcomp_rollback_folio(folio);
out_failure_folio:
	folio_unlock(folio);
	folio_put(folio);
}

static unsigned int memcomp_test_reclaimer_enter(void)
{
	unsigned int flags = current->flags & (PF_MEMALLOC | PF_KSWAPD);

	current->flags |= PF_MEMALLOC | PF_KSWAPD;
	return flags;
}

static void memcomp_test_reclaimer_exit(unsigned int flags)
{
	current_restore_flags(flags, PF_MEMALLOC | PF_KSWAPD);
}

static void memcomp_test_async_pool(struct kunit *test)
{
	struct memcomp_async_pool *pool;
	struct memcomp_async *a[3], *got[3];
	unsigned int i;

	pool = kunit_kzalloc(test, sizeof(*pool), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pool);
	spin_lock_init(&pool->lock);
	for (i = 0; i < ARRAY_SIZE(a); i++) {
		a[i] = kunit_kzalloc(test, sizeof(*a[i]), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, a[i]);
		init_completion(&a[i]->done);
		a[i]->id = i;
		pool->batches[i] = a[i];
	}
	pool->free = GENMASK(ARRAY_SIZE(a) - 1, 0);
	pool->nr = ARRAY_SIZE(a);

	for (i = 0; i < ARRAY_SIZE(got) - 1; i++) {
		got[i] = memcomp_async_take(pool, 1);
		KUNIT_ASSERT_PTR_EQ(test, got[i], a[i]);
	}
	KUNIT_EXPECT_NULL(test, memcomp_async_take(pool, 1));
	got[i] = memcomp_async_take(pool, 0);
	KUNIT_ASSERT_PTR_EQ(test, got[i], a[i]);
	KUNIT_EXPECT_NULL(test, memcomp_async_take(pool, 0));
	for (i = 0; i < ARRAY_SIZE(got); i++)
		memcomp_async_give(pool, got[i]);
	KUNIT_EXPECT_EQ(test, pool->free, GENMASK(ARRAY_SIZE(a) - 1, 0));
}

static void memcomp_test_same_filled_plausible(struct kunit *test)
{
	const unsigned long e = 0x5a5a12345a5a1234UL;
	const unsigned int last = PAGE_SIZE / sizeof(unsigned long) - 1;
	unsigned long *w;

	w = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, w);
	memset_l(w, e, last + 1);

	KUNIT_EXPECT_TRUE(test, memcomp_same_filled_plausible(w, PAGE_SIZE, e));
	KUNIT_EXPECT_FALSE(test,
			   memcomp_same_filled_plausible(w, PAGE_SIZE / 2, e));

	w[last / 2] ^= 1;
	KUNIT_EXPECT_FALSE(test, memcomp_same_filled_plausible(w, PAGE_SIZE, e));
	w[last / 2] ^= 1;

	w[last] ^= 1;
	KUNIT_EXPECT_FALSE(test, memcomp_same_filled_plausible(w, PAGE_SIZE, e));
	w[last] ^= 1;
	KUNIT_EXPECT_FALSE(test,
			   memcomp_same_filled_plausible(w, PAGE_SIZE, ~e));
}

static void memcomp_test_ctx_timing_helpers(struct kunit *test)
{
	struct memcomp_ctx_timing_record detached = {
		.timing_start_ns = 10,
		.timing_lease_end_ns = 110,
		.timing_pages = 1,
		.timing_source = MEMCOMP_RECLAIM_DIRECT,
		.timing_active = true,
	};
	struct memcomp_ctx_timing_stats stats = {};
	struct memcomp_ctx_timing_stats *direct_stats;
	struct memcomp_ctx_timing_record bucket = {
		.timing_store_start_ns = 1,
		.timing_compress_ns = 300,
		.timing_compress_cpu_ns = 240,
		.timing_compress_call_cpu_max_ns = 120,
		.timing_pages = 3,
		.timing_store_success = true,
	};
	struct memcomp_reclaim_ctx sampled = {
		.timing = {
			.timing_compress_call_cpu_max_ns = 200,
		},
	};
	struct memcomp_compress_timing_stamp delta = {
		.wall_ns = 100,
		.cpu_ns = 80,
	};
	struct memcomp_ctx_timing_record ctx = {
		.timing_active = true,
	};
	struct memcomp_ctx_wait_stats *wait_stats;
	atomic64_t maximum = ATOMIC64_INIT(5);
	enum memcomp_ctx_timing_kind kind;
	u64 global_wait_events;
	s64 lease_ns;
	s64 samples;
	u64 source_wait_events;
	u64 wait_start_ns;
	unsigned int outcome = MEMCOMP_COMPRESS_TIMING_RAW_HUGE;
	u32 seq = MEMCOMP_CTX_TIMING_SAMPLE_MASK - 1;
	u32 seed;

	KUNIT_EXPECT_FALSE(test, memcomp_ctx_timing_should_sample(&seq));
	KUNIT_EXPECT_TRUE(test, memcomp_ctx_timing_should_sample(&seq));
	KUNIT_EXPECT_EQ(test, seq,
			(u32)MEMCOMP_CTX_TIMING_SAMPLE_INTERVAL);
	seed = memcomp_ctx_timing_seed(1);
	KUNIT_EXPECT_LT(test, seed,
			(u32)MEMCOMP_CTX_TIMING_SAMPLE_INTERVAL);
	KUNIT_EXPECT_NE(test, seed, memcomp_ctx_timing_seed(2));
	KUNIT_EXPECT_EQ(test,
			memcomp_ctx_timing_remainder(100, 50), 50ULL);
	KUNIT_EXPECT_EQ(test,
			memcomp_ctx_timing_remainder(40, 50), 0ULL);
	KUNIT_EXPECT_EQ(test,
			memcomp_ctx_timing_remainder(20, 30), 0ULL);
	kind = memcomp_ctx_timing_aggregate_kind(MEMCOMP_RECLAIM_DIRECT);
	KUNIT_EXPECT_EQ(test, kind, MEMCOMP_CTX_TIMING_DIRECT);
	kind = memcomp_ctx_timing_aggregate_kind(MEMCOMP_RECLAIM_KSWAPD);
	KUNIT_EXPECT_EQ(test, kind, MEMCOMP_CTX_TIMING_BACKGROUND);

	sampled.timing_seq[MEMCOMP_RECLAIM_KSWAPD] =
		MEMCOMP_CTX_TIMING_SAMPLE_MASK;
	memcomp_ctx_timing_start(&sampled, 3,
				 MEMCOMP_RECLAIM_KSWAPD);
	KUNIT_EXPECT_TRUE(test, sampled.timing.timing_active);
	KUNIT_EXPECT_EQ(test, sampled.timing.timing_pages, 3U);
	KUNIT_EXPECT_EQ(test, sampled.timing.timing_source,
			MEMCOMP_RECLAIM_KSWAPD);
	KUNIT_EXPECT_EQ(test,
			sampled.timing.timing_compress_call_cpu_max_ns, 0ULL);
	KUNIT_EXPECT_EQ(test,
			sampled.timing_seq[MEMCOMP_RECLAIM_KSWAPD],
			(u32)MEMCOMP_CTX_TIMING_SAMPLE_INTERVAL);
	KUNIT_EXPECT_EQ(test,
			sampled.timing_seq[MEMCOMP_RECLAIM_DIRECT], 0U);
	sampled.timing.timing_active = false;

	memcomp_atomic64_update_max(&maximum, 3);
	KUNIT_EXPECT_EQ(test, atomic64_read(&maximum), 5LL);
	memcomp_atomic64_update_max(&maximum, 9);
	KUNIT_EXPECT_EQ(test, atomic64_read(&maximum), 9LL);
	direct_stats =
		&memcomp_ctx_timing[MEMCOMP_CTX_TIMING_DIRECT];
	lease_ns = atomic64_read(&direct_stats->lease_ns);
	samples = atomic64_read(&direct_stats->samples);
	memcomp_ctx_timing_account(&detached);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&direct_stats->lease_ns),
			lease_ns + 100);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&direct_stats->samples),
			samples + 1);
	KUNIT_EXPECT_FALSE(test, detached.timing_active);

	memcomp_compress_timing_account(&ctx, delta, outcome);
	KUNIT_EXPECT_EQ(test,
			ctx.timing_compress_outcome_calls[outcome], 1U);
	KUNIT_EXPECT_EQ(test,
			ctx.timing_compress_outcome_ns[outcome], 100ULL);
	KUNIT_EXPECT_EQ(test,
			ctx.timing_compress_outcome_cpu_ns[outcome], 80ULL);
	KUNIT_EXPECT_EQ(test, ctx.timing_compress_call_cpu_max_ns, 80ULL);
	delta.cpu_ns = 60;
	memcomp_compress_timing_account(&ctx, delta, outcome);
	KUNIT_EXPECT_EQ(test, ctx.timing_compress_call_cpu_max_ns, 80ULL);
	delta.cpu_ns = 120;
	memcomp_compress_timing_account(&ctx, delta, outcome);
	delta.cpu_ns = 90;
	memcomp_compress_timing_account(&ctx, delta, outcome);
	KUNIT_EXPECT_EQ(test, ctx.timing_compress_call_cpu_max_ns, 120ULL);
	ctx.timing_active = false;
	outcome = MEMCOMP_COMPRESS_TIMING_ERROR;
	memcomp_compress_timing_account(&ctx, delta, outcome);
	KUNIT_EXPECT_EQ(test,
			ctx.timing_compress_outcome_calls[outcome], 0U);

	global_wait_events = atomic64_read(&memcomp_comp_ctx_waits);
	wait_stats =
		&memcomp_ctx_source_wait[MEMCOMP_RECLAIM_KSWAPD];
	source_wait_events = atomic64_read(&wait_stats->events);
	memcomp_ctx_timing_wait_start(MEMCOMP_RECLAIM_KSWAPD);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_comp_ctx_waits),
			global_wait_events + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&wait_stats->events),
			source_wait_events + 1);
	wait_start_ns = ktime_get_ns();
	memcomp_ctx_timing_wait_end(wait_start_ns,
				    MEMCOMP_RECLAIM_KSWAPD);

	outcome = MEMCOMP_COMPRESS_TIMING_COMPRESSIBLE;
	bucket.timing_compress_outcome_calls[outcome] = 2;
	bucket.timing_compress_outcome_ns[outcome] = 300;
	bucket.timing_compress_outcome_cpu_ns[outcome] = 240;
	memcomp_ctx_timing_account_bucket(&bucket, &stats, 1000, 100, 50);
	bucket.timing_store_success = false;
	bucket.timing_compress_ns = 70;
	bucket.timing_compress_cpu_ns = 50;
	bucket.timing_compress_call_cpu_max_ns = 50;
	bucket.timing_compress_outcome_calls[outcome] = 1;
	bucket.timing_compress_outcome_ns[outcome] = 70;
	bucket.timing_compress_outcome_cpu_ns[outcome] = 50;
	memcomp_ctx_timing_account_bucket(&bucket, &stats, 500, 80, 30);
	KUNIT_EXPECT_EQ(test, atomic64_read(&stats.samples), 2LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&stats.pages), 6LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&stats.store_samples), 2LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&stats.store_success), 1LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&stats.compress_ns), 370LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&stats.compress_cpu_ns), 290LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_call_cpu_max_ns), 120LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_store_success_ns), 300LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_store_success_cpu_ns), 240LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_store_fail_ns), 70LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_store_fail_cpu_ns), 50LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_outcome[outcome].calls), 3LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_outcome[outcome].wall_ns),
			370LL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&stats.compress_outcome[outcome].cpu_ns),
			290LL);
}

static void memcomp_test_comp_ctx_configuration(struct kunit *test)
{
	unsigned int i;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	KUNIT_ASSERT_GT(test, memcomp_nr_comp_ctxs, 0U);
	KUNIT_EXPECT_LE(test, memcomp_nr_comp_ctxs,
			MEMCOMP_MAX_COMP_CTXS);
	KUNIT_EXPECT_LT(test, memcomp_nr_reserved_comp_ctxs,
			memcomp_nr_comp_ctxs);
	KUNIT_EXPECT_LE(test, memcomp_nr_borrowable_comp_ctxs,
			memcomp_nr_reserved_comp_ctxs);
	if (memcomp_nr_reserved_comp_ctxs)
		KUNIT_EXPECT_LT(test, memcomp_nr_borrowable_comp_ctxs,
				memcomp_nr_reserved_comp_ctxs);
	KUNIT_EXPECT_EQ(test, hweight_long(memcomp_comp_ctx_all),
			(unsigned long)memcomp_nr_comp_ctxs);
	KUNIT_EXPECT_EQ(test, hweight_long(memcomp_comp_ctx_foreground),
			(unsigned long)(memcomp_nr_comp_ctxs -
			memcomp_nr_reserved_comp_ctxs));
	for (i = 0; i < memcomp_nr_comp_ctxs; i++) {
		KUNIT_EXPECT_NOT_NULL(test, memcomp_comp_ctxs[i].tfm);
		KUNIT_EXPECT_NOT_NULL(test, memcomp_comp_ctxs[i].dst);
	}
}

static void memcomp_test_stock_transforms(struct kunit *test)
{
	struct crypto_comp *tfm;
	const char *driver;
	unsigned int i;
	int cpu;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	KUNIT_ASSERT_GT(test, memcomp_nr_comp_ctxs, 0U);
	tfm = crypto_alloc_comp(memcomp_compressor, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(tfm));
	driver = crypto_tfm_alg_driver_name(crypto_comp_tfm(tfm));
	for (i = 0; i < memcomp_nr_comp_ctxs; i++) {
		struct crypto_comp *comp = memcomp_comp_ctxs[i].tfm;

		KUNIT_EXPECT_NOT_NULL(test, comp);
		if (comp)
			KUNIT_EXPECT_STREQ(test,
					   crypto_tfm_alg_driver_name(crypto_comp_tfm(comp)),
				driver);
	}
	for_each_possible_cpu(cpu) {
		struct crypto_comp *decomp =
			per_cpu(memcomp_decomp_ctxs, cpu).tfm;

		KUNIT_EXPECT_NOT_NULL(test, decomp);
		if (decomp)
			KUNIT_EXPECT_STREQ(test,
					   crypto_tfm_alg_driver_name(crypto_comp_tfm(decomp)),
				driver);
	}
	crypto_free_comp(tfm);
}

static void memcomp_test_zs_gfp(struct kunit *test)
{
	gfp_t fast = memcomp_zs_gfp(true);
	gfp_t slow = memcomp_zs_gfp(false);

	KUNIT_EXPECT_FALSE(test, fast & __GFP_DIRECT_RECLAIM);
	KUNIT_EXPECT_TRUE(test, fast & __GFP_KSWAPD_RECLAIM);
	KUNIT_EXPECT_FALSE(test, fast & (__GFP_IO | __GFP_FS));
	KUNIT_EXPECT_TRUE(test, slow & __GFP_DIRECT_RECLAIM);
	KUNIT_EXPECT_TRUE(test, slow & __GFP_KSWAPD_RECLAIM);
	KUNIT_EXPECT_FALSE(test, slow & (__GFP_IO | __GFP_FS));
}

static void memcomp_test_reclaim_ctx_admission(struct kunit *test)
{
	struct memcomp_reclaim_ctx *ctx;
	s64 busy_pages;
	s64 all_busy_pages;
	s64 ctx_busy;
	s64 reserved_free_pages;
	s64 store_fail;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	ctx_busy = atomic64_read(&memcomp_ctx_busy);
	reserved_free_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages);
	all_busy_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages);
	busy_pages = reserved_free_pages + all_busy_pages;
	store_fail = atomic64_read(&memcomp_bg_store_fail) +
		atomic64_read(&memcomp_direct_store_fail) +
		atomic64_read(&memcomp_pageout_store_fail);
	atomic_set(&memcomp_kunit_ctx_get_fail, 1);
	ctx = memcomp_reclaim_ctx_get(3, MEMCOMP_RECLAIM_DIRECT);
	atomic_set(&memcomp_kunit_ctx_get_fail, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_ctx_busy),
			ctx_busy + 1);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages) +
			atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages),
			busy_pages + 3);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages),
			reserved_free_pages);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages),
			all_busy_pages + 3);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_bg_store_fail) +
			atomic64_read(&memcomp_direct_store_fail) +
			atomic64_read(&memcomp_pageout_store_fail),
			store_fail + 3);
}

static void memcomp_test_reclaim_ctx_precheck(struct kunit *test)
{
	struct reclaim_state reclaim_state = {};
	struct reclaim_state *old_reclaim_state;
	enum memcomp_comp_ctx_miss miss =
		MEMCOMP_COMP_CTX_BORROW_LIMIT;
	s64 all_busy_pages;
	s64 borrow_limit_pages;
	s64 borrow_waiter_pages;
	s64 bg_fail;
	s64 ctx_busy;
	s64 direct_fail;
	s64 early_reject;
	s64 early_reject_pages;
	s64 pageout_fail;
	s64 reserved_free_pages;
	bool admitted;

	ctx_busy = atomic64_read(&memcomp_ctx_busy);
	reserved_free_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages);
	all_busy_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages);
	borrow_limit_pages =
		atomic64_read(&memcomp_reclaim_ctx_borrow_limit_pages);
	borrow_waiter_pages =
		atomic64_read(&memcomp_reclaim_ctx_borrow_waiter_pages);
	bg_fail = atomic64_read(&memcomp_bg_store_fail);
	direct_fail = atomic64_read(&memcomp_direct_store_fail);
	pageout_fail = atomic64_read(&memcomp_pageout_store_fail);
	early_reject = atomic64_read(&memcomp_reclaim_ctx_early_reject);
	early_reject_pages =
		atomic64_read(&memcomp_reclaim_ctx_early_reject_pages);

	admitted = memcomp_reclaim_ctx_precheck(3,
						MEMCOMP_RECLAIM_KSWAPD);
	KUNIT_EXPECT_TRUE(test, admitted);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_ctx_busy), ctx_busy);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_early_reject),
			early_reject);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_early_reject_pages),
			early_reject_pages);

	old_reclaim_state = current->reclaim_state;
	current->reclaim_state = &reclaim_state;
	admitted = memcomp_reclaim_ctx_precheck_result(3, miss);
	current->reclaim_state = old_reclaim_state;
	KUNIT_EXPECT_FALSE(test, admitted);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_ctx_busy),
			ctx_busy + 1);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages),
			reserved_free_pages + 3);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages),
			all_busy_pages);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_borrow_limit_pages),
			borrow_limit_pages + 3);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_borrow_waiter_pages),
			borrow_waiter_pages);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_bg_store_fail),
			bg_fail);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_direct_store_fail),
			direct_fail + 3);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pageout_store_fail),
			pageout_fail);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_early_reject),
			early_reject + 1);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_reclaim_ctx_early_reject_pages),
			early_reject_pages + 3);
}

static void memcomp_test_reserve_eligibility(struct kunit *test)
{
	struct folio *folio;

	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_EXPECT_FALSE(test, memcomp_reserve_eligible(folio));
	folio_lock(folio);
	KUNIT_EXPECT_FALSE(test, memcomp_reserve_eligible(folio));
	folio->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(folio);
	KUNIT_EXPECT_FALSE(test, folio_test_ksm(folio));
	KUNIT_EXPECT_TRUE(test, memcomp_reserve_eligible(folio));
#ifdef CONFIG_KSM
	/*
	 * Eligibility only reads the mapping flags.  No stable-tree node or
	 * real KSM mapping is needed to check that KSM folios are rejected.
	 * Restore mapping before releasing the synthetic folio below.
	 */
	folio->mapping = (void *)PAGE_MAPPING_KSM;
	KUNIT_EXPECT_TRUE(test, folio_test_ksm(folio));
	KUNIT_EXPECT_FALSE(test, memcomp_reserve_eligible(folio));
#endif
	folio->mapping = NULL;
	folio_clear_swapbacked(folio);
	folio_unlock(folio);
	folio_put(folio);
}

static void memcomp_test_admission_rollback_unescaped(struct kunit *test)
{
	struct memcomp_reclaim_ctx *ctx;
	struct folio *folio;
	unsigned long token;
	s64 pending;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	pending = atomic64_read(&memcomp_pending_pages);
	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_lock(folio);
	folio->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(folio);
	KUNIT_ASSERT_TRUE(test, memcomp_reserve(folio));
	token = swp_offset(folio->swap);
	KUNIT_ASSERT_NE(test, token, 0UL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages),
			pending + 1);

	atomic_set(&memcomp_kunit_ctx_get_fail, 1);
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	atomic_set(&memcomp_kunit_ctx_get_fail, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
	memcomp_rollback_folio(folio);
	KUNIT_EXPECT_EQ(test, folio->swap.val, 0UL);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token));
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages),
			pending);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);

	folio->mapping = NULL;
	folio_clear_swapbacked(folio);
	folio_unlock(folio);
	folio_put(folio);
}

static void memcomp_test_samefill(struct kunit *test)
{
	unsigned long *page;
	unsigned long element = 0;
	unsigned int i;

	page = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_TRUE(test, page_same_filled(page, &element));
	KUNIT_EXPECT_EQ(test, element, 0UL);

	for (i = 0; i < PAGE_SIZE / sizeof(*page); i++)
		page[i] = 0xa5a5a5a5a5a5a5a5UL;
	KUNIT_EXPECT_TRUE(test, page_same_filled(page, &element));
	KUNIT_EXPECT_EQ(test, element, 0xa5a5a5a5a5a5a5a5UL);

	page[PAGE_SIZE / sizeof(*page) - 1] ^= 1;
	KUNIT_EXPECT_FALSE(test, page_same_filled(page, &element));

	/*
	 * A single differing word must be found wherever it sits: at either
	 * probe position, at the midpoint probe, and at offsets the probes do
	 * not cover so the full scan still has to run.
	 */
	{
		const unsigned int words = PAGE_SIZE / sizeof(*page);
		static const unsigned int frac[] = { 1, 2, 3, 4, 5, 7 };
		unsigned int f;

		for (i = 0; i < words; i++)
			page[i] = 0xa5a5a5a5a5a5a5a5UL;
		page[1] ^= 1;
		KUNIT_EXPECT_FALSE(test, page_same_filled(page, &element));
		page[1] ^= 1;

		page[(words - 1) / 2] ^= 1;
		KUNIT_EXPECT_FALSE(test, page_same_filled(page, &element));
		page[(words - 1) / 2] ^= 1;
		KUNIT_EXPECT_TRUE(test, page_same_filled(page, &element));

		for (f = 0; f < ARRAY_SIZE(frac); f++) {
			unsigned int at = words / frac[f];

			if (at >= words)
				at = words - 1;
			page[at] ^= 1;
			KUNIT_EXPECT_FALSE(test, page_same_filled(page,
								  &element));
			page[at] ^= 1;
		}
		KUNIT_EXPECT_TRUE(test, page_same_filled(page, &element));
		KUNIT_EXPECT_EQ(test, element, 0xa5a5a5a5a5a5a5a5UL);

		/*
		 * A long leading run followed by a mismatch is the case the
		 * probe exists for; it must still be rejected.
		 */
		page[words - 2] ^= 1;
		KUNIT_EXPECT_FALSE(test, page_same_filled(page, &element));
	}
}

static void memcomp_test_raw_payload_threshold(struct kunit *test)
{
	enum memcomp_raw_reason reason;

	reason = memcomp_payload_raw_reason(2048, PAGE_SIZE, 3264);
	KUNIT_EXPECT_EQ(test, reason, MEMCOMP_RAW_NONE);
	reason = memcomp_payload_raw_reason(3263, PAGE_SIZE, 3264);
	KUNIT_EXPECT_EQ(test, reason, MEMCOMP_RAW_NONE);
	reason = memcomp_payload_raw_reason(3264, PAGE_SIZE, 3264);
	KUNIT_EXPECT_EQ(test, reason, MEMCOMP_RAW_HUGE_CLASS);
	reason = memcomp_payload_raw_reason(PAGE_SIZE - 1, PAGE_SIZE, 3264);
	KUNIT_EXPECT_EQ(test, reason, MEMCOMP_RAW_HUGE_CLASS);
	reason = memcomp_payload_raw_reason(PAGE_SIZE, PAGE_SIZE, 3264);
	KUNIT_EXPECT_EQ(test, reason, MEMCOMP_RAW_GE_PAGE);
	reason = memcomp_payload_raw_reason(PAGE_SIZE - 1, PAGE_SIZE, 0);
	KUNIT_EXPECT_EQ(test, reason, MEMCOMP_RAW_NONE);
}

static void memcomp_test_compressor_e2big(struct kunit *test)
{
	struct memcomp_reclaim_ctx *ctx;
	struct memcomp_store_ctx store = {};
	unsigned int reclaim_flags;
	unsigned int length;
	unsigned long handle;
	s64 reject_ge, fail;
	u8 pool_idx;
	const u8 *payload;
	u8 *src;
	int ret;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	memcomp_test_fill_zstd_ratio_page(src);

	reclaim_flags = memcomp_test_reclaimer_enter();
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_KSWAPD);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	reject_ge = atomic64_read(&memcomp_raw_reject_ge_page);
	fail = atomic64_read(&memcomp_compress_fail);
	length = 0;
	handle = 0;
	pool_idx = 0;
	ctx->kunit_compress_error = -E2BIG;
	ret = memcomp_prepare_payload_locked(&ctx, src, PAGE_SIZE,
					     &length, &handle, &pool_idx,
						 false, NULL, NULL);
	KUNIT_EXPECT_EQ(test, ret, -E2BIG);
	KUNIT_EXPECT_EQ(test, length, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_raw_reject_ge_page),
			reject_ge + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compress_fail), fail);

	ctx->kunit_compress_error = -E2BIG;
	ret = memcomp_prepare_payload_locked(&ctx, src, PAGE_SIZE,
					     &length, &handle, &pool_idx,
						 true, NULL, NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, length, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compress_fail), fail);
	if (!ret && length == PAGE_SIZE) {
		struct zs_pool *pool = memcomp_pools[pool_idx];

		payload = zs_map_object(pool, handle, ZS_MM_RO);
		KUNIT_EXPECT_NOT_NULL(test, payload);
		if (payload) {
			KUNIT_EXPECT_MEMEQ(test, payload, src, PAGE_SIZE);
			zs_unmap_object(pool, handle);
		}
	}
	memcomp_test_release_prepared(ctx, &store, ret ? 0 : length,
				      handle, pool_idx);
	memcomp_test_reclaimer_exit(reclaim_flags);
}

static void memcomp_test_mixed_raw_net_reclaim(struct kunit *test)
{
	unsigned int lengths[MEMCOMP_MAX_COMPRESS_PAGES];
	bool reclaimable;
	unsigned int i;

	lengths[0] = PAGE_SIZE;
	KUNIT_EXPECT_FALSE(test, memcomp_payloads_net_reclaimable(lengths, 1));

	lengths[0] = PAGE_SIZE;
	lengths[1] = 0;
	KUNIT_EXPECT_TRUE(test, memcomp_payloads_net_reclaimable(lengths, 2));

	lengths[0] = PAGE_SIZE;
	lengths[1] = PAGE_SIZE;
	KUNIT_EXPECT_FALSE(test, memcomp_payloads_net_reclaimable(lengths, 2));

	lengths[0] = PAGE_SIZE;
	lengths[1] = PAGE_SIZE;
	lengths[2] = 1024;
	lengths[3] = 1024;
	KUNIT_EXPECT_TRUE(test, memcomp_payloads_net_reclaimable(lengths, 4));

	lengths[0] = PAGE_SIZE;
	lengths[1] = PAGE_SIZE;
	lengths[2] = PAGE_SIZE;
	lengths[3] = 0;
	KUNIT_EXPECT_TRUE(test, memcomp_payloads_net_reclaimable(lengths, 4));

	lengths[0] = PAGE_SIZE;
	lengths[1] = PAGE_SIZE;
	lengths[2] = PAGE_SIZE;
	lengths[3] = 1;
	KUNIT_EXPECT_FALSE(test, memcomp_payloads_net_reclaimable(lengths, 4));

	lengths[0] = 3263;
	KUNIT_EXPECT_TRUE(test, memcomp_payloads_net_reclaimable(lengths, 1));
	lengths[0] = PAGE_SIZE + 1;
	KUNIT_EXPECT_FALSE(test, memcomp_payloads_net_reclaimable(lengths, 1));

	for (i = 0; i < MEMCOMP_MAX_COMPRESS_PAGES; i++)
		lengths[i] = 1024;
	lengths[0] = PAGE_SIZE;
	reclaimable = memcomp_payloads_net_reclaimable(lengths,
						       ARRAY_SIZE(lengths));
	KUNIT_EXPECT_TRUE(test, reclaimable);

	for (i = 0; i < MEMCOMP_MAX_COMPRESS_PAGES; i++)
		lengths[i] = PAGE_SIZE;
	reclaimable = memcomp_payloads_net_reclaimable(lengths,
						       ARRAY_SIZE(lengths));
	KUNIT_EXPECT_FALSE(test, reclaimable);
}

static void memcomp_test_state_signed_sum(struct kunit *test)
{
	const struct memcomp_state_stats samples[] = {
		{
			.folio_owner_pages = 7,
			.samefill_pages = -2,
			.raw_pages = 2,
			.shmem_pages = 6,
		},
		{
			.folio_owner_pages = -4,
			.samefill_pages = 1,
			.raw_pages = -3,
			.shmem_pages = -1,
		},
	};
	struct memcomp_state_stats snapshot = {};
	int i;

#ifndef CONFIG_MEMCOMP_DEBUG_STATS
	kunit_skip(test, "state gauges require CONFIG_MEMCOMP_DEBUG_STATS");
	return;
#else
	for (i = 0; i < ARRAY_SIZE(samples); i++)
		memcomp_state_add(&snapshot, &samples[i]);
	/* Aggregate signed deltas first, then clamp once, not per CPU. */
	KUNIT_EXPECT_EQ(test, snapshot.folio_owner_pages, (s64)3);
	KUNIT_EXPECT_EQ(test, snapshot.samefill_pages, (s64)-1);
	KUNIT_EXPECT_EQ(test, snapshot.raw_pages, (s64)-1);
	KUNIT_EXPECT_EQ(test, snapshot.shmem_pages, (s64)5);
	memcomp_state_clamp(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.folio_owner_pages, (s64)3);
	KUNIT_EXPECT_EQ(test, snapshot.samefill_pages, (s64)0);
	KUNIT_EXPECT_EQ(test, snapshot.raw_pages, (s64)0);
	KUNIT_EXPECT_EQ(test, snapshot.shmem_pages, (s64)5);
#endif
}

static void memcomp_test_fill_incompressible(void *buf,
					     unsigned int nr_pages)
{
	u32 *words = buf;
	u32 value = 0x9e3779b9;
	unsigned int i;

	for (i = 0; i < nr_pages * PAGE_SIZE / sizeof(*words); i++) {
		value ^= value << 13;
		value ^= value >> 17;
		value ^= value << 5;
		words[i] = value;
	}
}

static void memcomp_test_incompressible_mark_lifetime(struct kunit *test)
{
	struct folio *folio, *dst;
	s64 changed;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	changed = atomic64_read(&memcomp_incompressible_changed_pages);
	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	dst = folio_alloc(GFP_KERNEL, 0);
	if (!dst)
		folio_put(folio);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	memcomp_test_fill_incompressible(folio_address(folio), 1);
	folio_lock(folio);
	folio_lock(dst);
	folio->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(folio);
	dst->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(dst);

	folio_set_incompressible(folio, memcomp_folio_hash(folio));
	KUNIT_EXPECT_TRUE(test, folio_test_incompressible(folio));
	KUNIT_EXPECT_FALSE(test, folio_memcomp_reserved(folio));
	KUNIT_EXPECT_FALSE(test, memcomp_release_folio(folio));
	memcomp_invalidate_folio(folio);
	memcomp_rollback_folio(folio);
	KUNIT_EXPECT_TRUE(test, folio_test_incompressible(folio));
	KUNIT_EXPECT_TRUE(test, memcomp_folio_incompressible(folio));
	KUNIT_EXPECT_TRUE(test, folio_test_incompressible(folio));
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_incompressible_changed_pages),
			changed);

	KUNIT_EXPECT_EQ(test, memcomp_migrate_folio(dst, folio), 0);
	KUNIT_EXPECT_EQ(test, dst->swap.val, folio->swap.val);
	dst->swap.val = 0;

	/* Written to since. */
	((u8 *)folio_address(folio))[PAGE_SIZE - 1] ^= 0x80;
	KUNIT_EXPECT_FALSE(test, memcomp_folio_incompressible(folio));
	KUNIT_EXPECT_FALSE(test, folio_test_incompressible(folio));
	KUNIT_EXPECT_EQ(test, folio->swap.val, 0UL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_incompressible_changed_pages),
			changed + 1);
	KUNIT_EXPECT_EQ(test, memcomp_migrate_folio(dst, folio), 0);
	KUNIT_EXPECT_EQ(test, dst->swap.val, 0UL);

	/* Reserved again. */
	folio_set_incompressible(folio, memcomp_folio_hash(folio));
	KUNIT_ASSERT_TRUE(test, memcomp_reserve(folio));
	KUNIT_EXPECT_TRUE(test, folio_memcomp_reserved(folio));
	KUNIT_EXPECT_FALSE(test, folio_test_incompressible(folio));
	memcomp_rollback_folio(folio);
	KUNIT_EXPECT_EQ(test, folio->swap.val, 0UL);

	dst->mapping = NULL;
	folio_clear_swapbacked(dst);
	folio_unlock(dst);
	folio_put(dst);
	folio->mapping = NULL;
	folio_clear_swapbacked(folio);
	folio_unlock(folio);
	folio_put(folio);
}

static struct folio *memcomp_test_user_folio(struct kunit *test,
					     unsigned long *addr)
{
	unsigned long *buf;
	struct folio *folio;
	struct page *page;
	unsigned int i;

	*addr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			      MAP_ANONYMOUS | MAP_PRIVATE, 0);
	if (IS_ERR_VALUE(*addr) || !*addr)
		return NULL;

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return NULL;
	for (i = 0; i < PAGE_SIZE / sizeof(*buf); i++)
		buf[i] = MEMCOMP_TEST_USER_PATTERN + i;
	if (copy_to_user((void __user *)*addr, buf, PAGE_SIZE))
		return NULL;

	if (get_user_pages_fast(*addr, 1, FOLL_WRITE, &page) != 1)
		return NULL;
	folio = page_folio(page);
	/* No per-CPU batch may hold a reference the store would count. */
	lru_add_drain();
	folio_lock(folio);
	return folio;
}

static bool memcomp_test_user_intact(unsigned long addr)
{
	unsigned long val;
	unsigned int i;

	for (i = 0; i < PAGE_SIZE / sizeof(val); i++) {
		if (get_user(val, (unsigned long __user *)addr + i))
			return false;
		if (val != MEMCOMP_TEST_USER_PATTERN + i)
			return false;
	}
	return true;
}

#if (defined(CONFIG_ADVISE_SYSCALLS))
static void memcomp_test_lazyfree_mark_discarded(struct kunit *test)
{
	struct folio *folio;
	unsigned long addr;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	folio = memcomp_test_user_folio(test, &addr);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_set_incompressible(folio, memcomp_folio_hash(folio));

	/* MADV_FREE locks a dirty folio to clean it. */
	folio_unlock(folio);
	KUNIT_ASSERT_EQ(test, do_madvise(current->mm, addr, PAGE_SIZE,
					 MADV_FREE), 0);
	lru_add_drain();
	folio_lock(folio);
	KUNIT_ASSERT_FALSE(test, folio_test_swapbacked(folio));
	KUNIT_ASSERT_FALSE(test, folio_test_dirty(folio));
	KUNIT_ASSERT_TRUE(test, folio_test_incompressible(folio));

	try_to_unmap(folio, 0);
	KUNIT_EXPECT_FALSE(test, folio_mapped(folio));
	KUNIT_EXPECT_FALSE(test, folio_test_swapbacked(folio));
	folio_unlock(folio);
	folio_put(folio);
}
#endif

static void memcomp_test_rollback_restores_ptes(struct kunit *test)
{
	s64 spans, ptes, missed, pending;
	struct anon_vma *anon_vma;
	struct folio *folio;
	unsigned long token;
	unsigned long addr;
	int refs;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	folio = memcomp_test_user_folio(test, &addr);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	anon_vma = folio_anon_vma(folio);
	KUNIT_ASSERT_NOT_NULL(test, anon_vma);
	refs = atomic_read(&anon_vma->refcount);
	spans = atomic64_read(&memcomp_restore_spans);
	ptes = atomic64_read(&memcomp_restore_ptes);
	missed = atomic64_read(&memcomp_restore_missed_pages);
	pending = atomic64_read(&memcomp_pending_pages);

	KUNIT_ASSERT_TRUE(test, PageAnonExclusive(&folio->page));
	KUNIT_ASSERT_TRUE(test, memcomp_reserve(folio));
	token = swp_offset(folio->swap);
	KUNIT_EXPECT_TRUE(test, memcomp_test_head_holds_anon_vma(token));
	KUNIT_EXPECT_EQ(test, atomic_read(&anon_vma->refcount), refs + 1);

	try_to_unmap(folio, 0);
	KUNIT_ASSERT_FALSE(test, folio_mapped(folio));
	KUNIT_EXPECT_FALSE(test, PageAnonExclusive(&folio->page));
	KUNIT_EXPECT_TRUE(test, memcomp_entry_present(token));

	memcomp_rollback_folio(folio);
	KUNIT_EXPECT_TRUE(test, folio_mapped(folio));
	/* Exclusive again, as the token PTE said; a write would not COW. */
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(&folio->page));
	KUNIT_EXPECT_EQ(test, folio->swap.val, 0UL);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token));
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_restore_spans),
			spans + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_restore_ptes),
			ptes + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_restore_missed_pages),
			missed);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages),
			pending);
	KUNIT_EXPECT_EQ(test, atomic_read(&anon_vma->refcount), refs);
	/* The mapping's reference and ours. */
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 2);
	folio_unlock(folio);
	folio_put(folio);

	KUNIT_EXPECT_TRUE(test, memcomp_test_user_intact(addr));
}

static void memcomp_test_store_drops_anon_vma(struct kunit *test)
{
	struct anon_vma *anon_vma;
	struct folio *folio;
	unsigned long token;
	unsigned long addr;
	int refs;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	folio = memcomp_test_user_folio(test, &addr);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	anon_vma = folio_anon_vma(folio);
	KUNIT_ASSERT_NOT_NULL(test, anon_vma);
	refs = atomic_read(&anon_vma->refcount);

	KUNIT_ASSERT_TRUE(test, memcomp_reserve(folio));
	token = swp_offset(folio->swap);
	KUNIT_EXPECT_EQ(test, atomic_read(&anon_vma->refcount), refs + 1);
	try_to_unmap(folio, 0);
	KUNIT_ASSERT_FALSE(test, folio_mapped(folio));

	if (!memcomp_test_store_after_unmap(folio)) {
		memcomp_rollback_folio(folio);
		folio_unlock(folio);
		folio_put(folio);
		KUNIT_FAIL(test, "failed to store the user page");
		return;
	}
	KUNIT_EXPECT_FALSE(test, memcomp_test_head_holds_anon_vma(token));
	KUNIT_EXPECT_EQ(test, atomic_read(&anon_vma->refcount), refs);

	/* Released the way reclaim frees a stored folio. */
	memcomp_invalidate_folio(folio);
	KUNIT_EXPECT_EQ(test, atomic_read(&anon_vma->refcount), refs);
	folio_unlock(folio);
	folio_put(folio);

	KUNIT_EXPECT_TRUE(test, memcomp_test_user_intact(addr));
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token));
}

static void memcomp_test_invalidate_drops_anon_vma(struct kunit *test)
{
	struct anon_vma *anon_vma;
	struct folio *folio;
	unsigned long token;
	unsigned long addr;
	int refs;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	folio = memcomp_test_user_folio(test, &addr);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	anon_vma = folio_anon_vma(folio);
	KUNIT_ASSERT_NOT_NULL(test, anon_vma);
	refs = atomic_read(&anon_vma->refcount);

	KUNIT_ASSERT_TRUE(test, memcomp_reserve(folio));
	token = swp_offset(folio->swap);
	KUNIT_EXPECT_EQ(test, atomic_read(&anon_vma->refcount), refs + 1);
	memcomp_invalidate_folio(folio);
	KUNIT_EXPECT_EQ(test, folio->swap.val, 0UL);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token));
	KUNIT_EXPECT_EQ(test, atomic_read(&anon_vma->refcount), refs);
	folio_unlock(folio);
	folio_put(folio);
}

static void memcomp_test_store_cache_rejects_mapped(struct kunit *test)
{
	struct folio *folio;
	unsigned long token = 0;
	s64 eligibility;
	s64 fail;
	s64 payload;
	s64 arch;

	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_lock(folio);
	eligibility = atomic64_read(&memcomp_shmem_eligibility_fail_pages);
	payload = atomic64_read(&memcomp_shmem_payload_fail_pages);
	arch = atomic64_read(&memcomp_shmem_arch_fail_pages);
	fail = atomic64_read(&memcomp_shmem_store_fail_pages);

	/* An initial -1 mapcount represents no mappings; zero represents one. */
	atomic_set(&folio->_mapcount, 0);
	KUNIT_EXPECT_FALSE(test, memcomp_store_cache(folio, &token));
	KUNIT_EXPECT_EQ(test, token, 0UL);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_shmem_eligibility_fail_pages),
			eligibility + 1);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&memcomp_shmem_payload_fail_pages),
			payload);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_shmem_arch_fail_pages),
			arch);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_shmem_store_fail_pages),
			fail + 1);

	atomic_set(&folio->_mapcount, -1);
	folio_unlock(folio);
	folio_put(folio);
}

#if (defined(CONFIG_SWAP))
static void memcomp_test_writeback_pack_fits(struct kunit *test)
{
	const unsigned int max = MEMCOMP_WRITEBACK_PACK_MAX;

	KUNIT_EXPECT_TRUE(test, memcomp_writeback_pack_fits(0, 0, 1));
	KUNIT_EXPECT_TRUE(test, memcomp_writeback_pack_fits(0, 0,
							    PAGE_SIZE - 1));
	KUNIT_EXPECT_FALSE(test, memcomp_writeback_pack_fits(0, 0,
							     PAGE_SIZE));
	/* Exactly filling the page is a fit; one byte more is not. */
	KUNIT_EXPECT_TRUE(test, memcomp_writeback_pack_fits(1, 1000,
							    PAGE_SIZE - 1000));
	KUNIT_EXPECT_FALSE(test, memcomp_writeback_pack_fits(1, 1000,
							     PAGE_SIZE - 999));
	/* The payload count caps a page of tiny payloads. */
	KUNIT_EXPECT_TRUE(test, memcomp_writeback_pack_fits(max - 1,
							    (max - 1) * 8,
								8));
	KUNIT_EXPECT_FALSE(test, memcomp_writeback_pack_fits(max, max * 8,
							     8));
	/* And the batch always leaves room for a close plus a write. */
	KUNIT_EXPECT_LT(test, (unsigned long)MEMCOMP_WRITEBACK_PACK_MAX,
			(unsigned long)SWAP_MAP_MAX);
}
#endif

#if (defined(CONFIG_SWAP))
static u8 *memcomp_test_packed_slot(struct kunit *test, u8 **pages,
				    unsigned int nr, unsigned int *offs,
					unsigned int *lens)
{
	static const char text[] = "memcomp packed slot ";
	unsigned int off = 0;
	unsigned int i, j;
	u8 *slot;

	slot = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, slot);
	for (i = 0; i < nr; i++) {
		unsigned long handle;
		struct zs_pool *pool;
		u8 pool_idx = 0;
		const u8 *src;
		int ret;

		pages[i] = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
		/* Text-like and distinct per page, so each compresses well. */
		for (j = 0; j < PAGE_SIZE; j++)
			pages[i][j] = text[(j + i * 7) % (sizeof(text) - 1)] ^
				      (j / 512 == i ? 0x20 : 0);
		ret = memcomp_prepare_payload(pages[i], PAGE_SIZE, &lens[i],
					      &handle, &pool_idx,
						  MEMCOMP_PAYLOAD_MUST_SUCCEED,
						  false);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_ASSERT_GT(test, lens[i], 0U);
		KUNIT_ASSERT_LT(test, lens[i], (unsigned int)PAGE_SIZE / 4);
		pool = memcomp_pools[pool_idx];
		src = zs_map_object(pool, handle, ZS_MM_RO);
		KUNIT_ASSERT_NOT_NULL(test, src);
		memcpy(slot + off, src, lens[i]);
		zs_unmap_object(pool, handle);
		memcomp_zs_free(pool_idx, handle);
		offs[i] = off;
		off += lens[i];
	}
	return slot;
}
#endif

#if (defined(CONFIG_SWAP))
static void memcomp_test_writeback_decode_slot(struct kunit *test)
{
	unsigned int offs[3], lens[3];
	u8 *pages[3];
	unsigned int i;
	u8 *slot, *dst;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	slot = memcomp_test_packed_slot(test, pages, 3, offs, lens);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	for (i = 0; i < 3; i++) {
		memset(dst, 0xa5, PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, memcomp_decode_slot(slot, true, offs[i],
							  lens[i], dst), 0);
		KUNIT_EXPECT_MEMEQ(test, dst, pages[i], PAGE_SIZE);
	}

	/* Raw: the slot is the page, whatever the entry's length says. */
	KUNIT_EXPECT_EQ(test, memcomp_decode_slot(pages[1], false, 0,
						  lens[1], dst), 0);
	KUNIT_EXPECT_MEMEQ(test, dst, pages[1], PAGE_SIZE);

	/* Out of the slot, empty, or a whole page: corrupt, never read. */
	KUNIT_EXPECT_EQ(test, memcomp_decode_slot(slot, true,
						  PAGE_SIZE - 10, 11, dst),
			-EIO);
	KUNIT_EXPECT_EQ(test, memcomp_decode_slot(slot, true, 0, 0, dst),
			-EIO);
	KUNIT_EXPECT_EQ(test, memcomp_decode_slot(slot, true, 0,
						  PAGE_SIZE, dst), -EIO);
	/* The wrong offset is a decompression failure, not someone else's page. */
	KUNIT_EXPECT_NE(test, memcomp_decode_slot(slot, true, offs[1] + 1,
						  lens[1], dst), 0);
}
#endif

#if (defined(CONFIG_SWAP))
static void memcomp_test_writeback_defrag_wanted(struct kunit *test)
{
	const long total = 1L << 20;
	const long short_free = (total >> MEMCOMP_WRITEBACK_DEFRAG_FREE_SHIFT) - 1;
	const long enough = total >> MEMCOMP_WRITEBACK_DEFRAG_FREE_SHIFT;

	KUNIT_EXPECT_TRUE(test, memcomp_writeback_defrag_wanted(false, false,
								short_free,
								    total));
	KUNIT_EXPECT_TRUE(test, memcomp_writeback_defrag_wanted(false, false,
								0, total));
	/* An eighth free is enough; so is anything more. */
	KUNIT_EXPECT_FALSE(test, memcomp_writeback_defrag_wanted(false, false,
								 enough,
								     total));
	/* Never while writeback may want the device, or reclaim is running. */
	KUNIT_EXPECT_FALSE(test, memcomp_writeback_defrag_wanted(true, false,
								 0, total));
	KUNIT_EXPECT_FALSE(test, memcomp_writeback_defrag_wanted(false, true,
								 0, total));
	KUNIT_EXPECT_FALSE(test, memcomp_writeback_defrag_wanted(false, false,
								 0, 0));
}
#endif

#if (defined(CONFIG_SWAP))
static bool memcomp_test_sysfs_has_u64(const char *buf, const char *key)
{
	const char *at = buf;
	char number[32];
	u64 value;
	size_t len;

	while ((at = strstr(at, key))) {
		const char *start = at + strlen(key);

		if ((at != buf && !strchr(" \t\n", at[-1])) ||
		    !*start || !strchr(" \t=:", *start)) {
			at = start;
			continue;
		}
		while (*start && strchr(" \t=:", *start))
			start++;
		len = strcspn(start, " \t\n");
		if (!len || len >= sizeof(number))
			return false;
		memcpy(number, start, len);
		number[len] = '\0';
		return !kstrtou64(number, 10, &value);
	}
	return false;
}
#endif

#if (defined(CONFIG_SWAP))
static void memcomp_test_sysfs_attrs_render(struct kunit *test)
{
	static const char * const state_keys[] = {
		"folio_owner_pages", "samefill_pages", "raw_pages",
		"shmem_pages",
	};
	bool state_seen[ARRAY_SIZE(state_keys)] = {};
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	static const char * const packed_keys[] = {
		"writeback_zs_reclaimed",
		"writeback_zs_no_progress",
	};
	bool packed_seen[ARRAY_SIZE(packed_keys)] = {};
	bool packed_events_seen = false;
#endif
	unsigned int huge_class_size;
	unsigned int i, j;
	char *buf;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}

	KUNIT_ASSERT_NOT_NULL(test, memcomp_pools[0]);
	huge_class_size = zs_huge_class_size(memcomp_pools[0]);
	KUNIT_EXPECT_GT(test, huge_class_size, 0U);
	kunit_info(test, "zsmalloc zs_huge_class_size=%u bytes", huge_class_size);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	for (i = 0; memcomp_attrs[i]; i++) {
		struct kobj_attribute *attr =
			container_of(memcomp_attrs[i],
				     struct kobj_attribute, attr);
		ssize_t len;

		KUNIT_ASSERT_NOT_NULL(test, attr->show);
		memset(buf, 0xff, PAGE_SIZE);
		len = attr->show(NULL, attr, buf);

		KUNIT_EXPECT_GT_MSG(test, len, 0, "%s emitted nothing",
				    memcomp_attrs[i]->name);
		KUNIT_EXPECT_LT_MSG(test, len, (ssize_t)PAGE_SIZE,
				    "%s filled the page - output truncated",
				    memcomp_attrs[i]->name);
		if (len <= 0 || len >= (ssize_t)PAGE_SIZE)
			continue;
		KUNIT_EXPECT_EQ_MSG(test, buf[len], '\0',
				    "%s left the buffer unterminated",
				    memcomp_attrs[i]->name);
		KUNIT_EXPECT_EQ_MSG(test, buf[len - 1], '\n',
				    "%s did not end in a newline",
				    memcomp_attrs[i]->name);
		/* Do not parse beyond the buffer on a broken emitter. */
		if (buf[len] != '\0')
			continue;
		for (j = 0; j < ARRAY_SIZE(state_keys); j++) {
			u64 value;

			if (!strcmp(attr->attr.name, state_keys[j]))
				state_seen[j] |= !kstrtou64(buf, 10, &value);
			else
				state_seen[j] |=
					memcomp_test_sysfs_has_u64(buf, state_keys[j]);
		}
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		if (!strcmp(attr->attr.name, "packed_swap_events")) {
			packed_events_seen = true;
			for (j = 0; j < ARRAY_SIZE(packed_keys); j++)
				packed_seen[j] =
					memcomp_test_sysfs_has_u64(buf, packed_keys[j]);
		}
#endif
	}
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	for (j = 0; j < ARRAY_SIZE(state_keys); j++)
		KUNIT_EXPECT_TRUE_MSG(test, state_seen[j],
				      "missing or nonnumeric state key: %s",
				state_keys[j]);
#endif
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_TRUE_MSG(test, packed_events_seen,
			      "missing packed_swap_events attribute");
	if (packed_events_seen)
		for (j = 0; j < ARRAY_SIZE(packed_keys); j++)
			KUNIT_EXPECT_TRUE_MSG(test, packed_seen[j],
					      "missing or nonnumeric packed key: %s",
					packed_keys[j]);
#endif
	KUNIT_EXPECT_GT(test, i, 0U);
	kunit_info(test, "rendered %u sysfs attributes", i);
}
#endif

#if (defined(CONFIG_MEMCG))
static void memcomp_test_objcg_source_identity(struct kunit *test)
{
	struct memcomp_test_objcg_fixture *f;
	struct obj_cgroup *actual, *expected;
	struct folio *other;

	f = memcomp_test_objcg_begin(test, 1);
	if (!f)
		return;
	other = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, other);
	/* An unrelated folio in the same task must use the real owner API. */
	expected = get_obj_cgroup_from_folio(other);
	actual = memcomp_objcg_get(other);
	KUNIT_EXPECT_PTR_EQ(test, actual, expected);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.get_calls), 0);
	if (actual)
		memcomp_objcg_put(actual);
	if (expected)
		obj_cgroup_put(expected);
	folio_put(other);

	actual = memcomp_objcg_get(f->folio);
	KUNIT_EXPECT_PTR_EQ(test, actual, &f->model.objcg);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.get_calls), 1);
	if (actual)
		memcomp_objcg_put(actual);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.put_calls), 1);
	kunit_release_action(test, memcomp_test_objcg_cleanup, f);
}
#endif

struct memcomp_test_acomp_tfm {
	struct crypto_comp *sync;
	struct workqueue_struct *wq;
	void *src;
	void *dst;
};

struct memcomp_test_acomp_req {
	struct work_struct work;
	struct acomp_req *req;
	struct completion started;
	struct completion release;
	struct completion finished;
	atomic_t submitted;
	atomic_t completed;
	bool backlog;
	bool decompress;
	int error;
};

#ifdef CONFIG_MEMCG
static void memcomp_test_acomp_work(struct work_struct *work)
{
	struct memcomp_test_acomp_req *r;
	struct memcomp_test_acomp_tfm *ctx;
	struct acomp_req *req;
	unsigned int len = MEMCOMP_MAX_COMPRESSED_SIZE;
	int ret;

	r = container_of(work, struct memcomp_test_acomp_req, work);
	req = r->req;
	ctx = acomp_tfm_ctx(crypto_acomp_reqtfm(req));
	if (r->backlog)
		acomp_request_complete(req, -EINPROGRESS);
	complete(&r->started);
	/* Cleanup opens this gate even when an assertion aborts the test. */
	wait_for_completion(&r->release);
	ret = r->error;
	if (ret)
		goto complete;
	if (sg_copy_to_buffer(req->src, sg_nents(req->src), ctx->src,
			      req->slen) != req->slen) {
		ret = -EINVAL;
		goto complete;
	}
	if (r->decompress)
		ret = crypto_comp_decompress(ctx->sync, ctx->src, req->slen,
					     ctx->dst, &len);
	else
		ret = crypto_comp_compress(ctx->sync, ctx->src, req->slen,
					   ctx->dst, &len);
	if (!ret && len > req->dlen)
		ret = -E2BIG;
	if (!ret && sg_copy_from_buffer(req->dst, sg_nents(req->dst),
					ctx->dst, len) != len)
		ret = -EINVAL;
	req->dlen = len;
complete:
	atomic_inc(&r->completed);
	acomp_request_complete(req, ret);
	/* Report callback return separately from its pending accounting. */
	complete(&r->finished);
}
#endif /* CONFIG_MEMCG */

static int memcomp_test_acomp_submit(struct acomp_req *req, bool decompress)
{
	struct memcomp_test_acomp_tfm *ctx;
	struct memcomp_test_acomp_req *r = acomp_request_ctx(req);

	if (!req->src || !req->dst || req->slen > PAGE_SIZE)
		return -EINVAL;
	if (r->backlog && !(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG))
		return -EBUSY;
	ctx = acomp_tfm_ctx(crypto_acomp_reqtfm(req));
	r->req = req;
	r->decompress = decompress;
	atomic_inc(&r->submitted);
	queue_work(ctx->wq, &r->work);
	return r->backlog ? -EBUSY : -EINPROGRESS;
}

static int memcomp_test_acomp_compress(struct acomp_req *req)
{
	return memcomp_test_acomp_submit(req, false);
}

static int memcomp_test_acomp_decompress(struct acomp_req *req)
{
	return memcomp_test_acomp_submit(req, true);
}

static atomic_t memcomp_test_acomp_inits = ATOMIC_INIT(0);
static atomic_t memcomp_test_acomp_exits = ATOMIC_INIT(0);

static int memcomp_test_acomp_init(struct crypto_acomp *tfm)
{
	struct memcomp_test_acomp_tfm *ctx = acomp_tfm_ctx(tfm);
	int ret = -ENOMEM;

	ctx->sync = crypto_alloc_comp(memcomp_compressor, 0, 0);
	if (IS_ERR(ctx->sync))
		return PTR_ERR(ctx->sync);
	ctx->src = kmalloc(PAGE_SIZE, GFP_KERNEL);
	ctx->dst = kmalloc(MEMCOMP_MAX_COMPRESSED_SIZE, GFP_KERNEL);
	/* One scratch context requires global ordering, including across NUMA nodes. */
	ctx->wq = alloc_ordered_workqueue("memcomp-kunit-acomp", 0);
	if (ctx->src && ctx->dst && ctx->wq) {
		atomic_inc(&memcomp_test_acomp_inits);
		return 0;
	}
	if (ctx->wq)
		destroy_workqueue(ctx->wq);
	kfree(ctx->dst);
	kfree(ctx->src);
	crypto_free_comp(ctx->sync);
	return ret;
}

static void memcomp_test_acomp_exit(struct crypto_acomp *tfm)
{
	struct memcomp_test_acomp_tfm *ctx = acomp_tfm_ctx(tfm);

	destroy_workqueue(ctx->wq);
	kfree(ctx->dst);
	kfree(ctx->src);
	crypto_free_comp(ctx->sync);
	atomic_inc(&memcomp_test_acomp_exits);
}

static struct acomp_alg memcomp_test_acomp_alg = {
	.compress = memcomp_test_acomp_compress,
	.decompress = memcomp_test_acomp_decompress,
	.init = memcomp_test_acomp_init,
	.exit = memcomp_test_acomp_exit,
	.reqsize = sizeof(struct memcomp_test_acomp_req),
	.base = {
		.cra_name = "memcomp-kunit-acomp",
		.cra_driver_name = "memcomp-kunit-acomp-async",
		.cra_flags = CRYPTO_ALG_ASYNC,
		.cra_blocksize = 1,
		.cra_ctxsize = sizeof(struct memcomp_test_acomp_tfm),
		.cra_module = THIS_MODULE,
	},
};

#include "late_async_test.h"

#endif /* _MM_MEMCOMP_TESTS_SHARED_TEST_H */
