/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_BUDGET_TEST_H
#define _MM_MEMCOMP_TESTS_BUDGET_TEST_H

/* Include after folio_test.h. Public positive-token dispatch uses clustered slots. */
#ifdef MC16_FOLIO_TEST_ENABLED
#define MC16_TEST_WORKERS 4
struct mc16_bm;
struct mc16_bm_worker {
	struct mc16_bm *f;
	unsigned int index;
	struct task_struct *task;
	struct completion ready;
	struct completion done;
	vm_fault_t ret;
	bool unlocked;
};

struct mc16_bm {
	struct mc16_flt *seed;
	struct mc16_flt page[4];
	unsigned int nr;
	struct completion go;
	struct mc16_bm_worker worker[4];
};

/* Actual present/token PTE transaction; no lookup retained for assertions. */
static int mc16_bm_pte(struct mc16_bm *f, unsigned long address, bool clone)
{
	struct mm_struct *mm = f->seed->mm;
	unsigned long parent = f->seed->addr;
	pte_t orig, *ptep, *child;
	spinlock_t *ptl; /* Serializes the real PTE ownership transaction. */
	pmd_t *pmd;
	int ret = -ENOENT;

	mmap_read_lock(mm);
	pmd = mm_find_pmd(mm, clone ? parent : address);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(mm, pmd, clone ? parent : address, &ptl);
	if (!ptep)
		goto out;
	orig = ptep_get(ptep);
	if (clone) {
		/* Fixture child is adjacent, in this same prevalidated PTE table. */
		child = ptep + 1;
		if (address != parent + PAGE_SIZE ||
		    (parent >> PMD_SHIFT) != (address >> PMD_SHIFT) ||
		    !pte_none(ptep_get(child)) || !is_swap_pte(orig) ||
		    !is_memcomp_entry(pte_to_swp_entry(orig))) {
			ret = -EINVAL;
		} else if (!memcomp_entry_get(swp_offset(pte_to_swp_entry(orig)))) {
			ret = -ENOENT;
		} else {
			/* The parent PTL remains held across get and child publication. */
			orig = pte_swp_clear_exclusive(orig);
			set_pte_at(mm, parent, ptep, orig);
			set_pte_at(mm, address, child, orig);
			add_mm_counter(mm, MM_SWAPENTS, 1);
			ret = 0;
		}
	} else {
		ret = 0;
		if (is_swap_pte(orig) && is_memcomp_entry(pte_to_swp_entry(orig))) {
			ptep_get_and_clear(mm, address, ptep);
			add_mm_counter(mm, MM_SWAPENTS, -1);
			memcomp_invalidate(swp_offset(pte_to_swp_entry(orig)));
		}
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(mm);
	return ret;
}

static void mc16_bm_cleanup(void *data)
{
	struct mc16_bm *f = data;
	unsigned int i;

	complete_all(&f->go);
	for (i = 0; i < ARRAY_SIZE(f->worker); i++) {
		if (f->worker[i].task) {
			kthread_stop(f->worker[i].task);
			put_task_struct(f->worker[i].task);
		}
	}
	/* The extra child branch lives inside the seed's existing mapping. */
	mc16_bm_pte(f, f->seed->addr + PAGE_SIZE, false);
	for (i = 1; i < f->nr; i++) {
		struct mc16_flt *p = &f->page[i];

		mc16_flt_zap(p, p->addr);
		if (p->folio) {
			folio_lock(p->folio);
			memcomp_invalidate_folio(p->folio);
			folio_unlock(p->folio);
			mc16_test_dontneed(p->mm, p->addr, PAGE_SIZE);
			lru_add_drain_all();
			folio_put(p->folio);
		}
		if (p->anon_vma)
			put_anon_vma(p->anon_vma);
	}
	f->seed->base->model.folio = f->seed->folio;
}

static struct mc16_bm *mc16_bm_begin(struct kunit *test, unsigned int nr,
				     bool samefill)
{
	struct mc16_bm *f;
	unsigned int i;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->seed = mc16_flt_begin(test, 0);
	if (!f->seed)
		return NULL;
	init_completion(&f->go);
	if (kunit_add_action_or_reset(test, mc16_bm_cleanup, f))
		return NULL;
	f->nr = 1;
	f->page[0] = *f->seed;
	for (i = 1; i < nr; i++) {
		struct mc16_flt *p = &f->page[i];

		p->test = test;
		p->base = f->seed->base;
		p->mm = f->seed->mm;
		p->addr = f->seed->addr + i * PAGE_SIZE;
		f->nr++;
		if (mc16_flt_map(p, 0))
			return NULL;
	}
	for (i = 0; i < nr; i++) {
		u8 *data = kmap_local_folio(f->page[i].folio, 0);

		memset(data, 0x51 + i, PAGE_SIZE);
		if (!samefill)
			data[17] ^= 0xff;
		kunmap_local(data);
	}
	return f;
}

/* Reserve -> actual rmap/PTE replacement -> public store -> source OWNER drop. */
static int mc16_bm_pageout(struct mc16_flt *p)
{
	struct memcomp_reclaim_ctx *ctx;
	unsigned int flags;
	bool reserved, stored, released = false;

	p->base->model.folio = p->folio;
	folio_lock(p->folio);
	reserved = memcomp_reserve(p->folio);
	folio_unlock(p->folio);
	if (!reserved)
		return -ENOMEM;
	if (mc16_flt_escape(p, 0))
		return -EINVAL;
	lru_add_drain_all();
	if (folio_test_lru(p->folio)) {
		if (!folio_isolate_lru(p->folio))
			return -EBUSY;
		/* Model reclaim isolation, not migration: the source is inactive. */
		folio_clear_active(p->folio);
		folio_put(p->folio); /* Keep only the original fixture hold. */
	}
	flags = memcomp_test_reclaimer_enter();
	folio_lock(p->folio);
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_KSWAPD);
	if (!ctx) {
		folio_unlock(p->folio);
		memcomp_test_reclaimer_exit(flags);
		return -EBUSY;
	}
	stored = memcomp_store_after_unmap(p->folio, &ctx);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	if (stored) {
		released = memcomp_release_folio(p->folio);
		/* Only keep the fixture's original physical folio alive. */
		if (released)
			folio_ref_unfreeze(p->folio, 1);
	}
	folio_unlock(p->folio);
	memcomp_test_reclaimer_exit(flags);
	return stored && released ? 0 : -EIO;
}

