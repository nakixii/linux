/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_CONSUMERS_TEST_H
#define _MM_MEMCOMP_TESTS_CONSUMERS_TEST_H

#include <kunit/test.h>
#include "../consumers.h"

/*
 * Include in suite.h after the storage probe implementation. Register
 * the cases in the enclosing suite. Load-touch assertions exercise the shared
 * load helper used by the public clustered-slot dispatch.
 */
#ifdef CONFIG_MEMCG
struct mc16_consumer_test {
	struct kunit *test;
	struct memcomp_kunit_objcg model;
	struct mc16_storage_fixture probe;
	struct folio *account;
	struct folio *dst;
	struct folio *large;
	struct memcomp_ref owners[3];
	struct memcomp_ref guard;
	unsigned long tokens[3];
	unsigned int pte_refs[3];
	unsigned int zero_pte;
	unsigned int zero_cache;
	s64 stored_before;
	s64 compressed_before;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	s64 pending_before;
#endif
};

/* No test-task context, folio lock, T or local mapping survives an actor. */
static void mc16_consumer_test_cleanup(void *data)
{
	struct mc16_consumer_test *f = data;
	unsigned int i;

	f->probe.fail_at = 0;
	f->probe.fail_descriptor = false;
	f->probe.allocation_window = NULL;
	f->model.fail_at = 0;
	mc16_cache_abort(&f->guard);
	for (i = 0; i < ARRAY_SIZE(f->owners); i++) {
		while (f->pte_refs[i]) {
			mc16_pte_put_nr(f->tokens[i], 1);
			f->pte_refs[i]--;
		}
		if (f->owners[i].slot)
			mc16_ref_put(&f->owners[i]);
	}
	mc16_zero_pte_put(f->zero_pte);
	mc16_zero_cache_put(f->zero_cache);
	mc16_test_drain_segments(&f->probe);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_TRUE(f->test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(f->test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(f->test, atomic_long_read(&f->model.charged), 0L);
	KUNIT_EXPECT_EQ(f->test, atomic_read(&f->model.get_calls),
			atomic_read(&f->model.put_calls));
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&memcomp_stored_bytes),
			f->stored_before);
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&memcomp_compressed_bytes),
			f->compressed_before);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&memcomp_pending_pages),
			f->pending_before);
#endif
	if (rcu_access_pointer(mc16_storage_fixture) == &f->probe)
		rcu_assign_pointer(mc16_storage_fixture, NULL);
	if (rcu_access_pointer(memcomp_kunit_objcg) == &f->model)
		rcu_assign_pointer(memcomp_kunit_objcg, NULL);
	synchronize_rcu();
	if (f->large) {
		KUNIT_EXPECT_FALSE(f->test, folio_test_locked(f->large));
		folio_put(f->large);
	}
	if (f->dst) {
		KUNIT_EXPECT_FALSE(f->test, folio_test_locked(f->dst));
		folio_put(f->dst);
	}
	if (f->account) {
		KUNIT_EXPECT_FALSE(f->test, folio_test_locked(f->account));
		KUNIT_EXPECT_EQ(f->test, folio_ref_count(f->account), 1);
		folio_put(f->account);
	}
}

/* Unlike the older objcg fixture, account/dst remain unlocked on return. */
static struct mc16_consumer_test *mc16_consumer_test_begin(struct kunit *test)
{
	struct mc16_consumer_test *f;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return NULL;
	}
	rcu_barrier();
	if (rcu_access_pointer(memcomp_kunit_objcg) ||
	    rcu_access_pointer(mc16_storage_fixture) || !xa_empty(&mc16_clusters)) {
		KUNIT_FAIL(test, "consumer fixture requires an idle registry/model");
		return NULL;
	}
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->stored_before = atomic64_read(&memcomp_stored_bytes);
	f->compressed_before = atomic64_read(&memcomp_compressed_bytes);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	f->pending_before = atomic64_read(&memcomp_pending_pages);
