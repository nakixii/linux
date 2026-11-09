/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_BUDGET_PACKED_TEST_H
#define _MM_MEMCOMP_TESTS_BUDGET_PACKED_TEST_H

/* Include after the frozen swapped_test.h. Own mode: do not use raw cleanup. */
#if defined(MC16_FOLIO_TEST_ENABLED) && defined(CONFIG_SWAP)
struct mc16_bpp {
	struct kunit *test;
	struct mc16_bm *bm;
	u32 tokens[MC16_TEST_WORKERS];
	unsigned int nr;
	struct swap_info_struct *device;
	swp_entry_t swap;
	u32 lengths[MC16_TEST_WORKERS];
	bool active, ready;
};

static void mc16_bpp_cleanup(void *data)
{
	struct mc16_bpp *f = data;
	unsigned int i;

	kunit_release_action(f->test, mc16_bm_cleanup, f->bm);
	kunit_release_action(f->test, mc16_flt_cleanup, f->bm->seed);
	/* All fixture PTEs are removed before real swap mode ends. */
	mc16_test_reclaim_retained();
	if (f->device) {
		KUNIT_EXPECT_EQ(f->test, __swap_count(f->swap), 0);
		put_swap_device(f->device);
	}
	if (f->active) {
		KUNIT_EXPECT_TRUE(f->test, xa_empty(&mc16_clusters));
		if (f->ready) {
			for (i = 0; i < f->nr; i++)
				KUNIT_EXPECT_FALSE(f->test, memcomp_entry_present(f->tokens[i]));
		}
		WRITE_ONCE(mc16_kunit_real_swap, false);
		synchronize_rcu();
		f->active = false;
	}
}

static struct mc16_bpp *mc16_bpp_begin(struct kunit *test, unsigned int nr)
{
	struct mc16_bpp *f;
	bool idle;

	if (get_nr_swap_pages() < 1) {
		kunit_skip(test, "requires real guest swap and the raw bridge");
		return NULL;
	}
	if (READ_ONCE(mc16_kunit_real_swap)) {
		KUNIT_FAIL(test, "another real-swap fixture is active");
		return NULL;
	}
	mutex_lock(&memcomp_writeback_lock);
	idle = !mc16_writeback_batch_used && !mc16_writeback_members_used &&
		!mc16_writeback_open_first && !mc16_writeback_open_bytes &&
		!memcomp_writeback_slots_free && !memcomp_writeback_plugged;
	mutex_unlock(&memcomp_writeback_lock);
	if (!idle) {
		KUNIT_FAIL(test, "writeback state is not idle");
		return NULL;
	}
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->bm = mc16_bm_begin(test, nr, false);
	if (!f->bm)
		return NULL;
	if (kunit_add_action_or_reset(test, mc16_bpp_cleanup, f))
		return NULL;
	f->nr = nr;
	f->active = true;
	WRITE_ONCE(mc16_kunit_real_swap, true);
	return f;
}

static int mc16_bpp_fields(struct mc16_bpp *f, unsigned int index, bool swapped,
			   struct mc16_swap_fields *fields)
{
	struct memcomp_ref ref = { };
	struct mc16_view view;
	unsigned int state = swapped ? MC16_SWAPPED : MC16_STORED;
	bool valid;

	if (!mc16_get(f->tokens[index], &ref))
		return -ENOENT;
	mutex_lock(memcomp_lock_of(ref.token));
	valid = mc16_view_locked(&ref, &view) && mc16_flags_valid(*view.flags) &&
		mc16_flags_state(*view.flags) == state && !view.member &&
		!(*view.flags & (MC16_F_OWNER | MC16_F_RESIDENT));
	if (valid && swapped)
		valid = mc16_swap_decode(*view.word, *view.length, fields) && fields->packed;
	else if (valid)
		valid = mc16_length_decode(*view.length, &f->lengths[index]);
	mutex_unlock(memcomp_lock_of(ref.token));
	mc16_ref_put(&ref);
	return valid ? 0 : -EIO;
}

