/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_PUBLIC_DRIVER_TEST_H
#define _MM_MEMCOMP_TESTS_PUBLIC_DRIVER_TEST_H

#include <kunit/test.h>
#include <linux/highmem.h>

/*
 * Include after public_api_test.h and folio_test.h in suite.h, before the
 * main case array. Requires the public slot-table dispatch in memcomp.c.
 * No public wrapper, walker, restore actor or IO function is mocked here.
 *
 * Resident cases reuse the real MM/folio/rmap fixture. Swapoff cases reuse the
 * unlocked consumer/public-API fixture and swapoff_test.h's encoded-backing setup.
 * Only metadata objcg and backing reference tallies use existing models.
 * The public driver has no device-read seam: type rejection and corrupt-state
 * errors intentionally stop before IO. These are driver binding tests, not
 * successful device restore tests or another copy of the swapoff model matrix.
 * Every actor drops its MM/PTL/folio/T/W locks and mapping before assertions;
 * fixture cleanup never inherits a lock from this header's test task.
 */
#ifdef MC16_FOLIO_TEST_ENABLED
#define MC16_DRIVER_RESIDENT_TEST_ENABLED

struct mc16_dct_resident {
	struct mc16_ct_snapshot before;
	struct mc16_ct_snapshot after;
	pte_t orig;
	pte_t result;
	int query;
	int lookup;
	int restore;
	bool consumed;
	bool bytes_equal;
	bool same_page;
	bool metadata_restored;
};

static bool mc16_dct_public_reserve(struct mc16_flt *f)
{
	bool reserved;

	folio_lock(f->folio);
	reserved = memcomp_reserve(f->folio);
	folio_unlock(f->folio);
	return reserved;
}

/* The independent observer guard protects metadata while restore consumes its. */
static bool mc16_dct_flags(struct memcomp_ref *observer, u16 *saved, bool restore)
{
	struct mc16_view view;
	bool valid;

	mutex_lock(memcomp_lock_of(observer->token));
	valid = mc16_view_locked(observer, &view);
	if (valid) {
		if (restore) {
			*view.flags = *saved;
		} else {
			*saved = *view.flags;
			*view.flags |= BIT(11); /* Reserved bit: effective flags invalid. */
		}
	}
	mutex_unlock(memcomp_lock_of(observer->token));
	return valid;
}

static int mc16_dct_restore(struct mc16_flt *f, bool corrupt,
			    struct mc16_dct_resident *r)
{
	struct memcomp_ref *lookup = &f->base->guard;
	struct vm_area_struct *vma;
	spinlock_t *ptl; /* Copy orig/result PTEs only within their own PTL scope. */
	unsigned long token = f->guards[0].token;
	pte_t *ptep;
	u16 saved = 0;
	void *bytes;
	bool injected = false;
	int ret = -ENOENT;

	memset(r, 0, sizeof(*r));
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (!vma)
		goto out;
	{
		struct vm_fault vmf = {
			.vma = vma, .address = f->addr, .real_address = f->addr,
			.flags = FAULT_FLAG_ORIG_PTE_VALID | FAULT_FLAG_RETRY_NOWAIT,
			.pmd = mm_find_pmd(f->mm, f->addr),
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};
		if (!vmf.pmd)
			goto out;
		ptep = pte_offset_map_lock(f->mm, vmf.pmd, f->addr, &ptl);
		if (!ptep)
			goto out;
		r->orig = ptep_get(ptep);
		vmf.orig_pte = r->orig;
		pte_unmap_unlock(ptep, ptl);

		r->query = memcomp_entry_resident(token);
		r->lookup = memcomp_entry_resident_get(token, lookup);
		if (r->lookup != 1)
			goto out;
		if (!mc16_ct_snapshot(&f->guards[0], &r->before))
			goto out;
		if (corrupt) {
			injected = mc16_dct_flags(&f->guards[0], &saved, false);
			if (!injected)
				goto out;
		}
		/* Real wrapper consumes the public lookup, on success and SIGBUS alike. */
		r->restore = memcomp_restore_resident(&vmf, lookup);
		r->consumed = !lookup->slot && !lookup->cluster && !lookup->token;
		if (injected) {
			r->metadata_restored = mc16_dct_flags(&f->guards[0], &saved, true);
			injected = false;
		} else {
			r->metadata_restored = true;
		}
		if (!mc16_ct_snapshot(&f->guards[0], &r->after))
			goto out;
		ptep = pte_offset_map_lock(f->mm, vmf.pmd, f->addr, &ptl);
		if (!ptep)
			goto out;
		r->result = ptep_get(ptep);
		if (pte_present(r->result) && !pte_special(r->result)) {
			r->same_page = pte_page(r->result) == folio_page(f->folio, 0);
			if (r->same_page) {
				bytes = kmap_local_page(pte_page(r->result));
				r->bytes_equal = !memchr_inv(bytes, 0x6b, PAGE_SIZE);
				kunmap_local(bytes);
			}
		}
		pte_unmap_unlock(ptep, ptl);
		ret = 0;
	}
out:
	if (injected)
		mc16_dct_flags(&f->guards[0], &saved, true);
	/* Also clean preparation failures before the folio fixture is torn down. */
	if (lookup->slot)
		memcomp_entry_put_ref(lookup);
	/* RETRY_NOWAIT keeps mmap_read_lock on every folio-lock retry outcome. */
	mmap_read_unlock(f->mm);
	return ret;
}