#endif
	f->model.task = current;
	f->model.error = -ENOMEM;
	f->probe.task = current;
	if (kunit_add_action_or_reset(test, mc16_consumer_test_cleanup, f))
		return NULL;
	f->account = folio_alloc(GFP_KERNEL, 0);
	f->dst = folio_alloc(GFP_KERNEL, 0);
	if (!f->account || !f->dst)
		return NULL;
	/* Native eviction uses source-folio charge, not the objcg model. */
	if (mem_cgroup_charge(f->account, NULL, GFP_KERNEL)) {
		KUNIT_FAIL(test, "consumer source memcg charge failed");
		return NULL;
	}
	f->model.folio = f->account;
	rcu_assign_pointer(memcomp_kunit_objcg, &f->model);
	rcu_assign_pointer(mc16_storage_fixture, &f->probe);
	return f;
}

static int mc16_consumer_test_reserve(struct mc16_consumer_test *f,
				      unsigned int index)
{
	struct mc16_record *record;
	int ret;

	record = mc16_record_alloc(f->account, 1, false);
	if (IS_ERR(record))
		return PTR_ERR(record);
	ret = mc16_reserve(record, 1, MC16_KEY_ANON, &f->owners[index]);
	if (ret) {
		mc16_record_abort(record);
		return ret;
	}
	mc16_account_reservation(&f->owners[index]);
	f->tokens[index] = f->owners[index].token;
	return 0;
}

static int mc16_consumer_test_store(struct mc16_consumer_test *f,
				    const void *src)
{
	int ret = mc16_cache_store(src, f->account, &f->owners[0]);

	if (!ret)
		f->tokens[0] = f->owners[0].token;
	return ret;
}

/* Every lock/mapping is released before returning to KUnit assertions. */
static int mc16_consumer_test_load(struct memcomp_ref *ref, struct folio *folio,
				   bool zero)
{
	int ret;

	folio_lock(folio);
	ret = zero ? mc16_cache_load_zero(folio) : mc16_cache_load(ref, folio);
	folio_unlock(folio);
	return ret;
}

static void mc16_consumer_test_fill(struct folio *folio, u8 byte)
{
	void *dst = kmap_local_folio(folio, 0);

	memset(dst, byte, PAGE_SIZE);
	kunmap_local(dst);
	folio_clear_uptodate(folio);
}

static bool mc16_consumer_test_bytes(struct folio *folio, u8 byte)
{
	void *dst = kmap_local_folio(folio, 0);
	bool same = !memchr_inv(dst, byte, PAGE_SIZE);

	kunmap_local(dst);
	return same;
}

static bool mc16_consumer_test_matches(struct folio *folio, const void *src)
{
	void *dst = kmap_local_folio(folio, 0);
	bool same = !memcmp(dst, src, PAGE_SIZE);

	kunmap_local(dst);
	return same;
}

static bool mc16_consumer_test_touch(struct memcomp_ref *ref, bool set)
{
	struct mc16_view view;
	bool touched = false;

	mutex_lock(memcomp_lock_of(ref->token));
	if (mc16_view_locked(ref, &view)) {
		if (set)
			*view.flags |= MC16_F_REFRESH_PENDING;
		touched = !!(*view.flags & MC16_F_REFERENCED);
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	return touched;
}

static void memcomp_test_consumer_pte_shared(struct kunit *test)
{
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	struct memcomp_ref *owner;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_consumer_test_reserve(f, 0), 0);
	owner = &f->owners[0];
	KUNIT_ASSERT_TRUE(test, mc16_pte_get(f->tokens[0]));
	f->pte_refs[0]++;
	KUNIT_ASSERT_TRUE(test, mc16_pte_get(f->tokens[0]));
	f->pte_refs[0]++;
	KUNIT_EXPECT_EQ(test, refcount_read(&owner->slot->refs), 3U);
	mc16_pte_put_nr(f->tokens[0], 1);
	f->pte_refs[0]--;
	KUNIT_EXPECT_EQ(test, refcount_read(&owner->slot->refs), 2U);
	mc16_pte_put_nr(f->tokens[0], 1);
	f->pte_refs[0]--;
	KUNIT_EXPECT_EQ(test, refcount_read(&owner->slot->refs), 1U);
	KUNIT_EXPECT_TRUE(test, owner->slot->flags & MC16_F_OWNER);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->account), 2);
}

