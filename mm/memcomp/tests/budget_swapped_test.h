/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_BUDGET_SWAPPED_TEST_H
#define _MM_MEMCOMP_TESTS_BUDGET_SWAPPED_TEST_H

/* Include after budget_test.h; only run serially in an isolated swap guest. */
#if defined(MC16_FOLIO_TEST_ENABLED) && defined(CONFIG_SWAP)
struct mc16_bw {
	struct kunit *test;
	struct mc16_bm *bm;
	swp_entry_t swap[MC16_TEST_WORKERS];
	struct swap_info_struct *device[MC16_TEST_WORKERS];
	bool real;
};

static void mc16_bw_cleanup(void *data)
{
	struct mc16_bw *f = data;
	unsigned int i;

	/* Join workers, stop the budget, then remove ALL PTEs while puts are real. */
	kunit_release_action(f->test, mc16_bm_cleanup, f->bm);
	kunit_release_action(f->test, mc16_flt_cleanup, f->bm->seed);
	/* All fixture PTEs are removed before real swap mode ends. */
	mc16_test_reclaim_retained();
	for (i = 0; i < MC16_TEST_WORKERS; i++) {
		if (!f->device[i])
			continue;
		KUNIT_EXPECT_EQ(f->test, __swap_count(f->swap[i]), 0);
		put_swap_device(f->device[i]);
		f->device[i] = NULL;
	}
	if (f->real) {
		KUNIT_EXPECT_TRUE(f->test, xa_empty(&mc16_clusters));
		WRITE_ONCE(mc16_kunit_real_swap, false);
		synchronize_rcu();
		f->real = false;
	}
}

static struct mc16_bw *mc16_bw_begin(struct kunit *test, unsigned int nr)
{
	struct mc16_bw *f;
	bool idle;

	if (get_nr_swap_pages() < nr) {
		kunit_skip(test, "requires active real swap; rerun after guest swapon");
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
		KUNIT_FAIL(test, "writeback staging or reservation pool is not idle");
		return NULL;
	}
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->bm = mc16_bm_begin(test, nr, false);
	if (!f->bm)
		return NULL;
	if (kunit_add_action_or_reset(test, mc16_bw_cleanup, f))
		return NULL;
	f->real = true;
	WRITE_ONCE(mc16_kunit_real_swap, true);
	return f;
}

/* The real drain has one W scope and no empty final selector (limit is one). */
static int mc16_bw_drain(struct mc16_flt *p)
{
	struct memcomp_lru *fifo = memcomp_lru_nid(folio_nid(p->folio));
	unsigned long scanned, freed = 0;
	unsigned int noreclaim;
	bool attempted = false;
	int ret = 0;

	if (!fifo)
		return -EINVAL;
	noreclaim = memalloc_noreclaim_save();
	mutex_lock(&memcomp_writeback_lock);
	scanned = mc16_writeback_drain_queue(folio_nid(p->folio), fifo, 1,
					     &freed, &ret, &attempted);
	mutex_unlock(&memcomp_writeback_lock);
	memalloc_noreclaim_restore(noreclaim);
	return ret ? ret : scanned == 1 && attempted && freed == 1 ? 0 : -EIO;
}

/* Capture effective fields under T, then retain only a real swap-device pin. */
static int mc16_bw_capture(struct mc16_bw *f, unsigned int index)
{
	struct mc16_flt *p = &f->bm->page[index];
	struct memcomp_ref ref = { };
	struct mc16_swap_fields fields;
	struct mc16_view view;
	unsigned long token;
	bool valid;

	/* Successful source release clears folio->swap: read the actual token PTE. */
	{
		pte_t pte = mc16_flt_read(f->bm->seed, p->addr);

		if (!is_swap_pte(pte) || !is_memcomp_entry(pte_to_swp_entry(pte)))
			return -EINVAL;
		token = swp_offset(pte_to_swp_entry(pte));
	}
	if (!mc16_get(token, &ref))
		return -ENOENT;
	mutex_lock(memcomp_lock_of(token));
	valid = mc16_view_locked(&ref, &view) && mc16_flags_valid(*view.flags) &&
		mc16_flags_state(*view.flags) == MC16_SWAPPED &&
		!(*view.flags & (MC16_F_OWNER | MC16_F_RESIDENT)) && !view.member &&
		mc16_swap_decode(*view.word, *view.length, &fields) &&
		!fields.packed && !fields.byte_offset && fields.length;
	mutex_unlock(memcomp_lock_of(token));
	mc16_ref_put(&ref);
	if (!valid)
		return -EIO;
	f->swap[index] = swp_entry(fields.type, fields.swap_offset);
	f->device[index] = get_swap_device(f->swap[index]);
	if (!f->device[index])
		return -ENODEV;
	return __swap_count(f->swap[index]) == 1 ? 0 : -EIO;
}

static void mc16_bw_expect_idle(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, memcomp_writeback_slots_free, 0U);
	KUNIT_EXPECT_EQ(test, mc16_writeback_batch_used, 0U);
	KUNIT_EXPECT_EQ(test, mc16_writeback_members_used, 0U);
}

static int mc16_bw_prepare(struct mc16_bw *f, unsigned int index)
{
	int ret = mc16_bm_pageout(&f->bm->page[index]);

	return ret ? ret : mc16_bw_drain(&f->bm->page[index]);
}

enum mc16_bw_op { MC16_BW_PAGEOUT, MC16_BW_REFAULT, MC16_BW_FORK, MC16_BW_EXIT };

