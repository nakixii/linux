/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SWAPOFF_YIELD_TEST_H
#define _MM_MEMCOMP_TESTS_SWAPOFF_YIELD_TEST_H

#if defined(CONFIG_SWAP) && defined(CONFIG_MEMCOMP_KUNIT_TEST)
#include <kunit/resource.h>
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>

/*
 * Include after memcomp_swapoff(), independently of suite.h. Exercise
 * the production walk with real XA lookups, temporary krefs and mutexes. Only
 * the device flush/restore edges use fixture callbacks. STORED misses still
 * run memcomp_swapoff_entry(); no synthetic backing reaches physical IO.
 *
 * The writer actually acquires W. It checks refs before replacing future and
 * already-scanned objects, then leaves both staging and plugged IO pending.
 * Every subsequent lookup must observe a flush. Disabling the unlock, skipping
 * the flush, resetting the cursor, counting only conversions, or retaining a
 * temporary reference must fail these assertions, independently of a counter
 * that merely claims that a yield happened.
 */
#define MEMCOMP_YIELD_TEST_OBJECTS 72
#define MEMCOMP_YIELD_TEST_FLAGS (PF_MEMALLOC_NOFS | PF_MEMALLOC)

struct memcomp_yield_test {
	struct xarray tree;
	/* Serializes the tree walker with fixture object updates. */
	struct mutex lock;
	struct memcomp_entry objects[MEMCOMP_YIELD_TEST_OBJECTS];
	unsigned long keys[MEMCOMP_YIELD_TEST_OBJECTS];
	unsigned int visits[MEMCOMP_YIELD_TEST_OBJECTS];
	unsigned int nr;
	struct memcomp_swapoff_yield_hooks hooks;
	struct completion writer_start;
	struct completion writer_attempting;
	struct completion writer_done;
	struct completion walker_done;
	struct task_struct *writer;
	struct task_struct *walker;
	unsigned int scans;
	unsigned int restores;
	unsigned int yields;
	unsigned int flushes;
	unsigned int settlements;
	unsigned int writer_at_scan;
	unsigned int flags_before;
	unsigned int flags_after;
	unsigned int fail_flush;
	unsigned int fail_restore;
	unsigned int signal_flush;
	unsigned int signal_scan;
	unsigned long last_key;
	int restore_result;
	int result;
	int writer_error;
	bool want_writer;
	bool dying;
	bool sparse;
	bool nested;
	bool signal_writer;
	bool cancel;
	bool timed_out;
	bool bad_ref;
	bool bad_scope;
	bool dirty_scan;
	bool staging;
	bool plugged;
	bool writer_acquired;
};

static void memcomp_yield_test_destroy(void *data)
{
	struct memcomp_yield_test *f = data;

	xa_destroy(&f->tree);
}

static struct memcomp_yield_test *
memcomp_yield_test_alloc(struct kunit *test, unsigned int nr, bool dying)
{
	struct memcomp_yield_test *f;
	unsigned int i;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	xa_init(&f->tree);
	mutex_init(&f->lock);
	init_completion(&f->writer_start);
	init_completion(&f->writer_attempting);
	init_completion(&f->writer_done);
	init_completion(&f->walker_done);
	if (kunit_add_action_or_reset(test, memcomp_yield_test_destroy, f))
		return NULL;
	f->nr = nr;
	f->dying = dying;
	/* The initial acquisition also has to flush, even on an empty index. */
	f->staging = true;
	f->plugged = true;
	for (i = 0; i < ARRAY_SIZE(f->objects); i++) {
		struct memcomp_entry *entry = &f->objects[i];

		f->keys[i] = 1 + 3 * i;
		entry->token = f->keys[i];
		entry->state = MC_STORED;
		kref_init(&entry->refcount);
		if (dying)
			refcount_set(&entry->refcount.refcount, 0);
	}
	for (i = 0; i < nr; i++) {
		if (xa_err(xa_store(&f->tree, f->keys[i], &f->objects[i],
				    GFP_KERNEL)))
			return NULL;
	}
	return f;
}

static int memcomp_yield_test_flush(void *data)
{
	struct memcomp_yield_test *f = data;

	lockdep_assert_held(&f->lock);
	f->flushes++;
	if (f->staging || f->plugged)
		f->settlements++;
	f->staging = false;
	f->plugged = false;
	if (f->signal_flush == f->flushes)
		send_sig(SIGUSR1, current, 0);
	return f->fail_flush == f->flushes ? -EIO : 0;
}

