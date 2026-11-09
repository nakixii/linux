/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ZERO_CALLERS_TEST_H
#define _MM_MEMCOMP_TESTS_ZERO_CALLERS_TEST_H

#include <kunit/resource.h>
#include <kunit/test.h>
#include <linux/highmem.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>

/*
 * Include in suite.h AFTER BOTH zero_load_test.h and
 * writeback_batch_test.h. The first supplies real MM actors;
 * under MEMCG the second supplies an unlocked, real zs/accounting sentinel.
 * Public APIs and stat_show() must already be visible from memcomp.c.
 *
 * Register ALL cases under MC16_ZERO_CALLERS_TEST_ENABLED, outside any
 * CONFIG_MEMCG-only block. It requires MEMCOMP_KUNIT_TEST, MMU, SWAP and
 * ADVISE_SYSCALLS, and supports MEMCG=n/y. Optional PTE protection bits skip
 * only their corresponding cases when the architecture/config lacks them.
 *
 * Requires zero-callers.patch plus the transport-release fix in live source.
 * get(0) adds a PTE owner; get_ref(0)/cache pin(0) reject with an empty ref.
 * Cache zero uses memcomp_load_folio_zero() and a later explicit cache drop;
 * there is no sentinel guard. No fallback public dispatch is supplied here.
 * Public get/put actors model fork/zap ownership under a PTL. Fault actors use
 * real MM/PTE/rmap/memcg through memcomp_load(), not the internal zero loader.
 */
#if defined(MC16_ZERO_LOAD_MM_TEST_ENABLED) && defined(CONFIG_SWAP)
#define MC16_ZERO_CALLERS_TEST_ENABLED

struct mc16_test_zcall {
	struct kunit *test;
	struct mc16_zl_mm watch;
	struct mc16_zero_owners owners;
	struct memcomp_ref guard;
	unsigned long unexpected_cache[2];
	struct folio *transport;
	struct folio *large;
	char *stat;
#ifdef CONFIG_MEMCG
	struct mc16_test_wbb *wb;
#endif
};

/*
 * Tests own a private delta in a quiescent ledger. Unwind that actual delta
 * after an early assertion, even if a broken public API failed to account.
 * Negative deltas are failures too; repair them so later cases stay isolated.
 * No production reset, global atomic, or inherited test-task lock is used.
 */
static void mc16_test_zcall_cleanup(void *data)
{
	struct mc16_test_zcall *f = data;
	struct mc16_zero_owners after;
	s64 pte, cache;
	unsigned int i;

	memcomp_load_folio_abort(&f->guard);
	for (i = 0; i < ARRAY_SIZE(f->unexpected_cache); i++) {
		if (f->unexpected_cache[i])
			memcomp_invalidate_cache(f->unexpected_cache[i]);
	}
	mc16_zero_owners_read(&after);
	pte = after.pte_pages - f->owners.pte_pages;
	cache = after.cache_pages - f->owners.cache_pages;
	KUNIT_EXPECT_GE(f->test, pte, 0LL);
	KUNIT_EXPECT_GE(f->test, cache, 0LL);
	if (pte > 0)
		mc16_zero_pte_put(pte);
	else if (pte < 0)
		mc16_zero_pte_init(-pte);
	if (cache > 0)
		mc16_zero_cache_put(cache);
	else if (cache < 0)
		mc16_zero_cache_get(-cache);
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(f->test, after.pte_pages, f->owners.pte_pages);
	KUNIT_EXPECT_EQ(f->test, after.cache_pages, f->owners.cache_pages);
	mc16_zl_storage_unchanged(&f->watch);
	if (f->large) {
		KUNIT_EXPECT_FALSE(f->test, folio_test_locked(f->large));
		f->large->swap.val = 0;
		folio_put(f->large);
	}
	if (f->transport) {
		KUNIT_EXPECT_FALSE(f->test, folio_test_locked(f->transport));
		f->transport->swap.val = 0;
		folio_put(f->transport);
	}
	if (f->stat)
		free_page((unsigned long)f->stat);
}

