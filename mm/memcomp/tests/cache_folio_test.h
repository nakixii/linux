/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_CACHE_FOLIO_TEST_H
#define _MM_MEMCOMP_TESTS_CACHE_FOLIO_TEST_H

#include <kunit/test.h>
#include <kunit/static_stub.h>
#if defined(CONFIG_ARM64) && defined(CONFIG_ARM64_MTE)
#include <linux/cpufeature.h>
#include <asm/mte.h>
#endif
#include "../cache_folio.h"

/*
 * Include in suite.h after storage_probe_impl and consumers.h/load.h
 * are visible. CONFIG_MEMCOMP_KUNIT_TEST encloses this header; cases below run
 * with MEMCG=n/y, except the two charge-model cases guarded by CONFIG_MEMCG.
 * Real folios, codec, zs payloads, FIFO, refs and RCU retirement are exercised.
 * Arch success is a synchronous model observing the temporary swap entry;
 * failure can pass through the existing arch fail seam. MEMCG metadata and
 * byte charges use the existing synthetic owner, not real memcg reclaim.
 * All actors release folio locks and local mappings before any assertion.
 */
struct mc16_cf_test {
	struct kunit *test;
	struct mc16_storage_fixture probe;
	struct memcomp_kunit_async_probe payload;
#ifdef CONFIG_MEMCG
	struct memcomp_kunit_objcg model;
#endif
	struct folio *src;
	struct folio *large;
	struct memcomp_ref guard;
	unsigned long token;
	bool cache_owner;
	unsigned int zero_cache;
	unsigned int zero_pte;
	unsigned int arch_calls;
	unsigned int initial_refs;
	unsigned int state;
	unsigned int eligible;
	unsigned int length;
	bool arch_contract;
	bool arch_passthrough;
	int arch_error;
	s64 stored;
	s64 compressed;
	long fifo_entries;
	unsigned long zs_pages;
	struct mc16_zero_owners zero_before;
};

/* Only the test's synchronous static-stub invocation uses this pointer. */
static struct mc16_cf_test *mc16_cf_actor;

static int mc16_cf_arch(struct folio *folio)
{
	struct mc16_cf_test *f = mc16_cf_actor;
	struct mc16_cluster *cluster;
	struct mc16_slot *segment, *slot;
	unsigned long token = swp_offset(folio->swap);
	unsigned int index = token & (MC16_CLUSTER_SLOTS - 1);
	u16 flags;

	if (!f)
		return -EINVAL;
	f->arch_calls++;
	f->arch_contract = folio == f->src && folio_test_locked(folio) &&
		is_memcomp_entry(folio->swap) && token && token <= U32_MAX;
	/* Observe under the continuous initial ref; do not get a second ref. */
	rcu_read_lock();
	cluster = xa_load(&mc16_clusters, token >> MC16_CLUSTER_SHIFT);
	if (!cluster)
		goto invalid;
	/* Pair with reserve's segment installation while the initial ref lives. */
	segment = smp_load_acquire(&cluster->segments[index >> MC16_SEGMENT_SHIFT]);
	if (!segment)
		goto invalid;
	slot = &segment[index & (MC16_SEGMENT_SLOTS - 1)];
	flags = READ_ONCE(slot->flags);
	f->state = mc16_flags_state(flags);
	f->initial_refs = refcount_read(&slot->refs);
	f->eligible = READ_ONCE(cluster->eligible_slots);
	f->length = READ_ONCE(slot->length);
	f->arch_contract &= mc16_flags_valid(flags) &&
		!(flags & (MC16_F_OWNER | MC16_F_RESIDENT)) &&
		cluster->key->kind == MC16_KEY_SHMEM;
	if (f->state == MC16_STORED) {
		f->payload.handle = READ_ONCE(slot->word);
		f->payload.pool = memcomp_pools[0];
		f->payload.live = true;
	}
	rcu_read_unlock();
	return f->arch_passthrough ? memcomp_arch_prepare_to_swap(folio) :
		f->arch_error;
invalid:
	f->arch_contract = false;
	rcu_read_unlock();
	return -EINVAL;
}

