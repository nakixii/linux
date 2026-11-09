/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_STORE_TRANSACTION_TEST_H
#define _MM_MEMCOMP_TESTS_STORE_TRANSACTION_TEST_H

/*
 * Author: nakixii
 *
 * Include after folio_test.h in suite.h. Register the
 * eight cases below under MC16_STORE_TRANSACTION_TEST_ENABLED. The enclosing
 * CONFIG_MEMCOMP_KUNIT_TEST suite supplies the codec and failure hooks.
 *
 * Transaction coverage:
 * success and failure consume the public context lease;
 * unlocked folios and missing spans fail preflight without changing owners;
 * failure accounting records exclusive reasons and whole-span outcomes;
 * payload failures record one event and whole-span pages, without a state event;
 * only real -E2BIG marks incompressible folios, and rollback installs the hash;
 * successful stores release the producer pin;
 * committed payloads and surviving guards remain live through owner detach,
 * with resource balance checked by the shared fixture;
 * rollback restores real PTEs; exact rewalk and lookup-only checks belong to
 * rollback_test.h.
 *
 * Real charged anonymous folios, PTEs, rmap, codec, zs payloads and public
 * reserve/store/release/rollback execute. objcg and slot allocation use the
 * existing consumer model. Compressor/arch/commit errors and task-context
 * routing are serial models; no reclaim scheduling, device or async engine
 * execution is claimed. No fabricated entries or successful stores.
 */
#ifdef MC16_FOLIO_TEST_ENABLED
#define MC16_STORE_TRANSACTION_TEST_ENABLED

struct mc16_sct_stats {
	s64 preflight;
	s64 busy;
	s64 arch;
	s64 payload;
	s64 payload_pages;
	s64 state;
	s64 state_pages;
	s64 bg_ok;
	s64 bg_fail;
	s64 direct_ok;
	s64 direct_fail;
	s64 pageout_ok;
	s64 pageout_fail;
};

static struct mc16_sct_stats mc16_sct_stats_read(void)
{
	struct mc16_sct_stats s = { };

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	s.preflight = atomic64_read(&memcomp_refresh_preflight_fail_pages);
	s.busy = atomic64_read(&memcomp_refresh_folio_busy_fail_pages);
	s.arch = atomic64_read(&memcomp_refresh_arch_fail_pages);
	s.payload = atomic64_read(&memcomp_refresh_payload_fail);
	s.payload_pages = atomic64_read(&memcomp_refresh_payload_fail_pages);
	s.state = atomic64_read(&memcomp_refresh_state_fail);
	s.state_pages = atomic64_read(&memcomp_refresh_state_fail_pages);
	s.bg_ok = atomic64_read(&memcomp_bg_store_ok);
	s.bg_fail = atomic64_read(&memcomp_bg_store_fail);
	s.direct_ok = atomic64_read(&memcomp_direct_store_ok);
	s.direct_fail = atomic64_read(&memcomp_direct_store_fail);
	s.pageout_ok = atomic64_read(&memcomp_pageout_store_ok);
	s.pageout_fail = atomic64_read(&memcomp_pageout_store_fail);
#endif
	return s;
}

/* Every unspecified counter must remain unchanged. */
static void mc16_sct_stats_expect(struct kunit *test,
				  struct mc16_sct_stats before,
				  struct mc16_sct_stats delta)
{
	struct mc16_sct_stats after = mc16_sct_stats_read();