static struct mc16_test_zcall *mc16_test_zcall_begin(struct kunit *test)
{
	struct mc16_test_zcall *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->watch.test = test;
#ifdef CONFIG_MEMCG
	/* This fixture returns its source folio unlocked, including on error. */
	f->wb = mc16_test_wbb_begin(test, false);
	if (!f->wb)
		return NULL;
	f->watch.refs = f->wb->refs;
#endif
	mc16_zero_owners_read(&f->owners);
	mc16_zl_storage_read(&f->watch, &f->watch.storage);
	if (kunit_add_action_or_reset(test, mc16_test_zcall_cleanup, f))
		return NULL;
	return f;
}

static void mc16_test_zcall_counts(struct mc16_test_zcall *f, s64 pte, s64 cache)
{
	struct mc16_zero_owners after;

	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(f->test, after.pte_pages, f->owners.pte_pages + pte);
	KUNIT_EXPECT_EQ(f->test, after.cache_pages, f->owners.cache_pages + cache);
	mc16_zl_storage_unchanged(&f->watch);
}

static void memcomp_test_zero_callers_fork_get(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	struct mc16_zero_diagnostics before, after;
	spinlock_t ptl; /* Models parent/child publication, not a fork syscall. */
	unsigned long flags;
	bool first, second;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_zero_diagnostics_read(&before);
	KUNIT_ASSERT_TRUE(test, mc16_zero_pte_init(1));
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(2));
	spin_lock_init(&ptl);
	spin_lock_irqsave(&ptl, flags);
	first = memcomp_entry_get(0);
	second = memcomp_entry_get(0);
	spin_unlock_irqrestore(&ptl, flags);
	KUNIT_EXPECT_TRUE(test, first);
	KUNIT_EXPECT_TRUE(test, second);
	mc16_test_zcall_counts(f, 3, 2);
	mc16_zero_diagnostics_read(&after);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, after.calls[MC16_ZERO_PTE_INIT] -
			before.calls[MC16_ZERO_PTE_INIT], 1ULL);
	KUNIT_EXPECT_EQ(test, after.pages[MC16_ZERO_PTE_GET] -
			before.pages[MC16_ZERO_PTE_GET], 2ULL);
	KUNIT_EXPECT_EQ(test, after.calls[MC16_ZERO_PTE_GET] -
			before.calls[MC16_ZERO_PTE_GET], 2ULL);
#endif
	memcomp_invalidate_nr(0, 3);
	memcomp_invalidate_cache(0);
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_put_domains(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	struct mc16_zero_diagnostics before, after, noop;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_zero_pte_init(5));
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(3));
	mc16_zero_diagnostics_read(&before);
	memcomp_invalidate(0);
	mc16_test_zcall_counts(f, 4, 3);
	memcomp_invalidate_nr(0, 3);
	mc16_test_zcall_counts(f, 1, 3);
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 1, 2);
	mc16_zero_diagnostics_read(&after);
	memcomp_invalidate_nr(0, 0);
	mc16_zero_diagnostics_read(&noop);
	KUNIT_EXPECT_EQ(test, memcmp(&after, &noop, sizeof(after)), 0);
	mc16_test_zcall_counts(f, 1, 2);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, after.calls[MC16_ZERO_PTE_PUT] -
			before.calls[MC16_ZERO_PTE_PUT], 2ULL);
	KUNIT_EXPECT_EQ(test, after.pages[MC16_ZERO_PTE_PUT] -
			before.pages[MC16_ZERO_PTE_PUT], 4ULL);
	KUNIT_EXPECT_EQ(test, after.pages[MC16_ZERO_CACHE_PUT] -
			before.pages[MC16_ZERO_CACHE_PUT], 1ULL);
