/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_CACHE_XARRAY_RACE_TEST_H
#define _MM_MEMCOMP_TESTS_CACHE_XARRAY_RACE_TEST_H

/* Include after shmem_retry_test.h. No load/error return is synthesized. */
#ifdef MC16_SHMEM_RETRY_TEST_ENABLED
struct mc16_cxr {
	struct mc16_sr_test *sr;
	struct folio *lost;
	unsigned int calls, refs_before, refs_truncated;
	u64 before[2], after[2], truncated_word;
	int errors[2];
	bool locked, unmapped, uptodate, bytes, entry_matched, emptied;
};

static struct mc16_cxr *mc16_cxr_current;

static bool mc16_cxr_bytes(struct folio *folio, struct folio *source)
{
	void *a = kmap_local_folio(source, 0);
	void *b = kmap_local_folio(folio, 0);
	bool same = !memcmp(a, b, PAGE_SIZE);

	kunmap_local(b);
	kunmap_local(a);
	return same;
}

static void mc16_cxr_cleanup(void *data)
{
	struct mc16_cxr *p = data;

	mc16_cxr_current = NULL;
	if (p->lost) {
		/* The original outer call owns unlock/put; retain only our pin. */
		KUNIT_EXPECT_FALSE(p->sr->test, folio_test_locked(p->lost));
		if (folio_test_locked(p->lost))
			folio_unlock(p->lost);
		folio_clear_active(p->lost);
		folio_put(p->lost);
	}
	/* A failed native-state assertion must not leave active on final put. */
	folio_clear_active(p->sr->probe);
}

static int mc16_cxr_load(struct memcomp_ref *ref, struct folio *folio)
{
	struct mc16_cxr *p = mc16_cxr_current;
	struct mc16_sr_test *f = p->sr;
	struct address_space *mapping = f->file->f_mapping;
	unsigned int call = p->calls++;
	void *entry;
	int ret;

	if (call < ARRAY_SIZE(p->before))
		p->before[call] = mc16_sr_shadow(ref);
	if (!call)
		p->refs_before = refcount_read(&ref->slot->refs);
	kunit_deactivate_static_stub(f->test, mc16_cache_load);
	ret = mc16_cache_load(ref, folio);
	if (call < ARRAY_SIZE(p->after)) {
		p->after[call] = mc16_sr_shadow(ref);
		p->errors[call] = ret;
	}
	if (!call && !ret) {
		/* Native read/refault finished; this private folio is not in XA. */
		folio_get(folio);
		p->lost = folio;
		p->locked = folio_test_locked(folio);
		p->unmapped = !folio->mapping;
		p->uptodate = folio_test_uptodate(folio);
		p->bytes = mc16_cxr_bytes(folio, f->cache->src);
		rcu_read_lock();
		entry = xa_load(&mapping->i_pages, 0);
		rcu_read_unlock();
		p->entry_matched = entry == f->entry;
		/* No T/XA/PTL/RCU section or local mapping survives this point. */
		shmem_truncate_range(file_inode(f->file), 0, (loff_t)-1);
		rcu_read_lock();
		entry = xa_load(&mapping->i_pages, 0);
		rcu_read_unlock();
		p->emptied = !entry;
		p->truncated_word = mc16_sr_shadow(ref);
		p->refs_truncated = refcount_read(&ref->slot->refs);
	}
	kunit_activate_static_stub(f->test, mc16_cache_load, mc16_cxr_load);
	return ret;
}

