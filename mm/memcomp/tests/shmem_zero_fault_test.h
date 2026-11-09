/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SHMEM_ZERO_FAULT_TEST_H
#define _MM_MEMCOMP_TESTS_SHMEM_ZERO_FAULT_TEST_H

/* Include after shmem_retry_test.h and fault_test.h in suite.h. */
#if defined(MC16_SHMEM_RETRY_TEST_ENABLED) && \
	defined(MC16_FAULT_MM_TEST_ENABLED)
#define MC16_SHMEM_ZERO_FAULT_TEST_ENABLED

#include <linux/pagemap.h>

/* Same real writepage actor as the retry fixture, with a zero source. */
static int mc16_sz_pageout(struct mc16_sr_test *f)
{
	struct address_space *mapping = f->file->f_mapping;
	struct writeback_control wbc = { .for_reclaim = 1 };
	struct folio *folio = NULL;
	int ret;

	ret = shmem_get_folio(file_inode(f->file), 0, 0, &folio, SGP_CACHE);
	if (ret)
		return ret;
	if (!folio || folio_test_large(folio)) {
		if (folio) {
			folio_unlock(folio);
			folio_put(folio);
		}
		return -EINVAL;
	}
	folio_put(f->cache->src);
	f->cache->src = folio;
	f->cache->fifo_entries =
		atomic_long_read(&memcomp_lrus[folio_nid(folio)].nr_entries);
#ifdef CONFIG_MEMCG
	f->cache->model.folio = folio;
#endif
	mc16_cf_fill(f->cache, MC16_CF_ZERO);
	folio_mark_uptodate(folio);
	folio_mark_dirty(folio);
	lru_add_drain_all();
	if (!folio_isolate_lru(folio)) {
		folio_unlock(folio);
		return -EBUSY;
	}
	folio_clear_active(folio);
	f->isolated = true;
	mc16_cf_actor = f->cache;
	ret = mapping->a_ops->writepage(&folio->page, &wbc);
	mc16_cf_actor = NULL;
	if (folio_test_locked(folio))
		folio_unlock(folio);
	if (folio->mapping)
		folio_putback_lru(folio);
	else
		folio_put(folio);
	f->isolated = false;
	return ret;
}

static void memcomp_test_shmem_token0_not_major(struct kunit *test)
{
	struct mc16_sr_test *f = mc16_sr_begin(test);
	struct vm_area_struct *vma;
	struct address_space *mapping;
	struct folio *folio;
	unsigned long addr;
	vm_fault_t ret = VM_FAULT_SIGBUS;
	void *entry, *data;
	bool mapped = false, zero;
	bool uptodate, correct_mapping;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mapping = f->file->f_mapping;
	addr = kunit_vm_mmap(test, f->file, 0, PAGE_SIZE,
			     PROT_READ | PROT_WRITE, MAP_SHARED, 0);
	KUNIT_ASSERT_NE(test, addr, 0UL);
	KUNIT_ASSERT_FALSE(test, IS_ERR_VALUE(addr));
	KUNIT_ASSERT_EQ(test, mc16_sz_pageout(f), 0);
	rcu_read_lock();
	entry = xa_load(&mapping->i_pages, 0);
	rcu_read_unlock();
	KUNIT_ASSERT_TRUE(test, xa_is_value(entry));
	KUNIT_ASSERT_TRUE(test, is_memcomp_entry(radix_to_swp_entry(entry)));
	KUNIT_ASSERT_EQ(test, swp_offset(radix_to_swp_entry(entry)), 0UL);
	KUNIT_ASSERT_EQ(test, mapping->nrpages, 0UL);

	/* No ALLOW_RETRY: this call retains the caller's mmap read lock. */
	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, addr);
	if (vma && vma->vm_file == f->file) {
		mapped = true;
		ret = handle_mm_fault(vma, addr, FAULT_FLAG_WRITE, NULL);
	}
	mmap_read_unlock(current->mm);
	KUNIT_ASSERT_TRUE(test, mapped);
	kunit_info(test, "shmem token0 native fault: ret=%#x",
		   (__force unsigned int)ret);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)(ret & VM_FAULT_MAJOR), 0U);
	KUNIT_EXPECT_EQ(test,
			(__force unsigned int)(ret & (VM_FAULT_ERROR | VM_FAULT_RETRY)),
			0U);

	/* Lookup only: do not allocate a replacement folio after a bad fault. */
	folio = filemap_get_folio(mapping, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(folio));
	f->result = folio;
	folio_lock(folio);
	uptodate = folio_test_uptodate(folio);
	correct_mapping = folio->mapping == mapping;
	data = kmap_local_folio(folio, 0);
	zero = !memchr_inv(data, 0, PAGE_SIZE);
	kunmap_local(data);
	folio_unlock(folio);
	KUNIT_EXPECT_TRUE(test, uptodate);
	KUNIT_EXPECT_TRUE(test, correct_mapping);
	KUNIT_EXPECT_TRUE(test, zero);
	KUNIT_EXPECT_EQ(test, SHMEM_I(file_inode(f->file))->swapped, 0UL);
	KUNIT_EXPECT_EQ(test, mapping->nrpages, 1UL);
}
#endif
#endif /* _MM_MEMCOMP_TESTS_SHMEM_ZERO_FAULT_TEST_H */