#endif
	memcomp_invalidate(0);
	memcomp_invalidate_cache(0);
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_put_bounds(struct kunit *test)
{
	static const unsigned int sizes[] = { 1, 2, 511, 512, 513, UINT_MAX };
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(1));
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		unsigned int nr = sizes[i];

		/* On 32-bit, UINT_MAX exceeds the zero ledger's signed range. */
		if (!mc16_zero_nr_valid(nr))
			continue;
		KUNIT_ASSERT_TRUE(test, mc16_zero_pte_init(nr));
		mc16_test_zcall_counts(f, nr, 1);
		memcomp_invalidate_nr(0, nr);
		mc16_test_zcall_counts(f, 0, 1);
	}
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_query(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	spinlock_t ptl; /* task_mmu/shmem-style query must not sleep under PTL. */
	unsigned long flags, base = ULONG_MAX, compressed = PAGE_SIZE;
	int mapcount = -1;
	unsigned int nr;
	bool present, stat;

	KUNIT_ASSERT_NOT_NULL(test, f);
	spin_lock_init(&ptl);
	spin_lock_irqsave(&ptl, flags);
	present = memcomp_entry_present(0);
	stat = memcomp_entry_stat(0, NULL, &mapcount);
	nr = memcomp_entry_nr_pages(0, &base);
	spin_unlock_irqrestore(&ptl, flags);
	KUNIT_EXPECT_TRUE(test, present);
	KUNIT_EXPECT_TRUE(test, stat);
	KUNIT_EXPECT_EQ(test, mapcount, 1);
	KUNIT_EXPECT_EQ(test, nr, 1U);
	KUNIT_EXPECT_EQ(test, base, 0UL);
	KUNIT_EXPECT_EQ(test, memcomp_entry_nr_pages(0, NULL), 1U);
	KUNIT_EXPECT_TRUE(test, memcomp_entry_stat(0, &compressed, &mapcount));
	KUNIT_EXPECT_EQ(test, compressed, 0UL);
	KUNIT_EXPECT_EQ(test, mapcount, 1);
	KUNIT_EXPECT_TRUE(test, memcomp_entry_stat(0, NULL, NULL));
	KUNIT_EXPECT_TRUE(test, memcomp_entry_stat(0, &compressed, NULL));
	KUNIT_EXPECT_FALSE(test, memcomp_entry_get_ref(0, &f->guard));
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, NULL);
	KUNIT_EXPECT_FALSE(test, memcomp_load_folio_pin(0, &f->guard));
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, NULL);
	memcomp_load_folio_abort(&f->guard);
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_stat(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	unsigned long long stored, compressed, pool;
	u64 expected, pool_before;
	ssize_t len;
	int fields;

	KUNIT_ASSERT_NOT_NULL(test, f);
	/* sysfs_emit requires a page-aligned buffer. */
	f->stat = (char *)get_zeroed_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->stat);
	KUNIT_ASSERT_TRUE(test, mc16_zero_pte_init(2));
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(3));
	expected = mc16_zero_stored_bytes(atomic64_read(&memcomp_stored_bytes));
	pool_before = memcomp_pool_total_size();
	len = stat_show(NULL, NULL, f->stat);
	KUNIT_ASSERT_GT(test, len, (ssize_t)0);
	fields = sscanf(f->stat, "%llu %llu %llu", &stored, &compressed, &pool);
	KUNIT_ASSERT_EQ(test, fields, 3);
	KUNIT_EXPECT_EQ(test, (u64)stored, expected);
	KUNIT_EXPECT_EQ(test, (u64)compressed, (u64)f->watch.storage.compressed);
	KUNIT_EXPECT_EQ(test, (u64)pool, pool_before);
	mc16_test_zcall_counts(f, 2, 3);
	memcomp_invalidate_nr(0, 2);
	memcomp_invalidate_cache(0);
	memcomp_invalidate_cache(0);
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 0, 0);
}

