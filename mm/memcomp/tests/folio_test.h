/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_FOLIO_TEST_H
#define _MM_MEMCOMP_TESTS_FOLIO_TEST_H

#include <kunit/resource.h>
#include <linux/mman.h>
#include <linux/rmap.h>
#include <linux/sched/mm.h>

/*
 * Author: nakixii
 *
 * Include after folio.h, resident.h, load.h and consumers_test.h. Register the
 * cases below under MC16_FOLIO_TEST_ENABLED. The objcg/slot-allocation probe
 * is the existing consumer model.
 * Anonymous VMAs, order-0/order-1 folios, PTEs, rmap, commits and loads are real.
 * PTE installation, partial commit and MOVE deliberately model serial states;
 * these tests do not invoke reclaim, fork or mremap, or claim concurrent races.
 */
#if defined(CONFIG_MEMCG) && defined(CONFIG_MMU) && \
	defined(CONFIG_ADVISE_SYSCALLS)
#define MC16_FOLIO_TEST_ENABLED

struct mc16_flt {
	struct kunit *test;
	struct mc16_consumer_test *base;
	struct mm_struct *mm;
	struct folio *folio;
	struct anon_vma *anon_vma;
	struct memcomp_ref guards[2];
	unsigned long mapping;
	unsigned long addr;
	unsigned int nr;
	s64 swapents;
	s64 anonpages;
};

static s64 mc16_flt_rss(struct mc16_flt *f, int member)
{
	return percpu_counter_sum(&f->mm->rss_stat[member]);
}

/* Remove token PTEs before releasing the fixture and its modeled objcg. */
static int mc16_flt_zap(struct mc16_flt *f, unsigned long address)
{
	struct memcomp_ref removed = { };
	spinlock_t *ptl; /* Actual token PTE ownership is protected by this PTL. */
	pte_t *ptep, pte;
	pmd_t *pmd;
	int ret = 0;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, address);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(f->mm, pmd, address, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if (is_swap_pte(pte) && is_memcomp_entry(pte_to_swp_entry(pte))) {
		if (!mc16_get(swp_offset(pte_to_swp_entry(pte)), &removed))
			ret = -ENOENT;
		ptep_get_and_clear(f->mm, address, ptep);
		add_mm_counter(f->mm, MM_SWAPENTS, -1);
	}
	pte_unmap_unlock(ptep, ptl);
	if (removed.slot)
		mc16_ref_put_count(&removed, 2); /* Lookup and removed PTE. */
out:
	mmap_read_unlock(f->mm);
	return ret;
}

