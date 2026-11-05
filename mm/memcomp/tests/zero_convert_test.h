/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ZERO_CONVERT_TEST_H
#define _MM_MEMCOMP_TESTS_ZERO_CONVERT_TEST_H

#include <kunit/resource.h>
#include <linux/mman.h>
#include <linux/rmap.h>
#include <linux/sched/mm.h>

/*
 * Include after zero_convert.h and the storage/refs test fixtures. Register
 * under the same guard below. Positive slots use real reserve/refresh/finish;
 * page tables, anonymous faults, rmap and the zero ledger are not stubbed.
 * The MOVE case models an interleaving with a PTL-protected PTE transfer; it
 * does not invoke the mremap syscall or claim concurrent fork coverage.
 */
#if defined(CONFIG_MEMCG) && defined(CONFIG_MMU) && \
	defined(CONFIG_ADVISE_SYSCALLS)
#define MC16_ZERO_CONVERT_TEST_ENABLED

struct mc16_test_zc {
	struct kunit *test;
	struct mc16_test_refs *refs;
	struct mc16_storage_fixture *saved_probe;
	struct memcomp_ref guard;
	struct memcomp_store_ctx store[2];
	struct mm_struct *mm;
	unsigned long addr;
	unsigned long mapping;
	struct folio *folio;
	struct anon_vma *anon_vma;
	struct mc16_zero_owners owners;
	s64 swapents;
	s64 anonpages;
	u32 tokens[2];
};

/* Acquire and release in this cleanup call, never inherit the test's lock. */
static void mc16_test_zc_objcg_cleanup(void *data)
{
	struct memcomp_test_objcg_fixture *owner = data;

	if (owner->folio)
		folio_lock(owner->folio);
	memcomp_test_objcg_cleanup(owner);
}

static void mc16_test_zc_storage_cleanup(void *data)
{
	struct mc16_test_zc *f = data;

	mc16_test_storage_cleanup(f->refs->storage);
	rcu_assign_pointer(mc16_storage_fixture, f->saved_probe);
	synchronize_rcu();
}

static s64 mc16_test_zc_rss(struct mm_struct *mm, int member)
{
	return percpu_counter_sum(&mm->rss_stat[member]);
}

/* No owner survives an unmap into the still-active entry80 dispatch. */
static int mc16_test_zc_zap(struct mc16_test_zc *f, unsigned long address)
{
	struct memcomp_ref removed = { };
	spinlock_t *ptl; /* Protects the actual PTE ownership transaction. */
	pte_t *ptep, pte;
	pmd_t *pmd;
	unsigned long token;
	int ret = 0;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, address);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(f->mm, pmd, address, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if (!is_swap_pte(pte) || !is_memcomp_entry(pte_to_swp_entry(pte)))
		goto out_pte;
	token = swp_offset(pte_to_swp_entry(pte));
	/* Cleanup also works after the test explicitly retires its main guard. */
	if (token && ((token != f->tokens[0] && token != f->tokens[1]) ||
		      !mc16_get(token, &removed)))
		ret = -ENOENT;
	ptep_get_and_clear(f->mm, address, ptep);
	add_mm_counter(f->mm, MM_SWAPENTS, -1);
	if (removed.slot)
		mc16_ref_put_count(&removed, 2); /* Lookup guard plus PTE owner. */
	else if (!token)
		mc16_zero_pte_put(1);
out_pte:
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	return ret;
}

