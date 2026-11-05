/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ZERO_TEST_H
#define _MM_MEMCOMP_TESTS_ZERO_TEST_H

#include <kunit/test.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/slab.h>

#include "../zero.h"

/*
 * Registration belongs to the enclosing memcomp KUnit suite. The MEMCG case
 * needs the existing storage/refs fixtures included first. No public wrapper,
 * fault, fork or rmap wiring is supplied or validated by these helper tests.
 */
static void memcomp_test_zero_page(struct kunit *test)
{
	unsigned long pattern = 0x9876543210fedcbaUL;
	u8 *allocation, *page;
	unsigned int i;
	static const unsigned int positions[] = {
		0, 1, sizeof(unsigned long) - 1, sizeof(unsigned long),
		PAGE_SIZE / 2, PAGE_SIZE - 2, PAGE_SIZE - 1,
	};

	allocation = kunit_kzalloc(test, PAGE_SIZE + 2, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, allocation);
	page = allocation + 1; /* Deliberately unaligned; preserve both guards. */
	allocation[0] = 0xa5;
	page[PAGE_SIZE] = 0x5a;
	KUNIT_EXPECT_TRUE(test, mc16_zero_is_page(page, PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(NULL, PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(page, 0));
	KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(page, PAGE_SIZE - 1));
	KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(page, PAGE_SIZE + 1));
	KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(page, SIZE_MAX));
	for (i = 0; i < ARRAY_SIZE(positions); i++) {
		page[positions[i]] = 1;
		KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(page, PAGE_SIZE));
		page[positions[i]] = 0;
	}
	/* A full repeated nonzero word is SAMEFILL, never slotless zero. */
	for (i = 0; i < PAGE_SIZE; i += sizeof(pattern))
		memcpy(page + i, &pattern, sizeof(pattern));
	KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(page, PAGE_SIZE));
	memset(page, 0xff, PAGE_SIZE);
	KUNIT_EXPECT_FALSE(test, mc16_zero_is_page(page, PAGE_SIZE));
	memset(page, 0, PAGE_SIZE);
	KUNIT_EXPECT_TRUE(test, mc16_zero_is_page(page, PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, allocation[0], (u8)0xa5);
	KUNIT_EXPECT_EQ(test, page[PAGE_SIZE], (u8)0x5a);
}

static void memcomp_test_zero_owners(struct kunit *test)
{
	struct mc16_zero_owners before, after;
	struct mc16_zero_diagnostics events, end;
	u64 pages = mc16_zero_pages();

	mc16_zero_owners_read(&before);
	mc16_zero_diagnostics_read(&events);
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_init(3));
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_get(2)); /* forked PTEs */
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_get(4)); /* xa owners */
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages + 5);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages + 4);
	KUNIT_EXPECT_EQ(test, mc16_zero_pages(), pages + 9);
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_put(2));
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_put(4));
	KUNIT_EXPECT_EQ(test, mc16_zero_pages(), pages + 3);
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_put(3));
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages);
	mc16_zero_diagnostics_read(&end);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, end.calls[MC16_ZERO_PTE_INIT] -
			events.calls[MC16_ZERO_PTE_INIT], 1ULL);
	KUNIT_EXPECT_EQ(test, end.pages[MC16_ZERO_PTE_INIT] -
			events.pages[MC16_ZERO_PTE_INIT], 3ULL);
	KUNIT_EXPECT_EQ(test, end.calls[MC16_ZERO_PTE_GET] -
			events.calls[MC16_ZERO_PTE_GET], 1ULL);
	KUNIT_EXPECT_EQ(test, end.pages[MC16_ZERO_PTE_GET] -
			events.pages[MC16_ZERO_PTE_GET], 2ULL);
	KUNIT_EXPECT_EQ(test, end.calls[MC16_ZERO_PTE_PUT] -
			events.calls[MC16_ZERO_PTE_PUT], 2ULL);
	KUNIT_EXPECT_EQ(test, end.pages[MC16_ZERO_PTE_PUT] -
			events.pages[MC16_ZERO_PTE_PUT], 5ULL);
	KUNIT_EXPECT_EQ(test, end.pages[MC16_ZERO_CACHE_GET] -
			events.pages[MC16_ZERO_CACHE_GET], 4ULL);
	KUNIT_EXPECT_EQ(test, end.pages[MC16_ZERO_CACHE_PUT] -
			events.pages[MC16_ZERO_CACHE_PUT], 4ULL);
#else
	KUNIT_EXPECT_EQ(test, memcmp(&events, &end, sizeof(end)), 0);
#endif
}