static void mc16_bpp_expect_distinct(struct kunit *test, struct mc16_bpp *f)
{
	u8 *first, *second;
	int different;

	first = kmap_local_folio(f->bm->page[0].folio, 0);
	second = kmap_local_folio(f->bm->page[1].folio, 0);
	different = memcmp(first, second, PAGE_SIZE);
	kunmap_local(second);
	kunmap_local(first);
	KUNIT_EXPECT_NE(test, different, 0);
}

/* The first packed fault leaves the shared swap-cache folio available. */
static void mc16_bpp_expect_tail(struct kunit *test, struct mc16_bpp *f)
{
	struct folio *folio;
	u8 *data, *zero;
	unsigned int i, off = 0;
	bool allocated = false;

	for (i = 0; i < f->nr; i++) {
		KUNIT_ASSERT_GT(test, f->lengths[i], 0U);
		off += f->lengths[i];
	}
	KUNIT_ASSERT_LT(test, off, PAGE_SIZE);
	zero = kunit_kzalloc(test, PAGE_SIZE - off, GFP_KERNEL);
	if (!zero)
		return;
	folio = __read_swap_cache_async(f->swap,
					GFP_HIGHUSER_MOVABLE | __GFP_NOWARN,
					get_task_policy(current), NO_INTERLEAVE_INDEX,
					&allocated, false);
	if (!folio) {
		KUNIT_FAIL(test, "packed swap-cache lookup failed");
		return;
	}
	if (allocated) {
		folio_put(folio);
		KUNIT_FAIL(test, "packed slot was not retained in swap cache");
		return;
	}
	folio_lock(folio);
	if (!folio_test_uptodate(folio)) {
		KUNIT_FAIL(test, "packed swap-cache folio is not uptodate");
		goto out_unlock;
	}
	data = kmap_local_folio(folio, 0);
	KUNIT_EXPECT_EQ(test, memcmp(data + off, zero, PAGE_SIZE - off), 0);
	kunmap_local(data);
out_unlock:
	folio_unlock(folio);
	folio_put(folio);
}

static int mc16_bpp_drain(struct mc16_bpp *f)
{
	struct memcomp_lru *fifo = memcomp_lru_nid(folio_nid(f->bm->page[0].folio));
	unsigned long scanned, freed = 0;
	unsigned int noreclaim;
	bool attempted = false;
	int ret = 0;

	if (!fifo)
		return -EINVAL;
	noreclaim = memalloc_noreclaim_save();
	mutex_lock(&memcomp_writeback_lock);
	scanned = mc16_writeback_drain_queue(folio_nid(f->bm->page[0].folio), fifo,
					     f->nr, &freed, &ret, &attempted);
	mutex_unlock(&memcomp_writeback_lock);
	memalloc_noreclaim_restore(noreclaim);
	return ret ? ret : scanned == f->nr && freed == f->nr &&
		attempted ? 0 : -EIO;
}

/* Page out each token, then validate the shared packed backing layout. */
static int mc16_bpp_prepare(struct mc16_bpp *f)
{
	struct kunit *test = f->test;
	unsigned int i, offset = 0;
	int ret;

	if (f->nr > 1)
		mc16_bpp_expect_distinct(test, f);
	for (i = 0; i < f->nr; i++) {
		pte_t pte;

		if (folio_nid(f->bm->page[i].folio) != folio_nid(f->bm->page[0].folio))
			return -EXDEV;
		ret = mc16_bm_pageout(&f->bm->page[i]);
		if (ret)
			return ret;
		pte = mc16_flt_read(f->bm->seed, f->bm->page[i].addr);
		if (!is_swap_pte(pte) || !is_memcomp_entry(pte_to_swp_entry(pte)))
			return -EINVAL;
		f->tokens[i] = swp_offset(pte_to_swp_entry(pte));
		kunit_info(test, "packed source pageout index=%u token=%u\n", i, f->tokens[i]);
		ret = mc16_bpp_fields(f, i, false, NULL);
		if (ret)
			return ret;
		if (!f->lengths[i] || f->lengths[i] >= PAGE_SIZE ||
		    offset > PAGE_SIZE - f->lengths[i])
			return -E2BIG;
		offset += f->lengths[i];
	}
	ret = mc16_bpp_drain(f);
	if (ret)
		return ret;
	offset = 0;
	for (i = 0; i < f->nr; i++) {
		struct mc16_swap_fields fields;
		swp_entry_t swap;

		ret = mc16_bpp_fields(f, i, true, &fields);
		if (ret)
			return ret;
		swap = swp_entry(fields.type, fields.swap_offset);
		if (fields.byte_offset != offset || fields.length != f->lengths[i])
			return -EIO;
		if (!i) {
			f->swap = swap;
			f->device = get_swap_device(swap);
			if (!f->device)
				return -ENODEV;
		} else if (swap.val != f->swap.val) {
			return -EIO;
		}
		offset += fields.length;
	}
	f->ready = true;
	return __swap_count(f->swap) == f->nr ? 0 : -EIO;
}