static void mc16_test_zc_cleanup(void *data)
{
	struct mc16_test_zc *f = data;
	struct mc16_zero_owners owners;
	int ret;

	ret = mc16_test_zc_zap(f, f->addr);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	ret = mc16_test_zc_zap(f, f->addr + PAGE_SIZE);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	/* Any present page left by an incomplete setup uses normal MM teardown. */
	ret = mc16_test_dontneed(f->mm, f->mapping, 3 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	if (f->anon_vma)
		put_anon_vma(f->anon_vma);
	if (f->guard.slot)
		mc16_ref_put(&f->guard);
	if (f->folio) {
		struct memcomp_ref *owner = &f->refs->storage->refs[0];

		folio_lock(f->folio);
		if (owner->slot)
			mc16_ref_put(owner);
		folio_unlock(f->folio);
		f->refs->storage->owner->model.folio = f->refs->storage->owner->folio;
		folio_put(f->folio);
	}
	mc16_zero_owners_read(&owners);
	KUNIT_EXPECT_EQ(f->test, owners.pte_pages, f->owners.pte_pages);
	KUNIT_EXPECT_EQ(f->test, owners.cache_pages, f->owners.cache_pages);
	KUNIT_EXPECT_EQ(f->test, mc16_test_zc_rss(f->mm, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(f->test, mc16_test_zc_rss(f->mm, MM_ANONPAGES), f->anonpages);
	mmput(f->mm);
}

/* Carrier readiness comes only from the real refresh's existing commit T. */
static int mc16_test_zc_refresh(struct folio *folio, struct memcomp_ref *ref,
				struct memcomp_store_ctx *store)
{
	struct memcomp_reclaim_ctx *ctx;
	unsigned int flags;
	void *src;
	int ret;

	flags = memcomp_test_reclaimer_enter();
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_KSWAPD);
	if (!ctx) {
		memcomp_test_reclaimer_exit(flags);
		return -EBUSY;
	}
	src = kmap_local_folio(folio, 0);
	ret = mc16_anon_refresh(&ctx, src, 1, store, ref);
	kunmap_local(src);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	memcomp_direct_staging_put(store->staging);
	store->staging = NULL;
	memcomp_test_reclaimer_exit(flags);
	return ret;
}

/* Shared reserve/cleanup logic, with no folio lock held on return. */
static int mc16_test_zc_storage(struct mc16_test_zc *f)
{
	struct kunit *test = f->test;
	struct memcomp_test_objcg_fixture *owner;
	struct mc16_test_storage *s;
	unsigned int i;
	int ret;

	f->refs = kunit_kzalloc(test, sizeof(*f->refs), GFP_KERNEL);
	s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	if (!f->refs || !s)
		return -ENOMEM;
	f->refs->storage = s;
	owner = memcomp_test_objcg_begin(test, 1);
	if (!owner)
		return -ENOMEM;
	folio_unlock(owner->folio);
	kunit_remove_action(test, memcomp_test_objcg_cleanup, owner);
	ret = kunit_add_action_or_reset(test, mc16_test_zc_objcg_cleanup, owner);
	if (ret)
		return ret;
	s->test = test;
	s->owner = owner;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&s->baseline);
#endif
	f->saved_probe = rcu_access_pointer(mc16_storage_fixture);
	s->probe.task = current;
	ret = kunit_add_action_or_reset(test, mc16_test_zc_storage_cleanup, f);
	if (ret)
		return ret;
	rcu_assign_pointer(mc16_storage_fixture, &s->probe);
	f->refs->stored_before = atomic64_read(&memcomp_stored_bytes);
	f->refs->compressed_before = atomic64_read(&memcomp_compressed_bytes);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	f->refs->pending_before = atomic64_read(&memcomp_pending_pages);
#endif
	ret = kunit_add_action_or_reset(test, mc16_test_refs_cleanup, f->refs);
	if (ret)
		return ret;
	/* Two real refreshes return separate borrowed carriers, without extra T. */
	for (i = 0; i < ARRAY_SIZE(f->tokens); i++) {
		struct memcomp_ref lookup = { };
		void *src;

		folio_lock(owner->folio);
		src = kmap_local_folio(owner->folio, 0);
		memset(src, 0, PAGE_SIZE);
		kunmap_local(src);
		ret = mc16_test_refs_reserve(f->refs, 1);
		if (!ret) {
			f->tokens[i] = s->refs[i].token;
			if (!mc16_get(f->tokens[i], &lookup))
				ret = -ENOENT;
			else
				ret = mc16_test_zc_refresh(owner->folio, &lookup,
							   &f->store[i]);
		}
		if (lookup.slot)
			mc16_ref_put(&lookup);
		folio_unlock(owner->folio);
		if (ret)
			return ret;
	}
	return 0;
}

static struct mc16_test_zc *mc16_test_zc_begin(struct kunit *test)
{
	struct mc16_test_zc *f;
	struct vm_area_struct *vma;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	if (mc16_test_zc_storage(f))
		return NULL;
	f->mapping = kunit_vm_mmap(test, NULL, 0, 3 * PAGE_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (!f->mapping || IS_ERR_VALUE(f->mapping))
		return NULL;
	/* Always choose two adjacent pages sharing one PTE-table lock. */
	f->addr = f->mapping;
	if ((f->addr >> PMD_SHIFT) != ((f->addr + PAGE_SIZE) >> PMD_SHIFT))
		f->addr += PAGE_SIZE;
	f->mm = current->mm;
	mmget(f->mm);
	mc16_zero_owners_read(&f->owners);
	f->swapents = mc16_test_zc_rss(f->mm, MM_SWAPENTS);
	f->anonpages = mc16_test_zc_rss(f->mm, MM_ANONPAGES);
	ret = kunit_add_action_or_reset(test, mc16_test_zc_cleanup, f);
	if (ret)
		return NULL;
	if (!mc16_get(f->tokens[0], &f->guard))
		return NULL;
	mmap_write_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma) {
		vma_start_write(vma);
		vm_flags_set(vma, VM_NOHUGEPAGE);
	}
	mmap_write_unlock(f->mm);
	return vma ? f : NULL;
}

/* Construct expected flags independently of pte_move_swp_offset(). */
static pte_t mc16_test_zc_pte(unsigned long token, unsigned int flags)
{
	pte_t pte = swp_entry_to_pte(make_memcomp_entry(token));

	if (flags & 1)
		pte = pte_swp_mksoft_dirty(pte);
	if (flags & 2)
		pte = pte_swp_mkuffd_wp(pte);
	if (flags & 4)
		pte = pte_swp_mkexclusive(pte);
	return pte;
}

static int mc16_test_zc_install(struct mc16_test_zc *f,
				unsigned long address, unsigned int index,
				unsigned int flags)
{
	struct memcomp_ref owner = { };
	spinlock_t *ptl; /* Transfers one actual positive-token PTE reference. */
	pte_t *ptep;
	int ret = 0;

	if (!mc16_get(f->tokens[index], &owner))
		return -ENOENT;
	mmap_read_lock(f->mm);
	ptep = get_locked_pte(f->mm, address, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out;
	}
	__acquire(RCU);
	if (!pte_none(ptep_get(ptep))) {
		ret = -EEXIST;
	} else {
		set_pte_at(f->mm, address, ptep,
			   mc16_test_zc_pte(owner.token, flags));
		add_mm_counter(f->mm, MM_SWAPENTS, 1);
		owner = (struct memcomp_ref) { };
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	if (owner.slot)
		mc16_ref_put(&owner);
	return ret;
}

static pte_t mc16_test_zc_read(struct mc16_test_zc *f, unsigned long address)
{
	spinlock_t *ptl; /* Samples the real PTE without retaining its pointer. */
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

/* Callback tests borrow the real refresh result; they do not prepare it. */
static unsigned int mc16_test_zc_callback(struct mc16_test_zc *f)
{
	struct mc16_zero_walk walk = { .ref = f->store[0].zero.ref };
	struct vm_area_struct *vma;

	if (!f->store[0].zero.valid || !f->store[0].zero.ready)
		return 0;
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma)
		mc16_zero_convert_one(f->refs->storage->owner->folio, vma,
				      f->addr, &walk);
	mmap_read_unlock(f->mm);
	return walk.converted;
}

static void mc16_test_zc_counts(struct mc16_test_zc *f, s64 zeros, s64 swapped)
{
	struct mc16_zero_owners owners;

	mc16_zero_owners_read(&owners);
	KUNIT_EXPECT_EQ(f->test, owners.pte_pages, f->owners.pte_pages + zeros);
	KUNIT_EXPECT_EQ(f->test, owners.cache_pages, f->owners.cache_pages);
	KUNIT_EXPECT_EQ(f->test, mc16_test_zc_rss(f->mm, MM_SWAPENTS),
			f->swapents + swapped);
}

/* Net owner totals alone would miss a spurious get/put pair. */
static void mc16_test_zc_events(struct mc16_test_zc *f,
				struct mc16_zero_diagnostics *before,
				unsigned int inits)
{
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct mc16_zero_diagnostics after;
	unsigned int event;

	mc16_zero_diagnostics_read(&after);
	for (event = 0; event < MC16_ZERO_EVENTS; event++) {
		u64 expected = event == MC16_ZERO_PTE_INIT ? inits : 0;

		KUNIT_EXPECT_EQ(f->test, after.calls[event] - before->calls[event], expected);
		KUNIT_EXPECT_EQ(f->test, after.pages[event] - before->pages[event], expected);
	}
#endif
}

/* Fault a genuine order-0 anonymous page, then pin before its last unmap. */
static int mc16_test_zc_unmap_anon(struct mc16_test_zc *f, unsigned int flags)
{
	struct vm_area_struct *vma;
	struct memcomp_ref owner = { };
	spinlock_t *ptl; /* Revalidates and replaces the faulted present PTE. */
	pte_t *ptep, orig;
	vm_fault_t fault;
	int ret = -EINVAL;

	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (!vma)
		goto out_mmap;
	fault = handle_mm_fault(vma, f->addr, FAULT_FLAG_WRITE, NULL);
	/* These returns mean the fault actor has already dropped mmap_lock. */
	if (fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED))
		return -EAGAIN;
	if (fault & VM_FAULT_ERROR)
		goto out_mmap;
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out_mmap;
	}
	__acquire(RCU);
	orig = ptep_get(ptep);
	if (!pte_present(orig) || pte_special(orig)) {
		pte_unmap_unlock(ptep, ptl);
		goto out_mmap;
	}
	f->folio = page_folio(pte_page(orig));
	folio_get(f->folio);
	pte_unmap_unlock(ptep, ptl);
	folio_lock(f->folio);
	if (!folio_test_anon(f->folio) || folio_test_large(f->folio) ||
	    !pages_identical(&f->folio->page, ZERO_PAGE(0)))
		goto out_folio;
	f->anon_vma = folio_get_anon_vma(f->folio);
	if (!f->anon_vma)
		goto out_folio;
	/* Replace the synthetic primary with this real source's own OWNER. */
	mc16_ref_put(&f->guard);
	mc16_ref_put(&f->refs->storage->refs[0]);
	f->refs->storage->owner->model.folio = f->folio;
	{
		struct mc16_record *record;
		struct memcomp_ref *initial = &f->refs->storage->refs[0];

		record = mc16_record_alloc(f->folio, 1, false);
		if (IS_ERR(record)) {
			ret = PTR_ERR(record);
			goto out_folio;
		}
		ret = mc16_reserve_hint(record, 1, MC16_KEY_ANON,
					mc16_folio_zero_hint(f->folio), initial);
		if (ret) {
			mc16_record_abort(record);
			goto out_folio;
		}
		mc16_account_reservation(initial);
		f->tokens[0] = initial->token;
	}
	if (!mc16_get(f->tokens[0], &f->guard) ||
	    !mc16_get(f->tokens[0], &owner)) {
		ret = -ENOENT;
		goto out_folio;
	}
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out_folio;
	}
	__acquire(RCU);
	if (!pte_same(ptep_get(ptep), orig)) {
		ret = -ESTALE;
	} else {
		flush_cache_page(vma, f->addr, pte_pfn(orig));
		ptep_clear_flush(vma, f->addr, ptep);
		folio_remove_rmap_pte(f->folio, &f->folio->page, vma);
		set_pte_at(f->mm, f->addr, ptep,
			   mc16_test_zc_pte(owner.token, flags));
		add_mm_counter(f->mm, MM_ANONPAGES, -1);
		add_mm_counter(f->mm, MM_SWAPENTS, 1);
		owner = (struct memcomp_ref) { };
		folio_put(f->folio); /* Retire the old present mapping's ref. */
		ret = 0;
	}
	pte_unmap_unlock(ptep, ptl);
	if (!ret)
		ret = mc16_test_zc_refresh(f->folio, &f->guard, &f->store[0]);