/* Each cleanup acquires its own locks; it never unlocks the expired test task. */
static void mc16_flt_cleanup(void *data)
{
	struct mc16_flt *f = data;
	unsigned int i;
	int ret;

	f->base->probe.fail_at = 0;
	f->base->account->swap.val = 0;
	if (f->base->large)
		f->base->large->swap.val = 0;
	for (i = 0; i < 4; i++) {
		ret = mc16_flt_zap(f, f->addr + i * PAGE_SIZE);
		KUNIT_EXPECT_EQ(f->test, ret, 0);
	}
	if (f->folio) {
		folio_lock(f->folio);
		mc16_invalidate_folio(f->folio);
		f->folio->swap.val = 0;
		folio_unlock(f->folio);
	}
	for (i = 0; i < ARRAY_SIZE(f->guards); i++) {
		if (f->guards[i].slot)
			mc16_ref_put(&f->guards[i]);
	}
	ret = mc16_test_dontneed(f->mm, f->mapping, 8 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	lru_add_drain_all();
	if (f->folio) {
		KUNIT_EXPECT_FALSE(f->test, folio_test_locked(f->folio));
		KUNIT_EXPECT_EQ(f->test, folio_ref_count(f->folio), 1);
		folio_put(f->folio);
	}
	if (f->anon_vma)
		put_anon_vma(f->anon_vma);
	f->base->model.folio = f->base->account;
	KUNIT_EXPECT_EQ(f->test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(f->test, mc16_flt_rss(f, MM_ANONPAGES), f->anonpages);
	mmput(f->mm);
}

/*
 * Install a real, charged anonymous folio into fresh PTEs in a real VMA.
 * Reserve one extra reference for the fixture, independently of the nr PTEs.
 * Materialize/check the whole PTE range before publishing any rmap or PTE.
 */
static int mc16_flt_map(struct mc16_flt *f, unsigned int order)
{
	struct vm_area_struct *vma;
	spinlock_t *ptl; /* All PTEs in this prevalidated range share one PTL. */
	pte_t *ptep, entry;
	unsigned int i;
	int ret = -ENOENT;
	void *bytes;

	f->folio = folio_alloc(GFP_HIGHUSER_MOVABLE | __GFP_ZERO, order);
	if (!f->folio)
		return -ENOMEM;
	f->nr = folio_nr_pages(f->folio);
	ret = mem_cgroup_charge(f->folio, f->mm, GFP_KERNEL);
	if (ret)
		return ret;
	for (i = 0; i < f->nr; i++) {
		bytes = kmap_local_folio(f->folio, i * PAGE_SIZE);
		memset(bytes, 0x6b, PAGE_SIZE);
		kunmap_local(bytes);
	}
	folio_mark_uptodate(f->folio);
	flush_dcache_folio(f->folio);
	mmap_write_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (!vma) {
		ret = -ENOENT;
		goto out;
	}
	vma_start_write(vma);
	vm_flags_set(vma, VM_NOHUGEPAGE);
#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_WP
	vm_flags_set(vma, VM_UFFD_WP);
#endif
	ret = anon_vma_prepare(vma);
	if (ret)
		goto out;
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out;
	}
	__acquire(RCU);
	for (i = 0; i < f->nr; i++) {
		if (!pte_none(ptep_get(ptep + i))) {
			ret = -EEXIST;
			goto out_pte;
		}
	}
	entry = pte_mkwrite(pte_mkdirty(mk_pte(&f->folio->page, vma->vm_page_prot)),
			    vma);
	folio_ref_add(f->folio, f->nr);
	add_mm_counter(f->mm, MM_ANONPAGES, f->nr);
	folio_add_new_anon_rmap(f->folio, vma, f->addr, RMAP_EXCLUSIVE);
	folio_add_lru_vma(f->folio, vma);
	set_ptes(f->mm, f->addr, ptep, entry, f->nr);
	update_mmu_cache_range(NULL, vma, f->addr, ptep, f->nr);
	f->anon_vma = folio_get_anon_vma(f->folio);
	ret = f->anon_vma ? 0 : -EINVAL;
out_pte:
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_write_unlock(f->mm);
	lru_add_drain_all();
	f->base->model.folio = f->folio;
	return ret;
}

static struct mc16_flt *mc16_flt_begin(struct kunit *test, unsigned int order)
{
	struct mc16_flt *f;

	if (order && !IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
		kunit_skip(test, "large anonymous folios need CONFIG_TRANSPARENT_HUGEPAGE");

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->base = mc16_consumer_test_begin(test);
	if (!f->base)
		return NULL;
	f->mapping = kunit_vm_mmap(test, NULL, 0, 8 * PAGE_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (!f->mapping || IS_ERR_VALUE(f->mapping))
		return NULL;
	/* Four adjacent pages, aligned for order 1, within one PTE table. */
	f->addr = ALIGN(f->mapping, 2 * PAGE_SIZE);
	if ((f->addr >> PMD_SHIFT) != ((f->addr + 3 * PAGE_SIZE) >> PMD_SHIFT))
		f->addr = ALIGN(f->addr, PMD_SIZE);
	f->mm = current->mm;
	mmget(f->mm);
	f->swapents = mc16_flt_rss(f, MM_SWAPENTS);
	f->anonpages = mc16_flt_rss(f, MM_ANONPAGES);
	if (kunit_add_action_or_reset(test, mc16_flt_cleanup, f))
		return NULL;
	if (mc16_flt_map(f, order))
		return NULL;
	return f;
}

static bool mc16_flt_reserve(struct mc16_flt *f)
{
	bool ret;

	folio_lock(f->folio);
	ret = mc16_reserve_folio(f->folio);
	folio_unlock(f->folio);
	return ret;
}

static void mc16_flt_drop(struct mc16_flt *f, bool rollback)
{
	mmap_read_lock(f->mm);
	folio_lock(f->folio);
	if (rollback)
		mc16_rollback_folio(f->folio);
	else
		mc16_invalidate_folio(f->folio);
	folio_unlock(f->folio);
	mmap_read_unlock(f->mm);
}

/* Every successful release is thawed before unlocking or any test assertion. */
static bool mc16_flt_release(struct folio *folio)
{
	bool ret;

	/* Reclaim releases an isolated source, with only its final hold left. */
	lru_add_drain_all();
	if (folio_test_lru(folio)) {
		if (!folio_isolate_lru(folio))
			return false;
		/* Model reclaim isolation, not migration: the source is inactive. */
		folio_clear_active(folio);
		folio_put(folio); /* Drop isolation's additional hold, not the fixture. */
	}
	folio_lock(folio);
	ret = mc16_release_folio(folio);
	if (ret)
		folio_ref_unfreeze(folio, 1);
	folio_unlock(folio);
	return ret;
}

static int mc16_flt_hold(struct mc16_flt *f)
{
	unsigned long token = swp_offset(f->folio->swap);
	unsigned int i;

	for (i = 0; i < f->nr; i++) {
		if (!mc16_get(token + i, &f->guards[i]))
			return -ENOENT;
	}
	return 0;
}

/* Actual pending -> SAMEFILL commit/finish; serial partial-store model. */
static int mc16_flt_commit(struct mc16_flt *f, unsigned int i)
{
	struct mc16_payload payload = { .word = 0x5a5a5a5a5a5a5a5aULL };
	struct mc16_payload_tail tail = { };
	struct memcomp_ref *ref = &f->guards[i];
	int ret;

	folio_lock(f->folio);
	mutex_lock(memcomp_lock_of(ref->token));
	ret = mc16_payload_commit_locked(ref, f->folio, f->nr, i, &payload, &tail);
	mutex_unlock(memcomp_lock_of(ref->token));
	if (!ret)
		mc16_payload_finish(ref, &tail);
	folio_unlock(f->folio);
	return ret;
}

static pte_t mc16_flt_read(struct mc16_flt *f, unsigned long address)
{
	spinlock_t *ptl; /* Only the copied PTE leaves the sampling scope. */
	pte_t *ptep, pte = __pte(0);
	pmd_t *pmd;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, address);
	if (pmd) {
		ptep = pte_offset_map_lock(f->mm, pmd, address, &ptl);
		if (ptep) {
			pte = ptep_get(ptep);
			pte_unmap_unlock(ptep, ptl);
		}
	}
	mmap_read_unlock(f->mm);
	return pte;
}

/* Transfer one lookup to a real token PTE, retire the old mapping's folio ref. */
static int mc16_flt_escape(struct mc16_flt *f, unsigned int i)
{
	struct memcomp_ref pte_ref = { };
	struct vm_area_struct *vma;
	spinlock_t *ptl; /* Protects the present -> token PTE transaction. */
	unsigned long address = f->addr + i * PAGE_SIZE;
	pte_t *ptep, old, entry;
	int ret = -EINVAL;

	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, address);
	if (!vma)
		goto out;
	folio_lock(f->folio);
	ptep = get_locked_pte(f->mm, address, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out_folio;
	}
	__acquire(RCU);
	old = ptep_get(ptep);
	if (!pte_present(old) || pte_special(old) ||
	    pte_page(old) != folio_page(f->folio, i) ||
	    !mc16_get(swp_offset(f->folio->swap) + i, &pte_ref))
		goto out_pte;
	entry = swp_entry_to_pte(make_memcomp_entry(pte_ref.token));
	entry = pte_swp_mkexclusive(pte_swp_mkuffd_wp(pte_swp_mksoft_dirty(entry)));
	ptep_clear_flush(vma, address, ptep);
	folio_remove_rmap_pte(f->folio, folio_page(f->folio, i), vma);
	add_mm_counter(f->mm, MM_ANONPAGES, -1);
	add_mm_counter(f->mm, MM_SWAPENTS, 1);
	set_pte_at(f->mm, address, ptep, entry);
	/* This get is now owned by the PTE, not by a C fixture reference. */
	folio_put(f->folio);
	ret = 0;
out_pte:
	pte_unmap_unlock(ptep, ptl);
out_folio:
	folio_unlock(f->folio);
out:
	mmap_read_unlock(f->mm);
	return ret;
}

static unsigned int mc16_flt_wrong_token(struct mc16_flt *f)
{
	struct mc16_folio_restore walk = {
		.token = swp_offset(f->folio->swap) + f->nr,
	};
	struct vm_area_struct *vma;

	mmap_read_lock(f->mm);
	folio_lock(f->folio);
	vma = vma_lookup(f->mm, f->addr);
	if (vma)
		mc16_folio_restore_one(f->folio, vma, f->addr, &walk);
	folio_unlock(f->folio);
	mmap_read_unlock(f->mm);
	return walk.found;
}

/* Snapshot only scalar state; no record/member pointer leaves T. */
static unsigned int mc16_flt_flags(struct memcomp_ref *ref, bool mark)
{
	struct mc16_view view;
	unsigned int flags = 0;

	mutex_lock(memcomp_lock_of(ref->token));
	if (mc16_view_locked(ref, &view)) {
		if (mark)
			*view.flags |= MC16_F_INCOMPRESSIBLE;
		flags = *view.flags;
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	return flags;
}

static void mc16_flt_expect_load(struct mc16_flt *f, unsigned int i)
{
	void *dst = kunit_kmalloc(f->test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(f->test, dst);
	memset(dst, 0xa7, PAGE_SIZE);
	ret = mc16_load_page(&f->guards[i], dst);
	KUNIT_ASSERT_EQ(f->test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(f->test, memchr_inv(dst, 0x5a, PAGE_SIZE), NULL);
}

static void mc16_flt_expect_present(struct mc16_flt *f, unsigned int i,
				    unsigned long address)
{
	pte_t pte = mc16_flt_read(f, address);

	KUNIT_ASSERT_TRUE(f->test, pte_present(pte));
	KUNIT_ASSERT_FALSE(f->test, pte_special(pte));
	KUNIT_EXPECT_PTR_EQ(f->test, pte_page(pte), folio_page(f->folio, i));
#ifdef CONFIG_MEM_SOFT_DIRTY
	KUNIT_EXPECT_TRUE(f->test, pte_soft_dirty(pte));
#endif
#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_WP
	KUNIT_EXPECT_TRUE(f->test, pte_uffd_wp(pte));
	KUNIT_EXPECT_FALSE(f->test, pte_write(pte));
#endif
	KUNIT_EXPECT_TRUE(f->test, PageAnonExclusive(folio_page(f->folio, i)));
}

static void memcomp_test_folio_reserve(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct memcomp_ref probe = { };
	unsigned long token;
	unsigned int refs;
	bool found;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_FALSE(test, mc16_reserve_folio(f->folio)); /* Unlocked. */
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	token = swp_offset(f->folio->swap);
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &probe));
	refs = refcount_read(&probe.slot->refs);
	KUNIT_EXPECT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_EXPECT_EQ(test, swp_offset(f->folio->swap), token);
	KUNIT_EXPECT_EQ(test, refcount_read(&probe.slot->refs), refs);
	mc16_ref_put(&probe);
	/* No PTE or test guard escaped: rollback must retire the whole span. */
	mc16_flt_drop(f, true);
	found = mc16_get(token, &probe);
	if (probe.slot)
		mc16_ref_put(&probe);
	KUNIT_EXPECT_FALSE(test, found);
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_commit(f, 0), 0);
	token = f->guards[0].token;
	/* A stored OWNER is rolled back before a fresh reservation is published. */
	KUNIT_EXPECT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_EXPECT_NE(test, swp_offset(f->folio->swap), token);
	KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[0], false) & MC16_F_OWNER);
	mc16_flt_expect_load(f, 0);
	mc16_flt_drop(f, true);
}