static int memcomp_yield_test_restore(struct memcomp_entry *entry,
				      unsigned int type, void *data)
{
	struct memcomp_yield_test *f = data;
	unsigned int object = entry - f->objects;
	int ret;

	lockdep_assert_held(&f->lock);
	f->restores++;
	if (object >= ARRAY_SIZE(f->objects))
		return -EINVAL;
	f->visits[object]++;
	if (f->fail_restore == f->restores)
		return -ENOMEM;
	/* All-miss cases execute the actual entry restore's state rejection. */
	ret = memcomp_swapoff_entry(entry, type, &memcomp_swapoff_ops);
	return ret ? ret : f->restore_result;
}

static void memcomp_yield_test_scanned(struct memcomp_entry *entry,
				       unsigned long key, void *data)
{
	struct memcomp_yield_test *f = data;

	lockdep_assert_held(&f->lock);
	WRITE_ONCE(f->scans, f->scans + 1);
	f->last_key = key;
	if (f->staging || f->plugged)
		f->dirty_scan = true;
	if ((current->flags & MEMCOMP_YIELD_TEST_FLAGS) !=
	    MEMCOMP_YIELD_TEST_FLAGS)
		f->bad_scope = true;
	if (entry && kref_read(&entry->refcount) != 2)
		f->bad_ref = true;
	if (f->signal_scan == f->scans)
		send_sig(SIGUSR1, current, 0);
	if (f->want_writer && f->scans == 32) {
		complete(&f->writer_start);
		/* The writer must try W while the scanner still owns it. */
		if (!wait_for_completion_timeout(&f->writer_attempting, 2 * HZ)) {
			f->timed_out = true;
			send_sig(SIGUSR1, current, 0);
		}
	}
}

static void memcomp_yield_test_unlocked(void *data)
{
	struct memcomp_yield_test *f = data;

	f->yields++;
	if ((current->flags & MEMCOMP_YIELD_TEST_FLAGS) !=
	    MEMCOMP_YIELD_TEST_FLAGS)
		f->bad_scope = true;
	/* Observe real writer progress before the scanner reacquires W. */
	if (f->want_writer && f->yields == 1 &&
	    !wait_for_completion_timeout(&f->writer_done, 2 * HZ)) {
		f->timed_out = true;
		send_sig(SIGUSR1, current, 0);
	}
}

static int memcomp_yield_test_writer(void *data)
{
	struct memcomp_yield_test *f = data;
	unsigned int i;

	wait_for_completion(&f->writer_start);
	if (READ_ONCE(f->cancel))
		goto done;
	complete(&f->writer_attempting);
	mutex_lock(&f->lock);
	f->writer_acquired = true;
	f->writer_at_scan = READ_ONCE(f->scans);
	for (i = 0; i < f->nr; i++) {
		if (kref_read(&f->objects[i].refcount) != !f->dying)
			f->bad_ref = true;
	}
	if (f->sparse) {
		/* Reuse a visited key, replace a future object, remove the tail. */
		f->writer_error = xa_err(xa_store(&f->tree, 16,
						  &f->objects[34], GFP_KERNEL));
		if (!f->writer_error)
			f->writer_error = xa_err(xa_store(&f->tree, 4096,
							  &f->objects[35], GFP_KERNEL));
		xa_erase(&f->tree, f->keys[31]);
	}
	f->staging = true;
	f->plugged = true;
	if (f->signal_writer)
		send_sig(SIGUSR1, f->walker, 0);
	mutex_unlock(&f->lock);
done:
	complete(&f->writer_done);
	return 0;
}

static int memcomp_yield_test_walker(void *data)
{
	struct memcomp_yield_test *f = data;
	unsigned int nofs = 0, noreclaim = 0;

	allow_signal(SIGUSR1);
	if (f->nested) {
		nofs = memalloc_nofs_save();
		noreclaim = memalloc_noreclaim_save();
	}
	f->flags_before = current->flags & MEMCOMP_YIELD_TEST_FLAGS;
	f->hooks.task = current;
	rcu_assign_pointer(memcomp_swapoff_yield_active, &f->hooks);
	f->result = memcomp_swapoff(0);
	rcu_assign_pointer(memcomp_swapoff_yield_active, NULL);
	f->flags_after = current->flags & MEMCOMP_YIELD_TEST_FLAGS;
	if (f->nested) {
		memalloc_noreclaim_restore(noreclaim);
		memalloc_nofs_restore(nofs);
	}
	flush_signals(current);
	/* Retire readers that fetched active before the task identity check. */
	synchronize_rcu();
	complete(&f->walker_done);
	return 0;
}

