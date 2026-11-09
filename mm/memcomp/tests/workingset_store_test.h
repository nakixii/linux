/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_WORKINGSET_STORE_TEST_H
#define _MM_MEMCOMP_TESTS_WORKINGSET_STORE_TEST_H

/*
 * Author: nakixii
 *
 * Include after cache_folio_test.h, folio_test.h and store_transaction_test.h.
 * Register the cases under MC16_WORKINGSET_STORE_TEST_ENABLED. Sources, guards,
 * payloads, LRU isolation, eviction and PTE removal use the real implementation.
 * The eviction observation stub calls the real stock-policy entry; it does
 * not fabricate a shadow or bypass the release transaction.
 *
 * The race case runs a separate reader/writeback/PTE-zap task while the
 * producer is stopped at its irrevocable eviction point with no T/PTL/XA.
 * No worker outlives the synchronous release actor or its fixture.
 */
#ifdef MC16_FOLIO_TEST_ENABLED
#define MC16_WORKINGSET_STORE_TEST_ENABLED

#include <linux/stop_machine.h>

struct mc16_ws_test {
	struct kunit *test;
	struct mc16_flt *anon;
	struct mem_cgroup *target;
	struct mem_cgroup *seen_target;
	struct folio *source;
	struct completion start;
	struct completion done;
	struct task_struct *worker;
	unsigned int calls;
	unsigned int pages;
	unsigned long native_before;
	unsigned long native_after;
	bool native_mglru;
	bool locked;
	bool isolated;
	bool token_locks_free;
	bool worker_done;
	int source_refs;
	int load[2];
	bool writeback[2];
	int zap[2];
	u8 *buffer;
	void *shadow;
};

/* Only the synchronous test-task eviction stub reads this pointer. */
static struct mc16_ws_test *mc16_ws_actor;

static u64 mc16_ws_word(struct memcomp_ref *ref)
{
	u64 word;

	mutex_lock(memcomp_lock_of(ref->token));
	word = *mc16_shadow_ptr(ref);
	mutex_unlock(memcomp_lock_of(ref->token));
	return word;
}

static void mc16_ws_clear(struct memcomp_ref *ref)
{
	mutex_lock(memcomp_lock_of(ref->token));
	*mc16_shadow_ptr(ref) = 0;
	mutex_unlock(memcomp_lock_of(ref->token));
}

static int mc16_ws_competitors(void *data)
{
	struct mc16_ws_test *p = data;
	struct mc16_flt *f = p->anon;
	unsigned int i;

	wait_for_completion(&p->start);
	if (kthread_should_stop())
		goto out;
	for (i = 0; i < f->nr; i++) {
		struct mc16_writeback snapshot;

		p->load[i] = mc16_load_page(&f->guards[i], p->buffer);
		mutex_lock(memcomp_lock_of(f->guards[i].token));
		p->writeback[i] =
			mc16_writeback_claim_locked(&f->guards[i], &snapshot);
		/* Repair a failed gate assertion before release can continue. */
		if (p->writeback[i])
			mc16_writeback_revert_locked(&f->guards[i]);
		mutex_unlock(memcomp_lock_of(f->guards[i].token));
		p->zap[i] = mc16_flt_zap(f, f->addr + i * PAGE_SIZE);
	}
out:
	complete(&p->done);
	return 0;
}

/*
 * No other CPU or interrupt can reclaim or advance an eviction bucket while
 * this callback samples the native entry. Assertions run after CPUs resume.
 * Do not reset native counters or substitute the producer call counters.
 */
static int mc16_ws_native_eviction(void *data)
{
	struct mc16_ws_test *p = data;
	struct folio *folio = p->source;
	struct lruvec *lruvec;
	atomic_long_t *counter;

	lruvec = mem_cgroup_lruvec(p->seen_target, folio_pgdat(folio));
	counter = &lruvec->nonresident_age;
#ifdef CONFIG_LRU_GEN
	if (lru_gen_enabled()) {
		int type = folio_is_file_lru(folio);
		int tier = lru_tier_from_refs(folio_lru_refs(folio),
					      folio_test_workingset(folio));
		unsigned long seq;

		lruvec = mem_cgroup_lruvec(folio_memcg(folio), folio_pgdat(folio));
		seq = READ_ONCE(lruvec->lrugen.min_seq[type]);
		counter = &lruvec->lrugen.evicted[lru_hist_from_seq(seq)][type][tier];
		p->native_mglru = true;
	}
#endif
	p->native_before = atomic_long_read(counter);
	p->shadow = workingset_memcomp_eviction(folio, p->seen_target);
	p->native_after = atomic_long_read(counter);
	return 0;
}

