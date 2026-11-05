/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_RESIDENT_TEST_H
#define _MM_MEMCOMP_TESTS_RESIDENT_TEST_H

#include <kunit/resource.h>
#include <kunit/static_stub.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include "../resident.h"

/* Include after consumers_test.h. Real MM/folio/rmap, modeled metadata objcg. */
#if defined(CONFIG_MEMCG) && defined(CONFIG_ADVISE_SYSCALLS)
#define MC16_RESIDENT_TEST_ENABLED
enum mc16_rl_race {
	MC16_RL_NONE,
	MC16_RL_STORED,
	MC16_RL_FOLIO,
	MC16_RL_RESIDENT,
};

struct mc16_rl {
	struct mc16_consumer_test *base;
	struct mm_struct *mm;
	unsigned long addr;
	struct folio *folio, *other;
	struct memcomp_ref owners[2], guard;
	unsigned int nr, target;
	struct anon_vma *anon_vma; /* Borrowed from the producer's record hold. */
	vm_flags_t saved_flags;
	pgprot_t saved_prot;
	bool saved_vma;
	pte_t orig;
	s64 swapents, anonpages;
	enum mc16_rl_race race;
	int hook_error, lookup;
	bool switched, cleared, handled;
	unsigned int fault_flags;
	pte_t *fault_pte;
	struct task_struct *worker;
	struct completion locked, go;
};

static struct mc16_rl *mc16_rl_current;

static s64 mc16_rl_rss(struct mc16_rl *f, int member)
{
	return percpu_counter_sum(&f->mm->rss_stat[member]);
}

