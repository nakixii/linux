/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_MINCORE_TEST_H
#define _MM_MEMCOMP_TESTS_MINCORE_TEST_H

#include <linux/types.h>

struct address_space;
struct mm_struct;

unsigned char memcomp_test_mincore_page(struct address_space *mapping,
					pgoff_t index);
int memcomp_test_mincore_walk(struct mm_struct *mm, unsigned long start,
			      unsigned long end, unsigned char *vec);

#ifndef MC16_MINCORE_DECLARE_TEST_API
#include <linux/file.h>
#include <linux/shmem_fs.h>
#include "../../swap.h"

/* Include after query_test.h, writeback_test.h and fault_test.h in suite.h. */

static void memcomp_test_mincore_zero(struct kunit *test)
{
	struct mc16_zero_owners before, after;

	mc16_zero_owners_read(&before);
	KUNIT_EXPECT_TRUE(test, memcomp_mincore(0));
	KUNIT_EXPECT_FALSE(test, memcomp_mincore(ULONG_MAX));
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, before.pte_pages);
	KUNIT_EXPECT_EQ(test, after.cache_pages, before.cache_pages);
}

#ifdef CONFIG_MEMCG
static void memcomp_test_mincore_pending(struct kunit *test)
{
	struct mc16_test_refs *f = mc16_test_refs_begin(test);
	struct mc16_test_query_observation before;
	struct memcomp_ref *ref;
	unsigned int refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 1), 0);
	ref = &f->storage->refs[0];
	refs = refcount_read(&ref->slot->refs);
	mc16_test_query_observe(f->storage, &before);
	KUNIT_EXPECT_TRUE(test, memcomp_mincore(ref->token));
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);
	mc16_test_query_unchanged(test, f->storage, &before);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 0, MC16_SAMEFILL), 0);
	mc16_test_query_observe(f->storage, &before);
	KUNIT_EXPECT_TRUE(test, memcomp_mincore(ref->token));
	mc16_test_query_unchanged(test, f->storage, &before);
}

#define MC16_MINCORE_PENDING_TEST_ENABLED
#endif

#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)
/* Only device/cache availability is modeled; guards, T and folio flags are real. */
struct mc16_mincore_test {
	struct kunit *test;
	struct folio *cache;
	struct file *file;
	unsigned long token;
	int dups;
	unsigned int lookups;
	bool cached;
	bool under_t;
	bool atomic;
	bool missing_pin;
	bool change_xa;
	bool changed;
	int change_error;
#ifdef MC16_FAULT_MM_TEST_ENABLED
	struct mc16_ft *fault;
#endif
};

static struct mc16_mincore_test *mc16_mincore_test_current;

static void mc16_mincore_test_cleanup(void *data)
{
	struct mc16_mincore_test *f = data;
	unsigned int i;

	if (mc16_mincore_test_current == f)
		mc16_mincore_test_current = NULL;
	/* The synthetic XA values borrow live fixture owners; erase before fput. */
	if (f->file) {
		for (i = 0; i < MC16_MAX_PAGES; i++)
			xa_erase(&f->file->f_mapping->i_pages, i);
		fput(f->file);
	}
	folio_put(f->cache);
}

#ifdef MC16_FAULT_MM_TEST_ENABLED
static int mc16_mincore_test_replace_pte(struct mc16_ft *f)
{
	struct memcomp_ref ref = { };
	spinlock_t *ptl; /* A real PTE replacement while the query has dropped PTL. */
	pte_t *ptep;
	int ret = -ENOENT;

	ptep = get_locked_pte(f->mm, f->addr, &ptl);
	if (!ptep)
		return -ENOMEM;
	__acquire(RCU);
	if (pte_same(ptep_get(ptep), mc16_ft_orig(f, 0)) &&
	    mc16_get(f->tokens[0], &ref)) {
		mc16_zero_pte_init(1);
		set_pte_at(f->mm, f->addr, ptep,
			   swp_entry_to_pte(make_memcomp_entry(0)));
		ret = 0;
	}
	pte_unmap_unlock(ptep, ptl);
	if (ref.slot)
		mc16_ref_put_count(&ref, 2); /* PTE owner and this lookup. */
	return ret;
}
#endif

static struct folio *mc16_mincore_test_swap_folio(swp_entry_t swap)
{
	struct mc16_mincore_test *f = mc16_mincore_test_current;
	struct mutex *lock = memcomp_lock_of(f->token);

