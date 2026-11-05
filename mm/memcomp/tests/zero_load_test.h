/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ZERO_LOAD_TEST_H
#define _MM_MEMCOMP_TESTS_ZERO_LOAD_TEST_H

#include <kunit/resource.h>
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>

/*
 * Include after zero_load.h and the storage/refs test fixtures in suite.h.
 * These are real mmap/PTE/folio/rmap actors calling mc16_load_zero() directly.
 * No old entry fixture, old zero ledger or public dispatch is used. Under
 * MEMCG an unrelated real STORED slot uses the existing modeled objcg fixture;
 * the fault folio itself always uses the real MM allocator and memcg charge.
 */
#if defined(CONFIG_MEMCOMP_KUNIT_TEST) && defined(CONFIG_MMU) && \
	defined(CONFIG_ADVISE_SYSCALLS)
#define MC16_ZERO_LOAD_MM_TEST_ENABLED

struct mc16_zl_storage {
	s64 stored;
	s64 compressed;
#ifdef CONFIG_MEMCG
	unsigned int allocations;
	unsigned int refs;
	int gets;
	int puts;
	int charges;
	int uncharges;
	int folio_refs;
	long charged;
	u64 word;
	u16 flags;
	u16 length;
#endif
};

struct mc16_zl_mm {
	struct kunit *test;
	struct mm_struct *mm;
	unsigned long addr;
	vm_flags_t flags;
	pgprot_t prot;
	bool saved_vma;
	struct mc16_zero_owners owners;
	s64 swapents;
	s64 anonpages;
	/* Pin any sampled present folio under PTL until cleanup, even on ASSERT. */
	struct folio *folio;
	struct mc16_zl_storage storage;
#ifdef CONFIG_MEMCG
	struct mc16_test_refs *refs;
#endif
#ifdef CONFIG_PER_VMA_LOCK
	struct completion writer_locked;
	struct completion writer_release;
	struct task_struct *writer;
#endif
};

static s64 mc16_zl_rss(struct mm_struct *mm, int member)
{
	return percpu_counter_sum(&mm->rss_stat[member]);
}

static void mc16_zl_storage_read(struct mc16_zl_mm *f,
				 struct mc16_zl_storage *out)
{
#ifdef CONFIG_MEMCG
	struct mc16_test_storage *s = f->refs->storage;
	struct memcomp_ref *ref = &s->refs[0];
	struct mc16_view view;

	out->allocations = s->probe.allocations;
	out->refs = refcount_read(&ref->slot->refs);
	out->gets = atomic_read(&s->owner->model.get_calls);
	out->puts = atomic_read(&s->owner->model.put_calls);
	out->charges = atomic_read(&s->owner->model.charge_calls);
	out->uncharges = atomic_read(&s->owner->model.uncharge_calls);
	out->folio_refs = folio_ref_count(s->owner->folio);
	out->charged = atomic_long_read(&s->owner->model.charged);
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	out->word = *view.word;
	out->flags = *view.flags;
	out->length = *view.length;
	mutex_unlock(memcomp_lock_of(ref->token));
#endif
	out->stored = atomic64_read(&memcomp_stored_bytes);
	out->compressed = atomic64_read(&memcomp_compressed_bytes);
}

static void mc16_zl_storage_unchanged(struct mc16_zl_mm *f)
{
	struct mc16_zl_storage after;
	struct kunit *test = f->test;
#ifdef CONFIG_MEMCG
#endif

	mc16_zl_storage_read(f, &after);
	KUNIT_EXPECT_EQ(test, after.stored, f->storage.stored);
	KUNIT_EXPECT_EQ(test, after.compressed, f->storage.compressed);
#ifdef CONFIG_MEMCG
	KUNIT_EXPECT_EQ(test, after.allocations, f->storage.allocations);
	KUNIT_EXPECT_EQ(test, after.refs, f->storage.refs);
	KUNIT_EXPECT_EQ(test, after.gets, f->storage.gets);
	KUNIT_EXPECT_EQ(test, after.puts, f->storage.puts);
	KUNIT_EXPECT_EQ(test, after.charges, f->storage.charges);
	KUNIT_EXPECT_EQ(test, after.uncharges, f->storage.uncharges);
	KUNIT_EXPECT_EQ(test, after.folio_refs, f->storage.folio_refs);
	KUNIT_EXPECT_EQ(test, after.charged, f->storage.charged);
	KUNIT_EXPECT_EQ(test, after.word, f->storage.word);
	KUNIT_EXPECT_EQ(test, after.flags, f->storage.flags);
	KUNIT_EXPECT_EQ(test, after.length, f->storage.length);
#endif
}

