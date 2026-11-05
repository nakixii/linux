/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SHMEM_RETRY_TEST_H
#define _MM_MEMCOMP_TESTS_SHMEM_RETRY_TEST_H

/*
 * Include after cache_folio_test.h in suite.h. CONFIG_SHMEM gates the cases.
 * The CONFIG_MEMCOMP_KUNIT_TEST entry seam in mc16_cache_load() observes
 * the real consumer before its publication-window check.
 *
 * A real shmem file is populated and evicted through its writepage operation.
 * The existing eviction stub computes a native shadow but delays visibility
 * by leaving the PUBLISHING marker. The load observer calls the original
 * consumer, records its actual -EAGAIN, then completes native publication.
 * Only shmem's real outer loop can retry; no retry/error translation is
 * implemented here. This is a scheduled window, not a concurrent stress test.
 * A direct preflight checks the seam before entering a potentially repeating
 * outer call, so forgetting the new seam fails rather than hangs the suite.
 */
#ifdef CONFIG_SHMEM
#define MC16_SHMEM_RETRY_TEST_ENABLED

#include <kunit/static_stub.h>
#include <linux/file.h>
#include <linux/shmem_fs.h>
#include <linux/writeback.h>

struct mc16_sr_test {
	struct kunit *test;
	struct mc16_cf_test *cache;
	struct file *file;
	struct folio *probe;
	struct folio *result;
	void *entry;
	void *shadow;
	unsigned int evictions;
	unsigned int calls;
	unsigned int reads;
	unsigned int refs[2];
	int errors[2];
	int read_error;
	bool isolated;
	bool preflight;
	bool published;
	bool contract;
	bool result_locked;
	bool result_uptodate;
	bool result_mapping;
	bool result_bytes;
};

static struct mc16_sr_test *mc16_sr_current;

static u64 mc16_sr_shadow(struct memcomp_ref *ref)
{
	u64 word;

	mutex_lock(memcomp_lock_of(ref->token));
	word = *mc16_shadow_ptr(ref);
	mutex_unlock(memcomp_lock_of(ref->token));
	return word;
}

static void mc16_sr_cleanup(void *data)
{
	struct mc16_sr_test *f = data;

	mc16_sr_current = NULL;
	mc16_cf_actor = NULL;
	f->cache->probe.task = current;
	f->cache->payload.task = current;
#ifdef CONFIG_MEMCG
	f->cache->model.task = current;
#endif
	if (f->result) {
		if (folio_test_locked(f->result))
			folio_unlock(f->result);
		folio_put(f->result);
	}
	if (f->probe)
		folio_put(f->probe);
	if (folio_test_locked(f->cache->src))
		folio_unlock(f->cache->src);
	if (f->isolated)
		folio_putback_lru(f->cache->src);
	if (f->file) {
		/* Synchronous truncate drops the cache owner before cf cleanup. */
		shmem_truncate_range(file_inode(f->file), 0, (loff_t)-1);
		fput(f->file);
	}
	f->cache->src->swap.val = 0;
}

static void *mc16_sr_eviction(struct folio *folio,
			      struct mem_cgroup *target_memcg)
{
	struct mc16_sr_test *f = mc16_sr_current;

	f->evictions++;
	f->contract &= folio == f->cache->src && folio_test_locked(folio) &&
		!folio_test_lru(folio);
	kunit_deactivate_static_stub(f->test, mc16_store_eviction);
	f->shadow = mc16_store_eviction(folio, target_memcg);
	/* Retain the actual publication gate until a reader observes it. */
	return (void *)(unsigned long)MC16_SHADOW_PUBLISHING;
}

static int mc16_sr_read(struct memcomp_ref *ref, void *dst)
{
	struct mc16_sr_test *f = mc16_sr_current;
	int ret;

	f->reads++;
	if (f->read_error && f->reads == 1)
		return f->read_error;
	kunit_deactivate_static_stub(f->test, mc16_load_page);
	ret = mc16_load_page(ref, dst);
	kunit_activate_static_stub(f->test, mc16_load_page, mc16_sr_read);
	return ret;
}