static void memcomp_test_cache_xarray_race_no_restore(struct kunit *test)
{
	struct mc16_sr_test *f = mc16_sr_begin(test);
	struct mc16_cxr *p;
	struct memcomp_ref stale = { };
	struct address_space *mapping;
	struct shmem_inode_info *info;
	unsigned long token;
	blkcnt_t blocks;
	void *data, *entry;
	bool zero, found;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->sr = f;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mc16_cxr_cleanup, p), 0);
	mc16_cxr_current = p;
	/* Use real eviction/publication; no PUBLISHING-delay or read-error stub. */
	KUNIT_ASSERT_EQ(test, mc16_sr_pageout(f), 0);
	mapping = f->file->f_mapping;
	info = SHMEM_I(file_inode(f->file));
	rcu_read_lock();
	f->entry = xa_load(&mapping->i_pages, 0);
	rcu_read_unlock();
	KUNIT_ASSERT_TRUE(test, xa_is_value(f->entry));
	KUNIT_ASSERT_TRUE(test, is_memcomp_entry(radix_to_swp_entry(f->entry)));
	token = swp_offset(radix_to_swp_entry(f->entry));
	f->cache->token = token;
	KUNIT_ASSERT_NE(test, token, 0UL);
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->cache->guard));
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)(unsigned long)
			mc16_sr_shadow(&f->cache->guard)));
	KUNIT_ASSERT_NE(test, mc16_sr_shadow(&f->cache->guard),
			(u64)MC16_SHADOW_PUBLISHING);
	KUNIT_ASSERT_EQ(test, refcount_read(&f->cache->guard.slot->refs), 2U);
	blocks = file_inode(f->file)->i_blocks;
	kunit_activate_static_stub(test, mc16_cache_load, mc16_cxr_load);
	ret = mc16_sr_outer(f);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, p->calls, 1U);
	KUNIT_ASSERT_NOT_NULL(test, p->lost);
	KUNIT_ASSERT_NOT_NULL(test, f->result);
	KUNIT_EXPECT_EQ(test, p->errors[0], 0);
	KUNIT_EXPECT_TRUE(test, xa_is_value((void *)(unsigned long)p->before[0]));
	KUNIT_EXPECT_EQ(test, p->after[0], 0ULL);
	KUNIT_EXPECT_EQ(test, p->truncated_word, 0ULL);
	KUNIT_EXPECT_TRUE(test, p->locked);
	KUNIT_EXPECT_TRUE(test, p->unmapped);
	KUNIT_EXPECT_TRUE(test, p->uptodate);
	KUNIT_EXPECT_TRUE(test, p->bytes);
	KUNIT_EXPECT_TRUE(test, p->entry_matched);
	KUNIT_EXPECT_TRUE(test, p->emptied);
	KUNIT_EXPECT_EQ(test, p->refs_before, 3U);
	KUNIT_EXPECT_EQ(test, p->refs_truncated, 2U);
	KUNIT_EXPECT_PTR_EQ(test, p->lost->mapping, NULL);
	KUNIT_EXPECT_FALSE(test, folio_test_locked(p->lost));
	KUNIT_EXPECT_EQ(test, folio_ref_count(p->lost), 1);
	KUNIT_EXPECT_PTR_NE(test, p->lost, f->result);
	KUNIT_EXPECT_TRUE(test, f->result_locked);
	KUNIT_EXPECT_TRUE(test, f->result_uptodate);
	KUNIT_EXPECT_TRUE(test, f->result_mapping);
	KUNIT_EXPECT_EQ(test, folio_order(f->result), 0U);
	data = kmap_local_folio(f->result, 0);
	zero = !memchr_inv(data, 0, PAGE_SIZE);
	kunmap_local(data);
	KUNIT_EXPECT_TRUE(test, zero);
	rcu_read_lock();
	entry = xa_load(&mapping->i_pages, 0);
	rcu_read_unlock();
	KUNIT_EXPECT_PTR_EQ(test, entry, f->result);
	KUNIT_EXPECT_EQ(test, info->alloced, 1UL);
	KUNIT_EXPECT_EQ(test, info->swapped, 0UL);
	KUNIT_EXPECT_EQ(test, mapping->nrpages, 1UL);
	KUNIT_EXPECT_EQ(test, file_inode(f->file)->i_blocks, blocks);
	KUNIT_EXPECT_EQ(test, mc16_sr_shadow(&f->cache->guard), 0ULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->cache->guard.slot->refs), 1U);
	/* Old XA owner is gone; the retained real guard still pins its payload. */
	ret = mc16_user_charge(&f->cache->guard, f->probe, NULL, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	folio_lock(f->probe);
	folio_set_swapbacked(f->probe);
	ret = mc16_cache_load(&f->cache->guard, f->probe);
	folio_unlock(f->probe);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, p->calls, 2U);
	KUNIT_EXPECT_EQ(test, p->errors[1], 0);
	KUNIT_EXPECT_EQ(test, p->before[1], 0ULL);
	KUNIT_EXPECT_EQ(test, p->after[1], 0ULL);
	KUNIT_EXPECT_TRUE(test, folio_test_uptodate(f->probe));
	KUNIT_EXPECT_TRUE(test, mc16_cxr_bytes(f->probe, f->cache->src));
	KUNIT_EXPECT_FALSE(test, folio_test_workingset(f->probe));
	KUNIT_EXPECT_FALSE(test, folio_test_active(f->probe));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->cache->guard.slot->refs), 1U);
	mc16_ref_put(&f->cache->guard);
	KUNIT_EXPECT_PTR_EQ(test, f->cache->guard.slot, NULL);
	found = mc16_get(token, &stale);
	KUNIT_EXPECT_FALSE(test, found);
	if (found)
		mc16_ref_put(&stale);
}
#endif /* MC16_SHMEM_RETRY_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_CACHE_XARRAY_RACE_TEST_H */