	f->lookups++;
	f->atomic |= irqs_disabled();
	if (mutex_trylock(lock))
		mutex_unlock(lock);
	else
		f->under_t = true;
	f->missing_pin |= atomic_read(&memcomp_kunit_slot_dups) <= f->dups;
	if (f->change_xa && !f->changed) {
		void *old = swp_to_radix_entry(make_memcomp_entry(f->token));
		void *zero = swp_to_radix_entry(make_memcomp_entry(0));

		f->changed = true;
		old = xa_cmpxchg(&f->file->f_mapping->i_pages, 0, old, zero,
				 GFP_KERNEL);
		if (xa_is_err(old))
			f->change_error = xa_err(old);
		else if (old != swp_to_radix_entry(make_memcomp_entry(f->token)))
			f->change_error = -ESTALE;
	}
#ifdef MC16_FAULT_MM_TEST_ENABLED
	if (f->fault && !f->changed) {
		f->changed = true;
		f->change_error = mc16_mincore_test_replace_pte(f->fault);
	}
#endif
	if (!swap.val || !f->cached)
		return ERR_PTR(-ENOENT);
	folio_get(f->cache);
	return f->cache;
}

static struct mc16_mincore_test *
mc16_mincore_test_begin(struct kunit *test, unsigned long token)
{
	struct mc16_mincore_test *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->cache = folio_alloc(GFP_KERNEL, 0);
	if (!f->cache)
		return NULL;
	f->test = test;
	f->token = token;
	f->dups = atomic_read(&memcomp_kunit_slot_dups);
	if (kunit_add_action_or_reset(test, mc16_mincore_test_cleanup, f))
		return NULL;
	mc16_mincore_test_current = f;
	kunit_activate_static_stub(test, mc16_mincore_swap_folio,
				   mc16_mincore_test_swap_folio);
	return f;
}

static void mc16_mincore_test_expect_unlocked(struct kunit *test,
					      struct mc16_mincore_test *f)
{
	KUNIT_EXPECT_FALSE(test, f->under_t);
	KUNIT_EXPECT_FALSE(test, f->atomic);
	KUNIT_EXPECT_FALSE(test, f->missing_pin);
	KUNIT_EXPECT_EQ(test, f->change_error, 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->cache), 1);
}

static int mc16_mincore_test_writeback(struct mc16_test_writeback *w, bool packed)
{
	struct mc16_swap_fields swap;
	int ret;

	if (!mc16_test_writeback_claim(w))
		return -EBUSY;
	swap = mc16_test_writeback_backing(w, packed);
	ret = mc16_test_writeback_settle(w, &swap);
	if (ret)
		mc16_test_writeback_revert(w);
	else
		mc16_writeback_finish(&w->writer, &w->tail);
	return ret;
}

static void memcomp_test_mincore_pool(struct kunit *test)
{
	struct mc16_test_writeback *w = mc16_test_writeback_begin(test, false);
	struct mc16_test_query_observation before;
	struct mc16_mincore_test *f;
	unsigned int refs;

	KUNIT_ASSERT_NOT_NULL(test, w);
	f = mc16_mincore_test_begin(test, w->writer.token);
	KUNIT_ASSERT_NOT_NULL(test, f);
	refs = refcount_read(&w->writer.slot->refs);
	mc16_test_query_observe(w->refs->storage, &before);
	KUNIT_EXPECT_TRUE(test, memcomp_mincore(w->writer.token));
	KUNIT_ASSERT_TRUE(test, mc16_test_writeback_claim(w));
	KUNIT_EXPECT_TRUE(test, memcomp_mincore(w->writer.token));
	KUNIT_EXPECT_TRUE(test, mc16_test_writeback_revert(w));
	KUNIT_EXPECT_EQ(test, f->lookups, 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&w->writer.slot->refs), refs);
	mc16_test_query_unchanged(test, w->refs->storage, &before);
}

static void mc16_mincore_test_swapped(struct kunit *test, bool cached, bool uptodate)
{
	struct mc16_test_writeback *w = mc16_test_writeback_begin(test, false);
	struct mc16_test_query_observation before;
	struct mc16_mincore_test *f;
	unsigned int refs;
	int dups, puts;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_EQ(test, mc16_mincore_test_writeback(w, true), 0);
	f = mc16_mincore_test_begin(test, w->writer.token);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->cached = cached;
	if (uptodate)
		folio_mark_uptodate(f->cache);
	else
		folio_clear_uptodate(f->cache);
	refs = refcount_read(&w->writer.slot->refs);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	mc16_test_query_observe(w->refs->storage, &before);
	KUNIT_EXPECT_EQ(test, memcomp_mincore(w->writer.token), cached && uptodate);
	KUNIT_EXPECT_EQ(test, f->lookups, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups), dups + 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts + 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&w->writer.slot->refs), refs);
	mc16_test_query_unchanged(test, w->refs->storage, &before);
	mc16_mincore_test_expect_unlocked(test, f);
}