static void mc16_bpp_expect_idle(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, memcomp_writeback_slots_free, 0U);
	KUNIT_EXPECT_EQ(test, mc16_writeback_members_used, 0U);
	KUNIT_EXPECT_EQ(test, mc16_writeback_batch_used, 0U);
}

static void mc16_bpp_fault(struct kunit *test, struct mc16_bpp *f, unsigned int index)
{
	bool unlocked;
	vm_fault_t ret;

	ret = mc16_bm_fault(&f->bm->page[index], &unlocked);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret, (__force unsigned int)VM_FAULT_NOPAGE);
	KUNIT_EXPECT_TRUE(test, unlocked);
	mc16_bm_expect_bytes(test, f->bm, index, false);
	if (__swap_count(f->swap) > 0)
		mc16_bpp_expect_tail(test, f);
}

static void mc16_bpp_exit(struct kunit *test, struct mc16_bpp *f, unsigned int index)
{
	int ret;

	ret = mc16_bm_pte(f->bm, f->bm->page[index].addr, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void memcomp_test_budget_packed_writeback(struct kunit *test)
{
	struct mc16_bpp *f = mc16_bpp_begin(test, 2);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_bpp_prepare(f), 0);
	mc16_bpp_expect_idle(test);
	for (i = 0; i < 2; i++) {
		mc16_bpp_fault(test, f, i);
		KUNIT_EXPECT_EQ(test, __swap_count(f->swap), (int)(1 - i));
	}
	mc16_bpp_expect_idle(test);
	kunit_release_action(test, mc16_bpp_cleanup, f);
}

static void memcomp_test_budget_packed_refault1(struct kunit *test)
{
	struct mc16_bpp *f = mc16_bpp_begin(test, 2);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_bpp_prepare(f), 0);
	mc16_bpp_fault(test, f, 0);
	KUNIT_EXPECT_EQ(test, __swap_count(f->swap), 1);
	mc16_bpp_expect_idle(test);
	/* The last sibling exit exercises free_swap_and_cache on the retained page. */
	mc16_bpp_exit(test, f, 1);
	KUNIT_EXPECT_EQ(test, __swap_count(f->swap), 0);
	mc16_bpp_expect_idle(test);
	kunit_release_action(test, mc16_bpp_cleanup, f);
}

static void memcomp_test_budget_packed_exit(struct kunit *test)
{
	struct mc16_bpp *f = mc16_bpp_begin(test, 2);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_bpp_prepare(f), 0);
	for (i = 0; i < 2; i++) {
		mc16_bpp_exit(test, f, i);
		KUNIT_EXPECT_EQ(test, __swap_count(f->swap), (int)(1 - i));
	}
	mc16_bpp_expect_idle(test);
	kunit_release_action(test, mc16_bpp_cleanup, f);
}