	if (!IS_ENABLED(CONFIG_MEMCOMP_DEBUG_STATS))
		return;
	KUNIT_EXPECT_EQ_MSG(test, after.preflight - before.preflight,
			    delta.preflight, "preflight");
	KUNIT_EXPECT_EQ_MSG(test, after.busy - before.busy,
			    delta.busy, "busy");
	KUNIT_EXPECT_EQ_MSG(test, after.arch - before.arch,
			    delta.arch, "arch");
	KUNIT_EXPECT_EQ_MSG(test, after.payload - before.payload,
			    delta.payload, "payload");
	KUNIT_EXPECT_EQ_MSG(test, after.payload_pages - before.payload_pages,
			    delta.payload_pages, "payload_pages");
	KUNIT_EXPECT_EQ_MSG(test, after.state - before.state,
			    delta.state, "state");
	KUNIT_EXPECT_EQ_MSG(test, after.state_pages - before.state_pages,
			    delta.state_pages, "state_pages");
	KUNIT_EXPECT_EQ_MSG(test, after.bg_ok - before.bg_ok,
			    delta.bg_ok, "bg_ok");
	KUNIT_EXPECT_EQ_MSG(test, after.bg_fail - before.bg_fail,
			    delta.bg_fail, "bg_fail");
	KUNIT_EXPECT_EQ_MSG(test, after.direct_ok - before.direct_ok,
			    delta.direct_ok, "direct_ok");
	KUNIT_EXPECT_EQ_MSG(test, after.direct_fail - before.direct_fail,
			    delta.direct_fail, "direct_fail");
	KUNIT_EXPECT_EQ_MSG(test, after.pageout_ok - before.pageout_ok,
			    delta.pageout_ok, "pageout_ok");
	KUNIT_EXPECT_EQ_MSG(test, after.pageout_fail - before.pageout_fail,
			    delta.pageout_fail, "pageout_fail");
}

enum mc16_sct_route {
	MC16_SCT_BACKGROUND,
	MC16_SCT_DIRECT,
	MC16_SCT_PAGEOUT,
};

struct mc16_sct_attempt {
	bool unlocked;
	bool extra_ref;
	bool bad_token;
	int compress_error;
	enum memcomp_kunit_fail_stage fail;
	unsigned int nth;
	enum mc16_sct_route route;
};

struct mc16_sct_result {
	int error;
	bool success;
	bool consumed;
};

/* All leases, flags, markers and locks are restored before any assertion. */
static struct mc16_sct_result
mc16_sct_store(struct mc16_flt *f, struct mc16_sct_attempt a)
{
	struct mc16_sct_result r = { .error = -EBUSY };
	struct reclaim_state direct = { };
	struct reclaim_state *saved_reclaim = current->reclaim_state;
	struct memcomp_reclaim_ctx *ctx;
	swp_entry_t saved_swap;
	unsigned int flags;

	flags = memcomp_test_reclaimer_enter();
	ctx = memcomp_reclaim_ctx_get(f->nr, MEMCOMP_RECLAIM_KSWAPD);
	if (!ctx)
		goto out_flags;
	ctx->kunit_compress_error = a.compress_error;
	if (a.route != MC16_SCT_BACKGROUND) {
		current->flags &= ~PF_KSWAPD;
		current->reclaim_state = a.route == MC16_SCT_DIRECT ? &direct : NULL;
	}
	if (!a.unlocked)
		folio_lock(f->folio);
	saved_swap = f->folio->swap;
	if (a.bad_token)
		f->folio->swap = make_memcomp_entry(U32_MAX - 1);
	if (a.extra_ref)
		folio_get(f->folio);
	if (a.fail)
		memcomp_kunit_fail_next(a.fail, a.nth,
					a.fail == MEMCOMP_KUNIT_FAIL_REFRESH_COMMIT ?
					 -EAGAIN : -EIO);
	r.success = memcomp_store_after_unmap(f->folio, &ctx);
	r.consumed = !ctx;
	/* Defensive put also makes a failed consumption oracle safe to report. */
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	memcomp_kunit_fail_clear();
	if (a.extra_ref)
		folio_put(f->folio);
	if (a.bad_token)
		f->folio->swap = saved_swap;
	if (!a.unlocked)
		folio_unlock(f->folio);
	r.error = 0;
out_flags:
	current->reclaim_state = saved_reclaim;
	memcomp_test_reclaimer_exit(flags);
	return r;
}

static bool mc16_sct_reserve(struct mc16_flt *f)
{
	bool ret;

	folio_lock(f->folio);
	ret = memcomp_reserve(f->folio);
	folio_unlock(f->folio);
	return ret;
}