static void memcomp_test_folio_reserve_failure(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	int pins, refs;
	bool reserved;

	KUNIT_ASSERT_NOT_NULL(test, f);
	pins = atomic_read(&f->anon_vma->refcount);
	refs = folio_ref_count(f->folio);
	f->base->probe.fail_at = f->base->probe.allocations + 1;
	reserved = mc16_flt_reserve(f);
	f->base->probe.fail_at = 0;
	KUNIT_EXPECT_FALSE(test, reserved);
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), refs);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins + 1);
	mc16_flt_drop(f, false);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
}

static void memcomp_test_folio_rollback_rmap(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	pte_t orig;
	u32 hash;
	int pins;

	KUNIT_ASSERT_NOT_NULL(test, f);
	pins = atomic_read(&f->anon_vma->refcount);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	KUNIT_EXPECT_FALSE(test, folio_mapped(f->folio));
	orig = mc16_flt_read(f, f->addr);
	KUNIT_EXPECT_EQ(test, mc16_flt_wrong_token(f), 0U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_flt_read(f, f->addr), orig));
	folio_lock(f->folio);
	mc16_flt_flags(&f->guards[0], true);
	hash = memcomp_folio_hash(f->folio);
	folio_unlock(f->folio);
	mc16_flt_drop(f, true); /* Runs the real rmap_walk(), even fully unmapped. */
	mc16_flt_expect_present(f, 0, f->addr);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_ANONPAGES), f->anonpages + 1);
	KUNIT_EXPECT_TRUE(test, folio_test_incompressible(f->folio));
	KUNIT_EXPECT_EQ(test, folio_incompressible_hash(f->folio), hash);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[0].slot->refs), 1U);
	KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[0], false) & MC16_F_OWNER);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
}

