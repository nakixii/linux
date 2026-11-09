/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_FAULT_TEST_H
#define _MM_MEMCOMP_TESTS_FAULT_TEST_H

/*
 * Include after zero_load_test.h and refs/payload/owner test fixtures.
 * Real mmap, PTL, folio allocation/charge, codec/zs and anon rmap. Only the
 * existing source objcg is modeled. No swap device or public dispatch is used.
 */
#if defined(MC16_ZERO_LOAD_MM_TEST_ENABLED) && defined(CONFIG_MEMCG)
#define MC16_FAULT_MM_TEST_ENABLED
#define MC16_FT_PAGES 4

struct mc16_ft {
	struct kunit *test;
	struct mc16_test_refs *refs;
	struct mm_struct *mm;
	unsigned long mapping;
	unsigned long addr;
	vm_flags_t flags;
	pgprot_t prot;
	bool saved_vma;
	struct mc16_zero_owners zero;
	s64 swapents;
	s64 anonpages;
	u32 tokens[MC16_FT_PAGES];
	unsigned int nr;
	struct folio *pins[MC16_FT_PAGES];
	unsigned int nr_pins;
	struct mc16_payload prepared[MC16_FT_PAGES];
	struct mc16_payload_tail tail[MC16_FT_PAGES];
	u8 *src;
	int actor_error;
	bool actor_ran;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	bool saved_orders;
	unsigned long orders;
#endif
};

/* The inherited source cleanup expects a locked folio; lock in this task. */
static void mc16_ft_objcg_cleanup(void *data)
{
	struct memcomp_test_objcg_fixture *owner = data;

	if (owner->folio)
		folio_lock(owner->folio);
	memcomp_test_objcg_cleanup(owner);
}

static int mc16_ft_storage(struct mc16_ft *f)
{
	struct kunit *test = f->test;
	struct mc16_test_storage *s;
	struct memcomp_test_objcg_fixture *owner;
	struct folio *source;
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
	ret = kunit_add_action_or_reset(test, mc16_ft_objcg_cleanup, owner);
	if (ret)
		return ret;
	/* A genuine four-page source for the immutable order-2 span fixture. */
	source = folio_alloc(GFP_KERNEL, 2);
	if (!source)
		return -ENOMEM;
	folio_put(owner->folio);
	owner->folio = source;
	owner->model.folio = source;
	owner->nr_pages = MC16_FT_PAGES;
	s->owner = owner;
	s->test = test;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	mc16_test_metadata_snapshot(&s->baseline);
#endif
	s->probe.task = current;
	ret = kunit_add_action_or_reset(test, mc16_test_storage_cleanup, s);
	if (ret)
		return ret;
	rcu_assign_pointer(mc16_storage_fixture, &s->probe);
	f->refs->stored_before = atomic64_read(&memcomp_stored_bytes);
	f->refs->compressed_before = atomic64_read(&memcomp_compressed_bytes);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	f->refs->pending_before = atomic64_read(&memcomp_pending_pages);
#endif
	return kunit_add_action_or_reset(test, mc16_test_refs_cleanup, f->refs);
}

static void mc16_ft_zero_unchanged(struct mc16_ft *f)
{
	struct mc16_zero_owners after;

	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(f->test, after.pte_pages, f->zero.pte_pages);
	KUNIT_EXPECT_EQ(f->test, after.cache_pages, f->zero.cache_pages);
}

/* Caller holds mmap_read_lock; a removed positive PTE owns exactly one ref. */
static int mc16_ft_zap(struct mc16_ft *f, unsigned int index)
{
	unsigned long addr = f->addr + index * PAGE_SIZE;
	spinlock_t *ptl; /* Actual PTE removal and token ownership transaction. */
	struct memcomp_ref ref;
	pte_t *ptep, pte;
	pmd_t *pmd;

	pmd = mm_find_pmd(f->mm, addr);
	if (!pmd)
		return -ENOENT;
	ptep = pte_offset_map_lock(f->mm, pmd, addr, &ptl);
	if (!ptep)
		return -ENOENT;
	pte = ptep_get(ptep);
	if (is_swap_pte(pte) && is_memcomp_entry(pte_to_swp_entry(pte))) {
		unsigned long token = swp_offset(pte_to_swp_entry(pte));

		ptep_get_and_clear(f->mm, addr, ptep);
		add_mm_counter(f->mm, MM_SWAPENTS, -1);
		if (!token)
			mc16_zero_pte_put(1);
		else if (mc16_get(token, &ref))
			mc16_ref_put_count(&ref, 2);
	}
	pte_unmap_unlock(ptep, ptl);
	return 0;
}