static void mc16_cf_cleanup(void *data)
{
	struct mc16_cf_test *f = data;
	struct mc16_zero_owners zero;
	s64 delta;

	/* Cleanup may run in a different KUnit task; it inherits no held lock. */
	f->probe.task = current;
	f->payload.task = current;
	f->probe.fail_at = 0;
#ifdef CONFIG_MEMCG
	f->model.fail_at = 0;
#endif
	if (f->guard.slot)
		mc16_ref_put(&f->guard);
	if (f->cache_owner)
		mc16_cache_drop(f->token);
	mc16_zero_cache_put(f->zero_cache);
	mc16_zero_pte_put(f->zero_pte);
	mc16_test_drain_segments(&f->probe);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_TRUE(f->test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(f->test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&memcomp_stored_bytes), f->stored);
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&memcomp_compressed_bytes),
			f->compressed);
	if (f->src)
		KUNIT_EXPECT_EQ(f->test,
				atomic_long_read(&memcomp_lrus[folio_nid(f->src)].nr_entries),
				f->fifo_entries);
	mc16_zero_owners_read(&zero);
	KUNIT_EXPECT_EQ(f->test, zero.pte_pages, f->zero_before.pte_pages);
	KUNIT_EXPECT_EQ(f->test, zero.cache_pages, f->zero_before.cache_pages);
	/* Repair a failed ledger assertion too, so later cases stay isolated. */
	delta = zero.pte_pages - f->zero_before.pte_pages;
	if (delta > 0)
		mc16_zero_pte_put(delta);
	else if (delta < 0)
		mc16_zero_pte_init(-delta);
	delta = zero.cache_pages - f->zero_before.cache_pages;
	if (delta > 0)
		mc16_zero_cache_put(delta);
	else if (delta < 0)
		mc16_zero_cache_get(-delta);
	/* Isolated pool: no mapped object or concurrent zs migration is retained. */
	KUNIT_EXPECT_EQ(f->test, zs_get_total_pages(memcomp_pools[0]), f->zs_pages);
	KUNIT_EXPECT_FALSE(f->test, f->payload.bad_lifetime);
	KUNIT_EXPECT_FALSE(f->test, f->payload.live);
#ifdef CONFIG_MEMCG
	KUNIT_EXPECT_EQ(f->test, atomic_long_read(&f->model.charged), 0L);
	KUNIT_EXPECT_EQ(f->test, atomic_read(&f->model.get_calls),
			atomic_read(&f->model.put_calls));
	if (rcu_access_pointer(memcomp_kunit_objcg) == &f->model)
		rcu_assign_pointer(memcomp_kunit_objcg, NULL);
#endif
	if (rcu_access_pointer(mc16_storage_fixture) == &f->probe)
		rcu_assign_pointer(mc16_storage_fixture, NULL);
	if (rcu_access_pointer(memcomp_kunit_async_probe) == &f->payload)
		rcu_assign_pointer(memcomp_kunit_async_probe, NULL);
	synchronize_rcu();
	if (f->large)
		folio_put(f->large);
	if (f->src) {
		KUNIT_EXPECT_FALSE(f->test, folio_test_locked(f->src));
		KUNIT_EXPECT_EQ(f->test, folio_ref_count(f->src), 1);
		folio_put(f->src);
	}
}

static struct mc16_cf_test *mc16_cf_begin(struct kunit *test)
{
	struct mc16_cf_test *f;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return NULL;
	}
	rcu_barrier();
	if (rcu_access_pointer(mc16_storage_fixture) ||
	    rcu_access_pointer(memcomp_kunit_async_probe) ||
	    READ_ONCE(memcomp_kunit_fail.task) || !xa_empty(&mc16_clusters) ||
	    !hash_empty(mc16_keys)) {
		KUNIT_FAIL(test, "cache folio fixture requires idle registry/probes");
		return NULL;
	}
#ifdef CONFIG_MEMCG
	if (rcu_access_pointer(memcomp_kunit_objcg)) {
		KUNIT_FAIL(test, "cache folio fixture requires idle objcg model");
		return NULL;
	}
#endif
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f) {
		KUNIT_FAIL(test, "cache folio fixture allocation failed");
		return NULL;
	}
	f->test = test;
	f->stored = atomic64_read(&memcomp_stored_bytes);
	f->compressed = atomic64_read(&memcomp_compressed_bytes);
	f->zs_pages = zs_get_total_pages(memcomp_pools[0]);
	mc16_zero_owners_read(&f->zero_before);
	f->probe.task = current;
	f->payload.task = current;
	if (kunit_add_action_or_reset(test, mc16_cf_cleanup, f))
		return NULL;
	f->src = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!f->src) {
		KUNIT_FAIL(test, "source folio allocation failed");
		return NULL;
	}
	f->fifo_entries = atomic_long_read(&memcomp_lrus[folio_nid(f->src)].nr_entries);
	/* The source also needs real memcg identity for native eviction. */
	if (mem_cgroup_charge(f->src, NULL, GFP_KERNEL)) {
		KUNIT_FAIL(test, "cache source memcg charge failed");
		return NULL;
	}