/* The PTE owns one existing slot ref; removing it requires no new lookup. */
static void mc16_rl_zap_token(struct mc16_rl *f)
{
	struct memcomp_ref removed;
	spinlock_t *ptl; /* Serializes the real PTE and its owner reference. */
	pte_t *ptep, pte;
	pmd_t *pmd;

	if (!f->owners[f->target].slot)
		return;
	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, f->addr);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(f->mm, pmd, f->addr, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if (is_swap_pte(pte) && is_memcomp_entry(pte_to_swp_entry(pte)) &&
	    swp_offset(pte_to_swp_entry(pte)) == f->owners[f->target].token) {
		ptep_get_and_clear(f->mm, f->addr, ptep);
		add_mm_counter(f->mm, MM_SWAPENTS, -1);
		removed = f->owners[f->target];
		mc16_ref_put(&removed);
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
}

/* Undo only deliberately injected invalid identity/flags before final puts. */
static void mc16_rl_restore_race(struct mc16_rl *f)
{
	struct mc16_view view;

	if (!f->switched && !f->cleared)
		return;
	folio_lock(f->folio);
	mutex_lock(memcomp_lock_of(f->owners[f->target].token));
	if (mc16_view_locked(&f->owners[f->target], &view)) {
		if (f->switched) {
			folio_get(f->folio);
			view.member->owner_folio = f->folio;
			folio_put(f->other);
		}
		if (f->cleared)
			*view.flags |= MC16_F_RESIDENT;
	}
	mutex_unlock(memcomp_lock_of(f->owners[f->target].token));
	folio_unlock(f->folio);
	f->switched = false;
	f->cleared = false;
}

static void mc16_rl_join(struct mc16_rl *f)
{
	if (f->worker) {
		complete_all(&f->go);
		kthread_stop_put(f->worker);
		f->worker = NULL;
	}
}

static void mc16_rl_cleanup(void *data)
{
	struct mc16_rl *f = data;
	struct vm_area_struct *vma;
	struct anon_vma *anon_vma = NULL;
	unsigned int i;
	int ret;

	mc16_rl_join(f);
	mc16_rl_current = NULL;
	mc16_rl_restore_race(f);
	if (f->guard.slot)
		mc16_ref_put(&f->guard);
	mc16_rl_zap_token(f);
	if (f->nr) {
		folio_lock(f->folio);
		mutex_lock(memcomp_lock_of(f->owners[0].token));
		anon_vma = mc16_span_take_anon_vma_locked(&f->owners[0],
							  f->folio, f->nr);
		mutex_unlock(memcomp_lock_of(f->owners[0].token));
		/* Producer siblings leave first; head/anon_vma bookkeeping last. */
		for (i = f->nr; i > 0; i--)
			mc16_ref_put(&f->owners[i - 1]);
		folio_unlock(f->folio);
	}
	KUNIT_EXPECT_PTR_EQ(f->base->test, anon_vma, f->anon_vma);
	if (anon_vma)
		put_anon_vma(anon_vma);
	f->anon_vma = NULL;
	mmap_write_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma && f->saved_vma) {
		vma_start_write(vma);
		vm_flags_reset(vma, f->saved_flags);
		vma->vm_page_prot = f->saved_prot;
	}
	mmap_write_unlock(f->mm);
	ret = mc16_test_dontneed(f->mm, f->addr, PAGE_SIZE);
	KUNIT_EXPECT_EQ(f->base->test, ret, 0);
	KUNIT_EXPECT_EQ(f->base->test, mc16_rl_rss(f, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(f->base->test, mc16_rl_rss(f, MM_ANONPAGES), f->anonpages);
	if (f->folio) {
		KUNIT_EXPECT_FALSE(f->base->test, folio_test_locked(f->folio));
		KUNIT_EXPECT_EQ(f->base->test, folio_mapcount(f->folio), 0);
		folio_put(f->folio);
	}
	if (f->other)
		folio_put(f->other);
	f->base->model.folio = f->base->account;
	mmput(f->mm);
}

static int mc16_rl_reserve(struct mc16_rl *f, unsigned int nr)
{
	struct mc16_record *record;
	struct anon_vma *anon_vma;
	unsigned int i;
	int ret;

	folio_lock(f->folio);
	record = mc16_record_alloc(f->folio, nr, false);
	if (IS_ERR(record)) {
		ret = PTR_ERR(record);
		goto unlock;
	}
	anon_vma = folio_get_anon_vma(f->folio);
	if (!anon_vma) {
		mc16_record_abort(record);
		ret = -EINVAL;
		goto unlock;
	}
	record->anon_vma = anon_vma;
	record->record_flags |= MC16_RECORD_ANON_VMA;
	ret = mc16_reserve(record, nr, MC16_KEY_ANON, f->owners);
	if (ret) {
		record->anon_vma = NULL;
		record->record_flags &= ~MC16_RECORD_ANON_VMA;
		mc16_record_abort(record);
		put_anon_vma(anon_vma);
		goto unlock;
	}
	f->nr = nr;
	f->anon_vma = anon_vma;
	for (i = 0; i < nr; i++)
		mc16_account_reservation(&f->owners[i]);
unlock:
	folio_unlock(f->folio);
	return ret;
}

/* Replace an actual anonymous PTE, retaining the folio's existing anon rmap. */
static int mc16_rl_install(struct mc16_rl *f)
{
	struct vm_area_struct *vma;
	struct memcomp_ref pte_ref;
	spinlock_t *ptl; /* Serializes the real PTE and its owner reference. */
	pte_t *ptep, old;
	void *data;
	int ret = -EINVAL;

	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (!vma)
		goto out;
	folio_lock(f->folio);
	data = kmap_local_folio(f->folio, 0);
	memset(data, 0x6b, PAGE_SIZE);
	kunmap_local(data);
	flush_dcache_folio(f->folio);
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto unlock_folio;
	}
	__acquire(RCU);
	old = ptep_get(ptep);
	if (!pte_present(old) || pte_page(old) != &f->folio->page ||
	    !mc16_get(f->owners[f->target].token, &pte_ref))
		goto unlock_pte;
	ptep_clear_flush(vma, f->addr, ptep);
	folio_remove_rmap_pte(f->folio, &f->folio->page, vma);
	add_mm_counter(f->mm, MM_ANONPAGES, -1);
	add_mm_counter(f->mm, MM_SWAPENTS, 1);
	set_pte_at(f->mm, f->addr, ptep, f->orig);
	/* The get is now the token PTE's; the old mapping drops its folio ref. */
	folio_put(f->folio);
	ret = 0;
unlock_pte:
	pte_unmap_unlock(ptep, ptl);
unlock_folio:
	folio_unlock(f->folio);
out:
	mmap_read_unlock(f->mm);
	return ret;
}

static struct mc16_rl *mc16_rl_begin(struct kunit *test, unsigned int nr,
				     unsigned int target)
{
	struct mc16_rl *f;
	struct vm_area_struct *vma;
	spinlock_t *ptl; /* Serializes the real PTE and its owner reference. */
	pte_t *ptep, pte;
	vm_fault_t fault;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->base = mc16_consumer_test_begin(test);
	if (!f->base)
		return NULL;
	f->addr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE,
				PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (!f->addr || IS_ERR_VALUE(f->addr))
		return NULL;
	f->mm = current->mm;
	mmget(f->mm);
	f->target = target;
	f->swapents = mc16_rl_rss(f, MM_SWAPENTS);
	f->anonpages = mc16_rl_rss(f, MM_ANONPAGES);
	init_completion(&f->locked);
	init_completion(&f->go);
	if (kunit_add_action_or_reset(test, mc16_rl_cleanup, f))
		return NULL;
	f->other = folio_alloc(GFP_KERNEL, 0);
	if (!f->other)
		return NULL;
	mmap_write_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma) {
		f->saved_flags = vma->vm_flags;
		f->saved_prot = vma->vm_page_prot;
		f->saved_vma = true;
		vma_start_write(vma);
		vm_flags_set(vma, VM_NOHUGEPAGE);
	}
	mmap_write_unlock(f->mm);
	if (!vma)
		return NULL;
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (!vma) {
		ret = -ENOENT;
		goto out;
	}
	fault = handle_mm_fault(vma, f->addr, FAULT_FLAG_WRITE, NULL);
	ret = fault & VM_FAULT_ERROR ? -EFAULT : -ENOMEM;
	if (fault & VM_FAULT_ERROR)
		goto out;
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep)
		goto out;
	__acquire(RCU);
	pte = ptep_get(ptep);
	if (pte_present(pte) && !pte_special(pte)) {
		f->folio = page_folio(pte_page(pte));
		folio_get(f->folio);
		ret = folio_test_large(f->folio) ? -EINVAL : 0;
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	if (ret)
		return NULL;
	f->base->model.folio = f->folio;
	if (mc16_rl_reserve(f, nr))
		return NULL;
	f->orig = swp_entry_to_pte(make_memcomp_entry(f->owners[target].token));
	f->orig = pte_swp_mkexclusive(f->orig);
	if (mc16_rl_install(f))
		return NULL;
	/* Remove pagevec's temporary folio ref before ownership assertions. */
	lru_add_drain_all();
	return f;
}

