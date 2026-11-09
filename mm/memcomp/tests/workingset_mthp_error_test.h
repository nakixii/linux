/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_WORKINGSET_MTHP_ERROR_TEST_H
#define _MM_MEMCOMP_TESTS_WORKINGSET_MTHP_ERROR_TEST_H

/* Include after workingset_load_test.h; reuse its real MM/load fixture. */
#ifdef MC16_WORKINGSET_MTHP_TEST_ENABLED
struct mc16_wl_mthp_error_read {
	unsigned long token;
	unsigned int order, pages;
	int result;
	bool locked, swapbacked, workingset, cleared, contents;
};

struct mc16_wl_mthp_error {
	struct mc16_wl *wl;
	struct mc16_wl_mthp_error_read read[1 + MC16_FT_PAGES];
	unsigned int nr;
	bool fail;
};

static int mc16_wl_mthp_error_read_page(struct memcomp_ref *ref, void *dst)
{
	struct mc16_wl_mthp_error *o = mc16_fault_probe.data;
	struct mc16_wl *f = o->wl;
	struct mc16_wl_mthp_error_read *r = NULL;
	struct folio *folio = page_folio(kmap_to_page(dst));
	unsigned int n = o->nr++, i;
	int ret;

	/* Snapshot while the actual destination is still locked and alive. */
	if (n < ARRAY_SIZE(o->read)) {
		r = &o->read[n];
		r->token = ref->token;
		r->order = folio_order(folio);
		r->pages = folio_nr_pages(folio);
		r->locked = folio_test_locked(folio);
		r->swapbacked = folio_test_swapbacked(folio);
		r->workingset = folio_test_workingset(folio);
		r->cleared = true;
		for (i = 0; i < f->nr; i++)
			r->cleared &= !mc16_wl_get_shadow(&f->refs[i]);
	}
	/* The existing observer calls the original mc16_load_page(). */
	ret = mc16_wl_read_page(ref, dst);
	if (r) {
		r->result = ret;
		for (i = 0; i < f->nr; i++) {
			void *src = f->fault->src + i * PAGE_SIZE;

			if (ref->token == f->fault->tokens[i])
				r->contents = !memcmp(dst, src, PAGE_SIZE);
		}
	}
	/* The old observer reinstalls itself; retain this observer on retry. */
	kunit_activate_static_stub(f->test, mc16_load_page,
				   mc16_wl_mthp_error_read_page);
	return ret ? ret : (o->fail ? -EIO : 0);
}

static const bool mc16_wl_mthp_error_cases[] = { false, true };

static void mc16_wl_mthp_error_desc(const bool *empty, char *desc)
{
	strscpy(desc, *empty ? "empty-first-read-error" : "first-only-read-error",
		KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(mc16_wl_mthp_error, mc16_wl_mthp_error_cases,
		  mc16_wl_mthp_error_desc);

static void memcomp_test_workingset_load_mthp_read_error(struct kunit *test)
{
	bool empty = *(const bool *)test->param_value;
	struct mc16_wl *f = mc16_wl_begin(test, MC16_FT_PAGES, true);
	struct mc16_wl_mthp_error *o;
	unsigned long shadow;
	unsigned int i;
	vm_fault_t ret;
	long before;

	KUNIT_ASSERT_NOT_NULL(test, f);
	o = kunit_kzalloc(test, sizeof(*o), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, o);
	o->wl = f;
	o->fail = true;
	f->fault->orders = READ_ONCE(huge_anon_orders_always);
	f->fault->saved_orders = true;
	WRITE_ONCE(huge_anon_orders_always, f->fault->orders | BIT(2));
	mc16_ft_policy(f->fault, VM_HUGEPAGE, VM_NOHUGEPAGE);
	KUNIT_ASSERT_NE(test, THP_ORDERS_ALL_ANON & BIT(2), 0UL);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	for (i = 0; i < f->nr; i++) {
		unsigned long value = (empty ? i != 0 : i == 0) ? shadow : 0;

		mc16_wl_set_shadow(&f->refs[i], value);
	}
	/* Inject after a real successful read; native refault is untouched. */
	mc16_wl_observe(f, 0, false);
	kunit_activate_static_stub(test, mc16_load_page,
				   mc16_wl_mthp_error_read_page);
	before = mc16_wl_refaults();
	mc16_ft_probe_start(f->fault);
	mc16_fault_probe.data = o;
	ret = mc16_wl_fault(f->fault, 2);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_SIGBUS);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.large_allocs, 1U);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.small_allocs, 0U);
	KUNIT_EXPECT_EQ(test, f->reads, 1U);
	/* Stock sees only the span's first shadow, even if the read fails. */
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before,
			empty ? 0L : (long)MC16_FT_PAGES);
	KUNIT_ASSERT_EQ(test, o->nr, 1U);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[i]), 0ULL);
		KUNIT_EXPECT_TRUE(test, pte_same(mc16_ft_read(f->fault, i),
						 mc16_ft_orig(f->fault, i)));
	}
	mc16_ft_counts(f->fault, MC16_FT_PAGES, 0);
	/* Retry the original middle token PTE, without replenishing shadow. */
	o->fail = false;
	mc16_ft_probe_start(f->fault);
	mc16_fault_probe.data = o;
	ret = mc16_wl_fault(f->fault, 2);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.large_allocs, 1U);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.small_allocs, 0U);
	KUNIT_EXPECT_EQ(test, f->reads, 1U + MC16_FT_PAGES);
	/* Every shadow is already cleared, so the retry adds no refault. */
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before,
			empty ? 0L : (long)MC16_FT_PAGES);
	KUNIT_ASSERT_EQ(test, o->nr, 1U + MC16_FT_PAGES);
	for (i = 0; i < o->nr; i++) {
		struct mc16_wl_mthp_error_read *a = &o->read[i];

		KUNIT_EXPECT_EQ(test, a->order, 2U);
		KUNIT_EXPECT_EQ(test, a->pages, (unsigned int)MC16_FT_PAGES);
		KUNIT_EXPECT_EQ(test, a->token, f->fault->tokens[i ? i - 1 : 0]);
		KUNIT_EXPECT_EQ(test, a->result, 0);
		KUNIT_EXPECT_TRUE(test, a->contents);
		KUNIT_EXPECT_TRUE(test, a->locked);
		KUNIT_EXPECT_TRUE(test, a->swapbacked);
		KUNIT_EXPECT_TRUE(test, a->cleared);
		/*
		 * Classic recency needs a refault distance the fixture lacks;
		 * MGLRU sets it in-fault if lruvec and generation still match.
		 */
		if (i || empty)
			KUNIT_EXPECT_FALSE(test, a->workingset);
		else if (lru_gen_enabled())
			KUNIT_EXPECT_TRUE(test, a->workingset);
	}
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[i]), 0ULL);
		mc16_ft_present(f->fault, i, 2, MC16_FT_PAGES);
	}
	mc16_ft_counts(f->fault, 0, MC16_FT_PAGES);
}
#endif /* MC16_WORKINGSET_MTHP_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_WORKINGSET_MTHP_ERROR_TEST_H */
