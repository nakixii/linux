/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ANON_STORE_TEST_H
#define _MM_MEMCOMP_TESTS_ANON_STORE_TEST_H

/*
 * Include in suite.h after refs_test.h.  Uses its
 * refs/storage/objcg fixtures and memcomp_test_reclaimer_enter()/exit().
 * Register the cases in the enclosing suite.
 */
#ifdef CONFIG_MEMCG
struct mc16_test_anon_store {
	struct mc16_test_refs *refs;
	struct memcomp_ref guards[2];
	struct memcomp_reclaim_ctx *ctx;
	struct memcomp_store_ctx store;
	struct memcomp_async_req precomp[2];
	struct folio *refresh_folio;
	unsigned long token;
	unsigned int reclaim_flags;
	bool owner_moved[2];
	bool owner_dropped[2];
	bool reclaiming;
	bool reserved;
	bool folio_unlocked;
	u8 *src;
	u8 *dst;
};

static void mc16_test_anon_ctx_put(struct mc16_test_anon_store *f)
{
	if (f->ctx) {
		f->ctx->kunit_compress_error = 0;
		memcomp_reclaim_ctx_put(f->ctx);
		f->ctx = NULL;
	}
	if (f->reclaiming) {
		memcomp_test_reclaimer_exit(f->reclaim_flags);
		f->reclaiming = false;
	}
}

/* Runs before shared refs/storage/model teardown, including fatal asserts. */
static void mc16_test_anon_store_cleanup(void *data)
{
	struct mc16_test_anon_store *f = data;
	struct mc16_test_storage *s = f->refs->storage;
	struct memcomp_ref owner;
	unsigned int i;

	memcomp_kunit_fail_clear();
	s->owner->model.fail_at = 0;
	s->probe.fail_at = 0;
	/* Restore the parent fixture contract in the cleanup task. */
	if (f->folio_unlocked) {
		folio_lock(s->owner->folio);
		f->folio_unlocked = false;
	}
	/* Actions may run after the test task exits; never unlock its ctx. */
	KUNIT_EXPECT_PTR_EQ(s->test, f->ctx, NULL);
	KUNIT_EXPECT_FALSE(s->test, f->reclaiming);
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		if (f->guards[i].slot)
			mc16_ref_put(&f->guards[i]);
	}
	/* Real OWNER removal, siblings first and producer/head last. */
	for (i = ARRAY_SIZE(f->guards); f->reserved && i; ) {
		i--;
		if (f->owner_dropped[i])
			continue;
		if (!mc16_get(f->token + i, &owner)) {
			KUNIT_FAIL(s->test, "reserved member %u disappeared", i);
			continue;
		}
		KUNIT_EXPECT_TRUE(s->test,
				  mc16_owner_drop_folio_held(&owner,
							     f->owner_moved[i] ?
							     f->refresh_folio :
							     s->owner->folio,
							     2, i));
	}
	f->reserved = false;
	if (f->refresh_folio) {
		KUNIT_EXPECT_EQ(s->test, folio_ref_count(f->refresh_folio), 1);
		folio_unlock(f->refresh_folio);
		folio_put(f->refresh_folio);
		f->refresh_folio = NULL;
	}
	KUNIT_EXPECT_EQ(s->test, folio_ref_count(s->owner->folio), 1);
	/* Shared cleanup checks bytes, metadata, registry and objcg balance. */
}

static struct mc16_test_anon_store *
mc16_test_anon_store_begin(struct kunit *test)
{
	struct mc16_test_anon_store *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs || kunit_add_action_or_reset(test,
						  mc16_test_anon_store_cleanup, f))
		return NULL;
	f->src = kunit_kzalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	f->dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->src || !f->dst)
		return NULL;
	/* Native source exit needs a real folio memcg charge. */
	if (mem_cgroup_charge(f->refs->storage->owner->folio, NULL,
			      GFP_KERNEL)) {
		KUNIT_FAIL(test, "anon source memcg charge failed");
		return NULL;
	}
	/* Each reserve actor acquires/releases the real producer folio lock. */
	folio_unlock(f->refs->storage->owner->folio);
	f->folio_unlocked = true;
	return f;
}