static void mc16_sct_rollback(struct mc16_flt *f)
{
	mmap_read_lock(f->mm);
	folio_lock(f->folio);
	memcomp_rollback_folio(f->folio);
	folio_unlock(f->folio);
	mmap_read_unlock(f->mm);
}

static bool mc16_sct_release(struct mc16_flt *f)
{
	bool ret;

	/* Match the real reclaim actor's isolated-source precondition. */
	lru_add_drain_all();
	if (folio_test_lru(f->folio)) {
		if (!folio_isolate_lru(f->folio))
			return false;
		/* Model reclaim isolation, not migration: the source is inactive. */
		folio_clear_active(f->folio);
		folio_put(f->folio);
	}
	folio_lock(f->folio);
	ret = memcomp_release_folio(f->folio);
	if (ret)
		folio_ref_unfreeze(f->folio, 1);
	folio_unlock(f->folio);
	return ret;
}

static void mc16_sct_copy(struct mc16_flt *f, const void *src)
{
	unsigned int i;
	void *dst;

	folio_lock(f->folio);
	for (i = 0; i < f->nr; i++) {
		dst = kmap_local_folio(f->folio, i * PAGE_SIZE);
		memcpy(dst, src + i * PAGE_SIZE, PAGE_SIZE);
		kunmap_local(dst);
	}
	flush_dcache_folio(f->folio);
	folio_unlock(f->folio);
}

/* Nonuniform, highly compressible bytes; page 0 can instead be nonzero SAMEFILL. */
static void mc16_sct_pattern(u8 *src, unsigned int nr, bool samefill)
{
	unsigned int i;

	for (i = 0; i < nr * PAGE_SIZE; i++)
		src[i] = (i % 251) ^ ((i / PAGE_SIZE) * 0x39);
	if (samefill)
		memset(src, 0x5a, PAGE_SIZE);
}

static void mc16_sct_run(struct mc16_flt *f, struct mc16_sct_attempt a,
			 bool success)
{
	struct mc16_sct_result r = mc16_sct_store(f, a);

	KUNIT_EXPECT_EQ(f->test, r.error, 0);
	KUNIT_EXPECT_TRUE(f->test, r.consumed);
	KUNIT_EXPECT_EQ(f->test, r.success, success);
}

static void mc16_sct_expect_pending(struct mc16_flt *f)
{
	unsigned int i, flags;

	for (i = 0; i < f->nr; i++) {
		flags = mc16_flt_flags(&f->guards[i], false);
		KUNIT_EXPECT_TRUE(f->test, flags & MC16_F_OWNER);
		KUNIT_EXPECT_EQ(f->test, mc16_flags_state(flags), MC16_PENDING);
		KUNIT_EXPECT_TRUE(f->test, flags & MC16_F_RESIDENT);
		KUNIT_EXPECT_FALSE(f->test, flags & MC16_F_REFRESH_PENDING);
		KUNIT_EXPECT_FALSE(f->test, flags & MC16_F_INCOMPRESSIBLE);
		KUNIT_EXPECT_EQ(f->test,
				refcount_read(&f->guards[i].slot->refs), 3U);
	}
	KUNIT_EXPECT_EQ(f->test, folio_ref_count(f->folio), 1 + f->nr);
}

static void mc16_sct_expect_load(struct mc16_flt *f, unsigned int i,
				 const void *src, void *dst)
{
	struct memcomp_ref load = { };
	void *bytes;
	int ret = -ENOENT;

	if (memcomp_load_folio_pin(f->guards[i].token, &load)) {
		folio_lock(f->base->dst);
		ret = memcomp_load_folio(&load, f->base->dst);
		if (!ret) {
			bytes = kmap_local_folio(f->base->dst, 0);
			memcpy(dst, bytes, PAGE_SIZE);
			kunmap_local(bytes);
		}
		folio_unlock(f->base->dst);
		memcomp_load_folio_abort(&load);
	}
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	if (!ret)
		KUNIT_EXPECT_EQ(f->test, memcmp(src, dst, PAGE_SIZE), 0);
}