#ifdef CONFIG_MEMCG
	f->model.task = current;
	f->model.folio = f->src;
	f->model.error = -ENOMEM;
	rcu_assign_pointer(memcomp_kunit_objcg, &f->model);
#endif
	rcu_assign_pointer(mc16_storage_fixture, &f->probe);
	rcu_assign_pointer(memcomp_kunit_async_probe, &f->payload);
	kunit_activate_static_stub(test, mc16_cache_folio_arch_prepare, mc16_cf_arch);
	return f;
}

enum mc16_cf_pattern {
	MC16_CF_ZERO,
	MC16_CF_BYTES,
	MC16_CF_FILL,
	MC16_CF_LAST_BYTE,
};

static void mc16_cf_fill(struct mc16_cf_test *f, enum mc16_cf_pattern pattern)
{
	u8 *src = kmap_local_folio(f->src, 0);
	unsigned int i;

	memset(src, 0, PAGE_SIZE);
	if (pattern == MC16_CF_BYTES)
		for (i = 0; i < PAGE_SIZE; i++)
			src[i] = i % 53;
	else if (pattern == MC16_CF_FILL)
		memset(src, 0x5a, PAGE_SIZE);
	else if (pattern == MC16_CF_LAST_BYTE)
		src[PAGE_SIZE - 1] = 1;
	kunmap_local(src);
}

static bool mc16_cf_store(struct mc16_cf_test *f)
{
	bool ok;

	mc16_cf_actor = f;
	folio_lock(f->src);
	ok = mc16_store_cache_folio(f->src, &f->token);
	folio_unlock(f->src);
	mc16_cf_actor = NULL;
	if (ok && f->token)
		f->cache_owner = true;
	return ok;
}

/* T, source mapping and destination allocation never escape this actor. */
static int mc16_cf_roundtrip(struct mc16_cf_test *f)
{
	void *dst, *src;
	int ret;

	dst = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!dst)
		return -ENOMEM;
	if (!mc16_get(f->token, &f->guard)) {
		kfree(dst);
		return -ENOENT;
	}
	/* Byte roundtrip models a successful XA exit, not preparation alone. */
	folio_lock(f->src);
	folio_set_swapbacked(f->src);
	mc16_publish_cache_eviction(f->src, &f->guard);
	folio_unlock(f->src);
	ret = mc16_load_page(&f->guard, dst);
	if (!ret) {
		src = kmap_local_folio(f->src, 0);
		ret = memcmp(src, dst, PAGE_SIZE) ? -EILSEQ : 0;
		kunmap_local(src);
	}
	mc16_ref_put(&f->guard);
	kfree(dst);
	return ret;
}

static void mc16_cf_expect_no_prepare(struct kunit *test, struct mc16_cf_test *f)
{
	KUNIT_EXPECT_EQ(test, f->arch_calls, 0U);
	KUNIT_EXPECT_EQ(test, f->probe.allocations, 0U);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_stored_bytes), f->stored);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compressed_bytes), f->compressed);
#ifdef CONFIG_MEMCG
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.get_calls), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->model.charge_calls), 0);
#endif
}