/* No assertion may abort this helper while either thread still owns f. */
static void memcomp_yield_test_run(struct kunit *test,
				   struct memcomp_yield_test *f)
{
	unsigned int i;

	KUNIT_ASSERT_PTR_EQ(test,
			    rcu_access_pointer(memcomp_swapoff_yield_active), NULL);
	f->hooks = (struct memcomp_swapoff_yield_hooks) {
		.entries = &f->tree,
		.lock = &f->lock,
		.flush = memcomp_yield_test_flush,
		.restore = memcomp_yield_test_restore,
		.scanned = memcomp_yield_test_scanned,
		.unlocked = memcomp_yield_test_unlocked,
		.ctx = f,
	};
	if (f->want_writer) {
		f->writer = kthread_create(memcomp_yield_test_writer, f,
					   "mc-swapoff-writer");
		if (IS_ERR(f->writer)) {
			KUNIT_FAIL(test, "cannot create writer: %ld",
				   PTR_ERR(f->writer));
			return;
		}
		get_task_struct(f->writer);
		wake_up_process(f->writer);
	}
	f->walker = kthread_create(memcomp_yield_test_walker, f,
				   "mc-swapoff-walker");
	if (IS_ERR(f->walker)) {
		KUNIT_FAIL(test, "cannot create walker: %ld", PTR_ERR(f->walker));
		goto stop_writer;
	}
	get_task_struct(f->walker);
	wake_up_process(f->walker);
	if (!wait_for_completion_timeout(&f->walker_done, 6 * HZ)) {
		f->timed_out = true;
		send_sig(SIGUSR1, f->walker, 0);
	}
	kthread_stop(f->walker);
	put_task_struct(f->walker);
stop_writer:
	WRITE_ONCE(f->cancel, true);
	complete_all(&f->writer_start);
	if (f->want_writer) {
		kthread_stop(f->writer);
		put_task_struct(f->writer);
	}
	KUNIT_EXPECT_PTR_EQ(test,
			    rcu_access_pointer(memcomp_swapoff_yield_active), NULL);
	KUNIT_EXPECT_FALSE(test, f->timed_out);
	KUNIT_EXPECT_FALSE(test, f->bad_ref);
	KUNIT_EXPECT_FALSE(test, f->bad_scope);
	KUNIT_EXPECT_FALSE(test, f->dirty_scan);
	KUNIT_EXPECT_EQ(test, f->writer_error, 0);
	KUNIT_EXPECT_EQ(test, f->flags_before, f->flags_after);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&f->lock));
	for (i = 0; i < ARRAY_SIZE(f->objects); i++)
		KUNIT_EXPECT_EQ_MSG(test, kref_read(&f->objects[i].refcount),
				    (unsigned int)!f->dying, "object %u", i);
}

static void memcomp_test_swapoff_yield_all_miss(struct kunit *test)
{
	struct memcomp_yield_test *f = memcomp_yield_test_alloc(test, 65, false);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->want_writer = true;
	memcomp_yield_test_run(test, f);
	KUNIT_EXPECT_EQ(test, f->result, 0);
	KUNIT_EXPECT_EQ(test, f->scans, 65U);
	KUNIT_EXPECT_EQ(test, f->restores, 65U);
	KUNIT_EXPECT_EQ(test, f->yields, 2U);
	KUNIT_EXPECT_EQ(test, f->flushes, 3U);
	KUNIT_EXPECT_EQ(test, f->settlements, 2U);
	KUNIT_EXPECT_TRUE(test, f->writer_acquired);
	KUNIT_EXPECT_EQ(test, f->writer_at_scan, 32U);
}