static void *mc16_ws_eviction(struct folio *folio,
			      struct mem_cgroup *target_memcg)
{
	struct mc16_ws_test *p = mc16_ws_actor;
	unsigned int i;
	int ret;

	p->calls++;
	p->source = folio;
	p->seen_target = target_memcg;
	p->pages = folio_nr_pages(folio);
	p->locked = folio_test_locked(folio);
	p->isolated = !folio_test_lru(folio);
	p->source_refs = folio_ref_count(folio);
	p->token_locks_free = true;
	if (p->anon) {
		for (i = 0; i < p->anon->nr; i++)
			if (mutex_is_locked(memcomp_lock_of(p->anon->guards[i].token)))
				p->token_locks_free = false;
	}
	if (p->worker && p->token_locks_free) {
		complete(&p->start);
		p->worker_done = wait_for_completion_timeout(&p->done, 5 * HZ);
	}
	ret = stop_machine(mc16_ws_native_eviction, p, NULL);
	KUNIT_EXPECT_EQ(p->test, ret, 0);
	KUNIT_EXPECT_EQ(p->test, p->native_after - p->native_before,
			(unsigned long)p->pages);
	kunit_info(p->test, "native eviction: lru=%s before=%lu after=%lu pages=%u",
		   p->native_mglru ? "mglru" : "classic",
		   p->native_before, p->native_after, p->pages);
	return p->shadow;
}

static void mc16_ws_cleanup(void *data)
{
	struct mc16_ws_test *p = data;

	if (p->worker) {
		complete(&p->start);
		kthread_stop_put(p->worker);
	}
	if (mc16_ws_actor == p)
		mc16_ws_actor = NULL;
}

static struct mc16_ws_test *mc16_ws_begin(struct kunit *test)
{
	struct mc16_ws_test *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);

	if (!p)
		return NULL;
	p->test = test;
	p->buffer = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!p->buffer)
		return NULL;
	init_completion(&p->start);
	init_completion(&p->done);
	if (kunit_add_action_or_reset(test, mc16_ws_cleanup, p))
		return NULL;
	kunit_activate_static_stub(test, mc16_store_eviction, mc16_ws_eviction);
	mc16_ws_actor = p;
	return p;
}

/* Drop exactly the extra isolation reference, preserving the fixture's one. */
static int mc16_ws_isolate(struct folio *folio)
{
	lru_add_drain_all();
	if (!folio_test_lru(folio) || !folio_isolate_lru(folio))
		return -EBUSY;
	/* Model reclaim isolation, not migration: the source is inactive. */
	folio_clear_active(folio);
	folio_put(folio);
	return 0;
}

static int mc16_ws_prepare(struct mc16_flt *f)
{
	unsigned int i;

	if (!mc16_flt_reserve(f) || mc16_flt_hold(f))
		return -EINVAL;
	for (i = 0; i < f->nr; i++) {
		if (mc16_flt_escape(f, i) || mc16_flt_commit(f, i))
			return -EINVAL;
	}
	return mc16_ws_isolate(f->folio);
}

static bool mc16_ws_release(struct mc16_ws_test *p)
{
	bool released;

	folio_lock(p->anon->folio);
	released = mc16_release_folio_reclaim(p->anon->folio, p->target);
	if (released)
		folio_ref_unfreeze(p->anon->folio, 1);
	folio_unlock(p->anon->folio);
	return released;
}

static void mc16_ws_expect_exit(struct kunit *test, struct mc16_ws_test *p)
{
	struct mc16_flt *f = p->anon;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, p->calls, 1U);
	KUNIT_EXPECT_EQ(test, p->pages, f->nr);
	KUNIT_EXPECT_PTR_EQ(test, p->seen_target, p->target);
	KUNIT_EXPECT_PTR_EQ(test, p->source, f->folio);
	KUNIT_EXPECT_TRUE(test, p->locked);
	KUNIT_EXPECT_TRUE(test, p->isolated);
	KUNIT_EXPECT_TRUE(test, p->token_locks_free);
	KUNIT_EXPECT_EQ(test, p->source_refs, 0);
	KUNIT_EXPECT_TRUE(test, xa_is_value(p->shadow));
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[i]),
				(u64)(unsigned long)p->shadow);
		KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[i], false) &
					 MC16_F_OWNER);
		KUNIT_EXPECT_NE(test, refcount_read(&f->guards[i].slot->refs), 0U);
	}
}

static void memcomp_test_workingset_store_single(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_ws_test *p;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ws_prepare(f), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->anon = f;
	p->target = root_mem_cgroup;
	KUNIT_ASSERT_TRUE(test, mc16_ws_release(p));
	mc16_ws_expect_exit(test, p);
	/* The compatibility API must not create a second eviction epoch. */
	mc16_ws_clear(&f->guards[0]);
	KUNIT_EXPECT_FALSE(test, mc16_flt_release(f->folio));
	KUNIT_EXPECT_EQ(test, p->calls, 1U);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[0]), 0ULL);
}