static void mc16_bw_one(struct kunit *test, enum mc16_bw_op op)
{
	struct mc16_bw *f = mc16_bw_begin(test, 1);
	struct mc16_bm *bm;
	bool unlocked = false;
	vm_fault_t fault = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	bm = f->bm;
	if (op != MC16_BW_PAGEOUT) {
		KUNIT_ASSERT_EQ(test, mc16_bw_prepare(f, 0), 0);
		KUNIT_ASSERT_EQ(test, mc16_bw_capture(f, 0), 0);
	}
	switch (op) {
	case MC16_BW_PAGEOUT:
		ret = mc16_bw_prepare(f, 0);
		break;
	case MC16_BW_REFAULT:
		fault = mc16_bm_fault(&bm->page[0], &unlocked);
		ret = fault == VM_FAULT_NOPAGE ? 0 : -EIO;
		break;
	case MC16_BW_FORK:
		ret = mc16_bm_pte(bm, bm->seed->addr + PAGE_SIZE, true);
		break;
	default:
		ret = mc16_bm_pte(bm, bm->seed->addr, false);
		break;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);
	switch (op) {
	case MC16_BW_PAGEOUT:
		KUNIT_ASSERT_EQ(test, mc16_bw_capture(f, 0), 0);
				break;
	case MC16_BW_REFAULT:
		KUNIT_EXPECT_TRUE(test, unlocked);
		mc16_bm_expect_bytes(test, bm, 0, false);
		break;
	case MC16_BW_FORK:
		KUNIT_EXPECT_EQ(test, __swap_count(f->swap[0]), 1);
		break;
	default:
		KUNIT_EXPECT_EQ(test, __swap_count(f->swap[0]), 0);
		break;
	}
	mc16_bw_expect_idle(test);
	kunit_release_action(test, mc16_bw_cleanup, f);
}

static void memcomp_test_budget_swapped_pageout(struct kunit *test)
{
	mc16_bw_one(test, MC16_BW_PAGEOUT);
}

static void memcomp_test_budget_swapped_refault1(struct kunit *test)
{
	mc16_bw_one(test, MC16_BW_REFAULT);
}

static void memcomp_test_budget_swapped_fork(struct kunit *test)
{
	mc16_bw_one(test, MC16_BW_FORK);
}

static void memcomp_test_budget_swapped_exit(struct kunit *test)
{
	mc16_bw_one(test, MC16_BW_EXIT);
}

static void memcomp_test_budget_swapped_refault4(struct kunit *test)
{
	struct mc16_bw *f = mc16_bw_begin(test, MC16_TEST_WORKERS);
	struct mc16_bm *bm;
	unsigned int i;
	bool done = true;

	KUNIT_ASSERT_NOT_NULL(test, f);
	bm = f->bm;
	for (i = 0; i < MC16_TEST_WORKERS; i++) {
		struct mc16_bm_worker *w = &bm->worker[i];

		/* Separate raw slots; setup is outside the refault measurement. */
		KUNIT_ASSERT_EQ(test, mc16_bw_prepare(f, i), 0);
		KUNIT_ASSERT_EQ(test, mc16_bw_capture(f, i), 0);
		w->f = bm;
		w->index = i;
		init_completion(&w->ready);
		init_completion(&w->done);
		w->task = kthread_run(mc16_bm_worker, w, "mc16-swap-budget-%u", i);
		if (IS_ERR(w->task)) {
			w->task = NULL;
			KUNIT_FAIL(test, "refault worker allocation failed");
			return;
		}
		get_task_struct(w->task);
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&w->ready, HZ), 0UL);
	}
	complete_all(&bm->go);
	for (i = 0; i < MC16_TEST_WORKERS; i++)
		done &= !!wait_for_completion_timeout(&bm->worker[i].done, 10 * HZ);
	if (!done) {
		KUNIT_FAIL(test, "real swap fault timed out");
		return;
	}
	for (i = 0; i < MC16_TEST_WORKERS; i++) {
		KUNIT_EXPECT_EQ(test, (__force unsigned int)bm->worker[i].ret,
				(__force unsigned int)VM_FAULT_NOPAGE);
		KUNIT_EXPECT_TRUE(test, bm->worker[i].unlocked);
		mc16_bm_expect_bytes(test, bm, i, false);
		KUNIT_EXPECT_EQ(test, __swap_count(f->swap[i]), 0);
	}
	mc16_bw_expect_idle(test);
	kunit_release_action(test, mc16_bw_cleanup, f);
}

/* A boot-time no-swap skip must not poison the later real-swap rerun. */
static int mc16_bw_suite_init(struct kunit_suite *suite)
{
	struct kunit_case *test_case;

	for (test_case = suite->test_cases; test_case->run_case; test_case++)
		test_case->status = KUNIT_SUCCESS;
	return 0;
}

/* This suite is deliberately excluded by the ordinary memcomp* filter. */
static struct kunit_case mc16_bw_cases[] = {
	KUNIT_CASE(memcomp_test_budget_swapped_pageout),
	KUNIT_CASE(memcomp_test_budget_swapped_refault1),
	KUNIT_CASE(memcomp_test_budget_swapped_refault4),
	KUNIT_CASE(memcomp_test_budget_swapped_fork),
	KUNIT_CASE(memcomp_test_budget_swapped_exit),
	{ }
};

static struct kunit_suite mc16_bw_suite = {
	.name = "swap-memcomp-budget",
	.suite_init = mc16_bw_suite_init,
	.test_cases = mc16_bw_cases,
};

kunit_test_suite(mc16_bw_suite);
#endif /* MC16_FOLIO_TEST_ENABLED && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_BUDGET_SWAPPED_TEST_H */