static void memcomp_test_mincore_swapped_absent(struct kunit *test)
{
	mc16_mincore_test_swapped(test, false, false);
}

static void memcomp_test_mincore_swapped_cached(struct kunit *test)
{
	mc16_mincore_test_swapped(test, true, true);
}

static void memcomp_test_mincore_swapped_incomplete(struct kunit *test)
{
	mc16_mincore_test_swapped(test, true, false);
}

#define MC16_MINCORE_SWAP_TEST_ENABLED

#ifdef CONFIG_SHMEM
static void mc16_mincore_test_shmem(struct kunit *test, bool race)
{
	struct mc16_test_writeback *w = mc16_test_writeback_begin(test, false);
	struct mc16_mincore_test *f;
	struct address_space *mapping;
	void *entry;
	struct folio *folio;

	KUNIT_ASSERT_NOT_NULL(test, w);
	f = mc16_mincore_test_begin(test, w->writer.token);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->file = shmem_kernel_file_setup("memcomp-mincore", MC16_MAX_PAGES * PAGE_SIZE,
					  VM_NORESERVE);
	if (IS_ERR(f->file)) {
		int error = PTR_ERR(f->file);

		f->file = NULL;
		KUNIT_FAIL(test, "shmem fixture: %d", error);
		return;
	}
	mapping = f->file->f_mapping;
	entry = swp_to_radix_entry(make_memcomp_entry(w->writer.token));
	KUNIT_ASSERT_FALSE(test, xa_is_err(xa_store(&mapping->i_pages, 0, entry,
						    GFP_KERNEL)));
	folio = filemap_get_incore_folio(mapping, 0);
	if (!IS_ERR(folio))
		folio_put(folio);
	KUNIT_EXPECT_PTR_EQ(test, folio, ERR_PTR(-EEXIST));
	KUNIT_EXPECT_EQ(test, memcomp_test_mincore_page(mapping, 0), (unsigned char)1);
	KUNIT_ASSERT_EQ(test, mc16_mincore_test_writeback(w, false), 0);
	f->dups = atomic_read(&memcomp_kunit_slot_dups);
	f->change_xa = race;
	KUNIT_EXPECT_EQ(test, memcomp_test_mincore_page(mapping, 0),
			(unsigned char)(race ? 1 : 0));
	KUNIT_EXPECT_EQ(test, f->lookups, 1U);
	if (race) {
		KUNIT_EXPECT_TRUE(test, f->changed);
	} else {
		f->cached = true;
		folio_mark_uptodate(f->cache);
		KUNIT_EXPECT_EQ(test, memcomp_test_mincore_page(mapping, 0),
				(unsigned char)1);
		folio_clear_uptodate(f->cache);
		KUNIT_EXPECT_EQ(test, memcomp_test_mincore_page(mapping, 0),
				(unsigned char)0);
	}
	entry = swp_to_radix_entry(make_memcomp_entry(0));
	KUNIT_ASSERT_FALSE(test, xa_is_err(xa_store(&mapping->i_pages, 1, entry,
						    GFP_KERNEL)));
	KUNIT_EXPECT_EQ(test, memcomp_test_mincore_page(mapping, 1), (unsigned char)1);
#ifdef MC16_FAULT_MM_TEST_ENABLED
	{
		unsigned char vec[2] = { };
		unsigned long addr;
		spinlock_t *ptl; /* Allocate empty PTEs, then drop PTL before walking. */
		pte_t *ptep;
		int ret;

		/* Empty but allocated PTEs reach the PTL-dropping shmem branch. */
		addr = kunit_vm_mmap(test, f->file, 0, 2 * PAGE_SIZE,
				     PROT_READ | PROT_WRITE, MAP_SHARED, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR_VALUE(addr));
		KUNIT_ASSERT_NE(test, addr, 0UL);
		mmap_read_lock(current->mm);
		ptep = get_locked_pte(current->mm, addr, &ptl);
		if (ptep) {
			__acquire(RCU);
			pte_unmap_unlock(ptep, ptl);
		}
		f->cached = false;
		ret = ptep ? memcomp_test_mincore_walk(current->mm, addr,
						      addr + 2 * PAGE_SIZE, vec) : -ENOMEM;
		mmap_read_unlock(current->mm);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, vec[0], (unsigned char)(race ? 1 : 0));
		KUNIT_EXPECT_EQ(test, vec[1], (unsigned char)1);
	}