static void mc16_ft_cleanup(void *data)
{
	struct mc16_ft *f = data;
	struct mc16_test_storage *s = f->refs->storage;
	struct vm_area_struct *vma;
	unsigned int i;
	int ret;

	mc16_fault_probe = (typeof(mc16_fault_probe)) { };
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (f->saved_orders)
		WRITE_ONCE(huge_anon_orders_always, f->orders);
#endif
	mmap_read_lock(f->mm);
	for (i = 0; i < MC16_FT_PAGES; i++)
		mc16_ft_zap(f, i);
	mmap_read_unlock(f->mm);
	for (i = 0; i < s->nr; i++) {
		if (!s->refs[i].slot)
			continue;
		mc16_payload_finish(&s->refs[i], &f->tail[i]);
		mc16_payload_abort(&s->refs[i], &f->prepared[i]);
	}
	mmap_write_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma && f->saved_vma) {
		vma_start_write(vma);
		vm_flags_reset(vma, f->flags);
		vma->vm_page_prot = f->prot;
	}
	mmap_write_unlock(f->mm);
	/* Present pages use normal MM rmap/LRU cleanup, after token PTE removal. */
	ret = mc16_test_dontneed(f->mm, f->mapping, 2 * MC16_FT_PAGES * PAGE_SIZE);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	for (i = 0; i < f->nr_pins; i++)
		folio_put(f->pins[i]);
	KUNIT_EXPECT_EQ(f->test, mc16_zl_rss(f->mm, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(f->test, mc16_zl_rss(f->mm, MM_ANONPAGES), f->anonpages);
	mc16_ft_zero_unchanged(f);
	mmput(f->mm);
}

static int mc16_ft_prepare(struct mc16_ft *f, unsigned int index, bool raw)
{
	struct mc16_test_storage *s = f->refs->storage;
	struct memcomp_ref *ref = &s->refs[index];
	struct mc16_payload *p = &f->prepared[index];
	unsigned long handle;
	unsigned int bytes;
	u8 pool = 0;
	int ret;

	if (raw)
		ret = memcomp_alloc_raw_payload(0, f->src + index * PAGE_SIZE,
						PAGE_SIZE, &bytes, &handle, false, NULL);
	else
		ret = memcomp_prepare_payload(f->src + index * PAGE_SIZE, PAGE_SIZE,
					      &bytes, &handle, &pool,
					      MEMCOMP_PAYLOAD_MUST_SUCCEED, false);
	if (ret)
		return ret;
	if (pool) {
		memcomp_zs_free(pool, handle);
		return -EINVAL;
	}
	*p = (struct mc16_payload) { .word = handle, .bytes = bytes };
	if (!bytes || (!raw && bytes >= PAGE_SIZE))
		return -E2BIG;
	ret = mc16_payload_charge(ref, p);
	if (ret)
		return ret;
	folio_lock(s->owner->folio);
	mutex_lock(memcomp_lock_of(ref->token));
	ret = mc16_payload_commit_locked(ref, s->owner->folio, f->nr, index,
					 p, &f->tail[index]);
	mutex_unlock(memcomp_lock_of(ref->token));
	mc16_payload_finish(ref, &f->tail[index]);
	folio_unlock(s->owner->folio);
	return ret;
}

static pte_t mc16_ft_orig(struct mc16_ft *f, unsigned int index)
{
	return pte_swp_mksoft_dirty(swp_entry_to_pte(make_memcomp_entry(f->tokens[index])));
}

static int mc16_ft_install(struct mc16_ft *f, unsigned int index, pte_t pte)
{
	unsigned long addr = f->addr + index * PAGE_SIZE;
	spinlock_t *ptl; /* Actual PTE owns a transferred mc16_get reference. */
	struct memcomp_ref ref;
	pte_t *ptep;
	int ret = 0;

	mmap_read_lock(f->mm);
	ptep = get_locked_pte(f->mm, addr, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out;
	}
	__acquire(RCU);
	if (!pte_none(ptep_get(ptep))) {
		ret = -EEXIST;
	} else if (!mc16_get(swp_offset(pte_to_swp_entry(pte)), &ref)) {
		ret = -ENOENT;
	} else {
		set_pte_at(f->mm, addr, ptep, pte);
		add_mm_counter(f->mm, MM_SWAPENTS, 1);
		/* ref ownership is now the installed PTE's. */
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	return ret;
}

static struct mc16_ft *mc16_ft_begin(struct kunit *test, unsigned int nr,
				     bool raw, bool normalize)
{
	struct mc16_ft *f;
	struct mc16_test_storage *s;
	struct vm_area_struct *vma;
	unsigned int i;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->nr = nr;
	if (mc16_ft_storage(f))
		return NULL;
	s = f->refs->storage;
	f->mapping = kunit_vm_mmap(test, NULL, 0, 2 * MC16_FT_PAGES * PAGE_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (!f->mapping || IS_ERR_VALUE(f->mapping))
		return NULL;
	f->addr = ALIGN(f->mapping, MC16_FT_PAGES * PAGE_SIZE);
	f->mm = current->mm;
	mmget(f->mm);
	f->swapents = mc16_zl_rss(f->mm, MM_SWAPENTS);
	f->anonpages = mc16_zl_rss(f->mm, MM_ANONPAGES);
	mc16_zero_owners_read(&f->zero);
	if (kunit_add_action_or_reset(test, mc16_ft_cleanup, f))
		return NULL;
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma) {
		f->flags = vma->vm_flags;
		f->prot = vma->vm_page_prot;
		f->saved_vma = true;
	}
	ret = vma ? anon_vma_prepare(vma) : -ENOENT;
	mmap_read_unlock(f->mm);
	if (ret)
		return NULL;
	f->src = kunit_kmalloc(test, nr * PAGE_SIZE, GFP_KERNEL);
	if (!f->src)
		return NULL;
	for (i = 0; i < nr * PAGE_SIZE; i++)
		f->src[i] = raw ? i * 37 + (i >> 5) : (i + i / PAGE_SIZE) % 53;
	folio_lock(s->owner->folio);
	ret = mc16_test_refs_reserve(f->refs, nr);
	folio_unlock(s->owner->folio);
	if (ret)
		return NULL;
	for (i = 0; i < nr; i++) {
		f->tokens[i] = s->refs[i].token;
		if (mc16_ft_prepare(f, i, raw) || mc16_ft_install(f, i, mc16_ft_orig(f, i)))
			return NULL;
	}
	if (normalize) {
		folio_lock(s->owner->folio);
		for (i = nr; i; ) {
			struct memcomp_ref ref;

			i--;
			if (!mc16_get(f->tokens[i], &ref)) {
				ret = -ENOENT;
				break;
			}
			if (!mc16_owner_drop_folio_held(&ref, s->owner->folio, nr, i)) {
				ret = -ESTALE;
				break;
			}
			s->refs[i] = (struct memcomp_ref) { };
		}
		folio_unlock(s->owner->folio);
		if (ret)
			return NULL;
	}
	return f;
}

/* All vm_fault const fields are initialized in the aggregate, as 6.12 needs. */
static vm_fault_t mc16_ft_fault(struct kunit *test, struct mm_struct *mm,
				unsigned long addr, pte_t orig, unsigned int flags)
{
	struct vm_area_struct *vma;
	vm_fault_t ret = VM_FAULT_SIGBUS;
	bool locked = true;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, addr);
	if (vma) {
		struct vm_fault vmf = {
			.vma = vma,
			.address = addr,
			.real_address = addr,
			.flags = flags | FAULT_FLAG_ORIG_PTE_VALID,
			.pmd = mm_find_pmd(mm, addr),
			.orig_pte = orig,
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};

		ret = mc16_load(&vmf, &locked);
		KUNIT_EXPECT_PTR_EQ(test, vmf.pte, NULL);
	}
	mmap_read_unlock(mm);
	KUNIT_EXPECT_NOT_NULL(test, vma);
	KUNIT_EXPECT_FALSE(test, locked);
	return ret;
}

static pte_t mc16_ft_read(struct mc16_ft *f, unsigned int index)
{
	unsigned long addr = f->addr + index * PAGE_SIZE;
	spinlock_t *ptl; /* Pin present folios under PTL, including ASSERT exits. */
	pte_t *ptep, pte = __pte(0);
	pmd_t *pmd;
	unsigned int i;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, addr);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(f->mm, pmd, addr, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if (pte_present(pte) && !pte_special(pte)) {
		struct folio *folio = page_folio(pte_page(pte));

		for (i = 0; i < f->nr_pins; i++) {
			if (f->pins[i] == folio)
				break;
		}
		if (i == f->nr_pins && i < ARRAY_SIZE(f->pins)) {
			folio_get(folio);
			f->pins[f->nr_pins++] = folio;
		}
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	return pte;
}

static void mc16_ft_counts(struct mc16_ft *f, unsigned int swapped,
			   unsigned int present)
{
	KUNIT_EXPECT_EQ(f->test, mc16_zl_rss(f->mm, MM_SWAPENTS), f->swapents + swapped);
	KUNIT_EXPECT_EQ(f->test, mc16_zl_rss(f->mm, MM_ANONPAGES), f->anonpages + present);
	mc16_ft_zero_unchanged(f);
}

static void mc16_ft_present(struct mc16_ft *f, unsigned int index,
			    unsigned int order, unsigned int mapped)
{
	pte_t pte = mc16_ft_read(f, index);
	struct page *page;
	struct folio *folio;
	unsigned int i;
	void *data;
	bool pinned = false;

	KUNIT_ASSERT_TRUE(f->test, pte_present(pte));
	KUNIT_ASSERT_FALSE(f->test, pte_special(pte));
	page = pte_page(pte);
	folio = page_folio(page);
	for (i = 0; i < f->nr_pins; i++)
		pinned |= f->pins[i] == folio;
	KUNIT_ASSERT_TRUE(f->test, pinned);
	KUNIT_EXPECT_EQ(f->test, folio_order(folio), order);
	KUNIT_EXPECT_EQ(f->test, folio_mapcount(folio), mapped);
	KUNIT_EXPECT_TRUE(f->test, folio_test_anon(folio));
	KUNIT_EXPECT_TRUE(f->test, folio_test_swapbacked(folio));
	KUNIT_EXPECT_TRUE(f->test, folio_test_uptodate(folio));
	KUNIT_EXPECT_FALSE(f->test, folio_test_locked(folio));
	KUNIT_EXPECT_TRUE(f->test, PageAnonExclusive(page));
	data = kmap_local_page(page);
	KUNIT_EXPECT_EQ(f->test, memcmp(data, f->src + index * PAGE_SIZE, PAGE_SIZE), 0);
	kunmap_local(data);
}

static void mc16_ft_token_refs(struct mc16_ft *f, unsigned int index,
			       unsigned int owners)
{
	struct memcomp_ref ref;
	bool found = mc16_get(f->tokens[index], &ref);

	KUNIT_EXPECT_EQ(f->test, found, owners != 0);
	if (found) {
		KUNIT_EXPECT_EQ(f->test, refcount_read(&ref.slot->refs), owners + 1);
		mc16_ref_put(&ref);
	}
}

static void mc16_ft_probe_start(struct mc16_ft *f)
{
	mc16_fault_probe = (typeof(mc16_fault_probe)) { .task = current, .data = f };
}

static void mc16_ft_policy(struct mc16_ft *f, vm_flags_t set, vm_flags_t clear)
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

static void mc16_ft_single(struct kunit *test, bool raw, bool attached)
{
	struct mc16_ft *f = mc16_ft_begin(test, 1, raw, !attached);
	pte_t orig, pte;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	orig = mc16_ft_orig(f, 0);
	mc16_ft_counts(f, 1, 0);
	ret = mc16_ft_fault(test, f->mm, f->addr, orig, FAULT_FLAG_WRITE);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	mc16_ft_present(f, 0, 0, 1);
	pte = mc16_ft_read(f, 0);
	KUNIT_EXPECT_TRUE(test, pte_write(pte));
	KUNIT_EXPECT_TRUE(test, pte_dirty(pte));
	if (pte_swp_soft_dirty(orig))
		KUNIT_EXPECT_TRUE(test, pte_soft_dirty(pte));
	mc16_ft_counts(f, 0, 1);
	mc16_ft_token_refs(f, 0, attached ? 1 : 0);
}

static void memcomp_test_fault_single(struct kunit *test)
{
	mc16_ft_single(test, false, false);
}

static void memcomp_test_fault_raw(struct kunit *test)
{
	mc16_ft_single(test, true, false);
}

static void memcomp_test_fault_attached(struct kunit *test)
{
	mc16_ft_single(test, false, true);
}

static void memcomp_test_fault_shared(struct kunit *test)
{
	struct mc16_ft *f = mc16_ft_begin(test, 1, false, true);
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ft_install(f, 1, mc16_ft_orig(f, 0)), 0);
	ret = mc16_ft_fault(test, f->mm, f->addr, mc16_ft_orig(f, 0), FAULT_FLAG_WRITE);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	mc16_ft_present(f, 0, 0, 1);
	KUNIT_EXPECT_TRUE(test, is_swap_pte(mc16_ft_read(f, 1)));
	mc16_ft_token_refs(f, 0, 1);
	mc16_ft_counts(f, 1, 1);
}

/* Caller owns mmap_read_lock, and updates flags of the same nonpresent owner. */
static int mc16_ft_change(struct mc16_ft *f, unsigned int index, pte_t pte)
{
	unsigned long addr = f->addr + index * PAGE_SIZE;
	spinlock_t *ptl; /* Only flags change; ownership and swap RSS are neutral. */
	pte_t *ptep, old;
	pmd_t *pmd = mm_find_pmd(f->mm, addr);
	int ret = -ENOENT;

	if (!pmd)
		return ret;
	ptep = pte_offset_map_lock(f->mm, pmd, addr, &ptl);
	if (!ptep)
		return ret;
	old = ptep_get(ptep);
	if (is_swap_pte(old) && is_memcomp_entry(pte_to_swp_entry(old)) &&
	    swp_offset(pte_to_swp_entry(old)) == swp_offset(pte_to_swp_entry(pte))) {
		set_pte_at(f->mm, addr, ptep, pte);
		ret = 0;
	}
	pte_unmap_unlock(ptep, ptl);
	return ret;
}

static void mc16_ft_protection(struct kunit *test, unsigned int policy)
{
	struct mc16_ft *f;
	pte_t orig, pte;
	vm_fault_t ret;
	int err;

	if (policy == 1 && !IS_ENABLED(CONFIG_MEM_SOFT_DIRTY)) {
		kunit_skip(test, "soft-dirty tracking disabled");
		return;
	}
	orig = pte_swp_mkuffd_wp(mc16_zl_zero_pte());
	if (policy == 2 && (!IS_ENABLED(CONFIG_USERFAULTFD) || !pte_swp_uffd_wp(orig))) {
		kunit_skip(test, "swap UFFD-WP unavailable");
		return;
	}
	f = mc16_ft_begin(test, 1, false, true);
	KUNIT_ASSERT_NOT_NULL(test, f);
	orig = mc16_ft_orig(f, 0);
	if (policy == 1) {
		orig = pte_swp_clear_soft_dirty(orig);
		mc16_ft_policy(f, 0, VM_SOFTDIRTY);
	} else if (policy == 2) {
		orig = pte_swp_mkuffd_wp(orig);
		mc16_ft_policy(f, VM_UFFD_WP, 0);
	} else if (policy == 3) {
		mc16_ft_policy(f, 0, VM_WRITE);
	}
	mmap_read_lock(f->mm);
	err = mc16_ft_change(f, 0, orig);
	mmap_read_unlock(f->mm);
	KUNIT_ASSERT_EQ(test, err, 0);
	ret = mc16_ft_fault(test, f->mm, f->addr, orig, 0);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	mc16_ft_present(f, 0, 0, 1);
	pte = mc16_ft_read(f, 0);
	if (policy) {
		KUNIT_EXPECT_FALSE(test, pte_write(pte));
		KUNIT_EXPECT_FALSE(test, pte_dirty(pte));
	}
	if (policy == 1)
		KUNIT_EXPECT_FALSE(test, pte_soft_dirty(pte));
	else if (pte_swp_soft_dirty(orig))
		KUNIT_EXPECT_TRUE(test, pte_soft_dirty(pte));
	if (policy == 2)
		KUNIT_EXPECT_TRUE(test, pte_uffd_wp(pte));
	mc16_ft_counts(f, 0, 1);
	mc16_ft_token_refs(f, 0, 0);
}

static void memcomp_test_fault_softdirty(struct kunit *test)
{
	if (!IS_ENABLED(CONFIG_MEM_SOFT_DIRTY)) {
		kunit_skip(test, "soft-dirty tracking disabled");
		return;
	}
	mc16_ft_protection(test, 0);
}

static void memcomp_test_fault_softdirty_wp(struct kunit *test)
{
	mc16_ft_protection(test, 1);
}

static void memcomp_test_fault_uffd_wp(struct kunit *test)
{
	mc16_ft_protection(test, 2);
}

static void memcomp_test_fault_readonly(struct kunit *test)
{
	mc16_ft_protection(test, 3);
}

static void mc16_ft_race(struct vm_fault *vmf, void *data)
{
	struct mc16_ft *f = data;

	mc16_fault_probe.before_install = NULL;
	f->actor_ran = true;
	f->actor_error = mc16_ft_change(f, 0, pte_swp_mkexclusive(vmf->orig_pte));
}

static void memcomp_test_fault_changed(struct kunit *test)
{
	struct mc16_ft *f = mc16_ft_begin(test, 1, false, true);
	pte_t orig;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	orig = mc16_ft_orig(f, 0);
	KUNIT_ASSERT_FALSE(test, pte_same(orig, pte_swp_mkexclusive(orig)));
	mc16_ft_probe_start(f);
	mc16_fault_probe.before_install = mc16_ft_race;
	ret = mc16_ft_fault(test, f->mm, f->addr, orig, 0);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_MAJOR);
	KUNIT_EXPECT_TRUE(test, f->actor_ran);
	KUNIT_EXPECT_EQ(test, f->actor_error, 0);
	KUNIT_EXPECT_TRUE(test,
			  pte_same(mc16_ft_read(f, 0), pte_swp_mkexclusive(orig)));
	mc16_ft_counts(f, 1, 0);
	mc16_ft_token_refs(f, 0, 1);
}