static void memcomp_test_folio_partial_rollback(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	/* Model a batch failure after the head has committed, before sibling 1. */
	KUNIT_ASSERT_EQ(test, mc16_flt_commit(f, 0), 0);
	mc16_flt_drop(f, true);
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	for (i = 0; i < f->nr; i++) {
		mc16_flt_expect_present(f, i, f->addr + i * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 1U);
		KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[i], false) & MC16_F_OWNER);
	}
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_ANONPAGES), f->anonpages + 2);
	mc16_flt_expect_load(f, 0);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(mc16_flt_flags(&f->guards[1], false)),
			MC16_PENDING);
}

static void memcomp_test_folio_partial_invalidate(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	unsigned int i;
	int pins;

	KUNIT_ASSERT_NOT_NULL(test, f);
	pins = atomic_read(&f->anon_vma->refcount);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_commit(f, 0), 0);
	KUNIT_EXPECT_FALSE(test, mc16_flt_release(f->folio)); /* Mixed span. */
	mc16_flt_drop(f, false);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 1U);
		KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[i], false) & MC16_F_OWNER);
	}
	mc16_flt_expect_load(f, 0);
	KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[1], false) & MC16_F_RESIDENT);
}

static void memcomp_test_folio_release(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	bool released;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_zap(f, f->addr), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 2); /* Fixture + resident. */
	KUNIT_EXPECT_FALSE(test, mc16_flt_release(f->folio));
	KUNIT_ASSERT_EQ(test, mc16_flt_commit(f, 0), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 1);
	folio_get(f->folio);
	released = mc16_flt_release(f->folio);
	folio_put(f->folio);
	KUNIT_EXPECT_FALSE(test, released); /* Extra ref prevents freeze(1). */
	KUNIT_EXPECT_FALSE(test, mc16_release_folio(f->folio)); /* Unlocked. */
	KUNIT_EXPECT_TRUE(test, mc16_flt_release(f->folio));
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[0].slot->refs), 1U);
	mc16_flt_expect_load(f, 0);
}

