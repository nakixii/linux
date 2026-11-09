/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_LOAD_TEST_H
#define _MM_MEMCOMP_TESTS_LOAD_TEST_H
#ifdef CONFIG_MEMCG
static void memcomp_test_load_pending(struct kunit *test)
{
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	void *dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	memset(dst, 0x5a, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, mc16_load_page(&f->refs->storage->refs[0], dst),
			-EAGAIN);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(dst, 0x5a, PAGE_SIZE), NULL);
}

static void memcomp_test_load_samefill(struct kunit *test)
{
	const unsigned long pattern = 0xfedcba9876543210UL;
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	struct mc16_record *detached;
	struct mc16_view view;
	unsigned long *dst;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, 0, pattern), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, storage->owner->folio), 0);
	mc16_payload_finish(ref, &f->tail);
	KUNIT_ASSERT_EQ(test, mc16_load_page(ref, dst), 0);
	for (i = 0; i < PAGE_SIZE / sizeof(*dst); i++)
		KUNIT_EXPECT_EQ(test, dst[i], pattern);

	/* Model the completed producer's detach while retaining slot ownership. */
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	mc16_account_state(ref, false);
	*view.flags &= ~MC16_F_OWNER;
	mc16_account_state(ref, true);
	detached = mc16_normalize_locked(ref);
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_ASSERT_NOT_NULL(test, detached);
	mc16_record_member_put(detached);
	memset(dst, 0, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, mc16_load_page(ref, dst), 0);
	for (i = 0; i < PAGE_SIZE / sizeof(*dst); i++)
		KUNIT_EXPECT_EQ(test, dst[i], pattern);
}

static void mc16_test_load_payload(struct kunit *test, bool raw,
				   bool writeback)
{
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	struct mc16_view view;
	unsigned int bytes = 0, i;
	unsigned long handle = 0;
	u8 pool = 0;
	u8 *src, *dst;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = raw ? (i * 37 + (i >> 5)) : (i % 53);
	if (raw)
		ret = memcomp_alloc_raw_payload(0, src, PAGE_SIZE, &bytes,
						&handle, false, NULL);
	else
		ret = memcomp_prepare_payload(src, PAGE_SIZE, &bytes, &handle,
					      &pool, MEMCOMP_PAYLOAD_MUST_SUCCEED,
					      false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->prepared = (struct mc16_payload) { .word = handle, .bytes = bytes };
	KUNIT_ASSERT_EQ(test, pool, (u8)0);
	if (raw) {
		KUNIT_EXPECT_EQ(test, bytes, (unsigned int)PAGE_SIZE);
	} else {
		KUNIT_EXPECT_GT(test, bytes, 0U);
		KUNIT_EXPECT_LT(test, bytes, (unsigned int)PAGE_SIZE);
	}
	KUNIT_ASSERT_EQ(test, mc16_payload_charge(ref, &f->prepared), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, storage->owner->folio), 0);
	mc16_payload_finish(ref, &f->tail);
	if (writeback) {
		mutex_lock(memcomp_lock_of(ref->token));
		mc16_view_locked(ref, &view);
		*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_WRITEBACK;
		mc16_fifo_transition(ref, MC16_STORED, MC16_WRITEBACK);
		mutex_unlock(memcomp_lock_of(ref->token));
	}
	memset(dst, 0, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, mc16_load_page(ref, dst), 0);
	KUNIT_EXPECT_EQ(test, memcmp(src, dst, PAGE_SIZE), 0);

	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	*view.flags |= MC16_F_REFRESH_PENDING;
	mutex_unlock(memcomp_lock_of(ref->token));
	memset(dst, 0x5a, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, mc16_load_page(ref, dst), -EAGAIN);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(dst, 0x5a, PAGE_SIZE), NULL);
}

static void memcomp_test_load_raw(struct kunit *test)
{
	mc16_test_load_payload(test, true, false);
}

static void memcomp_test_load_compressed(struct kunit *test)
{
	mc16_test_load_payload(test, false, false);
}

static void memcomp_test_load_writeback(struct kunit *test)
{
	mc16_test_load_payload(test, true, true);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_LOAD_TEST_H */