/* Parent and the unused +2 child PTE share the fixture's prevalidated PTL. */
static int mc16_bpp_clone(struct mc16_bpp *f)
{
	struct mm_struct *mm = f->bm->seed->mm;
	unsigned long address = f->bm->seed->addr;
	pte_t *ptep, pte;
	spinlock_t *ptl; /* Real parent ownership remains stable through publication. */
	pmd_t *pmd;
	int ret = -EINVAL;

	mmap_read_lock(mm);
	pmd = mm_find_pmd(mm, address);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if ((address >> PMD_SHIFT) == ((address + 2 * PAGE_SIZE) >> PMD_SHIFT) &&
	    pte_none(ptep_get(ptep + 2)) && is_swap_pte(pte) &&
	    is_memcomp_entry(pte_to_swp_entry(pte)) &&
	    memcomp_entry_get(swp_offset(pte_to_swp_entry(pte)))) {
		pte = pte_swp_clear_exclusive(pte);
		set_pte_at(mm, address, ptep, pte);
		set_pte_at(mm, address + 2 * PAGE_SIZE, ptep + 2, pte);
		add_mm_counter(mm, MM_SWAPENTS, 1);
		ret = 0;
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(mm);
	return ret;
}

static void memcomp_test_budget_packed_fork_exit(struct kunit *test)
{
	struct mc16_bpp *f = mc16_bpp_begin(test, 2);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_bpp_prepare(f), 0);
	ret = mc16_bpp_clone(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mc16_bm_pte(f->bm, f->bm->seed->addr, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, __swap_count(f->swap), 2);
	ret = mc16_bm_pte(f->bm, f->bm->seed->addr + 2 * PAGE_SIZE, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, __swap_count(f->swap), 1);
	mc16_bpp_exit(test, f, 1);
	KUNIT_EXPECT_EQ(test, __swap_count(f->swap), 0);
	mc16_bpp_expect_idle(test);
	kunit_release_action(test, mc16_bpp_cleanup, f);
}

static void memcomp_test_budget_packed_refault4(struct kunit *test)
{
	struct mc16_bpp *f = mc16_bpp_begin(test, 4);
	unsigned int i;
	bool done = true;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_bpp_prepare(f), 0);
	for (i = 0; i < 4; i++) {
		struct mc16_bm_worker *w = &f->bm->worker[i];

		w->f = f->bm;
		w->index = i;
		init_completion(&w->ready);
		init_completion(&w->done);
		w->task = kthread_run(mc16_bm_worker, w, "mc16-packed-budget-%u", i);
		if (IS_ERR(w->task)) {
			w->task = NULL;
			KUNIT_FAIL(test, "packed worker allocation failed");
			return;
		}
		get_task_struct(w->task);
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&w->ready, HZ), 0UL);
	}
	complete_all(&f->bm->go);
	for (i = 0; i < 4; i++)
		done &= !!wait_for_completion_timeout(&f->bm->worker[i].done, 10 * HZ);
	if (!done) {
		KUNIT_FAIL(test, "packed fault timed out");
		return;
	}
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, (__force unsigned int)f->bm->worker[i].ret,
				(__force unsigned int)VM_FAULT_NOPAGE);
		KUNIT_EXPECT_TRUE(test, f->bm->worker[i].unlocked);
		mc16_bm_expect_bytes(test, f->bm, i, false);
	}
	KUNIT_EXPECT_EQ(test, __swap_count(f->swap), 0);
	mc16_bpp_expect_idle(test);
	kunit_release_action(test, mc16_bpp_cleanup, f);
}

/* A boot-time no-swap skip must not poison the later real-swap rerun. */
static int mc16_bpp_suite_init(struct kunit_suite *suite)
{
	struct kunit_case *test_case;

	for (test_case = suite->test_cases; test_case->run_case; test_case++)
		test_case->status = KUNIT_SUCCESS;
	return 0;
}

static struct kunit_case mc16_bpp_cases[] = {
	KUNIT_CASE(memcomp_test_budget_packed_writeback),
	KUNIT_CASE(memcomp_test_budget_packed_refault1),
	KUNIT_CASE(memcomp_test_budget_packed_refault4),
	KUNIT_CASE(memcomp_test_budget_packed_fork_exit),
	KUNIT_CASE(memcomp_test_budget_packed_exit),
	{ }
};

static struct kunit_suite mc16_bpp_suite = {
	.name = "swap-memcomp-packed-budget",
	.suite_init = mc16_bpp_suite_init,
	.test_cases = mc16_bpp_cases,
};

kunit_test_suite(mc16_bpp_suite);
#endif /* MC16_FOLIO_TEST_ENABLED && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_BUDGET_PACKED_TEST_H */