static void memcomp_test_folio_transport(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct folio *transport;
	bool released;

	KUNIT_ASSERT_NOT_NULL(test, f);
	transport = f->base->account;
	transport->swap = make_memcomp_entry(0);
	KUNIT_EXPECT_FALSE(test, mc16_release_folio(transport));
	/* Mapcount-only rejection model; restore it before any assertion/put. */
	atomic_inc(&transport->_mapcount);
	released = mc16_flt_release(transport);
	atomic_dec(&transport->_mapcount);
	KUNIT_EXPECT_FALSE(test, released);
	folio_get(transport);
	released = mc16_flt_release(transport);
	folio_put(transport);
	KUNIT_EXPECT_FALSE(test, released);
	f->base->large = folio_alloc(GFP_KERNEL, 1);
	KUNIT_ASSERT_NOT_NULL(test, f->base->large);
	f->base->large->swap = make_memcomp_entry(0);
	KUNIT_EXPECT_FALSE(test, mc16_flt_release(f->base->large));
	f->base->large->swap.val = 0;
	f->folio->swap = make_memcomp_entry(0);
	KUNIT_EXPECT_FALSE(test, mc16_flt_release(f->folio)); /* Real anon mapping. */
	f->folio->swap.val = 0;
	KUNIT_EXPECT_TRUE(test, mc16_flt_release(transport));
	KUNIT_EXPECT_EQ(test, transport->swap.val, 0UL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(transport), 1);
}