static void memcomp_test_zero_batch_bounds(struct kunit *test)
{
	static const unsigned long sizes[] = {
		1, 2, 511, 512, 513, UINT_MAX,
#if BITS_PER_LONG > 32
		1UL << 32,
#endif
	};
	struct mc16_zero_owners before, after;
	struct mc16_zero_diagnostics events, end;
	unsigned long invalid = (unsigned long)LONG_MAX + 1;
	unsigned int i;

	mc16_zero_owners_read(&before);
	mc16_zero_diagnostics_read(&events);
	KUNIT_EXPECT_TRUE(test, mc16_zero_nr_valid(LONG_MAX));
	KUNIT_EXPECT_FALSE(test, mc16_zero_nr_valid(invalid));
	KUNIT_EXPECT_FALSE(test, mc16_zero_nr_valid(ULONG_MAX));
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_init(0));
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_get(0));
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_put(0));
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_get(0));
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_put(0));
	KUNIT_EXPECT_FALSE(test, mc16_zero_pte_init(invalid));
	KUNIT_EXPECT_FALSE(test, mc16_zero_pte_get(invalid));
	KUNIT_EXPECT_FALSE(test, mc16_zero_pte_put(invalid));
	KUNIT_EXPECT_FALSE(test, mc16_zero_cache_get(invalid));
	KUNIT_EXPECT_FALSE(test, mc16_zero_cache_put(invalid));
	mc16_zero_diagnostics_read(&end);
	KUNIT_EXPECT_EQ(test, memcmp(&events, &end, sizeof(end)), 0);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		unsigned long nr = sizes[i];

		if (!mc16_zero_nr_valid(nr))
			continue;
		KUNIT_EXPECT_TRUE(test, mc16_zero_pte_init(nr));
		KUNIT_EXPECT_TRUE(test, mc16_zero_cache_get(nr));
		mc16_zero_owners_read(&after);
		KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages + (s64)nr);
		KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages + (s64)nr);
		KUNIT_EXPECT_TRUE(test, mc16_zero_pte_put(nr));
		KUNIT_EXPECT_TRUE(test, mc16_zero_cache_put(nr));
	}
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages);
}

static void memcomp_test_zero_stat(struct kunit *test)
{
	unsigned long compressed = PAGE_SIZE;
	int mapcount = 42;
	u64 pages = mc16_zero_pages();
	u64 base = 7 * (u64)PAGE_SIZE;
	s64 stored = atomic64_read(&memcomp_stored_bytes);
	s64 payload = atomic64_read(&memcomp_compressed_bytes);

	KUNIT_EXPECT_TRUE(test, mc16_zero_entry_stat(0, &compressed, &mapcount));
	KUNIT_EXPECT_EQ(test, compressed, 0UL);
	KUNIT_EXPECT_EQ(test, mapcount, 1);
	KUNIT_EXPECT_TRUE(test, mc16_zero_entry_stat(0, NULL, NULL));
	KUNIT_EXPECT_TRUE(test, mc16_zero_entry_stat(0, &compressed, NULL));
	KUNIT_EXPECT_TRUE(test, mc16_zero_entry_stat(0, NULL, &mapcount));
	compressed = 123;
	mapcount = 45;
	KUNIT_EXPECT_FALSE(test, mc16_zero_entry_stat(1, &compressed, &mapcount));
	KUNIT_EXPECT_FALSE(test, mc16_zero_entry_stat(ULONG_MAX, NULL, NULL));
#if BITS_PER_LONG > 32
	KUNIT_EXPECT_FALSE(test, mc16_zero_entry_stat(1UL << 32, NULL, NULL));
#endif
	KUNIT_EXPECT_EQ(test, compressed, 123UL);
	KUNIT_EXPECT_EQ(test, mapcount, 45);
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_init(2));
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_get(3));
	KUNIT_EXPECT_EQ(test, mc16_zero_stored_bytes(base),
			base + ((pages + 5) << PAGE_SHIFT));
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_stored_bytes), stored);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compressed_bytes), payload);
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_put(2));
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_put(3));
	KUNIT_EXPECT_EQ(test, mc16_zero_stored_bytes(base),
			base + (pages << PAGE_SHIFT));
}

static void mc16_test_zero_cpu_get(void *unused)
{
	mc16_zero_pte_get(3);
	mc16_zero_cache_get(5);
}

static void mc16_test_zero_cpu_put(void *unused)
{
	mc16_zero_pte_put(3);
	mc16_zero_cache_put(5);
}