static int mc16_test_anon_reserve_call(struct mc16_test_anon_store *f,
				       unsigned long *token)
{
	struct folio *folio = f->refs->storage->owner->folio;
	unsigned int i;
	int ret;

	folio_lock(folio);
	/* The placement hint must inspect the same bytes supplied to refresh. */
	for (i = 0; i < folio_nr_pages(folio); i++) {
		void *dst = kmap_local_folio(folio, i * PAGE_SIZE);

		memcpy(dst, f->src + i * PAGE_SIZE, PAGE_SIZE);
		kunmap_local(dst);
	}
	ret = mc16_anon_reserve(folio, 2, NULL, token);
	folio_unlock(folio);
	return ret;
}

static int mc16_test_anon_reserve(struct mc16_test_anon_store *f)
{
	unsigned int i;
	int ret;

	/* No fabricated anon_vma: these transactions transfer no VMA hold. */
	ret = mc16_test_anon_reserve_call(f, &f->token);
	if (ret)
		return ret;
	/* Track all initial owners before any lookup or assertion can fail. */
	f->reserved = true;
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		if (!mc16_get(f->token + i, &f->guards[i]))
			return -ENOENT;
	}
	return 0;
}

static int mc16_test_anon_ctx_get(struct mc16_test_anon_store *f)
{
	f->reclaim_flags = memcomp_test_reclaimer_enter();
	f->reclaiming = true;
	f->ctx = memcomp_reclaim_ctx_get(2, MEMCOMP_RECLAIM_KSWAPD);
	if (!f->ctx) {
		mc16_test_anon_ctx_put(f);
		return -EBUSY;
	}
	return 0;
}

static int mc16_test_anon_refresh(struct mc16_test_anon_store *f)
{
	struct folio *folio = f->refs->storage->owner->folio;
	int ret;

	folio_lock(folio);
	ret = mc16_anon_refresh(&f->ctx, f->src, 2, &f->store, f->guards);
	folio_unlock(folio);
	return ret;
}

/* Keep every caller guard through real source exit and later payload loads. */
static bool mc16_test_anon_finalize(struct mc16_test_anon_store *f)
{
	struct folio *folio = f->owner_moved[0] ? f->refresh_folio :
		f->refs->storage->owner->folio;
	bool locked = folio == f->refresh_folio;
	bool swapbacked, released;
	swp_entry_t saved_swap;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		if (!f->guards[i].slot || f->owner_dropped[i] ||
		    f->owner_moved[i] != f->owner_moved[0])
			return false;
	}
	if (!locked)
		folio_lock(folio);
	/* Low-level reserve returns the token without binding folio->swap. */
	saved_swap = folio->swap;
	swapbacked = folio_test_swapbacked(folio);
	__folio_set_swapbacked(folio);
	folio->swap = make_memcomp_entry(f->token);
	released = mc16_release_folio(folio);
	if (released) {
		folio_ref_unfreeze(folio, 1);
		for (i = 0; i < ARRAY_SIZE(f->guards); i++)
			f->owner_dropped[i] = true;
	}
	/* Restore the allocator-only parent fixture, not a live source owner. */
	folio->swap = saved_swap;
	if (!swapbacked)
		__folio_clear_swapbacked(folio);
	if (!locked)
		folio_unlock(folio);
	return released;
}

/* Partial commit is rolled back, never recorded as a successful eviction. */
static bool mc16_test_anon_rollback(struct mc16_test_anon_store *f)
{
	struct folio *folio = f->refs->storage->owner->folio;
	unsigned int i;
	bool dropped = true;

	folio_lock(folio);
	for (i = ARRAY_SIZE(f->guards); i; ) {
		struct memcomp_ref owner = { };

		i--;
		if (f->owner_dropped[i])
			continue;
		if (!mc16_get(f->token + i, &owner)) {
			dropped = false;
			continue;
		}
		f->owner_dropped[i] =
			mc16_owner_drop_folio_held(&owner, folio, 2, i);
		dropped &= f->owner_dropped[i];
	}
	folio_unlock(folio);
	return dropped;
}