static void memcomp_test_workingset_store_mthp(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_ws_test *p;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ws_prepare(f), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->anon = f;
	KUNIT_ASSERT_TRUE(test, mc16_ws_release(p));
	/* One real stock call for the whole folio, not one call per slot. */
	mc16_ws_expect_exit(test, p);
}

static void memcomp_test_workingset_store_freeze_failure(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_ws_test *p;
	bool released;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ws_prepare(f), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->anon = f;
	folio_get(f->folio);
	released = mc16_ws_release(p);
	folio_put(f->folio);
	KUNIT_EXPECT_FALSE(test, released);
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[i]), 0ULL);
		KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[i], false) &
					MC16_F_OWNER);
	}
	KUNIT_ASSERT_TRUE(test, mc16_ws_release(p));
	mc16_ws_expect_exit(test, p);
}

static void memcomp_test_workingset_store_bad_sibling(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_ws_test *p;
	struct mc16_view view;
	u16 length;
	bool released;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ws_prepare(f), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->anon = f;
	mutex_lock(memcomp_lock_of(f->guards[1].token));
	mc16_view_locked(&f->guards[1], &view);
	length = *view.length;
	*view.length = U16_MAX;
	mutex_unlock(memcomp_lock_of(f->guards[1].token));
	released = mc16_ws_release(p);
	mutex_lock(memcomp_lock_of(f->guards[1].token));
	mc16_view_locked(&f->guards[1], &view);
	*view.length = length;
	mutex_unlock(memcomp_lock_of(f->guards[1].token));
	KUNIT_EXPECT_FALSE(test, released);
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[i]), 0ULL);
		KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[i], false) &
					MC16_F_OWNER);
	}
	KUNIT_ASSERT_TRUE(test, mc16_ws_release(p));
	mc16_ws_expect_exit(test, p);
}

static void memcomp_test_workingset_store_gate_race(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_ws_test *p;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ws_prepare(f), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->anon = f;
	p->worker = kthread_run(mc16_ws_competitors, p, "mc16-store-gate");
	if (IS_ERR(p->worker)) {
		p->worker = NULL;
		KUNIT_FAIL(test, "cannot start store-gate actor");
		return;
	}
	get_task_struct(p->worker);
	KUNIT_ASSERT_TRUE(test, mc16_ws_release(p));
	KUNIT_EXPECT_TRUE(test, p->worker_done);
	/* Join before inspecting results or letting fixture cleanup run. */
	kthread_stop_put(p->worker);
	p->worker = NULL;
	mc16_ws_expect_exit(test, p);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, p->load[i], -EAGAIN);
		KUNIT_EXPECT_FALSE(test, p->writeback[i]);
		KUNIT_EXPECT_EQ(test, p->zap[i], 0);
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 1U);
	}
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents);
}

static void memcomp_test_workingset_store_prepared(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_sct_result result;
	struct mc16_ws_test *p;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->anon = f;
	result = mc16_sct_store(f, (struct mc16_sct_attempt) { });
	KUNIT_ASSERT_EQ(test, result.error, 0);
	KUNIT_ASSERT_TRUE(test, result.success);
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[0]), MC16_SHADOW_PUBLISHING);
	KUNIT_EXPECT_EQ(test, mc16_load_page(&f->guards[0], p->buffer), -EAGAIN);
	KUNIT_ASSERT_EQ(test, mc16_ws_isolate(f->folio), 0);
	KUNIT_ASSERT_TRUE(test, mc16_ws_release(p));
	mc16_ws_expect_exit(test, p);
}

/* Cancel a real begun transaction before any payload commit, not after store. */
static void
memcomp_test_workingset_store_cancel_before_commit(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_sct_result result;
	struct mc16_ws_test *p;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	/* Cancel after the real gate opens, before a payload commit can win. */
	folio_lock(f->folio);
	mutex_lock(memcomp_lock_of(f->guards[0].token));
	result.error = mc16_store_begin_locked(&f->guards[0], f->folio, f->nr, 0);
	result.success = !result.error;
	mutex_unlock(memcomp_lock_of(f->guards[0].token));
	folio_unlock(f->folio);
	KUNIT_ASSERT_EQ(test, result.error, 0);
	KUNIT_ASSERT_TRUE(test, result.success);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[0]), MC16_SHADOW_PUBLISHING);
	mc16_flt_drop(f, true);
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[0]), 0ULL);
	KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[0], false) & MC16_F_OWNER);
	mc16_flt_expect_present(f, 0, f->addr);
}