static void memcomp_test_zero_cross_cpu(struct kunit *test)
{
	struct mc16_zero_owners before, after;
	s64 get_before, put_before, cache_get_before, cache_put_before;
	s64 get_after, put_after, cache_get_after, cache_put_after;
	u64 pages;
	unsigned int get_cpu, put_cpu;
	int ret, put_ret;

	cpus_read_lock();
	get_cpu = cpumask_first(cpu_online_mask);
	put_cpu = cpumask_next(get_cpu, cpu_online_mask);
	if (put_cpu >= nr_cpu_ids) {
		cpus_read_unlock();
		kunit_skip(test, "needs two online CPUs");
		return;
	}
	mc16_zero_owners_read(&before);
	pages = mc16_zero_pages();
	get_before = READ_ONCE(per_cpu(mc16_zero_owners, get_cpu).pte_pages);
	put_before = READ_ONCE(per_cpu(mc16_zero_owners, put_cpu).pte_pages);
	cache_get_before = READ_ONCE(per_cpu(mc16_zero_owners, get_cpu).cache_pages);
	cache_put_before = READ_ONCE(per_cpu(mc16_zero_owners, put_cpu).cache_pages);
	ret = smp_call_function_single(get_cpu, mc16_test_zero_cpu_get, NULL, 1);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (!ret) {
		KUNIT_EXPECT_EQ(test, mc16_zero_pages(), pages + 8);
		put_ret = smp_call_function_single(put_cpu, mc16_test_zero_cpu_put,
						   NULL, 1);
		KUNIT_EXPECT_EQ(test, put_ret, 0);
		if (put_ret) {
			mc16_zero_pte_put(3);
			mc16_zero_cache_put(5);
		} else {
			get_after = READ_ONCE(per_cpu(mc16_zero_owners, get_cpu).pte_pages);
			put_after = READ_ONCE(per_cpu(mc16_zero_owners, put_cpu).pte_pages);
			cache_get_after = READ_ONCE(per_cpu(mc16_zero_owners, get_cpu).cache_pages);
			cache_put_after = READ_ONCE(per_cpu(mc16_zero_owners, put_cpu).cache_pages);
			KUNIT_EXPECT_EQ(test, get_after, get_before + 3);
			KUNIT_EXPECT_EQ(test, put_after, put_before - 3);
			KUNIT_EXPECT_EQ(test, cache_get_after, cache_get_before + 5);
			KUNIT_EXPECT_EQ(test, cache_put_after, cache_put_before - 5);
		}
	}
	KUNIT_EXPECT_EQ(test, mc16_zero_pages(), pages);
	mc16_zero_owners_read(&after);
	cpus_read_unlock();
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages);
}

#ifdef CONFIG_MEMCG
static void memcomp_test_zero_no_storage(struct kunit *test)
{
	struct mc16_test_refs *f = mc16_test_refs_begin(test);
	struct mc16_test_storage *storage;
	struct mc16_view view;
	unsigned int allocations, refs, frees;
	int gets, puts, charges, uncharges;
	long charge;
	u8 *src;
	unsigned long compressed = PAGE_SIZE;
	int mapcount = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 0, MC16_STORED), 0);
	src = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	storage = f->storage;
	/* Observe the actual unrelated live payload, not an unbound counter. */
	mutex_lock(memcomp_lock_of(storage->refs[0].token));
	mc16_view_locked(&storage->refs[0], &view);
	storage->payload.handle = *view.word;
	mutex_unlock(memcomp_lock_of(storage->refs[0].token));
	storage->payload.task = current;
	storage->payload.pool = memcomp_pools[0];
	storage->payload.live = true;
	rcu_assign_pointer(memcomp_kunit_async_probe, &storage->payload);
	allocations = storage->probe.allocations;
	refs = refcount_read(&storage->refs[0].slot->refs);
	frees = storage->payload.frees;
	gets = atomic_read(&storage->owner->model.get_calls);
	puts = atomic_read(&storage->owner->model.put_calls);
	charges = atomic_read(&storage->owner->model.charge_calls);
	uncharges = atomic_read(&storage->owner->model.uncharge_calls);
	charge = atomic_long_read(&storage->owner->model.charged);
	/* Actual zero actors, with an unrelated positive slot remaining live. */
	KUNIT_EXPECT_TRUE(test, mc16_zero_is_page(src, PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test, mc16_zero_entry_stat(0, &compressed, &mapcount));
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_init(512));
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_get(2));
	KUNIT_EXPECT_TRUE(test, mc16_zero_pte_put(514));
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_get(1));
	KUNIT_EXPECT_TRUE(test, mc16_zero_cache_put(1));
	KUNIT_EXPECT_EQ(test, compressed, 0UL);
	KUNIT_EXPECT_EQ(test, mapcount, 1);
	KUNIT_EXPECT_EQ(test, storage->probe.allocations, allocations);
	KUNIT_EXPECT_EQ(test, refcount_read(&storage->refs[0].slot->refs), refs);
	KUNIT_EXPECT_EQ(test, storage->payload.frees, frees);
	KUNIT_EXPECT_TRUE(test, storage->payload.live);
	KUNIT_EXPECT_FALSE(test, storage->payload.bad_lifetime);
	KUNIT_EXPECT_EQ(test, atomic_read(&storage->owner->model.get_calls), gets);
	KUNIT_EXPECT_EQ(test, atomic_read(&storage->owner->model.put_calls), puts);
	KUNIT_EXPECT_EQ(test, atomic_read(&storage->owner->model.charge_calls), charges);
	KUNIT_EXPECT_EQ(test, atomic_read(&storage->owner->model.uncharge_calls),
			uncharges);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged), charge);
	/* The same probe must observe the subsequent real positive-slot free. */
	mc16_ref_put(&storage->refs[0]);
	f->published[0] = false;
	KUNIT_EXPECT_EQ(test, storage->payload.frees, frees + 1);
	KUNIT_EXPECT_FALSE(test, storage->payload.live);
	KUNIT_EXPECT_FALSE(test, storage->payload.bad_lifetime);
}
#endif /* CONFIG_MEMCG */

#endif /* _MM_MEMCOMP_TESTS_ZERO_TEST_H */