static vm_fault_t mc16_bm_fault(struct mc16_flt *p, bool *unlocked)
{
	struct vm_area_struct *vma;
	vm_fault_t ret = VM_FAULT_SIGBUS;
	bool locked = true;
	pte_t orig;
	pte_t *ptep;
	spinlock_t *ptl; /* Serializes the real PTE ownership transaction. */
	pmd_t *pmd;

	*unlocked = false;
	mmap_read_lock(p->mm);
	vma = vma_lookup(p->mm, p->addr);
	pmd = mm_find_pmd(p->mm, p->addr);
	if (!vma || !pmd)
		goto out;
	ptep = pte_offset_map_lock(p->mm, pmd, p->addr, &ptl);
	if (!ptep)
		goto out;
	orig = ptep_get(ptep);
	pte_unmap_unlock(ptep, ptl);
	{
		struct vm_fault vmf = {
			.vma = vma, .address = p->addr, .real_address = p->addr,
			.flags = FAULT_FLAG_ORIG_PTE_VALID,
			.pmd = pmd, .orig_pte = orig,
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};

		ret = memcomp_load(&vmf, &locked);
		*unlocked = !locked && !vmf.pte;
	}
out:
	mmap_read_unlock(p->mm);
	return ret;
}

/* Pin the installed page under PTL; all oracle work follows measurement. */
static void mc16_bm_expect_bytes(struct kunit *test, struct mc16_bm *f,
				 unsigned int index, bool samefill)
{
	struct mm_struct *mm = f->seed->mm;
	unsigned long address = f->page[index].addr;
	struct page *page = NULL;
	spinlock_t *ptl; /* Protects the PTE until the page pin is acquired. */
	u8 *expected, *actual;
	pte_t *ptep, pte;
	pmd_t *pmd;
	int different;

	expected = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, expected);
	memset(expected, 0x51 + index, PAGE_SIZE);
	if (!samefill)
		expected[17] ^= 0xff;
	mmap_read_lock(mm);
	pmd = mm_find_pmd(mm, address);
	if (!pmd)
		goto out_unlock;
	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!ptep)
		goto out_unlock;
	pte = ptep_get(ptep);
	if (pte_present(pte) && !pte_special(pte)) {
		page = pte_page(pte);
		get_page(page);
	}
	pte_unmap_unlock(ptep, ptl);