/* Public batch accepts an encoded PTE array; no mapping or owners are made. */
static unsigned int mc16_test_zcall_batch(pte_t *ptes, unsigned int nr)
{
	spinlock_t ptl; /* Serializes reads of the encoded PTE array. */
	unsigned long flags;
	unsigned int result;

	spin_lock_init(&ptl);
	spin_lock_irqsave(&ptl, flags);
	result = memcomp_pte_batch(ptes, nr);
	spin_unlock_irqrestore(&ptl, flags);
	return result;
}

static void memcomp_test_zero_callers_batch_zero(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	pte_t ptes[6];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < ARRAY_SIZE(ptes); i++)
		ptes[i] = mc16_zl_zero_pte();
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(NULL, 0), 0U);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 6), 6U);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 1), 1U);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 3), 3U);
	ptes[2] = swp_entry_to_pte(make_memcomp_entry(1));
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 6), 2U);
	ptes[2] = swp_entry_to_pte(swp_entry(0, 7));
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 6), 2U);
	ptes[2] = __pte(0);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 6), 2U);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(&ptes[2], 1), 0U);
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_batch_positive(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	pte_t ptes[4];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < ARRAY_SIZE(ptes); i++)
		ptes[i] = swp_entry_to_pte(make_memcomp_entry(100 + i));
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 4), 4U);
	ptes[2] = mc16_zl_zero_pte();
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 4), 2U);
	ptes[1] = ptes[0];
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 4), 1U);
	mc16_test_zcall_counts(f, 0, 0);
}

static void mc16_test_zcall_batch_protection(struct kunit *test, unsigned int bit)
{
	struct mc16_test_zcall *f;
	pte_t plain = mc16_zl_zero_pte(), protected;
	pte_t ptes[4];
	unsigned int i;

	if (!bit)
		protected = pte_swp_mkexclusive(plain);
	else if (bit == 1)
		protected = pte_swp_mksoft_dirty(plain);
	else
		protected = pte_swp_mkuffd_wp(plain);
	if (pte_same(plain, protected)) {
		kunit_skip(test, "requested swap protection bit is disabled");
		return;
	}
	f = mc16_test_zcall_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < ARRAY_SIZE(ptes); i++)
		ptes[i] = plain;
	ptes[2] = protected;
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 4), 2U);
	for (i = 0; i < ARRAY_SIZE(ptes); i++)
		ptes[i] = protected;
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 4), 4U);
	ptes[2] = plain;
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_batch(ptes, 4), 2U);
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_batch_exclusive(struct kunit *test)
{
	mc16_test_zcall_batch_protection(test, 0);
}

static void memcomp_test_zero_callers_batch_soft_dirty(struct kunit *test)
{
	mc16_test_zcall_batch_protection(test, 1);
}

static void memcomp_test_zero_callers_batch_uffd_wp(struct kunit *test)
{
	mc16_test_zcall_batch_protection(test, 2);
}

static int mc16_test_zcall_cache_load(struct folio *folio, bool lock)
{
	int ret;

	if (lock)
		folio_lock(folio);
	ret = memcomp_load_folio_zero(folio);
	if (lock)
		folio_unlock(folio);
	return ret;
}

static bool mc16_test_zcall_bytes(struct folio *folio, u8 value)
{
	void *data = kmap_local_folio(folio, 0);
	bool same = !memchr_inv(data, value, PAGE_SIZE);

	kunmap_local(data);
	return same;
}

static void mc16_test_zcall_fill(struct folio *folio)
{
	void *data = kmap_local_folio(folio, 0);

	memset(data, 0xa7, PAGE_SIZE);
	kunmap_local(data);
	folio_clear_uptodate(folio);
}