static void mc16_test_anon_fill(struct mc16_test_anon_store *f, bool samefill)
{
	unsigned int i;

	if (samefill) {
		memset_l((unsigned long *)f->src, 0xfedcba9876543210UL,
			 PAGE_SIZE / sizeof(unsigned long));
		memset_l((unsigned long *)(f->src + PAGE_SIZE), 0x13579bdf2468ace0UL,
			 PAGE_SIZE / sizeof(unsigned long));
	} else {
		for (i = 0; i < 2 * PAGE_SIZE; i++)
			f->src[i] = (i % PAGE_SIZE) % 53;
	}
}

/* Observe effective members under T, never the physical PENDING slot tag. */
static void mc16_test_anon_expect_member(struct kunit *test,
					 struct memcomp_ref *ref,
					 unsigned int expected, u32 *bytes)
{
	struct mc16_view view;
	unsigned int state = MC16_EMPTY;
	bool valid, decoded = false, resident = false;

	KUNIT_ASSERT_NOT_NULL(test, ref->slot);
	*bytes = 0;
	mutex_lock(memcomp_lock_of(ref->token));
	valid = mc16_view_locked(ref, &view);
	if (valid) {
		state = mc16_flags_state(*view.flags);
		resident = *view.flags & MC16_F_RESIDENT;
		if (state == MC16_PENDING)
			decoded = true;
		else
			decoded = mc16_payload_length_decode(state, *view.length,
							     bytes);
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_TRUE(test, valid);
	KUNIT_EXPECT_TRUE(test, decoded);
	KUNIT_EXPECT_EQ(test, state, expected);
	KUNIT_EXPECT_EQ(test, resident, expected == MC16_PENDING);
	/* Both the initial OWNER and the caller's lookup must survive. */
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 2U);
}

static void mc16_test_anon_roundtrip(struct kunit *test, bool samefill)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	struct mc16_test_storage *s;
	unsigned int i;
	u32 bytes;
	long charged;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	mc16_test_anon_fill(f, samefill);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 3);
	charged = atomic_long_read(&s->owner->model.charged);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	/* Failed async requests must fall back to real synchronous compression. */
	if (!samefill) {
		f->precomp[0].ret = -EIO;
		f->precomp[1].ret = -EIO;
		f->store.precomp = f->precomp;
	}
	ret = mc16_test_anon_refresh(f);
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 1);
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		mc16_test_anon_expect_member(test, &f->guards[i],
					     samefill ? MC16_SAMEFILL : MC16_STORED, &bytes);
		if (samefill) {
			KUNIT_EXPECT_EQ(test, bytes, 0U);
		} else {
			KUNIT_EXPECT_GT(test, bytes, 0U);
			KUNIT_EXPECT_LT(test, bytes, (u32)PAGE_SIZE);
		}
		charged += bytes;
	}
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			charged);
	/* All OWNER/refcount/charge observations precede source exit. */
	KUNIT_ASSERT_TRUE(test, mc16_test_anon_finalize(f));
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		KUNIT_ASSERT_EQ(test, mc16_load_page(&f->guards[i], f->dst), 0);
		KUNIT_EXPECT_EQ(test, memcmp(f->src + i * PAGE_SIZE,
					     f->dst, PAGE_SIZE), 0);
	}
}

static void memcomp_test_anon_store_samefill(struct kunit *test)
{
	mc16_test_anon_roundtrip(test, true);
}

static void memcomp_test_anon_store_compressed(struct kunit *test)
{
	mc16_test_anon_roundtrip(test, false);
}