static void memcomp_test_consumer_pte_bounds(struct kunit *test)
{
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < ARRAY_SIZE(f->owners); i++) {
		KUNIT_ASSERT_EQ(test, mc16_consumer_test_reserve(f, i), 0);
		KUNIT_ASSERT_TRUE(test, mc16_pte_get(f->tokens[i]));
		f->pte_refs[i]++;
		KUNIT_ASSERT_EQ(test, f->tokens[i], f->tokens[0] + i);
	}
	mc16_pte_put_nr(f->tokens[0], 0);
	mc16_pte_put_nr(U32_MAX, 2);
	mc16_pte_put_nr(U32_MAX, UINT_MAX);
	mc16_pte_put_nr(ULONG_MAX, 0);
#if BITS_PER_LONG > 32
	KUNIT_EXPECT_FALSE(test, mc16_pte_get(BIT_ULL(32) | f->tokens[0]));
	mc16_pte_put_nr(BIT_ULL(32) | f->tokens[0], 3);
	mc16_cache_drop(BIT_ULL(32) | f->tokens[0]);
	KUNIT_EXPECT_FALSE(test, mc16_cache_pin(BIT_ULL(32) | f->tokens[0],
						&f->guard));
#endif
	for (i = 0; i < ARRAY_SIZE(f->owners); i++)
		KUNIT_EXPECT_EQ(test, refcount_read(&f->owners[i].slot->refs), 2U);
	mc16_pte_put_nr(f->tokens[0], ARRAY_SIZE(f->owners));
	for (i = 0; i < ARRAY_SIZE(f->owners); i++) {
		f->pte_refs[i]--;
		KUNIT_EXPECT_EQ(test, refcount_read(&f->owners[i].slot->refs), 1U);
	}
}