/* Public preparation owns nothing; explicit publication owns one cache item. */
static void memcomp_test_zero_callers_cache_publish(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	unsigned long token = ULONG_MAX;
	unsigned int i;
	bool prepared;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->transport = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->transport);
	/* Two preparations model a discarded result and the eventual winner. */
	for (i = 0; i < 2; i++) {
		folio_lock(f->transport);
		prepared = memcomp_store_cache(f->transport, &token);
		folio_unlock(f->transport);
		/* Register each actual owner before an assertion or overwrite. */
		if (prepared && token)
			f->unexpected_cache[i] = token;
		KUNIT_ASSERT_TRUE(test, prepared);
		KUNIT_EXPECT_EQ(test, token, 0UL);
		mc16_test_zcall_counts(f, 0, 0);
	}
	/* The caller models publication; this is not a shmem XA-race test. */
	memcomp_publish_cache_zero();
	mc16_test_zcall_counts(f, 0, 1);
	KUNIT_EXPECT_FALSE(test, memcomp_load_folio_pin(token, &f->guard));
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	mc16_test_zcall_fill(f->transport);
	KUNIT_ASSERT_EQ(test, mc16_test_zcall_cache_load(f->transport, true), 0);
	KUNIT_EXPECT_TRUE(test, mc16_test_zcall_bytes(f->transport, 0));
	mc16_test_zcall_counts(f, 0, 1);
	memcomp_load_folio_abort(&f->guard);
	mc16_test_zcall_counts(f, 0, 1);
	memcomp_invalidate_cache(token);
	if (token)
		f->unexpected_cache[1] = 0;
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_cache_load(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->transport = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->transport);
	f->transport->swap.val = 0;
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(1));
	KUNIT_ASSERT_TRUE(test, mc16_zero_pte_init(2));
	KUNIT_EXPECT_FALSE(test, memcomp_load_folio_pin(0, &f->guard));
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, NULL);
	/* First xa replacement loses: retain the same logical cache owner. */
	for (i = 0; i < 2; i++) {
		mc16_test_zcall_fill(f->transport);
		KUNIT_ASSERT_EQ(test, mc16_test_zcall_cache_load(f->transport, true), 0);
		KUNIT_EXPECT_TRUE(test, folio_test_uptodate(f->transport));
		KUNIT_EXPECT_TRUE(test, mc16_test_zcall_bytes(f->transport, 0));
		KUNIT_EXPECT_EQ(test, folio_ref_count(f->transport), 1);
		KUNIT_EXPECT_EQ(test, f->transport->swap.val, 0UL);
		mc16_test_zcall_counts(f, 2, 1);
		memcomp_load_folio_abort(&f->guard);
		mc16_test_zcall_counts(f, 2, 1);
	}
	/* Model the winner's explicit cache drop only after replacing its xa. */
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 2, 0);
	memcomp_invalidate_nr(0, 2);
	mc16_test_zcall_counts(f, 0, 0);
}

static void memcomp_test_zero_callers_cache_invalid(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->transport = folio_alloc(GFP_KERNEL, 0);
	f->large = folio_alloc(GFP_KERNEL, 1);
	KUNIT_ASSERT_NOT_NULL(test, f->transport);
	KUNIT_ASSERT_NOT_NULL(test, f->large);
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(1));
	mc16_test_zcall_fill(f->transport);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_cache_load(NULL, false), -EINVAL);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_cache_load(f->transport, false), -EINVAL);
	KUNIT_EXPECT_EQ(test, mc16_test_zcall_cache_load(f->large, true), -EINVAL);
	KUNIT_EXPECT_FALSE(test, folio_test_uptodate(f->transport));
	KUNIT_EXPECT_TRUE(test, mc16_test_zcall_bytes(f->transport, 0xa7));
	mc16_test_zcall_counts(f, 0, 1);
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 0, 0);
}

/* Pair a successful freeze with unfreeze before returning to assertions. */
static bool mc16_test_zcall_release(struct folio *folio, bool lock)
{
	bool released;

	if (lock)
		folio_lock(folio);
	released = memcomp_release_folio(folio);
	if (released)
		folio_ref_unfreeze(folio, 1);
	if (lock)
		folio_unlock(folio);
	return released;
}