static void memcomp_test_anon_store_mixed_raw(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	unsigned int i;
	u32 bytes;
	int ret, compress_error;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_test_anon_fill(f, true);
	for (i = 0; i < PAGE_SIZE; i++)
		f->src[i] = i * 37 + (i >> 5);
	/* Force a codec no-shrink verdict without depending on its ratio. */
	f->precomp[0].ret = -ENOSPC;
	f->precomp[1].same_filled = true;
	f->precomp[1].element = 0x13579bdf2468ace0UL;
	f->store.precomp = f->precomp;
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	f->ctx->kunit_compress_error = -EIO;
	ret = mc16_test_anon_refresh(f);
	compress_error = f->ctx->kunit_compress_error;
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* Both precomputed decisions bypassed synchronous compression. */
	KUNIT_EXPECT_EQ(test, compress_error, -EIO);
	mc16_test_anon_expect_member(test, &f->guards[0], MC16_STORED, &bytes);
	KUNIT_EXPECT_EQ(test, bytes, (u32)PAGE_SIZE);
	mc16_test_anon_expect_member(test, &f->guards[1], MC16_SAMEFILL, &bytes);
	KUNIT_EXPECT_EQ(test, bytes, 0U);
	KUNIT_ASSERT_TRUE(test, mc16_test_anon_finalize(f));
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		KUNIT_ASSERT_EQ(test, mc16_load_page(&f->guards[i], f->dst), 0);
		KUNIT_EXPECT_EQ(test, memcmp(f->src + i * PAGE_SIZE,
					     f->dst, PAGE_SIZE), 0);
	}
}

static void memcomp_test_anon_store_reserve_failure(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	struct mc16_test_storage *s;
	unsigned long token = ULONG_MAX;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	s->owner->model.fail_at = 1;
	ret = mc16_test_anon_reserve_call(f, &token);
	/* Track an unexpected success as well, before any fatal assertion. */
	if (!ret) {
		f->token = token;
		f->reserved = true;
	}
	KUNIT_ASSERT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_EQ(test, token, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), 0L);
	s->owner->model.fail_at = 0;
	s->probe.fail_at = s->probe.allocations + 1;
	ret = mc16_test_anon_reserve_call(f, &token);
	if (!ret) {
		f->token = token;
		f->reserved = true;
	}
	KUNIT_ASSERT_EQ(test, ret, -ENOMEM);
	s->probe.fail_at = 0;
	rcu_barrier();
	KUNIT_EXPECT_EQ(test, token, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.get_calls),
			atomic_read(&s->owner->model.put_calls));
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
}

static void memcomp_test_anon_store_charge_failure(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	struct mc16_test_storage *s;
	unsigned int i;
	u32 bytes;
	long charged;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	mc16_test_anon_fill(f, false);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	charged = atomic_long_read(&s->owner->model.charged);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	/* The first payload charge must unwind when the second one fails. */
	s->owner->model.fail_at =
		atomic_read(&s->owner->model.charge_calls) + 2;
	ret = mc16_test_anon_refresh(f);
	s->owner->model.fail_at = 0;
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			charged);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 3);
	for (i = 0; i < ARRAY_SIZE(f->guards); i++)
		mc16_test_anon_expect_member(test, &f->guards[i], MC16_PENDING,
					     &bytes);
	/* The same caller guards remain valid for a real retry. */
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	ret = mc16_test_anon_refresh(f);
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test, mc16_test_anon_finalize(f));
}

static void memcomp_test_anon_store_partial_commit(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	struct mc16_test_storage *s;
	u32 bytes, pending_bytes;
	long charged;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	mc16_test_anon_fill(f, false);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	charged = atomic_long_read(&s->owner->model.charged);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	memcomp_kunit_fail_next(MEMCOMP_KUNIT_FAIL_REFRESH_COMMIT, 2, -EAGAIN);
	ret = mc16_test_anon_refresh(f);
	memcomp_kunit_fail_clear();
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
	mc16_test_anon_expect_member(test, &f->guards[0], MC16_STORED, &bytes);
	mc16_test_anon_expect_member(test, &f->guards[1], MC16_PENDING,
				     &pending_bytes);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			charged + bytes);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), 2);
	/* Observe the resident member before rollback, with both guards live. */
	memset(f->dst, 0x5a, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, mc16_load_page(&f->guards[1], f->dst), -EAGAIN);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(f->dst, 0x5a, PAGE_SIZE), NULL);
	KUNIT_ASSERT_TRUE(test, mc16_test_anon_rollback(f));
	KUNIT_ASSERT_EQ(test, mc16_load_page(&f->guards[0], f->dst), 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->src, f->dst, PAGE_SIZE), 0);
	/* Cleanup puts guards only; the real rollback already consumed OWNER. */
}