static void mc16_cf_positive(struct kunit *test, enum mc16_cf_pattern pattern)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	u32 bytes;

	if (!f)
		return;
	mc16_cf_fill(f, pattern);
	KUNIT_ASSERT_TRUE(test, mc16_cf_store(f));
	KUNIT_EXPECT_NE(test, f->token, 0UL);
	KUNIT_EXPECT_TRUE(test, f->arch_contract);
	KUNIT_EXPECT_EQ(test, f->arch_calls, 1U);
	KUNIT_EXPECT_EQ(test, f->initial_refs, 1U);
	KUNIT_EXPECT_EQ(test, f->state,
			(unsigned int)(pattern == MC16_CF_FILL ? MC16_SAMEFILL : MC16_STORED));
	KUNIT_EXPECT_EQ(test, f->eligible, pattern == MC16_CF_FILL ? 0U : 1U);
	KUNIT_EXPECT_EQ(test, f->src->swap.val, 0UL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->src), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_stored_bytes),
			f->stored + PAGE_SIZE);
	KUNIT_ASSERT_TRUE(test, mc16_payload_length_decode(f->state, f->length, &bytes));
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compressed_bytes),
			f->compressed + bytes);
	KUNIT_EXPECT_EQ(test, mc16_cf_roundtrip(f), 0);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->token, &f->guard));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), 2U);
	mc16_ref_put(&f->guard);
	/* Model truncate after the successful exit used by the byte roundtrip. */
	mc16_cache_drop(f->token);
	f->cache_owner = false;
	rcu_barrier();
	KUNIT_EXPECT_EQ(test, f->payload.frees, pattern == MC16_CF_FILL ? 0U : 1U);
	KUNIT_EXPECT_FALSE(test, mc16_entry_present(f->token));
}

static void memcomp_test_cache_folio_compressed(struct kunit *test)
{
	mc16_cf_positive(test, MC16_CF_BYTES);
}

static void memcomp_test_cache_folio_samefill(struct kunit *test)
{
	mc16_cf_positive(test, MC16_CF_FILL);
}

static void memcomp_test_cache_folio_last_byte(struct kunit *test)
{
	mc16_cf_positive(test, MC16_CF_LAST_BYTE);
}

static void mc16_cf_zero(struct kunit *test, bool winner)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	struct mc16_zero_owners zero;
	bool pending;

	if (!f)
		return;
	mc16_zero_pte_init(2);
	f->zero_pte = 2;
	f->token = ULONG_MAX;
	memcomp_kunit_fail_next(MEMCOMP_KUNIT_FAIL_ARCH_PREPARE, 1, -EIO);
	/* A zero preparation must not even reach the arch failure seam. */
	if (!mc16_cf_store(f)) {
		memcomp_kunit_fail_clear();
		KUNIT_FAIL(test, "zero preparation failed");
		return;
	}
	pending = memcomp_kunit_fail.stage == MEMCOMP_KUNIT_FAIL_ARCH_PREPARE &&
		memcomp_kunit_fail.remaining == 1;
	memcomp_kunit_fail_clear();
	KUNIT_EXPECT_TRUE(test, pending);
	KUNIT_EXPECT_EQ(test, f->token, 0UL);
	mc16_cf_expect_no_prepare(test, f);
	mc16_zero_owners_read(&zero);
	KUNIT_EXPECT_EQ(test, zero.pte_pages, f->zero_before.pte_pages + 2);
	KUNIT_EXPECT_EQ(test, zero.cache_pages, f->zero_before.cache_pages);
	if (!winner)
		return; /* A losing preparation performs no publish and owes no drop. */
	/* Model only the successful XA winner's explicit publication. */
	mc16_zero_cache_get(1);
	f->zero_cache = 1;
	mc16_zero_owners_read(&zero);
	KUNIT_EXPECT_EQ(test, zero.cache_pages, f->zero_before.cache_pages + 1);
	KUNIT_EXPECT_EQ(test, zero.pte_pages, f->zero_before.pte_pages + 2);
	mc16_cache_drop(0);
	f->zero_cache = 0;
	mc16_zero_owners_read(&zero);
	KUNIT_EXPECT_EQ(test, zero.cache_pages, f->zero_before.cache_pages);
	KUNIT_EXPECT_EQ(test, zero.pte_pages, f->zero_before.pte_pages + 2);
}

static void memcomp_test_cache_folio_zero_abort(struct kunit *test)
{
	mc16_cf_zero(test, false);
}

static void memcomp_test_cache_folio_zero_publish(struct kunit *test)
{
	mc16_cf_zero(test, true);
}