static pte_t mc16_rl_read(struct mc16_rl *f)
{
	spinlock_t *ptl; /* Serializes the real PTE and its owner reference. */
	pte_t *ptep, pte = __pte(0);
	pmd_t *pmd;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, f->addr);
	if (pmd) {
		ptep = pte_offset_map_lock(f->mm, pmd, f->addr, &ptl);
		if (ptep) {
			pte = ptep_get(ptep);
			pte_unmap_unlock(ptep, ptl);
		}
	}
	mmap_read_unlock(f->mm);
	return pte;
}

static int mc16_rl_change_pte(struct mc16_rl *f, pte_t pte)
{
	spinlock_t *ptl; /* Serializes the real PTE and its owner reference. */
	pte_t *ptep;
	int ret = 0;

	mmap_read_lock(f->mm);
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
	} else {
		__acquire(RCU);
		if (!pte_same(ptep_get(ptep), f->orig))
			ret = -ESTALE;
		else
			set_pte_at(f->mm, f->addr, ptep, pte);
		pte_unmap_unlock(ptep, ptl);
	}
	mmap_read_unlock(f->mm);
	return ret;
}

static void mc16_rl_policy(struct mc16_rl *f, vm_flags_t set, vm_flags_t clear)
{
	struct vm_area_struct *vma;

	mmap_write_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma) {
		vma_start_write(vma);
		vm_flags_reset(vma, (vma->vm_flags | set) & ~clear);
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	}
	mmap_write_unlock(f->mm);
	KUNIT_EXPECT_NOT_NULL(f->base->test, vma);
}