static int mc16_sr_load(struct memcomp_ref *ref, struct folio *folio)
{
	struct mc16_sr_test *f = mc16_sr_current;
	unsigned int call;
	u64 before, after;
	int ret;

	if (f->preflight)
		return -EINTR;
	call = f->calls++;
	before = mc16_sr_shadow(ref);
	f->contract &= ref->token == f->cache->token && folio_test_locked(folio);
	rcu_read_lock();
	f->contract &= xa_load(&f->file->f_mapping->i_pages, 0) == f->entry;
	rcu_read_unlock();
	if (call < ARRAY_SIZE(f->refs))
		f->refs[call] = refcount_read(&ref->slot->refs);
	kunit_deactivate_static_stub(f->test, mc16_cache_load);
	ret = mc16_cache_load(ref, folio);
	kunit_activate_static_stub(f->test, mc16_cache_load, mc16_sr_load);
	after = mc16_sr_shadow(ref);
	if (call < ARRAY_SIZE(f->errors))
		f->errors[call] = ret;
	if (!call) {
		f->contract &= before == MC16_SHADOW_PUBLISHING &&
			after == MC16_SHADOW_PUBLISHING && !f->reads;
		if (ret == -EAGAIN) {
			/* Finish the paused publisher; do not synthesize its shadow. */
			mc16_store_publish(ref, 1, f->shadow);
			f->published = true;
		}
	} else {
		f->contract &= before == (u64)(unsigned long)f->shadow && !after;
	}
	return ret;
}

static struct mc16_sr_test *mc16_sr_begin(struct kunit *test)
{
	struct mc16_sr_test *f;
	struct mc16_cf_test *cache = mc16_cf_begin(test);

	if (!cache)
		return NULL;
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->cache = cache;
	f->contract = true;
	if (kunit_add_action_or_reset(test, mc16_sr_cleanup, f))
		return NULL;
	f->file = shmem_kernel_file_setup("memcomp-shmem-retry", PAGE_SIZE,
					  VM_NORESERVE);
	if (IS_ERR(f->file)) {
		f->file = NULL;
		return NULL;
	}
	f->probe = folio_alloc(GFP_KERNEL, 0);
	if (!f->probe)
		return NULL;
	return f;
}

/* All locks and the isolate reference are released before caller assertions. */
static int mc16_sr_pageout(struct mc16_sr_test *f)
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
	f->cache->fifo_entries = atomic_long_read(&memcomp_lrus[folio_nid(folio)].nr_entries);
#ifdef CONFIG_MEMCG
	f->cache->model.folio = folio;
#endif
	mc16_cf_fill(f->cache, MC16_CF_BYTES);
	folio_mark_uptodate(folio);
	folio_mark_dirty(folio);
	lru_add_drain_all();
	if (!folio_isolate_lru(folio)) {
		folio_unlock(folio);
		return -EBUSY;
	}
	/* writepage is normally called with an inactive reclaim source. */
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

static int mc16_sr_outer(struct mc16_sr_test *f)
{
	struct folio *folio;
	void *src, *dst;
	int ret;

	ret = shmem_get_folio(file_inode(f->file), 0, 0, &f->result, SGP_CACHE);
	if (ret || !f->result)
		return ret;
	folio = f->result;
	f->result_locked = folio_test_locked(folio);
	f->result_uptodate = folio_test_uptodate(folio);
	f->result_mapping = folio->mapping == f->file->f_mapping;
	src = kmap_local_folio(f->cache->src, 0);
	dst = kmap_local_folio(folio, 0);
	f->result_bytes = !memcmp(src, dst, PAGE_SIZE);
	kunmap_local(dst);
	kunmap_local(src);
	if (f->result_locked)
		folio_unlock(folio);
	return ret;
}