static void memcomp_test_anon_store_invalid_refs(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	struct memcomp_ref saved;
	unsigned int i;
	u32 bytes;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_test_anon_fill(f, true);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	/* Restore the real guard before any assertion can run teardown. */
	saved = f->guards[1];
	f->guards[1] = (struct memcomp_ref) { };
	ret = mc16_test_anon_refresh(f);
	f->guards[1] = saved;
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);
	for (i = 0; i < ARRAY_SIZE(f->guards); i++)
		mc16_test_anon_expect_member(test, &f->guards[i], MC16_PENDING,
					     &bytes);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	ret = mc16_test_anon_refresh(f);
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test, mc16_test_anon_finalize(f));
}

static void memcomp_test_anon_store_precomp(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	unsigned int i, len;
	int ret, compress_error;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_test_anon_fill(f, false);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	/* Allocate before taking ctx: allocation assertions may abort. */
	for (i = 0; i < ARRAY_SIZE(f->precomp); i++) {
		f->precomp[i].buf = kunit_kmalloc(test,
						  MEMCOMP_MAX_COMPRESSED_SIZE,
						  GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, f->precomp[i].buf);
	}
	for (i = 0; i < ARRAY_SIZE(f->precomp); i++) {
		KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
		len = MEMCOMP_MAX_COMPRESSED_SIZE;
		ret = crypto_comp_compress(f->ctx->tfm, f->src + i * PAGE_SIZE,
					   PAGE_SIZE, f->precomp[i].buf, &len);
		mc16_test_anon_ctx_put(f);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_ASSERT_LT(test, len, (unsigned int)PAGE_SIZE);
		f->precomp[i].len = len;
	}
	f->store.precomp = f->precomp;
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	f->ctx->kunit_compress_error = -EIO;
	ret = mc16_test_anon_refresh(f);
	compress_error = f->ctx->kunit_compress_error;
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, compress_error, -EIO);
	KUNIT_ASSERT_TRUE(test, mc16_test_anon_finalize(f));
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		KUNIT_ASSERT_EQ(test, mc16_load_page(&f->guards[i], f->dst), 0);
		KUNIT_EXPECT_EQ(test, memcmp(f->src + i * PAGE_SIZE,
					     f->dst, PAGE_SIZE), 0);
	}
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static u64 mc16_test_refresh_cross_node_read(void)
{
	u64 pages = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		pages += READ_ONCE(per_cpu(mc16_refresh_cross_node_pages, cpu));
	return pages;
}