static void mc16_zl_counts(struct mc16_zl_mm *f, s64 ptes, s64 present)
{
	struct mc16_zero_owners owners;

	mc16_zero_owners_read(&owners);
	KUNIT_EXPECT_EQ(f->test, owners.pte_pages, f->owners.pte_pages + ptes);
	KUNIT_EXPECT_EQ(f->test, owners.cache_pages, f->owners.cache_pages);
	KUNIT_EXPECT_EQ(f->test, mc16_zl_rss(f->mm, MM_SWAPENTS),
			f->swapents + ptes);
	KUNIT_EXPECT_EQ(f->test, mc16_zl_rss(f->mm, MM_ANONPAGES),
			f->anonpages + present);
	mc16_zl_storage_unchanged(f);
}

static pte_t mc16_zl_zero_pte(void)
{
	return swp_entry_to_pte(make_memcomp_entry(0));
}

/* A real zap actor; old entry80 invalidation cannot retire this ledger. */
static void mc16_zl_zap_zero(struct mc16_zl_mm *f)
{
	spinlock_t *ptl; /* Serializes the actual token0 PTE owner. */
	pte_t *ptep, pte;
	pmd_t *pmd;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, f->addr);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(f->mm, pmd, f->addr, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if (is_swap_pte(pte) && is_memcomp_entry(pte_to_swp_entry(pte)) &&
	    !swp_offset(pte_to_swp_entry(pte))) {
		ptep_get_and_clear(f->mm, f->addr, ptep);
		add_mm_counter(f->mm, MM_SWAPENTS, -1);
		mc16_zero_pte_put(1);
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
}

static void mc16_zl_cleanup(void *data)
{
	struct mc16_zl_mm *f = data;
	struct vm_area_struct *vma;
	int ret;

	WRITE_ONCE(mc16_zero_fault_probe.task, NULL);
	mc16_zero_fault_probe.fail_alloc = false;
	mc16_zero_fault_probe.fail_charge = false;
#ifdef CONFIG_PER_VMA_LOCK
	if (f->writer) {
		complete_all(&f->writer_release);
		kthread_stop_put(f->writer);
		f->writer = NULL;
	}
#endif
	mc16_zl_zap_zero(f);
	mmap_write_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma && f->saved_vma) {
		vma_start_write(vma);
		vm_flags_reset(vma, f->flags);
		vma->vm_page_prot = f->prot;
	}
	mmap_write_unlock(f->mm);
	/* Normal MM removes rmap/LRU and drops the mapping's real folio ref. */
	ret = mc16_test_dontneed(f->mm, f->addr, PAGE_SIZE);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	if (f->folio) {
		folio_put(f->folio);
		f->folio = NULL;
	}
	mc16_zl_counts(f, 0, 0);
	mmput(f->mm);
}

static struct mc16_zl_mm *mc16_zl_begin(struct kunit *test)
{
	struct mc16_zl_mm *f;
	struct vm_area_struct *vma;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
#ifdef CONFIG_MEMCG
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs)
		return NULL;
	if (mc16_test_refs_reserve(f->refs, 1) ||
	    mc16_test_refs_payload(f->refs, 0, MC16_STORED))
		return NULL;
#endif
	f->addr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (!f->addr || IS_ERR_VALUE(f->addr))
		return NULL;
	f->mm = current->mm;
	mmget(f->mm);
	mc16_zero_owners_read(&f->owners);
	f->swapents = mc16_zl_rss(f->mm, MM_SWAPENTS);
	f->anonpages = mc16_zl_rss(f->mm, MM_ANONPAGES);
	mc16_zl_storage_read(f, &f->storage);
#ifdef CONFIG_PER_VMA_LOCK
	init_completion(&f->writer_locked);
	init_completion(&f->writer_release);
