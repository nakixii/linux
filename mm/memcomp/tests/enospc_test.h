/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ENOSPC_TEST_H
#define _MM_MEMCOMP_TESTS_ENOSPC_TEST_H

#ifdef CONFIG_MEMCG
/* Include after the shared fixtures in suite.h; register there. */
static void memcomp_test_enospc_result(struct kunit *test)
{
	static const struct {
		int status;
		int result;
		unsigned int length;
		bool reusable;
	} cases[] = {
		{ 0, 0, 17, true },
		{ -E2BIG, 0, PAGE_SIZE, true },
		{ -ENOSPC, 0, PAGE_SIZE, true },
		{ -EIO, -EIO, 17, false },
		{ -ENODATA, -ENODATA, 17, false },
		{ -EHWPOISON, -EHWPOISON, 17, false },
	};
	struct memcomp_async_req request = {};
	struct memcomp_store_ctx store = { .precomp = &request };
	const struct memcomp_async_req *expected;
	unsigned int i, length;
	int ret;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		length = 17;
		ret = memcomp_compress_result(cases[i].status, &length,
					      PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, ret, cases[i].result);
		KUNIT_EXPECT_EQ(test, length, cases[i].length);
		request.ret = cases[i].status;
		request.len = 17;
		expected = NULL;
		if (cases[i].reusable)
			expected = &request;
		KUNIT_EXPECT_PTR_EQ(test, memcomp_store_precomp(&store, 0), expected);
		/* A codec error cannot manufacture a software scan result. */
		KUNIT_EXPECT_FALSE(test, request.same_filled);
		KUNIT_EXPECT_EQ(test, request.ret, cases[i].status);
		KUNIT_EXPECT_EQ(test, request.len, 17U);
	}
	KUNIT_EXPECT_NULL(test, memcomp_store_precomp(NULL, 0));
	store.precomp = NULL;
	KUNIT_EXPECT_NULL(test, memcomp_store_precomp(&store, 0));
}

/* Leave the atomic zsmalloc mapping before KUnit can report a failure. */
static bool memcomp_test_enospc_raw_matches(u8 pool_idx,
					    unsigned long handle,
					    const void *src)
{
	struct zs_pool *pool = memcomp_pools[pool_idx];
	const void *payload;
	bool matches;

	payload = zs_map_object(pool, handle, ZS_MM_RO);
	matches = payload && !memcmp(payload, src, PAGE_SIZE);
	if (payload)
		zs_unmap_object(pool, handle);
	return matches;
}

/* Exercise raw policy through the real synchronous and precomputed paths. */
static void memcomp_test_enospc_payload(struct kunit *test,
					bool precomputed)
{
	static const int statuses[] = { -ENOSPC, -E2BIG, -EIO };
	struct memcomp_async_req request = { .len = 17 };
	struct memcomp_reclaim_ctx *ctx;
	struct memcomp_store_ctx store = {};
	const struct memcomp_async_req *selected;
	unsigned int reclaim_flags, length, i, raw;
	unsigned long handle;
	s64 candidate, reject, fail;
	u8 pool_idx;
	u8 *src;
	bool matches;
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
	if (!ctx) {
		memcomp_test_reclaimer_exit(reclaim_flags);
		KUNIT_FAIL(test, "failed to acquire a compressor context");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(statuses); i++) {
		request.ret = statuses[i];
		store.precomp = precomputed ? &request : NULL;
		selected = memcomp_store_precomp(&store, 0);
		for (raw = 0; raw <= 1; raw++) {
			candidate =
				atomic64_read(&memcomp_raw_candidate_ge_page);
			reject = atomic64_read(&memcomp_raw_reject_ge_page);
			fail = atomic64_read(&memcomp_compress_fail);
			length = 0;
			handle = 0;
			pool_idx = 0;
			/* Reused output must leave the sync sentinel intact. */
			ctx->kunit_compress_error =
				precomputed ? -EIO : statuses[i];
			ret = memcomp_prepare_payload_locked(&ctx, src,
							     PAGE_SIZE, &length,
							     &handle, &pool_idx,
							     raw, NULL, selected);
			if (statuses[i] == -EIO) {
				KUNIT_EXPECT_EQ(test, ret, -EIO);
				KUNIT_EXPECT_EQ(test, length, 0U);
				KUNIT_EXPECT_EQ(test,
						atomic64_read(&memcomp_compress_fail),
					fail + 1);
				KUNIT_EXPECT_EQ(test,
						ctx->kunit_compress_error, 0);
			} else {
				KUNIT_EXPECT_EQ(test, ret, raw ? 0 : -E2BIG);
				KUNIT_EXPECT_EQ(test, length,
						raw ? (unsigned int)PAGE_SIZE : 0U);
				KUNIT_EXPECT_EQ(test,
						atomic64_read(&memcomp_compress_fail),
					fail);
				KUNIT_EXPECT_EQ(test, ctx->kunit_compress_error,
						precomputed ? -EIO : 0);
				candidate++;
				reject += !raw;
			}
			KUNIT_EXPECT_EQ(test,
					atomic64_read(&memcomp_raw_candidate_ge_page),
				candidate);
			KUNIT_EXPECT_EQ(test,
					atomic64_read(&memcomp_raw_reject_ge_page),
				reject);
			if (!ret && length) {
				if (length == PAGE_SIZE) {
					matches = memcomp_test_enospc_raw_matches(pool_idx,
										  handle, src);
					KUNIT_EXPECT_TRUE(test, matches);
				}
				memcomp_zs_free(pool_idx, handle);
			}
		}
	}
	ctx->kunit_compress_error = 0;
	memcomp_reclaim_ctx_put(ctx);
	memcomp_test_reclaimer_exit(reclaim_flags);
}

