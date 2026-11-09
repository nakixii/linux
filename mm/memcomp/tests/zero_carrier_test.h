/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ZERO_CARRIER_TEST_H
#define _MM_MEMCOMP_TESTS_ZERO_CARRIER_TEST_H

/* Include after zero_convert_test.h and budget_test.h. */
#if defined(MC16_ZERO_CONVERT_TEST_ENABLED) && defined(MC16_FOLIO_TEST_ENABLED)
#define MC16_ZERO_CARRIER_TEST_ENABLED

static void mc16_zcar_expect_invalid(struct kunit *test,
				     const struct mc16_zero_carrier *zero)
{
	KUNIT_EXPECT_FALSE(test, zero->valid);
	KUNIT_EXPECT_FALSE(test, zero->ready);
	KUNIT_EXPECT_EQ(test, zero->token, 0UL);
	KUNIT_EXPECT_EQ(test, zero->ref.token, 0U);
	KUNIT_EXPECT_PTR_EQ(test, zero->ref.slot, NULL);
	KUNIT_EXPECT_PTR_EQ(test, zero->ref.cluster, NULL);
}

static unsigned int mc16_zcar_convert(struct mc16_test_zc *f,
				      const struct mc16_zero_carrier *zero)
{
	unsigned int converted;

	mmap_read_lock(f->mm);
	folio_lock(f->folio);
	converted = mc16_zero_convert_folio(f->folio, zero);
	folio_unlock(f->folio);
	mmap_read_unlock(f->mm);
	return converted;
}

/* Keep the old OWNER alive while reusing its real carrier on a fresh token. */
static int mc16_zcar_reserve_pending(struct mc16_test_zc *f)
{
	struct mc16_test_storage *storage = f->refs->storage;
	struct memcomp_ref guard = { };
	unsigned int index = storage->nr;
	int ret;

	folio_lock(storage->owner->folio);
	ret = mc16_test_refs_reserve(f->refs, 1);
	if (!ret && !mc16_get(storage->refs[index].token, &guard))
		ret = -ENOENT;
	if (!ret) {
		mc16_ref_put(&f->guard);
		f->guard = guard;
		f->tokens[0] = guard.token;
	}
	folio_unlock(storage->owner->folio);
	return ret;
}

static int mc16_zcar_refresh(struct mc16_test_zc *f, unsigned int pattern)
{
	struct folio *folio = f->refs->storage->owner->folio;
	u8 *src;
	unsigned int i;
	int ret;

	folio_lock(folio);
	src = kmap_local_folio(folio, 0);
	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = pattern == 256 ? i % 53 : pattern;
	kunmap_local(src);
	ret = mc16_test_zc_refresh(folio, &f->guard, &f->store[0]);
	folio_unlock(folio);
	return ret;
}

static void memcomp_test_zero_carrier_ready(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct mc16_zero_carrier *zero;
	struct mc16_view view;
	unsigned int refs;
	bool effective_zero;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zcar_reserve_pending(f), 0);
	refs = refcount_read(&f->guard.slot->refs);
	ret = mc16_zcar_refresh(f, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	zero = &f->store[0].zero;
	KUNIT_EXPECT_TRUE(test, zero->valid);
	KUNIT_EXPECT_TRUE(test, zero->ready);
	KUNIT_EXPECT_EQ(test, zero->token, (unsigned long)f->tokens[0]);
	KUNIT_EXPECT_EQ(test, zero->ref.token, f->tokens[0]);
	KUNIT_EXPECT_PTR_EQ(test, zero->ref.slot, f->guard.slot);
	KUNIT_EXPECT_PTR_EQ(test, zero->ref.cluster, f->guard.cluster);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs);
	/* Only validation and commit T: carrier preparation adds no operation. */
	/* Observe after measurement; never manufacture readiness in this T. */
	mutex_lock(memcomp_lock_of(f->tokens[0]));
	effective_zero = mc16_view_locked(&f->guard, &view) && view.record &&
		mc16_flags_state(*view.flags) == MC16_SAMEFILL &&
		!(*view.flags & (MC16_F_RESIDENT | MC16_F_REFRESH_PENDING)) &&
		!*view.word && !*view.length;
	mutex_unlock(memcomp_lock_of(f->tokens[0]));
	KUNIT_EXPECT_TRUE(test, effective_zero);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(f->guard.slot->flags),
			(unsigned int)MC16_PENDING);
}

static void memcomp_test_zero_carrier_commit_failure(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	unsigned int refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.valid);
	KUNIT_ASSERT_EQ(test, mc16_zcar_reserve_pending(f), 0);
	refs = refcount_read(&f->guard.slot->refs);
	f->store[0].kunit_refresh_commit_error = -EIO;
	KUNIT_EXPECT_EQ(test, mc16_zcar_refresh(f, 0), -EIO);
	KUNIT_EXPECT_EQ(test, f->store[0].kunit_refresh_commit_error, 0);
	mc16_zcar_expect_invalid(test, &f->store[0].zero);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs);
}