#endif
	/* Registered last: PTE/folio cleanup precedes the unrelated slot puts. */
	if (kunit_add_action_or_reset(test, mc16_zl_cleanup, f))
		return NULL;
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma) {
		f->flags = vma->vm_flags;
		f->prot = vma->vm_page_prot;
		f->saved_vma = true;
	}
	mmap_read_unlock(f->mm);
	/* Leave anon_vma unprepared for the genuine VMA-lock retry actor. */
	return vma ? f : NULL;
}

static int mc16_zl_install(struct mc16_zl_mm *f, pte_t pte)
{
	spinlock_t *ptl; /* Publishes and accounts one real PTE atomically. */
	pte_t *ptep;
	int ret = 0;

	mmap_read_lock(f->mm);
	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out;
	}
	__acquire(RCU);
	if (!pte_none(ptep_get(ptep))) {
		ret = -EEXIST;
	} else {
		set_pte_at(f->mm, f->addr, ptep, pte);
		add_mm_counter(f->mm, MM_SWAPENTS, 1);
		mc16_zero_pte_init(1);
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	return ret;
}

static pte_t mc16_zl_read(struct mc16_zl_mm *f)
{
	spinlock_t *ptl; /* Pins a sampled present page while it is still mapped. */
	pte_t *ptep, pte = __pte(0);
	pmd_t *pmd;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, f->addr);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(f->mm, pmd, f->addr, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if (pte_present(pte) && !pte_special(pte) && !f->folio) {
		f->folio = page_folio(pte_page(pte));
		folio_get(f->folio);
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	return pte;
}

static vm_fault_t mc16_zl_fault(struct mc16_zl_mm *f, pte_t orig,
				unsigned int flags)
{
	struct vm_area_struct *vma;
	vm_fault_t ret;
	bool locked = true;

	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (!vma) {
		mmap_read_unlock(f->mm);
		KUNIT_FAIL(f->test, "zero fault lost VMA");
		return VM_FAULT_SIGBUS;
	}
	{
		struct vm_fault vmf = {
			.vma = vma,
			.address = f->addr,
			.real_address = f->addr,
			.flags = flags | FAULT_FLAG_ORIG_PTE_VALID,
			.pmd = mm_find_pmd(f->mm, f->addr),
			.orig_pte = orig,
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};

		ret = mc16_load_zero(&vmf, &locked);
		KUNIT_EXPECT_PTR_EQ(f->test, vmf.pte, NULL);
	}
	mmap_read_unlock(f->mm);
	KUNIT_EXPECT_FALSE(f->test, locked);
	return ret;
}

static void mc16_zl_present(struct mc16_zl_mm *f, pte_t pte)
{
	struct kunit *test = f->test;
	struct folio *folio = f->folio;
	void *data;

	KUNIT_ASSERT_TRUE(test, pte_present(pte));
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_ASSERT_PTR_EQ(test, pte_page(pte), &folio->page);
	KUNIT_EXPECT_TRUE(test, folio_test_anon(folio));
	KUNIT_EXPECT_TRUE(test, folio_test_swapbacked(folio));
	KUNIT_EXPECT_TRUE(test, folio_test_uptodate(folio));
	KUNIT_EXPECT_FALSE(test, folio_test_locked(folio));
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(&folio->page));
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);
	data = kmap_local_folio(folio, 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(data, 0, PAGE_SIZE), NULL);
	kunmap_local(data);
	mc16_zl_counts(f, 0, 1);
}

static void memcomp_test_zero_load_write(struct kunit *test)
{
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	pte_t orig = pte_swp_mksoft_dirty(mc16_zl_zero_pte());
	pte_t pte;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	mc16_zl_counts(f, 1, 0);
	ret = mc16_zl_fault(f, orig, FAULT_FLAG_WRITE);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	pte = mc16_zl_read(f);
	mc16_zl_present(f, pte);
	KUNIT_EXPECT_TRUE(test, pte_write(pte));
	KUNIT_EXPECT_TRUE(test, pte_dirty(pte));
	/* A stale second fault must not consume the same logical owner twice. */
	ret = mc16_zl_fault(f, orig, FAULT_FLAG_WRITE);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret, 0U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_zl_read(f), pte));
	mc16_zl_counts(f, 0, 1);
}