out_unlock:
	mmap_read_unlock(mm);
	KUNIT_ASSERT_NOT_NULL(test, page);
	actual = kmap_local_page(page);
	different = memcmp(actual, expected, PAGE_SIZE);
	kunmap_local(actual);
	put_page(page);
	KUNIT_EXPECT_EQ(test, different, 0);
}

/* Real PTE operations remain covered without operation-budget probes. */
enum mc16_bm_op { MC16_BM_PAGEOUT, MC16_BM_REFAULT, MC16_BM_FORK, MC16_BM_EXIT };

static void mc16_bm_one(struct kunit *test, enum mc16_bm_op op, bool samefill)
{
	struct mc16_bm *f = mc16_bm_begin(test, 1, samefill);
	bool unlocked = false;
	vm_fault_t fault = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	if (op != MC16_BM_PAGEOUT)
		KUNIT_ASSERT_EQ(test, mc16_bm_pageout(&f->page[0]), 0);
	switch (op) {
	case MC16_BM_PAGEOUT:
		ret = mc16_bm_pageout(&f->page[0]);
		break;
	case MC16_BM_REFAULT:
		fault = mc16_bm_fault(&f->page[0], &unlocked);
		ret = (fault & VM_FAULT_NOPAGE) && (fault & VM_FAULT_MAJOR) ?
			0 : -EIO;
		break;
	case MC16_BM_FORK:
		ret = mc16_bm_pte(f, f->seed->addr + PAGE_SIZE, true);
		break;
	default:
		ret = mc16_bm_pte(f, f->seed->addr, false);
		break;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);
	switch (op) {
	case MC16_BM_PAGEOUT:
		break;
	case MC16_BM_REFAULT:
		KUNIT_EXPECT_TRUE(test, unlocked);
		KUNIT_EXPECT_TRUE(test, pte_present(mc16_flt_read(f->seed, f->seed->addr)));
		mc16_bm_expect_bytes(test, f, 0, samefill);
		/* Stable non-PENDING tags skip the resident actor T. */
		break;
	case MC16_BM_FORK:
		break;
	default:
		break;
	}
}

static int mc16_bm_worker(void *data)
{
	struct mc16_bm_worker *w = data;
	struct mc16_flt *p = &w->f->page[w->index];

	/* Keep deterministic work scheduling attached to the live parent test. */
	current->kunit_test = p->test;
	complete(&w->ready);
	wait_for_completion(&w->f->go);
	kthread_use_mm(p->mm);
	w->ret = mc16_bm_fault(p, &w->unlocked);
	kthread_unuse_mm(p->mm);
	KUNIT_EXPECT_FALSE(p->test, delayed_work_pending(&mc16_retained_aging));
	current->kunit_test = NULL;
	complete(&w->done);
	return 0;
}

static void mc16_bm_four(struct kunit *test, bool samefill)
{
	struct mc16_bm *f = mc16_bm_begin(test, 4, samefill);
	unsigned int i;
	bool ready = true;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < 4; i++) {
		struct mc16_bm_worker *w = &f->worker[i];

		KUNIT_ASSERT_EQ(test, mc16_bm_pageout(&f->page[i]), 0);
		w->f = f;
		w->index = i;
		init_completion(&w->ready);
		init_completion(&w->done);
		w->task = kthread_run(mc16_bm_worker, w, "mc16-budget-%u", i);
		if (IS_ERR(w->task)) {
			w->task = NULL;
			KUNIT_FAIL(test, "refault worker allocation failed");
			return;
		}
		get_task_struct(w->task);
		ready &= !!wait_for_completion_timeout(&w->ready, HZ);
	}
	KUNIT_ASSERT_TRUE(test, ready);
	complete_all(&f->go);
	for (i = 0; i < 4; i++)
		ready &= !!wait_for_completion_timeout(&f->worker[i].done, 10 * HZ);
	if (!ready) {
		KUNIT_FAIL(test, "refault worker timed out");
		return;
	}
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, (__force unsigned int)f->worker[i].ret,
				(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
		KUNIT_EXPECT_TRUE(test, f->worker[i].unlocked);
		KUNIT_EXPECT_TRUE(test, pte_present(mc16_flt_read(f->seed,
								  f->page[i].addr)));
		mc16_bm_expect_bytes(test, f, i, samefill);
	}
}

static void memcomp_test_budget_pageout_stored(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_PAGEOUT, false);
}

static void memcomp_test_budget_pageout_samefill(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_PAGEOUT, true);
}