static void memcomp_test_store_transaction_success(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_sct_stats before;
	u8 *src, *dst;
	unsigned int i;
	int anon_refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	mc16_sct_pattern(src, 2, true);
	mc16_sct_copy(f, src);
	anon_refs = atomic_read(&f->anon_vma->refcount);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), anon_refs + 1);
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	before = mc16_sct_stats_read();
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) { }, true);
	mc16_sct_stats_expect(test, before,
			      (struct mc16_sct_stats) { .bg_ok = 2 });
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), anon_refs);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 1);
	KUNIT_EXPECT_FALSE(test, folio_mapped(f->folio));
	KUNIT_EXPECT_EQ(test, mc16_flt_flags(&f->guards[0], false) &
			MC16_STATE_MASK, (unsigned int)MC16_SAMEFILL);
	KUNIT_EXPECT_EQ(test, mc16_flt_flags(&f->guards[1], false) &
			MC16_STATE_MASK, (unsigned int)MC16_STORED);
	KUNIT_ASSERT_TRUE(test, mc16_sct_release(f));
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[i], false) &
				   MC16_F_OWNER);
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 2U);
		mc16_sct_expect_load(f, i, src + i * PAGE_SIZE, dst);
	}
	/* Shared cleanup zaps real PTE refs, then puts guards and checks balances. */
}

static void memcomp_test_store_transaction_preflight_lookup(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct memcomp_ref absent = { };
	struct mc16_sct_stats before;
	bool found;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	found = memcomp_entry_get_ref(U32_MAX - 1, &absent);
	if (found)
		memcomp_entry_put_ref(&absent);
	KUNIT_ASSERT_FALSE(test, found);
	/* Each failed preflight preserves pending owners and consumes its context. */
	before = mc16_sct_stats_read();
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) { .unlocked = true }, false);
	mc16_sct_stats_expect(test, before,
			      (struct mc16_sct_stats) { .preflight = 2, .bg_fail = 2 });
	mc16_sct_expect_pending(f);
	before = mc16_sct_stats_read();
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) { .bad_token = true }, false);
	mc16_sct_stats_expect(test, before,
			      (struct mc16_sct_stats) { .preflight = 2, .bg_fail = 2 });
	mc16_sct_expect_pending(f);
	mc16_sct_rollback(f);
	mc16_flt_expect_present(f, 0, f->addr);
}

static void memcomp_test_store_transaction_busy(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_sct_stats before;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	before = mc16_sct_stats_read();
	/* Real mappings refuse store before the span lookup/codec path. */
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) { }, false);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) { .extra_ref = true }, false);
	mc16_sct_stats_expect(test, before,
			      (struct mc16_sct_stats) { .busy = 4, .bg_fail = 4 });
	mc16_sct_expect_pending(f);
	mc16_sct_rollback(f);
}

static void memcomp_test_store_transaction_arch_failure(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_sct_stats before;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	before = mc16_sct_stats_read();
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) {
			 .fail = MEMCOMP_KUNIT_FAIL_ARCH_PREPARE, .nth = 1,
		 }, false);
	mc16_sct_stats_expect(test, before,
			      (struct mc16_sct_stats) { .arch = 2, .bg_fail = 2 });
	mc16_sct_expect_pending(f);
	mc16_sct_rollback(f);
	mc16_flt_expect_present(f, 0, f->addr);
}

static void memcomp_test_store_transaction_payload_pages(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_sct_stats before;
	long charged;
	u8 *src;
	unsigned int i, page;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	s64 compress_fail;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	charged = atomic_long_read(&f->base->model.charged);
	for (page = 0; page < f->nr; page++) {
		/* SAMEFILL bypasses compression, so -EIO reaches page 1. */
		mc16_sct_pattern(src, 2, page == 1);
		mc16_sct_copy(f, src);
		before = mc16_sct_stats_read();
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		compress_fail = atomic64_read(&memcomp_compress_fail);
#endif
		mc16_sct_run(f,
			     (struct mc16_sct_attempt) { .compress_error = -EIO }, false);
		mc16_sct_stats_expect(test, before,
				      (struct mc16_sct_stats) {
				 .payload = 1, .payload_pages = 2, .bg_fail = 2,
			 });
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compress_fail),
				compress_fail + 1);