static void memcomp_test_zero_load_softdirty(struct kunit *test)
{
	struct mc16_zl_mm *f;
	pte_t orig = pte_swp_mksoft_dirty(mc16_zl_zero_pte());
	pte_t pte;
	vm_fault_t ret;

	if (!pte_swp_soft_dirty(orig)) {
		kunit_skip(test, "architecture has no swap soft-dirty bit");
		return;
	}
	f = mc16_zl_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	ret = mc16_zl_fault(f, orig, 0);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	pte = mc16_zl_read(f);
	mc16_zl_present(f, pte);
	KUNIT_EXPECT_TRUE(test, pte_soft_dirty(pte));
	/* Writable read PTEs can encode hardware dirty on arm64. */
}

/* Change only VMA policy; no replacement page or fake charge backend. */
static void mc16_zl_policy(struct mc16_zl_mm *f, vm_flags_t set,
			   vm_flags_t clear)
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
	KUNIT_EXPECT_NOT_NULL(f->test, vma);
}

static void memcomp_test_zero_load_softdirty_wp(struct kunit *test)
{
	struct mc16_zl_mm *f;
	pte_t orig = mc16_zl_zero_pte();
	pte_t pte;
	vm_fault_t ret;

	if (!IS_ENABLED(CONFIG_MEM_SOFT_DIRTY)) {
		kunit_skip(test, "soft-dirty tracking is disabled");
		return;
	}
	f = mc16_zl_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_zl_policy(f, 0, VM_SOFTDIRTY);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	ret = mc16_zl_fault(f, orig, 0);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	pte = mc16_zl_read(f);
	mc16_zl_present(f, pte);
	KUNIT_EXPECT_FALSE(test, pte_write(pte));
	KUNIT_EXPECT_FALSE(test, pte_soft_dirty(pte));
	KUNIT_EXPECT_FALSE(test, pte_dirty(pte));
}

static void memcomp_test_zero_load_uffd_wp(struct kunit *test)
{
	struct mc16_zl_mm *f;
	pte_t orig = pte_swp_mkuffd_wp(mc16_zl_zero_pte());
	pte_t pte;
	vm_fault_t ret;

	if (!IS_ENABLED(CONFIG_USERFAULTFD) || !pte_swp_uffd_wp(orig)) {
		kunit_skip(test, "architecture has no enabled swap UFFD-WP bit");
		return;
	}
	f = mc16_zl_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	/* Read fault needs the WP policy bit, not a registered userfaultfd. */
	mc16_zl_policy(f, VM_UFFD_WP, 0);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	ret = mc16_zl_fault(f, orig, 0);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	pte = mc16_zl_read(f);
	mc16_zl_present(f, pte);
	KUNIT_EXPECT_TRUE(test, pte_uffd_wp(pte));
	KUNIT_EXPECT_FALSE(test, pte_write(pte));
	KUNIT_EXPECT_FALSE(test, pte_dirty(pte));
}

static void memcomp_test_zero_load_readonly(struct kunit *test)
{
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	pte_t orig = pte_swp_mksoft_dirty(mc16_zl_zero_pte());
	pte_t pte;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_zl_policy(f, 0, VM_WRITE);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	ret = mc16_zl_fault(f, orig, 0);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	pte = mc16_zl_read(f);
	mc16_zl_present(f, pte);
	KUNIT_EXPECT_FALSE(test, pte_write(pte));
	KUNIT_EXPECT_FALSE(test, pte_dirty(pte));
}

static void memcomp_test_zero_load_changed(struct kunit *test)
{
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	pte_t orig = mc16_zl_zero_pte();
	pte_t changed = pte_swp_mkexclusive(orig);
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_FALSE(test, pte_same(orig, changed));
	/* Actor commits a newer complete PTE before the stale fault takes PTL. */
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, changed), 0);
	mc16_zl_counts(f, 1, 0);
	ret = mc16_zl_fault(f, orig, 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret, 0U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_zl_read(f), changed));
	mc16_zl_counts(f, 1, 0);
}

static void memcomp_test_zero_load_zapped(struct kunit *test)
{
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	pte_t orig = mc16_zl_zero_pte();
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	mc16_zl_zap_zero(f);
	mc16_zl_counts(f, 0, 0);
	ret = mc16_zl_fault(f, orig, 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret, 0U);
	KUNIT_EXPECT_TRUE(test, pte_none(mc16_zl_read(f)));
	mc16_zl_counts(f, 0, 0);
}