static void mc16_ft_oom(struct kunit *test, bool charge)
{
	struct mc16_ft *f = mc16_ft_begin(test, 1, false, true);
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_ft_probe_start(f);
	mc16_fault_probe.fail_small_alloc = !charge;
	mc16_fault_probe.fail_small_charge = charge;
	ret = mc16_ft_fault(test, f->mm, f->addr, mc16_ft_orig(f, 0), 0);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_OOM);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.small_allocs, 1U);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.small_charges, charge ? 1U : 0U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_ft_read(f, 0), mc16_ft_orig(f, 0)));
	mc16_ft_token_refs(f, 0, 1);
	mc16_ft_counts(f, 1, 0);
}

static void memcomp_test_fault_alloc_oom(struct kunit *test)
{
	mc16_ft_oom(test, false);
}

static void memcomp_test_fault_charge_oom(struct kunit *test)
{
	mc16_ft_oom(test, true);
}

/* Restore intentionally invalid metadata before any assertion or final put. */
static void mc16_ft_load_error(struct kunit *test, bool retry)
{
	struct mc16_ft *f = mc16_ft_begin(test, 1, false, true);
	struct memcomp_ref ref;
	struct mc16_view view;
	vm_fault_t ret;
	u16 saved;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->tokens[0], &ref));
	mutex_lock(memcomp_lock_of(ref.token));
	mc16_view_locked(&ref, &view);
	saved = *view.flags;
	*view.flags |= retry ? MC16_F_REFRESH_PENDING : BIT(15);
	mutex_unlock(memcomp_lock_of(ref.token));
	ret = mc16_ft_fault(test, f->mm, f->addr, mc16_ft_orig(f, 0), 0);
	mutex_lock(memcomp_lock_of(ref.token));
	mc16_view_locked(&ref, &view);
	*view.flags = saved;
	mutex_unlock(memcomp_lock_of(ref.token));
	mc16_ref_put(&ref);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			retry ? 0U : (__force unsigned int)VM_FAULT_SIGBUS);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_ft_read(f, 0), mc16_ft_orig(f, 0)));
	mc16_ft_token_refs(f, 0, 1);
	mc16_ft_counts(f, 1, 0);
}