#endif
		KUNIT_EXPECT_EQ(test, atomic_long_read(&f->base->model.charged),
				charged);
		mc16_sct_expect_pending(f);
	}
	/* The prepared page-0 prefix in the second attempt is SAMEFILL, not zs. */
	mc16_sct_rollback(f);
}

static void memcomp_test_store_transaction_partial_commit(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_sct_stats before;
	u8 *src, *dst;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	mc16_sct_pattern(src, 2, false);
	mc16_sct_copy(f, src);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	before = mc16_sct_stats_read();
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) {
			 .fail = MEMCOMP_KUNIT_FAIL_REFRESH_COMMIT, .nth = 2,
		 }, false);
	mc16_sct_stats_expect(test, before,
			      (struct mc16_sct_stats) {
			 .state = 1, .state_pages = 2, .bg_fail = 2,
		 });
	KUNIT_EXPECT_EQ(test, mc16_flt_flags(&f->guards[0], false) &
			MC16_STATE_MASK, (unsigned int)MC16_STORED);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(mc16_flt_flags(&f->guards[1], false)),
			MC16_PENDING);
	KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[1], false) &
			 MC16_F_RESIDENT);
	KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[1], false) &
			  MC16_F_REFRESH_PENDING);
	KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[0], false) &
			  MC16_F_INCOMPRESSIBLE);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 2);
	mc16_sct_rollback(f);
	KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[0], false) &
			  MC16_F_OWNER);
	mc16_sct_expect_load(f, 0, src, dst);
}

static void memcomp_test_store_transaction_incompressible(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_sct_stats before;
	u8 *src;
	bool skip;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	s64 rejected, compress_fail;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	memcomp_test_fill_incompressible(src, 1);
	mc16_sct_copy(f, src);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	before = mc16_sct_stats_read();
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	rejected = atomic64_read(&memcomp_raw_reject_huge) +
		atomic64_read(&memcomp_raw_reject_ge_page);
	compress_fail = atomic64_read(&memcomp_compress_fail);
#endif
	/* Real codec output must exceed the accepted single-page payload size. */
	mc16_sct_run(f,
		     (struct mc16_sct_attempt) { }, false);
	mc16_sct_stats_expect(test, before,
			      (struct mc16_sct_stats) {
			 .payload = 1, .payload_pages = 1, .bg_fail = 1,
		 });
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_raw_reject_huge) +
			atomic64_read(&memcomp_raw_reject_ge_page), rejected + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compress_fail), compress_fail);
#endif
	KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[0], false) &
			 MC16_F_INCOMPRESSIBLE);
	mc16_sct_rollback(f);
	mc16_flt_expect_present(f, 0, f->addr);
	folio_lock(f->folio);
	skip = memcomp_folio_incompressible(f->folio);
	folio_unlock(f->folio);
	KUNIT_EXPECT_TRUE(test, skip);
	/* Skip predicate executes; vmscan's skip counter increment is not modeled. */
	memcomp_entry_put_ref(&f->guards[0]);
}

static void memcomp_test_store_transaction_outcomes(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_sct_stats before, delta;
	unsigned int i;
	enum mc16_sct_route route;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	for (route = MC16_SCT_BACKGROUND; route <= MC16_SCT_PAGEOUT; route++) {
		before = mc16_sct_stats_read();
		mc16_sct_run(f,
			     (struct mc16_sct_attempt) {
				 .unlocked = true, .route = route,
			 }, false);
		delta = (struct mc16_sct_stats) { .preflight = 2 };
		if (route == MC16_SCT_BACKGROUND)
			delta.bg_fail = 2;
		else if (route == MC16_SCT_DIRECT)
			delta.direct_fail = 2;
		else
			delta.pageout_fail = 2;
		mc16_sct_stats_expect(test, before, delta);
	}
	mc16_sct_expect_pending(f);
	mc16_sct_rollback(f);
}
#endif /* MC16_FOLIO_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_STORE_TRANSACTION_TEST_H */