/* Move a nonpresent PTE outside the original folio's rmap address, same PTL. */
static int mc16_flt_move(struct mc16_flt *f, bool back)
{
	spinlock_t *ptl; /* Source and destination share the validated PTE table. */
	pte_t *ptep;
	unsigned int src = back ? 2 : 0, dst = back ? 0 : 2;
	int ret = -EINVAL;

	mmap_write_lock(f->mm);
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out;
	}
	__acquire(RCU);
	if (is_swap_pte(ptep_get(ptep + src)) && pte_none(ptep_get(ptep + dst))) {
		pte_t entry = ptep_get_and_clear(f->mm, f->addr + src * PAGE_SIZE,
						ptep + src);

		set_pte_at(f->mm, f->addr + dst * PAGE_SIZE, ptep + dst, entry);
		ret = 0;
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_write_unlock(f->mm);
	return ret;
}

static vm_fault_t mc16_flt_late_fault(struct mc16_flt *f, bool *handled)
{
	struct memcomp_ref lookup = { };
	unsigned long address = f->addr;
	struct vm_area_struct *vma;
	vm_fault_t ret = VM_FAULT_SIGBUS;

	*handled = false;
	if (!mc16_get(f->guards[0].token, &lookup))
		return ret;
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, address);
	if (vma) {
		struct vm_fault vmf = {
			.vma = vma, .address = address, .real_address = address,
			.flags = FAULT_FLAG_ORIG_PTE_VALID | FAULT_FLAG_RETRY_NOWAIT,
			.pmd = mm_find_pmd(f->mm, address),
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};
		spinlock_t *ptl; /* Copy orig_pte before the fault takes its own PTL. */
		pte_t *ptep;

		ptep = pte_offset_map_lock(f->mm, vmf.pmd, address, &ptl);
		if (ptep) {
			vmf.orig_pte = ptep_get(ptep);
			pte_unmap_unlock(ptep, ptl);
			ret = mc16_load_resident(&vmf, &lookup, handled);
		}
	}
	/* RETRY_NOWAIT retains mmap_read_lock even if folio_lock_or_retry fails. */
	mmap_read_unlock(f->mm);
	if (lookup.slot)
		mc16_ref_put(&lookup);
	return ret;
}

