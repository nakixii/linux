/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_TRANSPORT_TEST_H
#define _MM_MEMCOMP_TESTS_TRANSPORT_TEST_H

#include <linux/bitmap.h>

#ifdef CONFIG_SWAP
static void mc16_test_transport_cleanup(void *data)
{
	swap_memcomp_transport_free(data);
}

/* Exercise the same allocation and bit operations as swapon/reclaim/free. */
static struct swap_info_struct *
mc16_test_transport_info(struct kunit *test, unsigned int max)
{
	struct swap_info_struct *si;

	si = kunit_kzalloc(test, sizeof(*si), GFP_KERNEL);
	if (!si)
		return NULL;
	si->max = max;
	if (kunit_add_action_or_reset(test, mc16_test_transport_cleanup, si))
		return NULL;
	if (swap_memcomp_transport_alloc(si))
		return NULL;
	return si;
}

static void memcomp_test_transport_bitmap_lifecycle(struct kunit *test)
{
	struct swap_info_struct *si;
	unsigned long last = BITS_PER_LONG + 2;

	si = mc16_test_transport_info(test, last + 1);
	KUNIT_ASSERT_NOT_NULL(test, si);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(si->memcomp_transport, si->max));
	swap_memcomp_transport_set(si, 1);
	swap_memcomp_transport_set(si, last);
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(si, 1));
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(si, last));
	KUNIT_EXPECT_FALSE(test, swap_memcomp_transport_test(si, 2));

	swap_memcomp_transport_clear(si, 1, 1);
	KUNIT_EXPECT_FALSE(test, swap_memcomp_transport_test(si, 1));
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(si, last));
	/* A slot returned unused must not mark its next, ordinary owner. */
	swap_memcomp_transport_set(si, 1);
	swap_memcomp_transport_clear(si, 1, 1);
	KUNIT_EXPECT_FALSE(test, swap_memcomp_transport_test(si, 1));

	/* Teardown and reactivation of the same device start with an empty map. */
	swap_memcomp_transport_free(si);
	KUNIT_EXPECT_PTR_EQ(test, si->memcomp_transport, NULL);
	KUNIT_ASSERT_EQ(test, swap_memcomp_transport_alloc(si), 0);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(si->memcomp_transport, si->max));
}

static void memcomp_test_transport_bitmap_range(struct kunit *test)
{
	struct swap_info_struct *si;
	unsigned long first = BITS_PER_LONG - 1;
	unsigned int i;

	si = mc16_test_transport_info(test, 2 * BITS_PER_LONG + 5);
	KUNIT_ASSERT_NOT_NULL(test, si);
	swap_memcomp_transport_set(si, first - 1);
	for (i = 0; i < 4; i++)
		swap_memcomp_transport_set(si, first + i);
	swap_memcomp_transport_set(si, first + 4);
	swap_memcomp_transport_clear(si, first, 0);
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(si, first));
	swap_memcomp_transport_clear(si, first, 4);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_FALSE(test, swap_memcomp_transport_test(si, first + i));
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(si, first - 1));
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(si, first + 4));
}

static void memcomp_test_transport_bitmap_devices(struct kunit *test)
{
	struct swap_info_struct *a, *b;

	a = mc16_test_transport_info(test, BITS_PER_LONG + 1);
	KUNIT_ASSERT_NOT_NULL(test, a);
	b = mc16_test_transport_info(test, BITS_PER_LONG + 1);
	KUNIT_ASSERT_NOT_NULL(test, b);
	swap_memcomp_transport_set(a, BITS_PER_LONG);
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(a, BITS_PER_LONG));
	KUNIT_EXPECT_FALSE(test, swap_memcomp_transport_test(b, BITS_PER_LONG));
	swap_memcomp_transport_set(b, BITS_PER_LONG);
	swap_memcomp_transport_clear(a, BITS_PER_LONG, 1);
	KUNIT_EXPECT_FALSE(test, swap_memcomp_transport_test(a, BITS_PER_LONG));
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(b, BITS_PER_LONG));
}

static void memcomp_test_transport_bitmap_budget(struct kunit *test)
{
	struct swap_info_struct *si;
	unsigned long bytes;

	/* 1 GiB / 4 KiB is 262144 offsets, independent of this kernel's PAGE_SIZE. */
	si = mc16_test_transport_info(test, 1U << 18);
	KUNIT_ASSERT_NOT_NULL(test, si);
	bytes = BITS_TO_LONGS((unsigned long)si->max) * sizeof(unsigned long);
	KUNIT_EXPECT_EQ(test, bytes, 32UL * 1024);
	swap_memcomp_transport_set(si, si->max - 1);
	KUNIT_EXPECT_TRUE(test, swap_memcomp_transport_test(si, si->max - 1));
	KUNIT_EXPECT_FALSE(test, swap_memcomp_transport_test(si, si->max - 2));
	swap_memcomp_transport_clear(si, si->max - 1, 1);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(si->memcomp_transport, si->max));
}
#endif /* CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_TRANSPORT_TEST_H */