static void memcomp_test_zero_carrier_refresh_preflight(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct folio *folio;
	struct memcomp_reclaim_ctx *ctx;
	unsigned int flags;
	void *src;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.valid);
	folio = f->refs->storage->owner->folio;
	flags = memcomp_test_reclaimer_enter();
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_KSWAPD);
	if (!ctx) {
		memcomp_test_reclaimer_exit(flags);
		KUNIT_FAIL(test, "no codec context for preflight rejection");
		return;
	}
	folio_lock(folio);
	src = kmap_local_folio(folio, 0);
	ret = mc16_anon_refresh(&ctx, src, 0, &f->store[0], &f->guard);
	kunmap_local(src);
	folio_unlock(folio);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	memcomp_test_reclaimer_exit(flags);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	mc16_zcar_expect_invalid(test, &f->store[0].zero);
}

static void memcomp_test_zero_carrier_store_preflight(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct memcomp_reclaim_ctx *ctx;
	enum memcomp_anon_store_fail failure;
	struct folio *folio;
	unsigned int flags;
	bool stored;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.valid);
	folio = f->refs->storage->owner->folio;
	KUNIT_ASSERT_FALSE(test, folio_test_anon(folio));
	flags = memcomp_test_reclaimer_enter();
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_KSWAPD);
	if (!ctx) {
		memcomp_test_reclaimer_exit(flags);
		KUNIT_FAIL(test, "no codec context for store rejection");
		return;
	}
	folio_lock(folio);
	stored = mc16_anon_store_internal(folio, &ctx, &f->store[0], &failure);
	folio_unlock(folio);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	memcomp_test_reclaimer_exit(flags);
	KUNIT_EXPECT_FALSE(test, stored);
	KUNIT_EXPECT_EQ(test, failure, MEMCOMP_ANON_STORE_PREFLIGHT);
	mc16_zcar_expect_invalid(test, &f->store[0].zero);
}

static void memcomp_test_zero_carrier_nonzero(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.ready);
	/* Each payload is the first commit of its own real reservation. */
	KUNIT_ASSERT_EQ(test, mc16_zcar_reserve_pending(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_zcar_refresh(f, 0x6b), 0);
	mc16_zcar_expect_invalid(test, &f->store[0].zero);
	KUNIT_ASSERT_EQ(test, mc16_zcar_reserve_pending(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_zcar_refresh(f, 0), 0);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.ready);
	/* Also replace a real ready result with genuinely compressed data. */
	KUNIT_ASSERT_EQ(test, mc16_zcar_reserve_pending(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_zcar_refresh(f, 256), 0);
	mc16_zcar_expect_invalid(test, &f->store[0].zero);
}

static void memcomp_test_zero_carrier_mthp(struct kunit *test)
{
	struct mc16_test_anon_store *f = mc16_test_anon_store_begin(test);
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_reserve(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_anon_ctx_get(f), 0);
	ret = mc16_test_anon_refresh(f);
	mc16_test_anon_ctx_put(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	mc16_zcar_expect_invalid(test, &f->store.zero);
	KUNIT_EXPECT_EQ(test, folio_nr_pages(f->refs->storage->owner->folio), 2L);
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		struct mc16_view view;
		bool zero;

		mutex_lock(memcomp_lock_of(f->guards[i].token));
		zero = mc16_view_locked(&f->guards[i], &view) &&
			mc16_flags_state(*view.flags) == MC16_SAMEFILL &&
			!*view.word && !*view.length;
		mutex_unlock(memcomp_lock_of(f->guards[i].token));
		KUNIT_EXPECT_TRUE(test, zero);
	}
}

static void memcomp_test_zero_carrier_token_mismatch(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct mc16_zero_carrier bad;
	struct mc16_zero_diagnostics before;
	unsigned int refs;
	pte_t orig;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_zc_unmap_anon(f, 7), 0);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.ready);
	orig = mc16_test_zc_read(f, f->addr);
	refs = refcount_read(&f->guard.slot->refs);
	mc16_zero_diagnostics_read(&before);
	bad = f->store[0].zero;
	bad.token = f->tokens[1];
	KUNIT_EXPECT_EQ(test, mc16_zcar_convert(f, &bad), 0U);
	bad = f->store[0].zero;
	bad.ref.token = f->tokens[1];
	KUNIT_EXPECT_EQ(test, mc16_zcar_convert(f, &bad), 0U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_test_zc_read(f, f->addr), orig));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs);
	mc16_test_zc_counts(f, 0, 1);
	mc16_test_zc_events(f, &before, 0);
}