static void memcomp_test_public_resident_restore(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_dct_resident r;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_dct_public_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_dct_restore(f, false, &r), 0);
	KUNIT_EXPECT_EQ(test, r.query, 1);
	KUNIT_EXPECT_EQ(test, r.lookup, 1);
	KUNIT_EXPECT_EQ(test, r.restore, 0);
	KUNIT_EXPECT_TRUE(test, r.consumed);
	KUNIT_EXPECT_TRUE(test, r.metadata_restored);
	KUNIT_EXPECT_TRUE(test, r.same_page);
	KUNIT_EXPECT_TRUE(test, r.bytes_equal);
	KUNIT_EXPECT_EQ(test, r.before.refs, 4U); /* OWNER, observer, PTE, lookup. */
	KUNIT_EXPECT_EQ(test, r.after.refs, 2U); /* OWNER and observer survive. */
	KUNIT_EXPECT_EQ(test, mc16_flags_state(r.after.flags), MC16_PENDING);
	KUNIT_EXPECT_TRUE(test, r.after.flags & MC16_F_RESIDENT);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_ANONPAGES), f->anonpages + 1);
}

static void memcomp_test_public_restore_error(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_dct_resident r;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_dct_public_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_dct_restore(f, true, &r), 0);
	KUNIT_EXPECT_EQ(test, r.query, 1);
	KUNIT_EXPECT_EQ(test, r.lookup, 1);
	KUNIT_EXPECT_EQ(test, r.restore, -EFAULT);
	KUNIT_EXPECT_TRUE(test, r.consumed);
	KUNIT_EXPECT_TRUE(test, r.metadata_restored);
	KUNIT_EXPECT_TRUE(test, pte_same(r.orig, r.result));
	KUNIT_EXPECT_EQ(test, r.before.refs, 4U);
	KUNIT_EXPECT_EQ(test, r.after.refs, 3U); /* Failed restore retains PTE. */
	KUNIT_EXPECT_EQ(test, r.after.flags, r.before.flags);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents + 1);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_ANONPAGES), f->anonpages);
	KUNIT_EXPECT_EQ(test, memcomp_entry_resident(f->guards[0].token), 1);
}
#endif /* MC16_FOLIO_TEST_ENABLED */

#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)
#define MC16_DRIVER_SWAPOFF_TEST_ENABLED

/* Same encoded-backing setup as swapoff_test.h, on an unlocked base fixture. */
static int mc16_dct_swapped(struct mc16_ct *f)
{
	struct memcomp_ref *owner = &f->base->owners[0];
	struct mc16_view view;
	u64 word;
	u16 length;
	int ret;

	ret = mc16_consumer_test_reserve(f->base, 0);
	if (ret)
		return ret;
	if (!mc16_swap_encode(7, 0x567, 0, PAGE_SIZE, false, &word, &length))
		return -EINVAL;
	mutex_lock(memcomp_lock_of(owner->token));
	if (!mc16_view_locked(owner, &view) || !view.member) {
		ret = -EINVAL;
		goto out;
	}
	mc16_account_state(owner, false);
	folio_put(view.member->owner_folio);
	*view.word = word;
	*view.length = length;
	*view.flags = MC16_SWAPPED | MC16_F_OWNER | MC16_F_REFERENCED;
	memcomp_debug_dec(&memcomp_pending_pages);
	memcomp_debug_long_inc(&memcomp_swapped_entries);
	memcomp_compressed_bytes_add(PAGE_SIZE);
	mc16_account_state(owner, true);
	ret = 0;
out:
	mutex_unlock(memcomp_lock_of(owner->token));
	if (!ret) {
		f->token = owner->token;
		if (!memcomp_entry_get_ref(f->token, &f->guard))
			ret = -ENOENT;
	}
	return ret;
}

struct mc16_dct_swapoff {
	struct mc16_ct_snapshot before;
	struct mc16_ct_snapshot after;
	int result;
	int dups;
	int puts;
	unsigned int flags_before;
	unsigned int flags_after;
	bool metadata_restored;
	bool unlocked;
};