static void memcomp_test_enospc_sync(struct kunit *test)
{
	memcomp_test_enospc_payload(test, false);
}

static void memcomp_test_enospc_precomp(struct kunit *test)
{
	memcomp_test_enospc_payload(test, true);
}

/* Result events precede admission: a later store refusal still counts. */
static void memcomp_test_enospc_async_events(struct kunit *test)
{
	static const struct {
		int status;
		bool same_filled;
		unsigned int payloads;
		unsigned int failures;
	} cases[] = {
		{ 0, false, 2, 0 },
		{ -ENOSPC, false, 2, 0 },
		{ -E2BIG, false, 2, 0 },
		{ -EIO, false, 0, 2 },
		{ -ENODATA, false, 0, 2 },
		{ -EHWPOISON, false, 0, 2 },
		{ 0, true, 0, 0 },
		{ -ENOSPC, true, 0, 0 },
	};
	struct memcomp_reclaim_ctx *ctx;
	struct memcomp_async *async;
	struct folio *folio;
	unsigned int reclaim_flags, i, j;
	s64 payloads, failures;
	bool stored;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return;
	}
	async = kunit_kzalloc(test, sizeof(*async), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, async);
	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 1);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_lock(folio);
	/* A non-anon folio refuses preflight before reading payload buffers. */
	async->nr = 3;
	async->reqs[0].ret = -EIO;
	for (j = 1; j < async->nr; j++)
		async->reqs[j].folio = folio;
	reclaim_flags = memcomp_test_reclaimer_enter();
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		for (j = 1; j < async->nr; j++) {
			async->reqs[j].ret = cases[i].status;
			async->reqs[j].same_filled = cases[i].same_filled;
		}
		ctx = memcomp_reclaim_ctx_get(2, MEMCOMP_RECLAIM_KSWAPD);
		if (!ctx) {
			KUNIT_FAIL(test, "no compressor context");
			break;
		}
		payloads = atomic64_read(&memcomp_async_payloads);
		failures = atomic64_read(&memcomp_async_fail);
		stored = memcomp_store_after_unmap_async(folio, &ctx, async);
		KUNIT_EXPECT_FALSE(test, stored);
		KUNIT_EXPECT_PTR_EQ(test, ctx, NULL);
		if (ctx)
			memcomp_reclaim_ctx_put(ctx);
		KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_async_payloads),
				payloads + cases[i].payloads);
		KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_async_fail),
				failures + cases[i].failures);
	}
	memcomp_test_reclaimer_exit(reclaim_flags);
	folio_unlock(folio);
	folio_put(folio);
}

#endif /* CONFIG_MEMCG */

#endif /* _MM_MEMCOMP_TESTS_ENOSPC_TEST_H */