static void memcomp_test_fault_payload_retry(struct kunit *test)
{
	mc16_ft_load_error(test, true);
}

static void memcomp_test_fault_payload_invalid(struct kunit *test)
{
	mc16_ft_load_error(test, false);
}

static bool mc16_ft_large_enable(struct mc16_ft *f)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	struct vm_area_struct *vma;
	unsigned long orders = 0;

	if (!(THP_ORDERS_ALL_ANON & BIT(2))) {
		kunit_skip(f->test, "order-2 anonymous THP unsupported");
		return false;
	}
	f->orders = READ_ONCE(huge_anon_orders_always);
	f->saved_orders = true;
	WRITE_ONCE(huge_anon_orders_always, f->orders | BIT(2));
	mc16_ft_policy(f, VM_HUGEPAGE, VM_NOHUGEPAGE);
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma) {
		orders = thp_vma_allowable_orders(vma, vma->vm_flags,
						  TVA_IN_PF | TVA_ENFORCE_SYSFS,
						  BIT(2));
		orders = thp_vma_suitable_orders(vma, f->addr + 2 * PAGE_SIZE, orders);
	}
	mmap_read_unlock(f->mm);
	if (!(orders & BIT(2))) {
		kunit_skip(f->test, "real VMA policy disallows order-2 THP");
		return false;
	}
	return true;