/* A genuine producer commit removes RESIDENT and its folio hold. */
static int mc16_rl_store(struct mc16_rl *f)
{
	struct mc16_payload payload = { .word = 0x123456789abcdef0ULL };
	struct mc16_payload_tail tail = { };
	struct memcomp_ref *ref = &f->owners[0];
	int ret;

	folio_lock(f->folio);
	mutex_lock(memcomp_lock_of(ref->token));
	ret = mc16_payload_commit_locked(ref, f->folio, 1, 0, &payload, &tail);
	mutex_unlock(memcomp_lock_of(ref->token));
	if (!ret)
		mc16_payload_finish(ref, &tail);
	folio_unlock(f->folio);
	return ret;
}

static vm_fault_t mc16_rl_lock(struct folio *folio, struct vm_fault *vmf)
{
	struct mc16_rl *f = mc16_rl_current;
	struct memcomp_ref *ref = &f->owners[f->target];
	struct mc16_view view;

	if (f->race == MC16_RL_STORED) {
		f->hook_error = mc16_rl_store(f);
	} else {
		folio_lock(folio);
		mutex_lock(memcomp_lock_of(ref->token));
		if (!mc16_view_locked(ref, &view)) {
			f->hook_error = -EIO;
		} else if (f->race == MC16_RL_FOLIO) {
			folio_get(f->other);
			view.member->owner_folio = f->other;
			folio_put(folio);
			f->switched = true;
		} else if (f->race == MC16_RL_RESIDENT) {
			*view.flags &= ~MC16_F_RESIDENT;
			f->cleared = true;
		}
		mutex_unlock(memcomp_lock_of(ref->token));
		folio_unlock(folio);
	}
	return folio_lock_or_retry(folio, vmf);
}

/* No fatal assertions, resource allocations or returned locks in this helper. */
static vm_fault_t mc16_rl_fault(struct mc16_rl *f, unsigned int flags,
				bool hold_folio)
{
	struct vm_area_struct *vma;
	vm_fault_t ret = VM_FAULT_SIGBUS;

	f->lookup = mc16_entry_resident_get(f->owners[f->target].token, &f->guard);
	if (f->lookup < 0 || !f->guard.slot)
		return ret;
	if (f->race != MC16_RL_NONE) {
		mc16_rl_current = f;
		kunit_activate_static_stub(f->base->test, mc16_resident_lock_or_retry,
					   mc16_rl_lock);
	}
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (!vma)
		goto unlock_mm;
	if (hold_folio)
		folio_lock(f->folio);
	if (f->worker)
		complete(&f->go);
	{
		struct vm_fault vmf = {
			.vma = vma, .address = f->addr, .real_address = f->addr,
			.flags = flags | FAULT_FLAG_ORIG_PTE_VALID,
			.pmd = mm_find_pmd(f->mm, f->addr), .orig_pte = f->orig,
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};

		ret = mc16_load_resident(&vmf, &f->guard, &f->handled);
		f->fault_pte = vmf.pte;
		f->fault_flags = vmf.flags;
	}
	if (hold_folio)
		folio_unlock(f->folio);
	/* NOWAIT retains the read lock; ordinary first RETRY has dropped it. */
	if ((ret & VM_FAULT_RETRY) && !(flags & FAULT_FLAG_RETRY_NOWAIT))
		goto unlocked_mm;
unlock_mm:
	mmap_read_unlock(f->mm);
unlocked_mm:
	if (f->race != MC16_RL_NONE) {
		kunit_deactivate_static_stub(f->base->test, mc16_resident_lock_or_retry);
		mc16_rl_current = NULL;
	}
	mc16_rl_join(f);
	mc16_rl_restore_race(f);
	return ret;
}