static void mc16_consumer_test_roundtrip(struct kunit *test, bool samefill,
					 bool commit)
{
	const unsigned long pattern = 0x9876543210fedcbaUL;
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	struct mc16_slot *slot;
	unsigned int i;
	u8 *src;

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	if (samefill)
		memset_l((unsigned long *)src, pattern,
			 PAGE_SIZE / sizeof(unsigned long));
	else
		for (i = 0; i < PAGE_SIZE; i++)
			src[i] = i % 53;
	KUNIT_ASSERT_EQ(test, mc16_consumer_test_store(f, src), 0);
	slot = f->owners[0].slot;
	KUNIT_EXPECT_EQ(test, mc16_flags_state(slot->flags),
			(unsigned int)(samefill ? MC16_SAMEFILL : MC16_STORED));
	if (commit) {
		KUNIT_ASSERT_TRUE(test, mc16_pte_get(f->tokens[0]));
		f->pte_refs[0]++;
	}
	KUNIT_ASSERT_TRUE(test, mc16_cache_pin(f->tokens[0], &f->guard));
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, slot);
	mc16_consumer_test_fill(f->dst, 0xa5);
	KUNIT_ASSERT_EQ(test, mc16_consumer_test_load(&f->guard, f->dst, false), 0);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, slot);
	KUNIT_EXPECT_TRUE(test, folio_test_uptodate(f->dst));
	KUNIT_EXPECT_TRUE(test, mc16_consumer_test_matches(f->dst, src));
	KUNIT_EXPECT_TRUE(test, mc16_consumer_test_touch(&f->guard, false));
	if (!commit) {
		mc16_cache_abort(&f->guard);
		KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&slot->refs), 1U);
		/* Abort retained the cache owner and its complete payload. */
		KUNIT_ASSERT_TRUE(test, mc16_cache_pin(f->tokens[0], &f->guard));
		mc16_consumer_test_fill(f->dst, 0x5a);
		KUNIT_ASSERT_EQ(test,
				mc16_consumer_test_load(&f->guard, f->dst, false), 0);
		KUNIT_EXPECT_TRUE(test, mc16_consumer_test_matches(f->dst, src));
		mc16_cache_abort(&f->guard);
		mc16_cache_drop(f->tokens[0]);
		f->owners[0] = (struct memcomp_ref) { };
	} else {
		KUNIT_EXPECT_EQ(test, refcount_read(&slot->refs), 3U);
		/* Model the winner of the expected-shadow xarray replacement. */
		mc16_cache_commit(&f->guard);
		f->owners[0] = (struct memcomp_ref) { };
		KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, NULL);
		KUNIT_ASSERT_TRUE(test, mc16_get(f->tokens[0], &f->guard));
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), 2U);
		mc16_cache_abort(&f->guard);
		/* Only the shared PTE remains; its put releases the real zs payload. */
		mc16_pte_put_nr(f->tokens[0], 1);
		f->pte_refs[0]--;
	}
	KUNIT_EXPECT_FALSE(test, mc16_get(f->tokens[0], &f->guard));
	/* Already-absent numeric tokens acquire no guard and consume nothing. */
	mc16_cache_drop(f->tokens[0]);
	mc16_pte_put_nr(f->tokens[0], 1);
	/* Absent tokens do not imply the current header has been reclaimed. */
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged), 0L);
}

static void memcomp_test_consumer_cache_abort(struct kunit *test)
{
	mc16_consumer_test_roundtrip(test, false, false);
}

static void memcomp_test_consumer_cache_samefill(struct kunit *test)
{
	mc16_consumer_test_roundtrip(test, true, false);
}

static void memcomp_test_consumer_cache_commit(struct kunit *test)
{
	mc16_consumer_test_roundtrip(test, false, true);
}

static void memcomp_test_consumer_cache_retry(struct kunit *test)
{
	const unsigned long pattern = 0x0123456789abcdefUL;
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	unsigned long *src;

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	memset_l(src, pattern, PAGE_SIZE / sizeof(*src));
	KUNIT_ASSERT_EQ(test, mc16_consumer_test_store(f, src), 0);
	KUNIT_ASSERT_TRUE(test, mc16_cache_pin(f->tokens[0], &f->guard));
	mc16_consumer_test_touch(&f->guard, true);
	mc16_consumer_test_fill(f->dst, 0x5a);
	KUNIT_EXPECT_EQ(test, mc16_consumer_test_load(&f->guard, f->dst, false),
			-EAGAIN);
	KUNIT_EXPECT_FALSE(test, folio_test_uptodate(f->dst));
	KUNIT_EXPECT_TRUE(test, mc16_consumer_test_bytes(f->dst, 0x5a));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), 2U);
	mc16_cache_abort(&f->guard);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->owners[0].slot->refs), 1U);
}

