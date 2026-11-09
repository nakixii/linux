/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_CODEC_SCOPE_TEST_H
#define _MM_MEMCOMP_TESTS_CODEC_SCOPE_TEST_H

#ifdef CONFIG_MEMCOMP_KUNIT_TEST
#include <kunit/test.h>

/* Include after the production async and store-precomp definitions. */
static void memcomp_test_async_error_recycled(struct kunit *test)
{
	const int errors[] = { -EIO, -EHWPOISON };
	struct memcomp_store_ctx store = {};
	struct memcomp_async_pool *pool;
	struct memcomp_async *batch, *got;
	struct memcomp_async_req *r;
	unsigned int i, wait_first;

	pool = kunit_kzalloc(test, sizeof(*pool), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pool);
	batch = kunit_kzalloc(test, sizeof(*batch), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, batch);
	r = &batch->reqs[0];
	r->req = kunit_kzalloc(test, sizeof(*r->req), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r->req);
	r->async = batch;
	/* Failed output must never become an accepted precomputed payload. */
	r->buf = NULL;
	store.precomp = r;
	spin_lock_init(&pool->lock);
	init_completion(&batch->done);
	pool->nr = 1;
	pool->free = BIT(0);
	pool->batches[0] = batch;

	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		for (wait_first = 0; wait_first < 2; wait_first++) {
			got = memcomp_async_take(pool, 0);
			KUNIT_ASSERT_PTR_EQ(test, got, batch);
			KUNIT_EXPECT_EQ(test, got->nr, 0U);
			KUNIT_EXPECT_EQ(test, pool->free, 0UL);
			got->nr = 1;
			r->ret = -EAGAIN;
			r->len = 0;
			r->req->dlen = PAGE_SIZE;
			atomic_inc(&got->pending);
			/* Model a waiter dropping its sentinel before completion. */
			if (wait_first)
				atomic_dec(&got->pending);

			/* A backlog notification neither finishes nor frees a batch. */
			memcomp_async_done(r, -EINPROGRESS);
			KUNIT_ASSERT_EQ(test, atomic_read(&got->pending),
					wait_first ? 1 : 2);
			KUNIT_EXPECT_EQ(test, r->ret, -EAGAIN);
			KUNIT_EXPECT_EQ(test, r->len, 0U);
			KUNIT_EXPECT_FALSE(test, completion_done(&got->done));
			KUNIT_EXPECT_EQ(test, pool->free, 0UL);

			memcomp_async_done(r, errors[i]);
			KUNIT_EXPECT_EQ(test, r->ret, errors[i]);
			KUNIT_EXPECT_EQ(test, r->len, (unsigned int)PAGE_SIZE);
			KUNIT_EXPECT_NULL(test, memcomp_store_precomp(&store, 0));
			KUNIT_ASSERT_EQ(test, atomic_read(&got->pending),
					wait_first ? 0 : 1);
			KUNIT_EXPECT_EQ(test, completion_done(&got->done),
					(bool)wait_first);
			memcomp_async_give(pool, got);
			KUNIT_EXPECT_EQ(test, atomic_read(&got->pending), 0);
			KUNIT_ASSERT_EQ(test, pool->free, BIT(0));
		}
	}

	/* The last error must leave the same batch available for another run. */
	got = memcomp_async_take(pool, 0);
	KUNIT_ASSERT_PTR_EQ(test, got, batch);
	KUNIT_EXPECT_EQ(test, got->nr, 0U);
	KUNIT_EXPECT_FALSE(test, completion_done(&got->done));
	memcomp_async_give(pool, got);
	KUNIT_EXPECT_EQ(test, pool->free, BIT(0));
}

static struct kunit_case memcomp_codec_scope_test_cases[] = {
	KUNIT_CASE(memcomp_test_async_error_recycled),
	{}
};

static struct kunit_suite memcomp_codec_scope_test_suite = {
	.name = "memcomp-codec-scope",
	.test_cases = memcomp_codec_scope_test_cases,
};

kunit_test_suite(memcomp_codec_scope_test_suite);
#endif /* CONFIG_MEMCOMP_KUNIT_TEST */
#endif /* _MM_MEMCOMP_TESTS_CODEC_SCOPE_TEST_H */