static void mc16_rl_counts(struct mc16_rl *f, unsigned int ptes,
			   unsigned int present, unsigned int refs)
{
	struct mc16_view view;
	bool owner = false, anon = false;

	KUNIT_EXPECT_EQ(f->base->test, mc16_rl_rss(f, MM_SWAPENTS), f->swapents + ptes);
	KUNIT_EXPECT_EQ(f->base->test, mc16_rl_rss(f, MM_ANONPAGES), f->anonpages + present);
	KUNIT_EXPECT_EQ(f->base->test, refcount_read(&f->owners[f->target].slot->refs),
			refs);
	KUNIT_EXPECT_EQ(f->base->test, folio_mapcount(f->folio), (int)present);
	KUNIT_EXPECT_FALSE(f->base->test, folio_test_locked(f->folio));
	KUNIT_EXPECT_PTR_EQ(f->base->test, f->fault_pte, NULL);
	mutex_lock(memcomp_lock_of(f->owners[0].token));
	if (mc16_view_locked(&f->owners[0], &view)) {
		owner = *view.flags & MC16_F_OWNER;
		anon = view.record && view.record->anon_vma == f->anon_vma &&
			(view.record->record_flags & MC16_RECORD_ANON_VMA);
	}
	mutex_unlock(memcomp_lock_of(f->owners[0].token));
	KUNIT_EXPECT_TRUE(f->base->test, owner);
	KUNIT_EXPECT_TRUE(f->base->test, anon);
}

static void mc16_rl_present(struct mc16_rl *f, bool write, bool dirty)
{
	pte_t pte = mc16_rl_read(f);
	void *data;
	bool same;

	KUNIT_ASSERT_TRUE(f->base->test, pte_present(pte));
	KUNIT_EXPECT_PTR_EQ(f->base->test, pte_page(pte), &f->folio->page);
	KUNIT_EXPECT_EQ(f->base->test, !!pte_write(pte), write);
	if (dirty)
		KUNIT_EXPECT_TRUE(f->base->test, pte_dirty(pte));
	else if (!write)
		KUNIT_EXPECT_FALSE(f->base->test, pte_dirty(pte));
	KUNIT_EXPECT_TRUE(f->base->test, folio_test_anon(f->folio));
	KUNIT_EXPECT_TRUE(f->base->test, folio_test_swapbacked(f->folio));
	KUNIT_EXPECT_TRUE(f->base->test, folio_test_uptodate(f->folio));
	data = kmap_local_folio(f->folio, 0);
	same = !memchr_inv(data, 0x6b, PAGE_SIZE);
	kunmap_local(data);
	KUNIT_EXPECT_TRUE(f->base->test, same);
	mc16_rl_counts(f, 0, 1, 1);
}

static void memcomp_test_resident_write(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);
	pte_t orig;
	vm_fault_t ret;
	int refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	refs = folio_ref_count(f->folio);
	orig = pte_swp_mksoft_dirty(f->orig);
	KUNIT_ASSERT_EQ(test, mc16_rl_change_pte(f, orig), 0);
	f->orig = orig;
	ret = mc16_rl_fault(f, FAULT_FLAG_WRITE, false);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_FALSE(test, f->fault_flags & FAULT_FLAG_WRITE);
	mc16_rl_present(f, true, true);
	KUNIT_EXPECT_EQ(test, !!pte_soft_dirty(mc16_rl_read(f)), !!pte_swp_soft_dirty(orig));
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(&f->folio->page));
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), refs + 1);
	/* The still-resident producer cannot make a stale second fault win. */
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false), 0U);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), refs + 1);
	mc16_rl_counts(f, 0, 1, 1);
}