out_folio:
	if (owner.slot)
		mc16_ref_put(&owner);
	folio_unlock(f->folio);
out_mmap:
	mmap_read_unlock(f->mm);
	return ret;
}

/* The helper acquires/releases anon_vma's read lock; we retain the real pin. */
static unsigned int mc16_test_zc_walk(struct mc16_test_zc *f)
{
	unsigned int converted;

	mmap_read_lock(f->mm);
	folio_lock(f->folio);
	converted = mc16_zero_convert_folio(f->folio, &f->store[0].zero);
	folio_unlock(f->folio);
	mmap_read_unlock(f->mm);
	return converted;
}

static void memcomp_test_zero_convert_flags(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	unsigned int flags;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (flags = 0; flags < 8; flags++) {
		struct mc16_zero_diagnostics before;
		unsigned int refs;
		pte_t orig, actual;

		KUNIT_ASSERT_EQ(test, mc16_test_zc_install(f, f->addr, 0, flags), 0);
		orig = mc16_test_zc_read(f, f->addr);
		refs = refcount_read(&f->guard.slot->refs);
		mc16_zero_diagnostics_read(&before);
		KUNIT_EXPECT_EQ(test, mc16_test_zc_callback(f), 1U);
		actual = mc16_test_zc_read(f, f->addr);
		KUNIT_EXPECT_TRUE(test, pte_same(actual,
						 mc16_test_zc_pte(0, flags)));
		KUNIT_EXPECT_EQ(test, pte_swp_soft_dirty(actual), pte_swp_soft_dirty(orig));
		KUNIT_EXPECT_EQ(test, pte_swp_uffd_wp(actual), pte_swp_uffd_wp(orig));
		KUNIT_EXPECT_EQ(test, pte_swp_exclusive(actual), pte_swp_exclusive(orig));
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs - 1);
		mc16_test_zc_counts(f, 1, 1);
		/* A second PTL visit loses; no extra ledger init or positive put. */
		KUNIT_EXPECT_EQ(test, mc16_test_zc_callback(f), 0U);
		KUNIT_EXPECT_TRUE(test, pte_same(mc16_test_zc_read(f, f->addr), actual));
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs - 1);
		mc16_test_zc_counts(f, 1, 1);
		mc16_test_zc_events(f, &before, 1);
		KUNIT_ASSERT_EQ(test, mc16_test_zc_zap(f, f->addr), 0);
		mc16_test_zc_counts(f, 0, 0);
	}
}