static int mc16_dct_swapoff(struct mc16_ct *f, bool corrupt,
			    struct mc16_dct_swapoff *r)
{
	struct mc16_view view;
	u16 saved = 0;
	bool injected = false;
	int dups, puts;

	memset(r, 0, sizeof(*r));
	if (!mc16_ct_snapshot(&f->guard, &r->before))
		return -EINVAL;
	/* The public driver acquires W itself; never call it under an outer W. */
	if (corrupt) {
		mutex_lock(memcomp_lock_of(f->token));
		if (mc16_view_locked(&f->guard, &view)) {
			saved = *view.flags;
			*view.flags |= BIT(11);
			injected = true;
		}
		mutex_unlock(memcomp_lock_of(f->token));
		if (!injected)
			return -EINVAL;
	}
	dups = atomic_read(&memcomp_kunit_slot_dups);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	r->flags_before = current->flags & (PF_MEMALLOC_NOFS | PF_MEMALLOC);
	/* Valid backing type 7 must be rejected by the real restore binding. */
	r->result = memcomp_swapoff(8);
	r->flags_after = current->flags & (PF_MEMALLOC_NOFS | PF_MEMALLOC);
	r->dups = atomic_read(&memcomp_kunit_slot_dups) - dups;
	r->puts = atomic_read(&memcomp_kunit_slot_puts) - puts;
	if (injected) {
		mutex_lock(memcomp_lock_of(f->token));
		if (mc16_view_locked(&f->guard, &view)) {
			*view.flags = saved;
			r->metadata_restored = true;
		}
		mutex_unlock(memcomp_lock_of(f->token));
	} else {
		r->metadata_restored = true;
	}
	r->unlocked = mutex_trylock(&memcomp_writeback_lock);
	if (r->unlocked)
		mutex_unlock(&memcomp_writeback_lock);
	return mc16_ct_snapshot(&f->guard, &r->after) ? 0 : -EINVAL;
}

static void mc16_dct_driver(struct kunit *test, bool corrupt)
{
	struct mc16_ct *f = mc16_ct_begin(test);
	struct mc16_dct_swapoff r;
	s64 stored, compressed;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_dct_swapped(f), 0);
	stored = atomic64_read(&memcomp_stored_bytes);
	compressed = atomic64_read(&memcomp_compressed_bytes);
	KUNIT_ASSERT_EQ(test, mc16_dct_swapoff(f, corrupt, &r), 0);
	KUNIT_EXPECT_EQ(test, r.result, corrupt ? -EIO : 0);
	KUNIT_EXPECT_TRUE(test, r.metadata_restored);
	KUNIT_EXPECT_TRUE(test, r.unlocked);
	KUNIT_EXPECT_EQ(test, r.flags_after, r.flags_before);
	KUNIT_EXPECT_EQ(test, r.dups, corrupt ? 0 : 1);
	KUNIT_EXPECT_EQ(test, r.puts, corrupt ? 0 : 1);
	KUNIT_EXPECT_EQ(test, r.before.refs, 2U);
	KUNIT_EXPECT_EQ(test, r.after.refs, r.before.refs);
	KUNIT_EXPECT_EQ(test, r.after.word, r.before.word);
	KUNIT_EXPECT_EQ(test, r.after.length, r.before.length);
	KUNIT_EXPECT_EQ(test, r.after.flags, r.before.flags);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(r.after.flags), MC16_SWAPPED);
	KUNIT_EXPECT_NOT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_stored_bytes), stored);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compressed_bytes), compressed);
	/* Explicitly prove public guard put leaves only the fixture's owner. */
	memcomp_entry_put_ref(&f->guard);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->base->owners[0].slot->refs), 1U);
}

static void memcomp_test_public_swapoff_type_miss(struct kunit *test)
{
	mc16_dct_driver(test, false);
}

static void memcomp_test_public_swapoff_error(struct kunit *test)
{
	mc16_dct_driver(test, true);
}
#endif /* CONFIG_MEMCG && CONFIG_SWAP */

/*
 * Register these additions in the existing suite:
 *
 * #ifdef MC16_DRIVER_RESIDENT_TEST_ENABLED
 * KUNIT_CASE(memcomp_test_public_resident_restore),
 * KUNIT_CASE(memcomp_test_public_restore_error),
 * #endif
 * #ifdef MC16_DRIVER_SWAPOFF_TEST_ENABLED
 * KUNIT_CASE(memcomp_test_public_swapoff_type_miss),
 * KUNIT_CASE(memcomp_test_public_swapoff_error),
 * #endif
 */

#endif /* _MM_MEMCOMP_TESTS_PUBLIC_DRIVER_TEST_H */