static void memcomp_test_workingset_store_cache(struct kunit *test)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	struct mc16_ws_test *p;
	u64 shadow;

	KUNIT_ASSERT_NOT_NULL(test, f);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	folio_set_swapbacked(f->src);
	mc16_cf_fill(f, MC16_CF_BYTES);
	KUNIT_ASSERT_TRUE(test, mc16_cf_store(f));
	KUNIT_ASSERT_NE(test, f->token, 0UL);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->token, &f->guard));
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guard), MC16_SHADOW_PUBLISHING);
	KUNIT_EXPECT_EQ(test, mc16_load_page(&f->guard, p->buffer), -EAGAIN);
	/*
	 * This models the actual winning XA replacement's post-unlock tail.
	 * Removing its owner concurrently is legal: our prior guard survives.
	 */
	mc16_cache_drop(f->token);
	f->cache_owner = false;
	folio_lock(f->src);
	mc16_publish_cache_eviction(f->src, &f->guard);
	folio_unlock(f->src);
	shadow = mc16_ws_word(&f->guard);
	KUNIT_EXPECT_EQ(test, p->calls, 1U);
	KUNIT_EXPECT_EQ(test, p->pages, 1U);
	KUNIT_EXPECT_EQ(test, p->source_refs, 1);
	KUNIT_EXPECT_TRUE(test, p->locked);
	KUNIT_EXPECT_TRUE(test, p->isolated);
	KUNIT_EXPECT_TRUE(test, xa_is_value((void *)(unsigned long)shadow));
	KUNIT_EXPECT_EQ(test, mc16_load_page(&f->guard, p->buffer), 0);
	{
		void *src = kmap_local_folio(f->src, 0);

		KUNIT_EXPECT_EQ(test, memcmp(src, p->buffer, PAGE_SIZE), 0);
		kunmap_local(src);
	}
	mc16_ws_clear(&f->guard);
	folio_lock(f->src);
	mc16_publish_cache_eviction(f->src, &f->guard);
	folio_unlock(f->src);
	KUNIT_EXPECT_EQ(test, p->calls, 1U);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guard), 0ULL);
}

static void memcomp_test_workingset_store_cache_abort(struct kunit *test)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	struct mc16_ws_test *p;

	KUNIT_ASSERT_NOT_NULL(test, f);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	folio_set_swapbacked(f->src);
	mc16_cf_fill(f, MC16_CF_FILL);
	KUNIT_ASSERT_TRUE(test, mc16_cf_store(f));
	KUNIT_ASSERT_NE(test, f->token, 0UL);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->token, &f->guard));
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guard), MC16_SHADOW_PUBLISHING);
	mc16_cache_drop(f->token); /* Losing replacement: no eviction tail. */
	f->cache_owner = false;
	mc16_ref_put(&f->guard);
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	KUNIT_EXPECT_FALSE(test, mc16_entry_present(f->token));
}

static void memcomp_test_workingset_store_legacy(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_ws_test *p;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ws_prepare(f), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->anon = f;
	/* Successful old API executes the same real eviction, with NULL target. */
	KUNIT_ASSERT_TRUE(test, mc16_flt_release(f->folio));
	mc16_ws_expect_exit(test, p);
}

static void memcomp_test_workingset_store_zero_excluded(struct kunit *test)
{
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	struct mc16_ws_test *p;
	bool released;

	KUNIT_ASSERT_NOT_NULL(test, f);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	f->account->swap = make_memcomp_entry(0);
	released = mc16_flt_release(f->account);
	f->account->swap.val = 0;
	KUNIT_EXPECT_TRUE(test, released);
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
}

static void memcomp_test_workingset_store_partial(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_sct_result result;
	struct mc16_ws_test *p;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	p = mc16_ws_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, p);
	result = mc16_sct_store(f, (struct mc16_sct_attempt) {
		.fail = MEMCOMP_KUNIT_FAIL_REFRESH_COMMIT, .nth = 2,
	});
	KUNIT_ASSERT_EQ(test, result.error, 0);
	KUNIT_EXPECT_FALSE(test, result.success);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[0]), MC16_SHADOW_PUBLISHING);
	KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[1]), 0ULL);
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
	mc16_flt_drop(f, true);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, mc16_ws_word(&f->guards[i]), 0ULL);
		KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[i], false) &
					 MC16_F_OWNER);
		mc16_flt_expect_present(f, i, f->addr + i * PAGE_SIZE);
	}
	KUNIT_EXPECT_EQ(test, p->calls, 0U);
}

#endif /* MC16_FOLIO_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_WORKINGSET_STORE_TEST_H */