static void memcomp_test_zero_convert_stale(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct memcomp_ref *other;
	struct mc16_zero_diagnostics before;
	unsigned int refs, other_refs;
	pte_t orig;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_zero_diagnostics_read(&before);
	KUNIT_EXPECT_EQ(test, mc16_test_zc_callback(f), 0U); /* No PTE table. */
	KUNIT_ASSERT_EQ(test, mc16_test_zc_install(f, f->addr, 1, 7), 0);
	orig = mc16_test_zc_read(f, f->addr);
	other = &f->refs->storage->refs[1];
	refs = refcount_read(&f->guard.slot->refs);
	other_refs = refcount_read(&other->slot->refs);
	KUNIT_EXPECT_EQ(test, mc16_test_zc_callback(f), 0U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_test_zc_read(f, f->addr), orig));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs);
	KUNIT_EXPECT_EQ(test, refcount_read(&other->slot->refs), other_refs);
	mc16_test_zc_counts(f, 0, 1);
	mc16_test_zc_events(f, &before, 0);
}

static void memcomp_test_zero_convert_rmap(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct memcomp_ref probe = { };
	struct mc16_zero_diagnostics before;
	struct mc16_slot *slot;
	unsigned int converted;
	bool locked;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_zc_unmap_anon(f, 7), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->anon_vma);
	KUNIT_EXPECT_FALSE(test, folio_mapped(f->folio));
	KUNIT_EXPECT_FALSE(test, folio_test_locked(f->folio));
	/* Source OWNER survives conversion, alongside the PTE and test guard. */
	slot = f->guard.slot;
	KUNIT_EXPECT_EQ(test, refcount_read(&slot->refs), 3U);
	mc16_zero_diagnostics_read(&before);
	converted = mc16_test_zc_walk(f);
	KUNIT_EXPECT_EQ(test, converted, 1U);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.slot, slot);
	KUNIT_EXPECT_EQ(test, refcount_read(&slot->refs), 2U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_test_zc_read(f, f->addr),
					 mc16_test_zc_pte(0, 7)));
	mc16_test_zc_counts(f, 1, 1);
	KUNIT_EXPECT_EQ(test, mc16_test_zc_rss(f->mm, MM_ANONPAGES), f->anonpages);
	KUNIT_EXPECT_EQ(test, mc16_test_zc_walk(f), 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&slot->refs), 2U);
	mc16_test_zc_events(f, &before, 1);
	/* A writer can acquire the rmap lock after the walk has returned. */
	locked = down_write_trylock(&f->anon_vma->root->rwsem);
	if (locked)
		up_write(&f->anon_vma->root->rwsem);
	KUNIT_EXPECT_TRUE(test, locked);
	mc16_ref_put(&f->guard);
	folio_lock(f->folio);
	mc16_ref_put(&f->refs->storage->refs[0]);
	folio_unlock(f->folio);
	/* No access to slot after final put. A fresh lookup must now miss. */
	locked = mc16_get(f->tokens[0], &probe);
	if (probe.slot)
		mc16_ref_put(&probe);
	KUNIT_EXPECT_FALSE(test, locked);
}