static void memcomp_test_resident_readonly(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_rl_policy(f, 0, VM_WRITE);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false),
			(__force unsigned int)VM_FAULT_NOPAGE);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	mc16_rl_present(f, false, false);
}

static void memcomp_test_resident_shared(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);
	pte_t orig;

	KUNIT_ASSERT_NOT_NULL(test, f);
	orig = pte_swp_clear_exclusive(f->orig);
	KUNIT_ASSERT_EQ(test, mc16_rl_change_pte(f, orig), 0);
	f->orig = orig;
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, FAULT_FLAG_WRITE, false),
			(__force unsigned int)VM_FAULT_NOPAGE);
	KUNIT_EXPECT_TRUE(test, f->fault_flags & FAULT_FLAG_WRITE);
	mc16_rl_present(f, false, false);
}

static void memcomp_test_resident_softdirty_wp(struct kunit *test)
{
	struct mc16_rl *f;

	if (!IS_ENABLED(CONFIG_MEM_SOFT_DIRTY)) {
		kunit_skip(test, "soft-dirty tracking is disabled");
		return;
	}
	f = mc16_rl_begin(test, 1, 0);
	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_rl_policy(f, 0, VM_SOFTDIRTY);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false),
			(__force unsigned int)VM_FAULT_NOPAGE);
	mc16_rl_present(f, false, false);
	KUNIT_EXPECT_FALSE(test, pte_soft_dirty(mc16_rl_read(f)));
}

static void memcomp_test_resident_uffd_wp(struct kunit *test)
{
	struct mc16_rl *f;
	pte_t orig = pte_swp_mkuffd_wp(swp_entry_to_pte(make_memcomp_entry(1)));

	if (!IS_ENABLED(CONFIG_USERFAULTFD) || !pte_swp_uffd_wp(orig)) {
		kunit_skip(test, "architecture has no enabled swap UFFD-WP bit");
		return;
	}
	f = mc16_rl_begin(test, 1, 0);
	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_rl_policy(f, VM_UFFD_WP, 0);
	orig = pte_swp_mkuffd_wp(f->orig);
	KUNIT_ASSERT_EQ(test, mc16_rl_change_pte(f, orig), 0);
	f->orig = orig;
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false),
			(__force unsigned int)VM_FAULT_NOPAGE);
	mc16_rl_present(f, false, false);
	KUNIT_EXPECT_TRUE(test, pte_uffd_wp(mc16_rl_read(f)));
}

static void memcomp_test_resident_changed_pte(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);
	pte_t changed;

	KUNIT_ASSERT_NOT_NULL(test, f);
	changed = pte_swp_mksoft_dirty(f->orig);
	if (pte_same(changed, f->orig))
		changed = pte_swp_clear_exclusive(f->orig);
	KUNIT_ASSERT_FALSE(test, pte_same(changed, f->orig));
	KUNIT_ASSERT_EQ(test, mc16_rl_change_pte(f, changed), 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false), 0U);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_rl_read(f), changed));
	mc16_rl_counts(f, 1, 0, 2);
}

static void memcomp_test_resident_zapped(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_rl_zap_token(f);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false), 0U);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_TRUE(test, pte_none(mc16_rl_read(f)));
	mc16_rl_counts(f, 0, 0, 1);
}

static void memcomp_test_resident_unhandled(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_rl_store(f), 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false), 0U);
	KUNIT_EXPECT_EQ(test, f->lookup, 0);
	KUNIT_EXPECT_FALSE(test, f->handled);
	KUNIT_EXPECT_NOT_NULL(test, f->guard.slot);
	mc16_rl_counts(f, 1, 0, 3);
	mc16_ref_put(&f->guard);
}