static void mc16_cf_arch_fail(struct kunit *test, bool samefill, bool seam)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	bool ok;

	if (!f)
		return;
	mc16_cf_fill(f, samefill ? MC16_CF_FILL : MC16_CF_BYTES);
	f->token = ULONG_MAX;
	f->arch_error = -EIO;
	f->arch_passthrough = seam;
	if (seam)
		memcomp_kunit_fail_next(MEMCOMP_KUNIT_FAIL_ARCH_PREPARE, 1, -EIO);
	ok = mc16_cf_store(f);
	memcomp_kunit_fail_clear();
	KUNIT_EXPECT_FALSE(test, ok);
	KUNIT_EXPECT_EQ(test, f->token, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, f->src->swap.val, 0UL);
	KUNIT_EXPECT_TRUE(test, f->arch_contract);
	KUNIT_EXPECT_EQ(test, f->initial_refs, 1U);
	KUNIT_EXPECT_EQ(test, f->arch_calls, 1U);
	KUNIT_EXPECT_EQ(test, f->payload.frees, samefill ? 0U : 1U);
	/* Rollback releases payloads while preserving the appendable header. */
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_FALSE(test, hash_empty(mc16_keys));
	/* Cleanup explicitly reclaims metadata before checking charge refunds. */
}

static void memcomp_test_cache_folio_arch_failure(struct kunit *test)
{
	mc16_cf_arch_fail(test, false, false);
}

static void memcomp_test_cache_folio_arch_seam(struct kunit *test)
{
	mc16_cf_arch_fail(test, false, true);
}

static void memcomp_test_cache_folio_samefill_arch_failure(struct kunit *test)
{
	mc16_cf_arch_fail(test, true, false);
}

static void memcomp_test_cache_folio_disabled(struct kunit *test)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	bool saved, zero_ok, nonzero_ok;

	if (!f)
		return;
	f->token = ULONG_MAX;
	saved = READ_ONCE(memcomp_enabled);
	/* Publish the temporary disabled state to the existing enabled reader. */
	smp_store_release(&memcomp_enabled, false);
	zero_ok = mc16_cf_store(f);
	mc16_cf_fill(f, MC16_CF_BYTES);
	nonzero_ok = mc16_cf_store(f);
	/* Restore before assertions and before cleanup can call other helpers. */
	smp_store_release(&memcomp_enabled, saved);
	KUNIT_EXPECT_FALSE(test, zero_ok);
	KUNIT_EXPECT_FALSE(test, nonzero_ok);
	KUNIT_EXPECT_EQ(test, f->token, ULONG_MAX);
	mc16_cf_expect_no_prepare(test, f);
}

/* Mapcount/writeback/mlock/ref-bias are eligibility models, not real GUP/IO. */
static void memcomp_test_cache_folio_preflight(struct kunit *test)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	bool null_folio, null_token, unlocked, mapped, mlocked = false;
	bool writeback, pinned;

	if (!f)
		return;
	f->token = ULONG_MAX;
	null_folio = mc16_store_cache_folio(NULL, &f->token);
	unlocked = mc16_store_cache_folio(f->src, &f->token);
	folio_lock(f->src);
	null_token = mc16_store_cache_folio(f->src, NULL);
	atomic_set(&f->src->_mapcount, 0);
	mapped = mc16_store_cache_folio(f->src, &f->token);
	atomic_set(&f->src->_mapcount, -1);
#ifdef CONFIG_MMU
	folio_set_mlocked(f->src);
	mlocked = mc16_store_cache_folio(f->src, &f->token);
	folio_clear_mlocked(f->src);
#endif
	/* Raw flag manipulation avoids pretending to start real writeback IO. */
	__set_bit(PG_writeback, &f->src->flags);
	writeback = mc16_store_cache_folio(f->src, &f->token);
	__clear_bit(PG_writeback, &f->src->flags);
	folio_ref_add(f->src, GUP_PIN_COUNTING_BIAS);
	pinned = mc16_store_cache_folio(f->src, &f->token);
	folio_ref_sub(f->src, GUP_PIN_COUNTING_BIAS);
	folio_unlock(f->src);
	KUNIT_EXPECT_FALSE(test, null_folio);
	KUNIT_EXPECT_FALSE(test, null_token);
	KUNIT_EXPECT_FALSE(test, unlocked);
	KUNIT_EXPECT_FALSE(test, mapped);
	KUNIT_EXPECT_FALSE(test, mlocked);
	KUNIT_EXPECT_FALSE(test, writeback);
	KUNIT_EXPECT_FALSE(test, pinned);
	KUNIT_EXPECT_EQ(test, f->token, ULONG_MAX);
	mc16_cf_expect_no_prepare(test, f);
}