#else
	kunit_skip(f->test, "THP disabled");
	return false;
#endif
}

static void mc16_ft_sibling_gone(struct vm_fault *vmf, void *data)
{
	struct mc16_ft *f = data;

	mc16_fault_probe.after_alloc = NULL;
	f->actor_ran = true;
	f->actor_error = mc16_ft_zap(f, 3);
}

static void mc16_ft_large(struct kunit *test, unsigned int fallback)
{
	struct mc16_ft *f = mc16_ft_begin(test, MC16_FT_PAGES, false, true);
	unsigned int i, present = fallback ? 1 : MC16_FT_PAGES;
	vm_fault_t ret;
	int err = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	if (!mc16_ft_large_enable(f))
		return;
	mc16_ft_probe_start(f);
	mc16_fault_probe.fail_large_alloc = fallback == 1;
	mc16_fault_probe.fail_large_charge = fallback == 2;
	if (fallback == 3) {
		mmap_read_lock(f->mm);
		err = mc16_ft_change(f, 1, pte_swp_mkexclusive(mc16_ft_orig(f, 1)));
		mmap_read_unlock(f->mm);
	}
	if (fallback == 4)
		mc16_fault_probe.after_alloc = mc16_ft_sibling_gone;
	KUNIT_ASSERT_EQ(test, err, 0);
	/* Fault in the middle: wrong fault_index movement must fail this test. */
	ret = mc16_ft_fault(test, f->mm, f->addr + 2 * PAGE_SIZE,
			    mc16_ft_orig(f, 2), FAULT_FLAG_WRITE);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.large_allocs, fallback == 3 ? 0U : 1U);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.small_allocs, fallback ? 1U : 0U);
	if (fallback == 2)
		KUNIT_EXPECT_EQ(test, mc16_fault_probe.large_charges, 1U);
	if (fallback == 4) {
		KUNIT_EXPECT_TRUE(test, f->actor_ran);
		KUNIT_EXPECT_EQ(test, f->actor_error, 0);
		KUNIT_EXPECT_TRUE(test, pte_none(mc16_ft_read(f, 3)));
	}
	for (i = 0; i < MC16_FT_PAGES; i++) {
		if (!fallback || i == 2) {
			mc16_ft_present(f, i, fallback ? 0 : 2, present);
			mc16_ft_token_refs(f, i, 0);
		} else if (fallback == 4 && i == 3) {
			mc16_ft_token_refs(f, i, 0);
		} else {
			KUNIT_EXPECT_TRUE(test, is_swap_pte(mc16_ft_read(f, i)));
			mc16_ft_token_refs(f, i, 1);
		}
	}
	mc16_ft_counts(f, MC16_FT_PAGES - present - (fallback == 4), present);
}

static void memcomp_test_fault_thp(struct kunit *test)
{
	mc16_ft_large(test, 0);
}

static void memcomp_test_fault_thp_alloc_fallback(struct kunit *test)
{
	mc16_ft_large(test, 1);
}

static void memcomp_test_fault_thp_charge_fallback(struct kunit *test)
{
	mc16_ft_large(test, 2);
}

static void memcomp_test_fault_thp_pte_fallback(struct kunit *test)
{
	mc16_ft_large(test, 3);
}

static void memcomp_test_fault_thp_sibling_gone(struct kunit *test)
{
	mc16_ft_large(test, 4);
}

static void memcomp_test_fault_zero(struct kunit *test)
{
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	pte_t orig = pte_swp_mksoft_dirty(mc16_zl_zero_pte());
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	ret = mc16_ft_fault(test, f->mm, f->addr, orig, FAULT_FLAG_WRITE);
	KUNIT_ASSERT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	mc16_zl_present(f, mc16_zl_read(f));
}
#endif /* MC16_FAULT_MM_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_FAULT_TEST_H */