static void memcomp_test_zero_callers_release(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->transport = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->transport);
	KUNIT_ASSERT_TRUE(test, mc16_zero_pte_init(1));
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(1));
	f->transport->swap = make_memcomp_entry(0);
	KUNIT_ASSERT_FALSE(test, folio_test_anon(f->transport));
	KUNIT_ASSERT_PTR_EQ(test, f->transport->mapping, NULL);
	KUNIT_ASSERT_TRUE(test, folio_memcomp_reserved(f->transport));
	KUNIT_EXPECT_FALSE(test, mc16_test_zcall_release(f->transport, false));
	KUNIT_EXPECT_TRUE(test, folio_memcomp_reserved(f->transport));
	folio_get(f->transport);
	KUNIT_EXPECT_FALSE(test, mc16_test_zcall_release(f->transport, true));
	folio_put(f->transport);
	KUNIT_EXPECT_TRUE(test, folio_memcomp_reserved(f->transport));
	KUNIT_EXPECT_TRUE(test, mc16_test_zcall_release(f->transport, true));
	KUNIT_EXPECT_EQ(test, f->transport->swap.val, 0UL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->transport), 1);
	mc16_test_zcall_counts(f, 1, 1);
	memcomp_invalidate(0);
	memcomp_invalidate_cache(0);
	mc16_test_zcall_counts(f, 0, 0);
}

/* Only the eligibility predicate sees this unlinked mapping, under the lock. */
static bool mc16_test_zcall_release_mapped(struct folio *folio,
					   struct address_space *mapping)
{
	bool released;

	folio_lock(folio);
	folio->mapping = mapping;
	released = memcomp_release_folio(folio);
	if (released)
		folio_ref_unfreeze(folio, 1);
	folio->mapping = NULL;
	folio_unlock(folio);
	return released;
}

static void memcomp_test_zero_callers_release_bounds(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	struct address_space mapping = { };

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->transport = folio_alloc(GFP_KERNEL, 0);
	f->large = folio_alloc(GFP_KERNEL, 1);
	KUNIT_ASSERT_NOT_NULL(test, f->transport);
	KUNIT_ASSERT_NOT_NULL(test, f->large);
	f->transport->swap = make_memcomp_entry(0);
	f->large->swap = make_memcomp_entry(0);
	KUNIT_EXPECT_FALSE(test, mc16_test_zcall_release_mapped(f->transport, &mapping));
	KUNIT_EXPECT_TRUE(test, folio_memcomp_reserved(f->transport));
	KUNIT_EXPECT_FALSE(test, mc16_test_zcall_release(f->large, true));
	KUNIT_EXPECT_TRUE(test, folio_memcomp_reserved(f->large));
	/* No marker is left on a folio returned to the allocator by cleanup. */
	f->large->swap.val = 0;
	mc16_test_zcall_counts(f, 0, 0);
}

/* Use the existing MM actors with an independently unlocked WB sentinel. */
static struct mc16_test_zcall *mc16_test_zcall_mm(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_begin(test);
	struct mc16_zl_mm *m;
	struct vm_area_struct *vma;

	if (!f)
		return NULL;
	m = &f->watch;
	m->addr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (!m->addr || IS_ERR_VALUE(m->addr))
		return NULL;
	m->mm = current->mm;
	mmget(m->mm);
	mc16_zero_owners_read(&m->owners);
	m->swapents = mc16_zl_rss(m->mm, MM_SWAPENTS);
	m->anonpages = mc16_zl_rss(m->mm, MM_ANONPAGES);
#ifdef CONFIG_PER_VMA_LOCK
	init_completion(&m->writer_locked);
	init_completion(&m->writer_release);
#endif
	if (kunit_add_action_or_reset(test, mc16_zl_cleanup, m))
		return NULL;
	mmap_read_lock(m->mm);
	vma = vma_lookup(m->mm, m->addr);
	if (vma) {
		m->flags = vma->vm_flags;
		m->prot = vma->vm_page_prot;
		m->saved_vma = true;
	}
	mmap_read_unlock(m->mm);
	return vma ? f : NULL;
}