static void memcomp_test_zero_carrier_invalid(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct mc16_zero_carrier bad[6];
	struct mc16_zero_diagnostics before;
	unsigned int i, refs;
	pte_t orig;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_zc_unmap_anon(f, 7), 0);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.ready);
	orig = mc16_test_zc_read(f, f->addr);
	refs = refcount_read(&f->guard.slot->refs);
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		bad[i] = f->store[0].zero;
	bad[0].valid = false;
	bad[1].ready = false;
	bad[2].ref.slot = NULL;
	bad[3].ref.cluster = NULL;
	bad[4].token = 0;
	bad[5].ref.token = 0;
	mc16_zero_diagnostics_read(&before);
	KUNIT_EXPECT_EQ(test, mc16_zcar_convert(f, NULL), 0U);
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		KUNIT_EXPECT_EQ(test, mc16_zcar_convert(f, &bad[i]), 0U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_test_zc_read(f, f->addr), orig));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs);
	mc16_test_zc_events(f, &before, 0);
}

static void memcomp_test_zero_carrier_borrow_owner(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct mc16_zero_diagnostics before;
	struct mc16_slot *slot;
	unsigned int converted, again;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_zc_unmap_anon(f, 7), 0);
	KUNIT_ASSERT_TRUE(test, f->store[0].zero.ready);
	slot = f->guard.slot;
	mc16_ref_put(&f->guard);
	KUNIT_ASSERT_EQ(test, refcount_read(&slot->refs), 2U);
	mc16_zero_diagnostics_read(&before);
	converted = mc16_test_zc_walk(f);
	again = mc16_test_zc_walk(f);
	KUNIT_EXPECT_EQ(test, converted, 1U);
	KUNIT_EXPECT_EQ(test, again, 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&slot->refs), 1U);
	mc16_test_zc_counts(f, 1, 1);
	mc16_test_zc_events(f, &before, 1);
}

static void mc16_zcar_expect_zero_page(struct kunit *test, struct mc16_flt *p)
{
	struct page *page = NULL;
	/* Protects the mapped PTE while its contents are read. */
	spinlock_t *ptl;
	pte_t *ptep, pte = __pte(0);
	pmd_t *pmd;
	void *bytes;
	bool zero;

	mmap_read_lock(p->mm);
	pmd = mm_find_pmd(p->mm, p->addr);
	if (pmd) {
		ptep = pte_offset_map_lock(p->mm, pmd, p->addr, &ptl);
		if (ptep) {
			pte = ptep_get(ptep);
			if (pte_present(pte) && !pte_special(pte)) {
				page = pte_page(pte);
				get_page(page);
			}
			pte_unmap_unlock(ptep, ptl);
		}
	}
	mmap_read_unlock(p->mm);
	KUNIT_ASSERT_TRUE(test, pte_present(pte));
	if (pte_special(pte)) {
		KUNIT_EXPECT_TRUE(test, is_zero_pfn(pte_pfn(pte)));
		return;
	}
	KUNIT_ASSERT_NOT_NULL(test, page);
	bytes = kmap_local_page(page);
	zero = !memchr_inv(bytes, 0, PAGE_SIZE);
	kunmap_local(bytes);
	put_page(page);
	KUNIT_EXPECT_TRUE(test, zero);
}

static void mc16_zcar_lifecycle(struct kunit *test, bool refault)
{
	struct mc16_bm *f = mc16_bm_begin(test, 1, true);
	struct mc16_zero_owners before, after;
	pte_t canonical;
	void *src;
	bool unlocked = false;
	vm_fault_t fault = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	folio_lock(f->page[0].folio);
	src = kmap_local_folio(f->page[0].folio, 0);
	memset(src, 0, PAGE_SIZE);
	kunmap_local(src);
	folio_unlock(f->page[0].folio);
	mc16_zero_owners_read(&before);
	ret = mc16_bm_pageout(&f->page[0]);
	if (ret) {
		KUNIT_FAIL(test, "canonical-zero pageout failed: %d", ret);
		return;
	}
	canonical = mc16_flt_read(f->seed, f->seed->addr);
	/* This read observes PTE state only and adds no token lookup or T. */
	if (!is_swap_pte(canonical) ||
	    pte_to_swp_entry(canonical).val != make_memcomp_entry(0).val) {
		KUNIT_FAIL(test, "pageout did not canonically convert the zero PTE");
		return;
	}
	if (refault) {
		fault = mc16_bm_fault(&f->page[0], &unlocked);
		ret = fault == VM_FAULT_NOPAGE ? 0 : -EIO;
	} else {
		ret = mc16_bm_pte(f, f->seed->addr, false);
	}
	KUNIT_ASSERT_EQ(test, ret, 0);
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f->seed, MM_SWAPENTS), f->seed->swapents);
	if (refault) {
		KUNIT_EXPECT_TRUE(test, unlocked);
		mc16_zcar_expect_zero_page(test, &f->page[0]);
	} else {
		KUNIT_EXPECT_TRUE(test, pte_none(mc16_flt_read(f->seed, f->seed->addr)));
	}
}

static void memcomp_test_zero_carrier_lifecycle_refault1(struct kunit *test)
{
	mc16_zcar_lifecycle(test, true);
}

static void memcomp_test_zero_carrier_lifecycle_exit(struct kunit *test)
{
	mc16_zcar_lifecycle(test, false);
}

#endif /* MC16_ZERO_CONVERT_TEST_ENABLED && MC16_FOLIO_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_ZERO_CARRIER_TEST_H */