static void memcomp_test_budget_refault1_stored(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_REFAULT, false);
}

static void memcomp_test_budget_refault1_samefill(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_REFAULT, true);
}

static void memcomp_test_budget_fork_stored(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_FORK, false);
}

static void memcomp_test_budget_fork_samefill(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_FORK, true);
}

static void memcomp_test_budget_exit_stored(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_EXIT, false);
}

static void memcomp_test_budget_exit_samefill(struct kunit *test)
{
	mc16_bm_one(test, MC16_BM_EXIT, true);
}

static void memcomp_test_budget_refault4_stored(struct kunit *test)
{
	mc16_bm_four(test, false);
}

static void memcomp_test_budget_refault4_samefill(struct kunit *test)
{
	mc16_bm_four(test, true);
}

#ifdef CONFIG_MEMCG
struct mc16_bs {
	struct mc16_consumer_test *base;
	unsigned long tokens[MC16_CLUSTER_SLOTS];
	bool held[MC16_CLUSTER_SLOTS];
};

static void mc16_bs_cleanup(void *data)
{
	struct mc16_bs *f = data;
	unsigned int i;

	folio_lock(f->base->account);
	memcomp_rollback_folio(f->base->account);
	folio_unlock(f->base->account);
	for (i = 0; i < MC16_CLUSTER_SLOTS; i++) {
		if (f->held[i])
			memcomp_invalidate(f->tokens[i]);
	}
	rcu_barrier();
}

/* A real reference holder, explicitly not a real PTE or the normal MM scene. */
static int mc16_bs_publish(struct mc16_bs *f, unsigned int index)
{
	struct folio *folio = f->base->account;
	struct memcomp_reclaim_ctx *ctx = NULL;
	bool success = false;
	int ret = -ENOMEM;

	folio_lock(folio);
	if (!memcomp_reserve(folio))
		goto out;
	f->tokens[index] = swp_offset(folio->swap);
	if (!memcomp_entry_get(f->tokens[index]))
		goto out;
	f->held[index] = true;
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_DIRECT);
	if (!ctx)
		goto out;
	if (!memcomp_store_after_unmap(folio, &ctx)) {
		ret = -EIO;
		goto out;
	}
	success = memcomp_release_folio(folio);
	if (success)
		folio_ref_unfreeze(folio, 1);
	ret = success ? 0 : -EIO;
out:
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	folio_unlock(folio);
	return ret;
}

static void memcomp_test_budget_fixed512(struct kunit *test)
{
	struct mc16_bs *f;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct mc16_storage_debug before, after;
	static const u64 expected[MC16_META_CLASSES] = {
		[MC16_META_SEGMENT] = 8,
		[MC16_META_CLUSTER] = 1,
		[MC16_META_KEY] = 1,
		[MC16_META_DESCRIPTOR] = 8,
		[MC16_META_SHADOW] = 8,
	};
#endif
	unsigned int i;
	u8 *src;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->base = mc16_consumer_test_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f->base);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mc16_bs_cleanup, f), 0);
	f->base->account->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(f->base->account);
	src = kmap_local_folio(f->base->account, 0);
	memset(src, 0x67, PAGE_SIZE);
	kunmap_local(src);
	/* Start from an empty publication registry. */
	KUNIT_ASSERT_TRUE(test, xa_empty(&mc16_clusters));
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&before);
#endif
	for (i = 0; i < MC16_CLUSTER_SLOTS; i++) {
		int ret = mc16_bs_publish(f, i);

		if (ret) {
			KUNIT_FAIL(test, "structural page %u failed: %d", i, ret);
			return;
		}
	}
	for (i = 0; i < MC16_CLUSTER_SLOTS; i++) {
		memcomp_invalidate(f->tokens[i]);
		f->held[i] = false;
	}
	/* Wait for physical metadata frees before checking DEBUG_STATS. */
	rcu_barrier();
	/* Only memcomp-owned objects are counted; XA is not observed. */
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&after);
	for (i = 0; i < MC16_META_CLASSES; i++) {
		KUNIT_EXPECT_EQ(test, after.allocated[i] - before.allocated[i],
				expected[i]);
		KUNIT_EXPECT_EQ(test, after.freed[i] - before.freed[i],
				expected[i]);
	}
#endif
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	kunit_release_action(test, mc16_bs_cleanup, f);
}
#endif /* CONFIG_MEMCG */
#endif /* MC16_FOLIO_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_BUDGET_TEST_H */