static void memcomp_test_swapoff_yield_dying(struct kunit *test)
{
	struct memcomp_yield_test *f = memcomp_yield_test_alloc(test, 65, true);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->want_writer = true;
	memcomp_yield_test_run(test, f);
	KUNIT_EXPECT_EQ(test, f->result, 0);
	KUNIT_EXPECT_EQ(test, f->scans, 65U);
	KUNIT_EXPECT_EQ(test, f->restores, 0U);
	KUNIT_EXPECT_EQ(test, f->yields, 2U);
	KUNIT_EXPECT_EQ(test, f->flushes, 3U);
	KUNIT_EXPECT_TRUE(test, f->writer_acquired);
	KUNIT_EXPECT_EQ(test, f->writer_at_scan, 32U);
}

static void memcomp_test_swapoff_yield_cursor(struct kunit *test)
{
	struct memcomp_yield_test *f = memcomp_yield_test_alloc(test, 34, false);

	KUNIT_ASSERT_NOT_NULL(test, f);
	xa_erase(&f->tree, f->keys[32]);
	xa_erase(&f->tree, f->keys[33]);
	f->objects[32].token = 4096;
	f->keys[32] = 4096;
	f->objects[33].token = UINT_MAX;
	f->keys[33] = UINT_MAX;
	f->objects[34].token = 16;
	f->objects[35].token = 4096;
	KUNIT_ASSERT_EQ(test, xa_err(xa_store(&f->tree, 4096,
					      &f->objects[32], GFP_KERNEL)), 0);
	KUNIT_ASSERT_EQ(test, xa_err(xa_store(&f->tree, UINT_MAX,
					      &f->objects[33], GFP_KERNEL)), 0);
	f->want_writer = true;
	f->sparse = true;
	memcomp_yield_test_run(test, f);
	KUNIT_EXPECT_EQ(test, f->result, 0);
	KUNIT_EXPECT_EQ(test, f->scans, 34U);
	KUNIT_EXPECT_EQ(test, f->yields, 1U);
	KUNIT_EXPECT_EQ(test, f->last_key, (unsigned long)UINT_MAX);
	KUNIT_EXPECT_EQ(test, f->visits[5], 1U);
	KUNIT_EXPECT_EQ(test, f->visits[32], 0U);
	KUNIT_EXPECT_EQ(test, f->visits[33], 1U);
	KUNIT_EXPECT_EQ(test, f->visits[34], 0U);
	KUNIT_EXPECT_EQ(test, f->visits[35], 1U);
	KUNIT_EXPECT_TRUE(test, f->writer_acquired);
	KUNIT_EXPECT_EQ(test, f->writer_at_scan, 32U);
}

static void memcomp_test_swapoff_yield_flush_error(struct kunit *test)
{
	unsigned int flush;

	for (flush = 1; flush <= 2; flush++) {
		struct memcomp_yield_test *f =
			memcomp_yield_test_alloc(test, 65, false);

		KUNIT_ASSERT_NOT_NULL(test, f);
		f->fail_flush = flush;
		f->want_writer = flush == 2;
		f->nested = flush == 2;
		memcomp_yield_test_run(test, f);
		KUNIT_EXPECT_EQ(test, f->result, -EIO);
		KUNIT_EXPECT_EQ(test, f->flushes, flush);
		KUNIT_EXPECT_EQ(test, f->scans, flush == 1 ? 0U : 32U);
		KUNIT_EXPECT_EQ(test, f->yields, flush - 1);
		KUNIT_EXPECT_FALSE(test, f->staging);
		KUNIT_EXPECT_FALSE(test, f->plugged);
	}
}

static void memcomp_test_swapoff_yield_signal(struct kunit *test)
{
	unsigned int phase;

	for (phase = 0; phase < 4; phase++) {
		struct memcomp_yield_test *f =
			memcomp_yield_test_alloc(test, phase == 3 ? 0 : 65, false);

		KUNIT_ASSERT_NOT_NULL(test, f);
		if (phase == 0 || phase == 3) {
			f->signal_flush = 1;
		} else if (phase == 1) {
			f->signal_scan = 1;
		} else {
			f->want_writer = true;
			f->signal_writer = true;
		}
		memcomp_yield_test_run(test, f);
		KUNIT_EXPECT_EQ(test, f->result, -EINTR);
		KUNIT_EXPECT_EQ(test, f->scans,
				phase == 1 ? 1U : phase == 2 ? 32U : 0U);
		if (phase == 2) {
			KUNIT_EXPECT_TRUE(test, f->writer_acquired);
			KUNIT_EXPECT_EQ(test, f->flushes, 2U);
			KUNIT_EXPECT_FALSE(test, f->staging);
			KUNIT_EXPECT_FALSE(test, f->plugged);
		}
	}
}