static vm_fault_t mc16_test_zcall_fault(struct mc16_test_zcall *f, pte_t orig,
					unsigned int flags, bool *locked, pte_t **left)
{
	struct mc16_zl_mm *m = &f->watch;
	struct vm_area_struct *vma;
	vm_fault_t ret = VM_FAULT_SIGBUS;

	mmap_read_lock(m->mm);
	vma = vma_lookup(m->mm, m->addr);
	if (vma) {
		struct vm_fault vmf = {
			.vma = vma,
			.address = m->addr,
			.real_address = m->addr,
			.flags = flags | FAULT_FLAG_ORIG_PTE_VALID,
			.pmd = mm_find_pmd(m->mm, m->addr),
			.orig_pte = orig,
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};

		ret = memcomp_load(&vmf, locked);
		*left = vmf.pte;
	}
	mmap_read_unlock(m->mm);
	return ret;
}

static void mc16_test_zcall_load(struct kunit *test, bool write)
{
	struct mc16_test_zcall *f = mc16_test_zcall_mm(test);
	pte_t orig = pte_swp_mksoft_dirty(mc16_zl_zero_pte()), pte;
	pte_t *left = NULL;
	bool locked = true;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(&f->watch, orig), 0);
	ret = mc16_test_zcall_fault(f, orig, write ? FAULT_FLAG_WRITE : 0, &locked, &left);
	KUNIT_EXPECT_FALSE(test, locked);
	KUNIT_EXPECT_PTR_EQ(test, left, NULL);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	pte = mc16_zl_read(&f->watch);
	mc16_zl_present(&f->watch, pte);
	KUNIT_EXPECT_TRUE(test, pte_write(pte));
	/* A writable read PTE may encode hardware dirty on arm64. */
	if (write)
		KUNIT_EXPECT_TRUE(test, pte_dirty(pte));
	KUNIT_EXPECT_EQ(test, !!pte_soft_dirty(pte), !!pte_swp_soft_dirty(orig));
	/* A stale second public fault must not consume the same owner twice. */
	locked = true;
	ret = mc16_test_zcall_fault(f, orig, 0, &locked, &left);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret, 0U);
	KUNIT_EXPECT_FALSE(test, locked);
	KUNIT_EXPECT_PTR_EQ(test, left, NULL);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_zl_read(&f->watch), pte));
	mc16_zl_counts(&f->watch, 0, 1);
}

static void memcomp_test_zero_callers_load_read(struct kunit *test)
{
	mc16_test_zcall_load(test, false);
}

static void memcomp_test_zero_callers_load_write(struct kunit *test)
{
	mc16_test_zcall_load(test, true);
}

static void memcomp_test_zero_callers_load_oom(struct kunit *test)
{
	struct mc16_test_zcall *f = mc16_test_zcall_mm(test);
	pte_t orig = mc16_zl_zero_pte();
	pte_t *left = NULL;
	bool locked = true;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(&f->watch, orig), 0);
	mc16_zero_fault_probe.fail_alloc = true;
	WRITE_ONCE(mc16_zero_fault_probe.task, current);
	ret = mc16_test_zcall_fault(f, orig, 0, &locked, &left);
	WRITE_ONCE(mc16_zero_fault_probe.task, NULL);
	mc16_zero_fault_probe.fail_alloc = false;
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_OOM);
	KUNIT_EXPECT_FALSE(test, locked);
	KUNIT_EXPECT_PTR_EQ(test, left, NULL);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_zl_read(&f->watch), orig));
	mc16_zl_counts(&f->watch, 1, 0);
}
#endif /* MC16_ZERO_LOAD_MM_TEST_ENABLED && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_ZERO_CALLERS_TEST_H */