static void memcomp_test_anon_store_cross_node_refresh(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	struct mc16_test_storage *s;
	struct mc16_writeback snapshot = { };
	struct mc16_writeback_tail tail = { };
	struct mc16_swap_fields swap;
	struct memcomp_lru *fifo_a, *fifo_b;
	struct mc16_view view;
	struct folio *source, *target;
	u64 cross_before;
	long fifo_a_before, fifo_b_before;
	u16 eligible_before;
	unsigned int bytes;
	int nid, source_nid, target_nid = NUMA_NO_NODE;
	bool valid;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	source = s->owner->folio;
	source_nid = folio_nid(source);
	for_each_node_state(nid, N_MEMORY) {
		if (nid != source_nid) {
			target_nid = nid;
			break;
		}
	}
	if (target_nid == NUMA_NO_NODE) {
		kunit_skip(test, "requires two N_MEMORY nodes");
		return;
	}
	target = __folio_alloc_node(GFP_KERNEL | __GFP_THISNODE, 1, target_nid);
	if (!target) {
		kunit_skip(test, "could not allocate target-node folio");
		return;
	}
	folio_lock(target);
	f->refresh_folio = target;
	if (mem_cgroup_charge(target, NULL, GFP_KERNEL)) {
		KUNIT_FAIL(test, "refresh source memcg charge failed");
		return;
	}
	fifo_a = memcomp_lru_nid(source_nid);
	fifo_b = memcomp_lru_nid(target_nid);
	fifo_a_before = atomic_long_read(&fifo_a->nr_entries);
	fifo_b_before = atomic_long_read(&fifo_b->nr_entries);
	mc16_test_anon_fill(f, false);
	for (nid = 0; nid < folio_nr_pages(target); nid++) {
		void *dst = kmap_local_folio(target, nid * PAGE_SIZE);

		memcpy(dst, f->src + nid * PAGE_SIZE, PAGE_SIZE);
		kunmap_local(dst);
	}
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	eligible_before = f->guards[0].cluster->eligible_slots;
	KUNIT_ASSERT_PTR_EQ(test, f->guards[0].cluster,
			    f->guards[1].cluster);
	for (nid = 0; nid < ARRAY_SIZE(f->guards); nid++) {
		mutex_lock(memcomp_lock_of(f->guards[nid].token));
		valid = mc16_view_locked(&f->guards[nid], &view) &&
			view.member->owner_folio == source;
		if (valid) {
			folio_get(target);
			view.member->owner_folio = target;
			folio_put(source);
			f->owner_moved[nid] = true;
		}
		mutex_unlock(memcomp_lock_of(f->guards[nid].token));
		KUNIT_ASSERT_TRUE(test, valid);
	}
	KUNIT_EXPECT_EQ(test, folio_nid(target), target_nid);
	KUNIT_EXPECT_EQ(test, folio_ref_count(source), 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(target), 3);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	cross_before = mc16_test_refresh_cross_node_read();
	ret = mc16_test_anon_refresh(f);
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, mc16_test_refresh_cross_node_read(), cross_before + 2);
	KUNIT_EXPECT_EQ(test, folio_ref_count(target), 1);
	KUNIT_EXPECT_EQ(test, f->guards[0].cluster->key->nid, source_nid);
	KUNIT_EXPECT_EQ(test, f->guards[1].cluster->key->nid, source_nid);
	KUNIT_EXPECT_EQ(test, f->guards[0].cluster->eligible_slots,
			(u16)(eligible_before + ARRAY_SIZE(f->guards)));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo_a->nr_entries),
			fifo_a_before + ARRAY_SIZE(f->guards));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo_b->nr_entries), fifo_b_before);
	/* Keep the NUMA/FIFO observations above in their prepared phase. */
	KUNIT_ASSERT_TRUE(test, mc16_test_anon_finalize(f));

	mutex_lock(memcomp_lock_of(f->guards[0].token));
	valid = mc16_writeback_claim_locked(&f->guards[0], &snapshot);
	mutex_unlock(memcomp_lock_of(f->guards[0].token));
	KUNIT_ASSERT_TRUE(test, valid);
	if (!mc16_length_decode(snapshot.length, &bytes)) {
		mutex_lock(memcomp_lock_of(f->guards[0].token));
		mc16_writeback_revert_locked(&f->guards[0]);
		mutex_unlock(memcomp_lock_of(f->guards[0].token));
		KUNIT_FAIL(test, "claimed writeback length did not decode");
		return;
	}
	swap = (struct mc16_swap_fields) {
		.type = MAX_SWAPFILES - 1,
		.swap_offset = mc16_swap_offset_limit(),
		.length = bytes,
	};
	memcomp_slot_dup(swp_entry(swap.type, swap.swap_offset));
	mutex_lock(memcomp_lock_of(f->guards[0].token));
	ret = mc16_writeback_settle_locked(&f->guards[0], &snapshot, &swap,
					   &tail);
	if (ret)
		mc16_writeback_revert_locked(&f->guards[0]);
	mutex_unlock(memcomp_lock_of(f->guards[0].token));
	if (ret) {
		memcomp_slot_put(swp_entry(swap.type, swap.swap_offset));
		KUNIT_FAIL(test, "writeback settle failed: %d", ret);
		return;
	}
	mc16_writeback_finish(&f->guards[0], &tail);
	mutex_lock(memcomp_lock_of(f->guards[0].token));
	valid = mc16_view_locked(&f->guards[0], &view);
	ret = valid ? mc16_flags_state(*view.flags) : MC16_EMPTY;
	mutex_unlock(memcomp_lock_of(f->guards[0].token));
	KUNIT_EXPECT_EQ(test, ret, (int)MC16_SWAPPED);
}
#endif
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_ANON_STORE_TEST_H */