static void memcomp_test_swapoff_yield_positive_status(struct kunit *test)
{
	struct memcomp_yield_test *f = memcomp_yield_test_alloc(test, 33, false);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->restore_result = 1;
	memcomp_yield_test_run(test, f);
	KUNIT_EXPECT_EQ(test, f->result, 0);
	KUNIT_EXPECT_EQ(test, f->restores, 33U);
	KUNIT_EXPECT_EQ(test, f->yields, 1U);
}

static void memcomp_test_swapoff_yield_restore_error(struct kunit *test)
{
	struct memcomp_yield_test *f = memcomp_yield_test_alloc(test, 33, false);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->fail_restore = 12;
	f->restore_result = 1;
	memcomp_yield_test_run(test, f);
	KUNIT_EXPECT_EQ(test, f->result, -ENOMEM);
	KUNIT_EXPECT_EQ(test, f->scans, 12U);
	KUNIT_EXPECT_EQ(test, f->restores, 12U);
	KUNIT_EXPECT_EQ(test, f->yields, 0U);
}

static void memcomp_test_swapoff_yield_budget(struct kunit *test)
{
	static const unsigned int counts[] = { 0, 31, 32, 33 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(counts); i++) {
		struct memcomp_yield_test *f =
			memcomp_yield_test_alloc(test, counts[i], false);

		KUNIT_ASSERT_NOT_NULL(test, f);
		f->nested = i & 1;
		memcomp_yield_test_run(test, f);
		KUNIT_EXPECT_EQ(test, f->result, 0);
		KUNIT_EXPECT_EQ(test, f->scans, counts[i]);
		KUNIT_EXPECT_EQ(test, f->yields, counts[i] / 32);
		KUNIT_EXPECT_EQ(test, f->flushes, 1 + counts[i] / 32);
	}
}

static void memcomp_test_swapoff_yield_uint_max(struct kunit *test)
{
	unsigned int signal;

	for (signal = 0; signal <= 1; signal++) {
		struct memcomp_yield_test *f =
			memcomp_yield_test_alloc(test, 32, false);

		KUNIT_ASSERT_NOT_NULL(test, f);
		xa_erase(&f->tree, f->keys[31]);
		f->objects[31].token = UINT_MAX;
		f->keys[31] = UINT_MAX;
		KUNIT_ASSERT_EQ(test, xa_err(xa_store(&f->tree, UINT_MAX,
						      &f->objects[31], GFP_KERNEL)), 0);
		f->restore_result = 1;
		f->signal_scan = signal ? 32 : 0;
		memcomp_yield_test_run(test, f);
		KUNIT_EXPECT_EQ(test, f->result, signal ? -EINTR : 0);
		KUNIT_EXPECT_EQ(test, f->scans, 32U);
		KUNIT_EXPECT_EQ(test, f->last_key, (unsigned long)UINT_MAX);
		KUNIT_EXPECT_EQ(test, f->yields, 0U);
		KUNIT_EXPECT_EQ(test, f->flushes, 1U);
	}
}

static struct kunit_case memcomp_swapoff_yield_test_cases[] = {
	KUNIT_CASE(memcomp_test_swapoff_yield_all_miss),
	KUNIT_CASE(memcomp_test_swapoff_yield_dying),
	KUNIT_CASE(memcomp_test_swapoff_yield_cursor),
	KUNIT_CASE(memcomp_test_swapoff_yield_flush_error),
	KUNIT_CASE(memcomp_test_swapoff_yield_signal),
	KUNIT_CASE(memcomp_test_swapoff_yield_positive_status),
	KUNIT_CASE(memcomp_test_swapoff_yield_restore_error),
	KUNIT_CASE(memcomp_test_swapoff_yield_budget),
	KUNIT_CASE(memcomp_test_swapoff_yield_uint_max),
	{}
};

static struct kunit_suite memcomp_swapoff_yield_test_suite = {
	.name = "memcomp-swapoff-yield",
	.test_cases = memcomp_swapoff_yield_test_cases,
};

kunit_test_suite(memcomp_swapoff_yield_test_suite);
#endif /* CONFIG_SWAP && CONFIG_MEMCOMP_KUNIT_TEST */
#endif /* _MM_MEMCOMP_TESTS_SWAPOFF_YIELD_TEST_H */