static void memcomp_test_consumer_zero_domains(struct kunit *test)
{
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	struct mc16_zero_owners before, after;
	struct mc16_zero_diagnostics events, end;
	unsigned int allocations;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_zero_owners_read(&before);
	mc16_zero_diagnostics_read(&events);
	allocations = f->probe.allocations;
	KUNIT_ASSERT_TRUE(test, mc16_zero_pte_init(2));
	f->zero_pte += 2;
	KUNIT_ASSERT_TRUE(test, mc16_pte_get(0));
	f->zero_pte++;
	KUNIT_ASSERT_TRUE(test, mc16_zero_cache_get(2));
	f->zero_cache += 2;
	KUNIT_EXPECT_FALSE(test, mc16_cache_pin(0, &f->guard));
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, NULL);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.cluster, NULL);
	mc16_cache_abort(&f->guard);
	mc16_cache_commit(&f->guard);
	mc16_pte_put_nr(0, 0);
	mc16_consumer_test_fill(f->dst, 0x5a);
	KUNIT_EXPECT_EQ(test, mc16_consumer_test_load(NULL, f->dst, true), 0);
	KUNIT_EXPECT_TRUE(test, folio_test_uptodate(f->dst));
	KUNIT_EXPECT_TRUE(test, mc16_consumer_test_bytes(f->dst, 0));
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages + 3);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages + 2);
	/* A failed xa replacement consumes neither zero owner. */
	KUNIT_EXPECT_EQ(test, mc16_consumer_test_load(NULL, f->dst, true), 0);
	mc16_pte_put_nr(0, 2);
	f->zero_pte -= 2;
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages + 1);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages + 2);
	/* Replacement and truncate each explicitly drop one cache owner. */
	mc16_cache_drop(0);
	f->zero_cache--;
	mc16_cache_drop(0);
	f->zero_cache--;
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages + 1);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages);
	mc16_pte_put_nr(0, 1);
	f->zero_pte--;
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages);
	KUNIT_EXPECT_EQ(test, f->probe.allocations, allocations);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.charge_calls), 0);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	mc16_zero_diagnostics_read(&end);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, end.calls[MC16_ZERO_PTE_GET] -
			events.calls[MC16_ZERO_PTE_GET], 1ULL);
	KUNIT_EXPECT_EQ(test, end.pages[MC16_ZERO_PTE_PUT] -
			events.pages[MC16_ZERO_PTE_PUT], 3ULL);
	KUNIT_EXPECT_EQ(test, end.pages[MC16_ZERO_CACHE_PUT] -
			events.pages[MC16_ZERO_CACHE_PUT], 2ULL);
#endif
}

static void memcomp_test_consumer_load_bounds(struct kunit *test)
{
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	const unsigned long pattern = 0x1234567812345678UL;
	unsigned long *src;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_FALSE(test, mc16_cache_pin(0, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_cache_pin(ULONG_MAX, &f->guard));
	KUNIT_EXPECT_EQ(test, mc16_cache_load(NULL, f->dst), -ENOENT);
	KUNIT_EXPECT_EQ(test, mc16_cache_load(&f->guard, f->dst), -ENOENT);
	KUNIT_EXPECT_EQ(test, mc16_cache_load_zero(NULL), -EINVAL);
	mc16_consumer_test_fill(f->dst, 0xa5);
	KUNIT_EXPECT_EQ(test, mc16_cache_load_zero(f->dst), -EINVAL);
	KUNIT_EXPECT_TRUE(test, mc16_consumer_test_bytes(f->dst, 0xa5));
	f->large = folio_alloc(GFP_KERNEL, 1);
	KUNIT_ASSERT_NOT_NULL(test, f->large);
	KUNIT_EXPECT_EQ(test, mc16_consumer_test_load(NULL, f->large, true), -EINVAL);
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	memset_l(src, pattern, PAGE_SIZE / sizeof(*src));
	KUNIT_ASSERT_EQ(test, mc16_consumer_test_store(f, src), 0);
	KUNIT_ASSERT_TRUE(test, mc16_cache_pin(f->tokens[0], &f->guard));
	KUNIT_EXPECT_EQ(test, mc16_cache_load(&f->guard, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, mc16_cache_load(&f->guard, f->dst), -EINVAL);
	KUNIT_EXPECT_EQ(test, mc16_consumer_test_load(&f->guard, f->large, false),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), 2U);
	mc16_cache_abort(&f->guard);
	mc16_cache_abort(&f->guard);
	mc16_cache_commit(&f->guard);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->owners[0].slot->refs), 1U);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_CONSUMERS_TEST_H */
