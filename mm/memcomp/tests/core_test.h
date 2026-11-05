/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_CORE_TEST_H
#define _MM_MEMCOMP_TESTS_CORE_TEST_H

#include "test_api.h"

#ifdef CONFIG_MEMCG
struct memcomp_test_owner {
	void *record;
	unsigned int remaining;
};

static void memcomp_test_cleanup(void *data)
{
	struct memcomp_test_owner *owner = data;

	while (owner->remaining) {
		owner->remaining--;
		memcomp_test_record_put(owner->record);
	}
	owner->record = NULL;
}

static struct memcomp_test_owner *
memcomp_test_new(struct kunit *test, struct folio *folio,
		 unsigned int nr_pages)
{
	struct memcomp_test_owner *owner;
	int ret;

	owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return NULL;
	owner->record = memcomp_test_record_alloc(folio, nr_pages);
	if (IS_ERR(owner->record)) {
		KUNIT_FAIL(test, "record allocation failed: %ld",
			   PTR_ERR(owner->record));
		return NULL;
	}
	owner->remaining = nr_pages;
	ret = kunit_add_action_or_reset(test, memcomp_test_cleanup,
					owner);
	return ret ? NULL : owner;
}

static void memcomp_test_record_members(struct kunit *test)
{
	struct memcomp_test_objcg_fixture *f;
	struct memcomp_test_owner *owner;
	struct memcomp_record_info info;
	spinlock_t lock; /* Models an atomic final-put caller. */
	unsigned long flags;

	f = memcomp_test_objcg_begin(test, 2);
	KUNIT_ASSERT_NOT_NULL(test, f);
	owner = memcomp_test_new(test, f->folio, 2);
	KUNIT_ASSERT_NOT_NULL(test, owner);
	info = memcomp_test_record_info(owner->record);
	KUNIT_EXPECT_EQ(test, info.members, 2U);
	KUNIT_EXPECT_EQ(test, info.charged_bytes, info.allocated_bytes);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged),
			(long)info.charged_bytes);

	owner->remaining--;
	memcomp_test_record_put(owner->record);
	info = memcomp_test_record_info(owner->record);
	KUNIT_EXPECT_EQ(test, info.members, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.put_calls), 0);

	/* Last member release must be valid in the atomic final-put context. */
	spin_lock_init(&lock);
	spin_lock_irqsave(&lock, flags);
	owner->remaining--;
	memcomp_test_record_put(owner->record);
	owner->record = NULL;
	spin_unlock_irqrestore(&lock, flags);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.get_calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.put_calls), 1);
}

static void memcomp_test_record_abort(struct kunit *test)
{
	struct memcomp_test_objcg_fixture *f;
	struct memcomp_test_owner *owner;

	f = memcomp_test_objcg_begin(test, 2);
	KUNIT_ASSERT_NOT_NULL(test, f);
	owner = memcomp_test_new(test, f->folio, 2);
	KUNIT_ASSERT_NOT_NULL(test, owner);
	owner->remaining = 0;
	memcomp_test_record_handle_abort(owner->record);
	owner->record = NULL;
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.get_calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.put_calls), 1);
}

static void memcomp_test_record_charge_failure(struct kunit *test)
{
	struct memcomp_test_objcg_fixture *f;
	void *record;

	f = memcomp_test_objcg_begin(test, 1);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->model.fail_at = 1;
	record = memcomp_test_record_alloc(f->folio, 1);
	if (!IS_ERR(record)) {
		memcomp_test_record_handle_abort(record);
		KUNIT_FAIL(test, "record metadata charge unexpectedly succeeded");
		return;
	}
	KUNIT_EXPECT_EQ(test, PTR_ERR(record), (long)-ENOMEM);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.get_calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.put_calls), 1);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_CORE_TEST_H */