#endif
	mc16_mincore_test_expect_unlocked(test, f);
}

static void memcomp_test_mincore_shmem(struct kunit *test)
{
	mc16_mincore_test_shmem(test, false);
}

static void memcomp_test_mincore_shmem_replaced(struct kunit *test)
{
	mc16_mincore_test_shmem(test, true);
}

#define MC16_MINCORE_SHMEM_TEST_ENABLED
#endif

#ifdef MC16_FAULT_MM_TEST_ENABLED
/* Change a real member to a synthetic device slot using the real WB transaction. */
static int mc16_mincore_test_member_swap(struct mc16_ft *f, unsigned int index)
{
	struct memcomp_ref *ref = &f->refs->storage->refs[index];
	struct mc16_writeback snapshot;
	struct mc16_writeback_tail tail = { };
	struct mc16_swap_fields fields = {
		.type = MAX_SWAPFILES - 1, .swap_offset = 1024 + index,
		.byte_offset = 0, .packed = false,
	};
	bool pinned = false;
	int ret = -EBUSY;

	mutex_lock(memcomp_lock_of(ref->token));
	if (mc16_writeback_claim_locked(ref, &snapshot)) {
		mc16_length_decode(snapshot.length, &fields.length);
		ret = memcomp_slot_dup(swp_entry(fields.type, fields.swap_offset));
		if (!ret) {
			pinned = true;
			ret = mc16_writeback_settle_locked(ref, &snapshot, &fields, &tail);
		}
		if (ret)
			mc16_writeback_revert_locked(ref);
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	if (pinned && ret)
		memcomp_slot_put(swp_entry(fields.type, fields.swap_offset));
	if (!ret)
		mc16_writeback_finish(ref, &tail);
	return ret;
}

static void memcomp_test_mincore_mthp(struct kunit *test)
{
	struct mc16_ft *mm = mc16_ft_begin(test, MC16_FT_PAGES, false, false);
	struct mc16_mincore_test *f;
	unsigned char vec[MC16_FT_PAGES] = { };
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, mm);
	KUNIT_ASSERT_EQ(test, mc16_mincore_test_member_swap(mm, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_mincore_test_member_swap(mm, 3), 0);
	f = mc16_mincore_test_begin(test, mm->tokens[1]);
	KUNIT_ASSERT_NOT_NULL(test, f);
	mmap_read_lock(mm->mm);
	ret = memcomp_test_mincore_walk(mm->mm, mm->addr,
					mm->addr + sizeof(vec) * PAGE_SIZE, vec);
	mmap_read_unlock(mm->mm);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < ARRAY_SIZE(vec); i++)
		KUNIT_EXPECT_EQ(test, vec[i], (unsigned char)(!(i & 1)));
	KUNIT_EXPECT_EQ(test, f->lookups, 2U);
	f->cached = true;
	folio_mark_uptodate(f->cache);
	mmap_read_lock(mm->mm);
	ret = memcomp_test_mincore_walk(mm->mm, mm->addr,
					mm->addr + sizeof(vec) * PAGE_SIZE, vec);
	mmap_read_unlock(mm->mm);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < ARRAY_SIZE(vec); i++)
		KUNIT_EXPECT_EQ(test, vec[i], (unsigned char)1);
	mc16_mincore_test_expect_unlocked(test, f);
}

static void memcomp_test_mincore_pte_replaced(struct kunit *test)
{
	struct mc16_ft *mm = mc16_ft_begin(test, 1, false, false);
	struct mc16_mincore_test *f;
	unsigned char vec = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, mm);
	KUNIT_ASSERT_EQ(test, mc16_mincore_test_member_swap(mm, 0), 0);
	f = mc16_mincore_test_begin(test, mm->tokens[0]);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->fault = mm;
	mmap_read_lock(mm->mm);
	ret = memcomp_test_mincore_walk(mm->mm, mm->addr, mm->addr + PAGE_SIZE, &vec);
	mmap_read_unlock(mm->mm);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, f->changed);
	KUNIT_EXPECT_EQ(test, vec, (unsigned char)1);
	KUNIT_EXPECT_EQ(test, f->lookups, 1U);
	mc16_mincore_test_expect_unlocked(test, f);
}

#define MC16_MINCORE_MM_TEST_ENABLED
#endif /* MC16_FAULT_MM_TEST_ENABLED */
#endif /* CONFIG_MEMCG && CONFIG_SWAP */
#endif /* !MC16_MINCORE_DECLARE_TEST_API */
#endif /* _MM_MEMCOMP_TESTS_MINCORE_TEST_H */