static void memcomp_test_cache_folio_large(struct kunit *test)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	bool ok;

	if (!f)
		return;
	f->large = folio_alloc(GFP_KERNEL | __GFP_ZERO, 1);
	KUNIT_ASSERT_NOT_NULL(test, f->large);
	f->token = ULONG_MAX;
	folio_lock(f->large);
	ok = mc16_store_cache_folio(f->large, &f->token);
	folio_unlock(f->large);
	KUNIT_EXPECT_FALSE(test, ok);
	KUNIT_EXPECT_EQ(test, f->token, ULONG_MAX);
	mc16_cf_expect_no_prepare(test, f);
}

static void mc16_cf_prepare_fail(struct kunit *test, unsigned int charge)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	bool ok;

	if (!f)
		return;
	mc16_cf_fill(f, MC16_CF_BYTES);
	f->token = ULONG_MAX;
	if (!charge)
		f->probe.fail_at = f->probe.allocations + 1;
#ifdef CONFIG_MEMCG
	else
		f->model.fail_at = atomic_read(&f->model.charge_calls) + charge;
#endif
	ok = mc16_cf_store(f);
	f->probe.fail_at = 0;
#ifdef CONFIG_MEMCG
	f->model.fail_at = 0;
#endif
	KUNIT_EXPECT_FALSE(test, ok);
	KUNIT_EXPECT_EQ(test, f->token, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, f->arch_calls, 0U);
	KUNIT_EXPECT_EQ(test, f->src->swap.val, 0UL);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	/* Payload preparation is real; deferred metadata refunds checked at exit. */
}

static void memcomp_test_cache_folio_metadata_failure(struct kunit *test)
{
	mc16_cf_prepare_fail(test, 0);
}

#ifdef CONFIG_MEMCG
static void memcomp_test_cache_folio_charge_failure(struct kunit *test)
{
	mc16_cf_prepare_fail(test, 1);
}

static void memcomp_test_cache_folio_key_charge_failure(struct kunit *test)
{
	mc16_cf_prepare_fail(test, 2);
}
#endif

#if defined(CONFIG_ARM64) && defined(CONFIG_ARM64_MTE)
static void memcomp_test_cache_folio_tagged_zero(struct kunit *test)
{
	struct mc16_cf_test *f = mc16_cf_begin(test);
	bool ok;

	if (!f)
		return;
	if (!system_supports_mte()) {
		kunit_skip(test, "requires the arm64 MTE pages_identical predicate");
		return;
	}
	/* Predicate/flag model only: no real tag instructions or arch swap IO. */
	set_page_mte_tagged(&f->src->page);
	ok = mc16_cf_store(f);
	clear_bit(PG_mte_tagged, &f->src->flags);
	KUNIT_ASSERT_TRUE(test, ok);
	KUNIT_EXPECT_NE(test, f->token, 0UL);
	KUNIT_EXPECT_EQ(test, f->state, (unsigned int)MC16_SAMEFILL);
	KUNIT_EXPECT_EQ(test, f->arch_calls, 1U);
	KUNIT_EXPECT_EQ(test, mc16_cf_roundtrip(f), 0);
}
#endif

/*
 * Register in the enclosing memcomp_test_cases[] (no new suite):
 *
 * KUNIT_CASE(memcomp_test_cache_folio_compressed),
 * KUNIT_CASE(memcomp_test_cache_folio_samefill),
 * KUNIT_CASE(memcomp_test_cache_folio_last_byte),
 * KUNIT_CASE(memcomp_test_cache_folio_zero_abort),
 * KUNIT_CASE(memcomp_test_cache_folio_zero_publish),
 * KUNIT_CASE(memcomp_test_cache_folio_arch_failure),
 * KUNIT_CASE(memcomp_test_cache_folio_arch_seam),
 * KUNIT_CASE(memcomp_test_cache_folio_samefill_arch_failure),
 * KUNIT_CASE(memcomp_test_cache_folio_disabled),
 * KUNIT_CASE(memcomp_test_cache_folio_preflight),
 * KUNIT_CASE(memcomp_test_cache_folio_large),
 * KUNIT_CASE(memcomp_test_cache_folio_metadata_failure),
 *
 * Additional CONFIG_MEMCG cases:
 * KUNIT_CASE(memcomp_test_cache_folio_charge_failure),
 * KUNIT_CASE(memcomp_test_cache_folio_key_charge_failure),
 *
 * Additional CONFIG_ARM64 && CONFIG_ARM64_MTE case (runtime MTE required):
 * KUNIT_CASE(memcomp_test_cache_folio_tagged_zero),
 */

#endif /* _MM_MEMCOMP_TESTS_CACHE_FOLIO_TEST_H */