/* Move one swap PTE inside the same VMA, after folio->index was established. */
static int mc16_test_zc_move(struct mc16_test_zc *f)
{
	spinlock_t *ptl; /* Both addresses share a prevalidated PTE-table lock. */
	pte_t *src, *dst;
	unsigned long destination = f->addr + PAGE_SIZE;
	int ret = 0;

	/* Materialize the destination table before taking either PTL. */
	mmap_write_lock(f->mm);
	dst = get_locked_pte(f->mm, destination, &ptl);
	if (!dst) {
		ret = -ENOMEM;
		goto out;
	}
	__acquire(RCU);
	pte_unmap_unlock(dst, ptl);
	/* Different PTE tables would require two locks and a different model. */
	if ((f->addr >> PMD_SHIFT) != (destination >> PMD_SHIFT)) {
		ret = -ERANGE;
		goto out;
	}
	src = pte_offset_map_lock(f->mm, mm_find_pmd(f->mm, f->addr), f->addr, &ptl);
	if (!src) {
		ret = -ENOMEM;
		goto out;
	}
	dst = src + 1;
	if (!is_swap_pte(ptep_get(src)) || !pte_none(ptep_get(dst))) {
		ret = -EINVAL;
	} else {
		pte_t pte = ptep_get_and_clear(f->mm, f->addr, src);

		set_pte_at(f->mm, destination, dst, pte);
	}
	pte_unmap_unlock(src, ptl);
out:
	mmap_write_unlock(f->mm);
	return ret;
}