static void mc16_rl_race_case(struct kunit *test, enum mc16_rl_race race)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);
	int refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	refs = folio_ref_count(f->folio);
	f->race = race;
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false), 0U);
	KUNIT_EXPECT_EQ(test, f->hook_error, 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), refs - (race == MC16_RL_STORED));
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_rl_read(f), f->orig));
	mc16_rl_counts(f, 1, 0, 2);
}

static void memcomp_test_resident_state_race(struct kunit *test)
{
	mc16_rl_race_case(test, MC16_RL_STORED);
}

static void memcomp_test_resident_folio_race(struct kunit *test)
{
	mc16_rl_race_case(test, MC16_RL_FOLIO);
}

static void memcomp_test_resident_flag_race(struct kunit *test)
{
	mc16_rl_race_case(test, MC16_RL_RESIDENT);
}

static void memcomp_test_resident_bad_index(struct kunit *test)
{
	/* A real two-member record on an order-0 folio models corrupt geometry. */
	struct mc16_rl *f = mc16_rl_begin(test, 2, 1);
	pte_t changed;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false),
			(__force unsigned int)VM_FAULT_SIGBUS);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_rl_read(f), f->orig));
	mc16_rl_counts(f, 1, 0, 2);
	/* Corruption must not SIGBUS a PTE that has already changed identity. */
	changed = pte_swp_clear_exclusive(f->orig);
	KUNIT_ASSERT_EQ(test, mc16_rl_change_pte(f, changed), 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)mc16_rl_fault(f, 0, false), 0U);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_rl_read(f), changed));
	mc16_rl_counts(f, 1, 0, 2);
}

static void memcomp_test_resident_retry_nowait(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);
	vm_fault_t ret;
	int refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	refs = folio_ref_count(f->folio);
	ret = mc16_rl_fault(f, FAULT_FLAG_ALLOW_RETRY | FAULT_FLAG_RETRY_NOWAIT, true);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_RETRY);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), refs);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_rl_read(f), f->orig));
	mc16_rl_counts(f, 1, 0, 2);
}

static int mc16_rl_retry_writer(void *data)
{
	struct mc16_rl *f = data;

	folio_lock(f->folio);
	complete(&f->locked);
	wait_for_completion(&f->go);
	/* The ordinary retry must drop mmap_read_lock before waiting on us. */
	mmap_write_lock(f->mm);
	folio_unlock(f->folio);
	mmap_write_unlock(f->mm);
	return 0;
}

static void memcomp_test_resident_retry(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);
	unsigned long ready;
	vm_fault_t ret;
	int refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	refs = folio_ref_count(f->folio);
	f->worker = kthread_run(mc16_rl_retry_writer, f, "mc16-resident-retry");
	if (IS_ERR(f->worker)) {
		f->worker = NULL;
		KUNIT_FAIL(test, "could not start resident writer");
		return;
	}
	get_task_struct(f->worker);
	ready = wait_for_completion_timeout(&f->locked, msecs_to_jiffies(5000));
	if (!ready) {
		mc16_rl_join(f);
		KUNIT_FAIL(test, "resident writer did not lock folio");
		return;
	}
	ret = mc16_rl_fault(f, FAULT_FLAG_ALLOW_RETRY, false);
	/* No test thread owns another task's folio or mmap lock on assertion. */
	mc16_rl_join(f);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_RETRY);
	KUNIT_EXPECT_TRUE(test, f->handled);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), refs);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_rl_read(f), f->orig));
	mc16_rl_counts(f, 1, 0, 2);
}
#endif /* CONFIG_MEMCG && CONFIG_ADVISE_SYSCALLS */
#endif /* _MM_MEMCOMP_TESTS_RESIDENT_TEST_H */