static void mc16_sr_run(struct kunit *test, int error)
{
	struct mc16_sr_test *f = mc16_sr_begin(test);
	struct shmem_inode_info *info;
	struct address_space *mapping;
	blkcnt_t blocks;
	void *entry;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_sr_current = f;
	kunit_activate_static_stub(test, mc16_store_eviction, mc16_sr_eviction);
	KUNIT_ASSERT_EQ(test, mc16_sr_pageout(f), 0);
	mapping = f->file->f_mapping;
	info = SHMEM_I(file_inode(f->file));
	rcu_read_lock();
	f->entry = xa_load(&mapping->i_pages, 0);
	rcu_read_unlock();
	KUNIT_ASSERT_TRUE(test, xa_is_value(f->entry));
	KUNIT_ASSERT_TRUE(test, is_memcomp_entry(radix_to_swp_entry(f->entry)));
	f->cache->token = swp_offset(radix_to_swp_entry(f->entry));
	KUNIT_ASSERT_NE(test, f->cache->token, 0UL);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->cache->token, &f->cache->guard));
	KUNIT_ASSERT_EQ(test, f->evictions, 1U);
	KUNIT_ASSERT_TRUE(test, xa_is_value(f->shadow));
	KUNIT_ASSERT_NE(test, (u64)(unsigned long)f->shadow, MC16_SHADOW_PUBLISHING);
	KUNIT_ASSERT_EQ(test, mc16_sr_shadow(&f->cache->guard), MC16_SHADOW_PUBLISHING);
	kunit_activate_static_stub(test, mc16_cache_load, mc16_sr_load);
	/* Without the prerequisite seam this returns real -EAGAIN, not -EINTR. */
	f->preflight = true;
	folio_lock(f->probe);
	ret = mc16_cache_load(&f->cache->guard, f->probe);
	folio_unlock(f->probe);
	f->preflight = false;
	KUNIT_ASSERT_EQ(test, ret, -EINTR);
	f->read_error = error;
	kunit_activate_static_stub(test, mc16_load_page, mc16_sr_read);
	blocks = file_inode(f->file)->i_blocks;
	ret = mc16_sr_outer(f);
	KUNIT_EXPECT_EQ(test, ret, error);
	KUNIT_EXPECT_EQ(test, f->calls, 2U);
	KUNIT_EXPECT_EQ(test, f->errors[0], -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->errors[1], error);
	KUNIT_EXPECT_EQ(test, f->reads, 1U);
	KUNIT_EXPECT_EQ(test, f->refs[0], 3U);
	KUNIT_EXPECT_EQ(test, f->refs[1], 3U);
	KUNIT_EXPECT_TRUE(test, f->contract);
	KUNIT_EXPECT_TRUE(test, f->published);
	KUNIT_EXPECT_EQ(test, mc16_sr_shadow(&f->cache->guard), 0ULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->cache->guard.slot->refs), error ? 2U : 1U);
	KUNIT_EXPECT_EQ(test, file_inode(f->file)->i_blocks, blocks);
	KUNIT_EXPECT_EQ(test, info->alloced, 1UL);
	KUNIT_EXPECT_EQ(test, info->swapped, error ? 1UL : 0UL);
	KUNIT_EXPECT_EQ(test, mapping->nrpages, error ? 0UL : 1UL);
	if (error) {
		KUNIT_EXPECT_PTR_EQ(test, f->result, NULL);
		rcu_read_lock();
		entry = xa_load(&mapping->i_pages, 0);
		rcu_read_unlock();
		KUNIT_EXPECT_PTR_EQ(test, entry, f->entry);
	} else {
		KUNIT_EXPECT_NOT_NULL(test, f->result);
		KUNIT_EXPECT_TRUE(test, f->result_locked);
		KUNIT_EXPECT_TRUE(test, f->result_uptodate);
		KUNIT_EXPECT_TRUE(test, f->result_mapping);
		KUNIT_EXPECT_TRUE(test, f->result_bytes);
	}
}

static void memcomp_test_shmem_publish_retry(struct kunit *test)
{
	mc16_sr_run(test, 0);
}

static void memcomp_test_shmem_retry_eio(struct kunit *test)
{
	mc16_sr_run(test, -EIO);
}

static void memcomp_test_shmem_retry_enomem(struct kunit *test)
{
	mc16_sr_run(test, -ENOMEM);
}
#endif /* CONFIG_SHMEM */
#endif /* _MM_MEMCOMP_TESTS_SHMEM_RETRY_TEST_H */