static void memcomp_test_zero_convert_move_miss(struct kunit *test)
{
	struct mc16_test_zc *f = mc16_test_zc_begin(test);
	struct mc16_zero_diagnostics before;
	unsigned long *dst;
	unsigned int refs;
	swp_entry_t saved_swap;
	pte_t orig;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	KUNIT_ASSERT_EQ(test, mc16_test_zc_unmap_anon(f, 7), 0);
	orig = mc16_test_zc_read(f, f->addr);
	ret = mc16_test_zc_move(f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	refs = refcount_read(&f->guard.slot->refs);
	mc16_zero_diagnostics_read(&before);
	KUNIT_EXPECT_EQ(test, mc16_test_zc_walk(f), 0U);
	KUNIT_EXPECT_TRUE(test, pte_none(mc16_test_zc_read(f, f->addr)));
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_test_zc_read(f, f->addr + PAGE_SIZE), orig));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs);
	mc16_test_zc_counts(f, 0, 1);
	mc16_test_zc_events(f, &before, 0);
	/* The checks above cover conversion miss before source exit. */
	folio_lock(f->folio);
	saved_swap = f->folio->swap;
	/* The low-level reservation has not bound its token to the source. */
	f->folio->swap = make_memcomp_entry(f->tokens[0]);
	folio_unlock(f->folio);
	ret = mc16_flt_release(f->folio);
	if (ret) {
		/* Release consumed OWNER, not this fixture's guard or moved PTE. */
		f->refs->storage->refs[0] = (struct memcomp_ref) { };
	} else {
		/* Failed release leaves OWNER for the original cleanup actor. */
		folio_lock(f->folio);
		f->folio->swap = saved_swap;
		folio_unlock(f->folio);
	}
	KUNIT_ASSERT_TRUE(test, ret);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs - 1);
	/* The read phase starts after real publication and OWNER retirement. */
	refs = refcount_read(&f->guard.slot->refs);
	memset(dst, 0xa7, PAGE_SIZE);
	ret = mc16_load_page(&f->guard, dst);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(dst, 0, PAGE_SIZE), NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), refs);
}
#endif /* CONFIG_MEMCG && CONFIG_MMU && CONFIG_ADVISE_SYSCALLS */
#endif /* _MM_MEMCOMP_TESTS_ZERO_CONVERT_TEST_H */