static void memcomp_test_folio_missed_rmap(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	pte_t orig;
	vm_fault_t fault;
	bool handled;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_move(f, false), 0);
	orig = mc16_flt_read(f, f->addr + 2 * PAGE_SIZE);
	mc16_flt_drop(f, true); /* Bounded walks miss the modeled MOVE destination. */
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_flt_read(f, f->addr + 2 * PAGE_SIZE), orig));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[0].slot->refs), 2U);
	KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[0], false) & MC16_F_OWNER);
	KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[0], false) & MC16_F_RESIDENT);
	/*
	 * Move back before the real fault: this bare PTE-transfer model does not
	 * update VMA pgoff as mremap does. Resident rmap requires the original
	 * page index. The guard stays continuous across both moves and detach.
	 */
	KUNIT_ASSERT_EQ(test, mc16_flt_move(f, true), 0);
	fault = mc16_flt_late_fault(f, &handled);
	KUNIT_EXPECT_TRUE(test, handled);
	KUNIT_ASSERT_EQ(test, fault, (vm_fault_t)VM_FAULT_NOPAGE);
	mc16_flt_expect_present(f, 0, f->addr);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[0].slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents);
}

/* Invalid lengths model corrupt metadata; restore it before any cleanup. */
static void mc16_flt_bad_release(struct kunit *test, unsigned int bad)
{
	struct mc16_flt *f = mc16_flt_begin(test, bad ? 1 : 0);
	struct mc16_view view;
	unsigned long swap;
	unsigned int i;
	u16 length;
	bool released;
	int pins;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_flt_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++) {
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
		KUNIT_ASSERT_EQ(test, mc16_flt_zap(f, f->addr + i * PAGE_SIZE), 0);
		KUNIT_ASSERT_EQ(test, mc16_flt_commit(f, i), 0);
	}
	swap = f->folio->swap.val;
	pins = atomic_read(&f->anon_vma->refcount);
	mutex_lock(memcomp_lock_of(f->guards[bad].token));
	mc16_view_locked(&f->guards[bad], &view);
	length = *view.length;
	*view.length = U16_MAX;
	mutex_unlock(memcomp_lock_of(f->guards[bad].token));
	released = mc16_flt_release(f->folio);
	mutex_lock(memcomp_lock_of(f->guards[bad].token));
	mc16_view_locked(&f->guards[bad], &view);
	*view.length = length;
	mutex_unlock(memcomp_lock_of(f->guards[bad].token));
	KUNIT_EXPECT_FALSE(test, released);
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, swap);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
	KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[0], false) & MC16_F_OWNER);
	KUNIT_EXPECT_TRUE(test, mc16_flt_flags(&f->guards[bad], false) & MC16_F_OWNER);
	KUNIT_EXPECT_TRUE(test, mc16_flt_release(f->folio));
	for (i = 0; i < f->nr; i++)
		mc16_flt_expect_load(f, i);
}

static void memcomp_test_folio_release_bad_head(struct kunit *test)
{
	mc16_flt_bad_release(test, 0);
}

static void memcomp_test_folio_release_bad_sibling(struct kunit *test)
{
	mc16_flt_bad_release(test, 1);
}
#endif /* CONFIG_MEMCG && CONFIG_MMU && CONFIG_ADVISE_SYSCALLS */
#endif /* _MM_MEMCOMP_TESTS_FOLIO_TEST_H */