static void mc16_zl_oom(struct kunit *test, bool charge)
{
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	pte_t orig = mc16_zl_zero_pte();
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	mc16_zero_fault_probe.fail_alloc = !charge;
	mc16_zero_fault_probe.fail_charge = charge;
	WRITE_ONCE(mc16_zero_fault_probe.task, current);
	ret = mc16_zl_fault(f, orig, 0);
	WRITE_ONCE(mc16_zero_fault_probe.task, NULL);
	mc16_zero_fault_probe.fail_alloc = false;
	mc16_zero_fault_probe.fail_charge = false;
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_OOM);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_zl_read(f), orig));
	mc16_zl_counts(f, 1, 0);
	/* Normal return deliberately leaves this owner to registered cleanup. */
}

static void memcomp_test_zero_load_alloc_oom(struct kunit *test)
{
	mc16_zl_oom(test, false);
}

static void memcomp_test_zero_load_charge_oom(struct kunit *test)
{
	/* Injection is after real allocation and before the real charge call. */
	mc16_zl_oom(test, true);
}

#ifdef CONFIG_PER_VMA_LOCK
static int mc16_zl_mmap_writer(void *data)
{
	struct mc16_zl_mm *f = data;

	/* Deliberately no VMA write lock: it is held for reading by the actor. */
	mmap_write_lock(f->mm);
	complete(&f->writer_locked);
	wait_for_completion(&f->writer_release);
	mmap_write_unlock(f->mm);
	return 0;
}
#endif

static void memcomp_test_zero_load_retry(struct kunit *test)
{
#ifdef CONFIG_PER_VMA_LOCK
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	struct vm_area_struct *vma;
	pmd_t *pmd = NULL;
	pte_t *pte_after;
	pte_t orig = mc16_zl_zero_pte();
	unsigned long ready;
	vm_fault_t ret;
	bool locked = true, held = false;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma && !vma->anon_vma)
		held = vma_start_read(vma);
	if (held)
		pmd = mm_find_pmd(f->mm, f->addr);
	mmap_read_unlock(f->mm);
	if (!held) {
		kunit_skip(test, "fresh anon VMA read lock unavailable");
		return;
	}
	/* No fatal assertion or KUnit resource allocation while VMA is locked. */
	f->writer = kthread_run(mc16_zl_mmap_writer, f, "mc16-zero-retry");
	if (IS_ERR(f->writer)) {
		f->writer = NULL;
		vma_end_read(vma);
		KUNIT_FAIL(test, "could not start mmap writer");
		return;
	}
	/* The worker may exit as soon as writer_release is completed. */
	get_task_struct(f->writer);
	ready = wait_for_completion_timeout(&f->writer_locked,
					    msecs_to_jiffies(5000));
	if (!ready) {
		vma_end_read(vma);
		complete_all(&f->writer_release);
		kthread_stop_put(f->writer);
		f->writer = NULL;
		KUNIT_FAIL(test, "mmap writer did not acquire its lock");
		return;
	}
	{
		struct vm_fault vmf = {
			.vma = vma,
			.address = f->addr,
			.real_address = f->addr,
			.flags = FAULT_FLAG_VMA_LOCK | FAULT_FLAG_ORIG_PTE_VALID,
			.pmd = pmd,
			.orig_pte = orig,
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};

		ret = mc16_load_zero(&vmf, &locked);
		pte_after = vmf.pte;
	}
	/* vmf_anon_prepare() drops the VMA read lock only on RETRY. */
	if (!(ret & VM_FAULT_RETRY))
		vma_end_read(vma);
	complete_all(&f->writer_release);
	kthread_stop_put(f->writer);
	f->writer = NULL;
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_RETRY);
	KUNIT_EXPECT_FALSE(test, locked);
	KUNIT_EXPECT_PTR_EQ(test, pte_after, NULL);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_zl_read(f), orig));
	mc16_zl_counts(f, 1, 0);
#else
	kunit_skip(test, "per-VMA locking is disabled");
#endif
}
#endif /* MC16_ZERO_LOAD_MM_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_ZERO_LOAD_TEST_H */
