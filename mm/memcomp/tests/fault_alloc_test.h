/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_FAULT_ALLOC_TEST_H
#define _MM_MEMCOMP_TESTS_FAULT_ALLOC_TEST_H

#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>

#include "../fault_alloc.h"

static void memcomp_test_fault_alloc_single_page(struct kunit *test)
{
	struct vm_area_struct vma = { };

	KUNIT_EXPECT_EQ(test, memcomp_fault_alloc_gfp(&vma, 1),
			GFP_HIGHUSER_MOVABLE);
	vm_flags_init(&vma, VM_HUGEPAGE);
	KUNIT_EXPECT_EQ(test, memcomp_fault_alloc_gfp(&vma, 1),
			GFP_HIGHUSER_MOVABLE);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/* No assertions or allocations may interrupt the save/restore interval. */
static gfp_t
memcomp_test_fault_alloc_with_policy(struct vm_area_struct *vma,
				     unsigned int nr_pages, unsigned long policy)
{
	const unsigned long defrag_mask =
		BIT(TRANSPARENT_HUGEPAGE_DEFRAG_DIRECT_FLAG) |
		BIT(TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_FLAG) |
		BIT(TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_OR_MADV_FLAG) |
		BIT(TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG);
	unsigned long saved = READ_ONCE(transparent_hugepage_flags);
	gfp_t gfp;

	WRITE_ONCE(transparent_hugepage_flags, (saved & ~defrag_mask) | policy);
	gfp = memcomp_fault_alloc_gfp(vma, nr_pages);
	WRITE_ONCE(transparent_hugepage_flags, saved);

	return gfp;
}
#endif

static void memcomp_test_fault_alloc_defrag_policy(struct kunit *test)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	/* Expected reclaim/retry behavior, for ordinary and madvised VMAs. */
	static const struct {
		unsigned long policy;
		gfp_t behavior[2];
	} cases[] = {
		{ 0, { 0, 0 } }, /* never */
		{ BIT(TRANSPARENT_HUGEPAGE_DEFRAG_DIRECT_FLAG),
		  { __GFP_DIRECT_RECLAIM | __GFP_NORETRY,
		    __GFP_DIRECT_RECLAIM } }, /* always */
		{ BIT(TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_FLAG),
		  { __GFP_KSWAPD_RECLAIM, __GFP_KSWAPD_RECLAIM } }, /* defer */
		/* defer+madvise */
		{ BIT(TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_OR_MADV_FLAG),
		  { __GFP_KSWAPD_RECLAIM, __GFP_DIRECT_RECLAIM } },
		/* madvise */
		{ BIT(TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG),
		  { 0, __GFP_DIRECT_RECLAIM } },
	};
	const gfp_t behavior_mask = __GFP_RECLAIM | __GFP_NORETRY |
				    __GFP_RETRY_MAYFAIL | __GFP_NOFAIL;
	const gfp_t required = __GFP_COMP | __GFP_NOMEMALLOC | __GFP_NOWARN;
	struct vm_area_struct vma = { };
	unsigned int i, advised;
	gfp_t gfp;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		for (advised = 0; advised < 2; advised++) {
			vm_flags_init(&vma, advised ? VM_HUGEPAGE : 0);
			/* The historical regression used an order-four span. */
			gfp = memcomp_test_fault_alloc_with_policy(&vma, 16,
								   cases[i].policy);
			KUNIT_EXPECT_EQ_MSG(test, gfp & behavior_mask,
					    cases[i].behavior[advised],
					    "policy %u advised %u", i, advised);
			KUNIT_EXPECT_EQ(test, gfp & required, required);
			KUNIT_EXPECT_EQ(test, gfp & ~behavior_mask,
					GFP_TRANSHUGE_LIGHT & ~behavior_mask);
			gfp = memcomp_test_fault_alloc_with_policy(&vma, 1,
								   cases[i].policy);
			KUNIT_EXPECT_EQ(test, gfp, GFP_HIGHUSER_MOVABLE);
		}
	}
#else
	kunit_skip(test, "THP defrag requires CONFIG_TRANSPARENT_HUGEPAGE");
#endif
}

static void memcomp_test_fault_alloc_single_fallback(struct kunit *test)
{
	struct vm_area_struct vma = { };
	gfp_t large, single;

	vm_flags_init(&vma, VM_HUGEPAGE);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	/* MADV_HUGEPAGE obeys never; order-0 fallback permits reclaim. */
	large = memcomp_test_fault_alloc_with_policy(&vma, 16, 0);
	single = memcomp_test_fault_alloc_with_policy(&vma, 1, 0);
	KUNIT_EXPECT_EQ(test, large & __GFP_RECLAIM, (gfp_t)0);
#else
	large = memcomp_fault_alloc_gfp(&vma, 16);
	single = memcomp_fault_alloc_gfp(&vma, 1);
	KUNIT_EXPECT_EQ(test, large,
			GFP_HIGHUSER_MOVABLE | __GFP_NORETRY | __GFP_NOWARN);
#endif
	KUNIT_EXPECT_EQ(test, single, GFP_HIGHUSER_MOVABLE);
	KUNIT_EXPECT_NE(test, large, single);
	KUNIT_EXPECT_EQ(test, single & __GFP_RECLAIM,
			GFP_HIGHUSER_MOVABLE & __GFP_RECLAIM);
	KUNIT_EXPECT_EQ(test, single & (__GFP_COMP | __GFP_NOMEMALLOC |
		__GFP_NOWARN | __GFP_NORETRY), (gfp_t)0);
}

#endif /* _MM_MEMCOMP_TESTS_FAULT_ALLOC_TEST_H */
