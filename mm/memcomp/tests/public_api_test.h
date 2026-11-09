/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_PUBLIC_API_TEST_H
#define _MM_MEMCOMP_TESTS_PUBLIC_API_TEST_H

/* Include after consumers_test.h. Definitions only; register in the main suite. */
#ifdef CONFIG_MEMCG
struct mc16_ct_snapshot {
	u64 word;
	u32 refs;
	u16 flags;
	u16 length;
	bool attached;
};

/* Copy values under T. Never export a record/member pointer to an assertion. */
static bool
mc16_ct_snapshot(struct memcomp_ref *ref, struct mc16_ct_snapshot *snapshot)
{
	struct mc16_view view;
	bool valid;

	*snapshot = (struct mc16_ct_snapshot) { };
	if (!ref->slot)
		return false;
	mutex_lock(memcomp_lock_of(ref->token));
	valid = mc16_view_locked(ref, &view);
	if (valid) {
		snapshot->word = *view.word;
		snapshot->flags = *view.flags;
		snapshot->length = *view.length;
		snapshot->refs = refcount_read(&ref->slot->refs);
		snapshot->attached = !!view.record;
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	return valid;
}

struct mc16_ct {
	struct mc16_consumer_test *base;
	struct memcomp_ref guard;
	unsigned long token;
	bool cache_owner;
	bool pte_owner;
};

static void mc16_ct_cleanup(void *data)
{
	struct mc16_ct *f = data;

	f->base->model.fail_at = 0;
	if (f->guard.slot)
		memcomp_entry_put_ref(&f->guard);
	if (f->pte_owner)
		memcomp_invalidate(f->token);
	if (f->cache_owner && f->token)
		memcomp_invalidate_cache(f->token);
}

static struct mc16_ct *mc16_ct_begin(struct kunit *test)
{
	struct mc16_ct *f;
	void *data;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->base = mc16_consumer_test_begin(test);
	if (!f->base)
		return NULL;
	if (kunit_add_action_or_reset(test, mc16_ct_cleanup, f))
		return NULL;
	data = kmap_local_folio(f->base->account, 0);
	memcomp_test_fill_zstd_ratio_page(data);
	kunmap_local(data);
	return f;
}

/* All locks are released before returning to any KUnit assertion. */
static bool mc16_ct_store(struct mc16_ct *f)
{
	bool stored;

	folio_lock(f->base->account);
	stored = memcomp_store_cache(f->base->account, &f->token);
	folio_unlock(f->base->account);
	f->cache_owner = stored && f->token;
	return stored;
}

/* Model the successful XA replacer's unlocked publication tail. */
static void mc16_ct_publish(struct mc16_ct *f)
{
	folio_lock(f->base->account);
	folio_set_swapbacked(f->base->account);
	memcomp_publish_cache_eviction(f->base->account, &f->guard);
	folio_unlock(f->base->account);
}

static int mc16_ct_load(struct mc16_ct *f, bool *equal)
{
	void *src, *dst;
	int ret;

	*equal = false;
	folio_lock(f->base->dst);
	ret = memcomp_load_folio(&f->guard, f->base->dst);
	if (!ret) {
		src = kmap_local_folio(f->base->account, 0);
		dst = kmap_local_folio(f->base->dst, 0);
		*equal = !memcmp(src, dst, PAGE_SIZE);
		kunmap_local(dst);
		kunmap_local(src);
	}
	folio_unlock(f->base->dst);
	return ret;
}

static void memcomp_test_public_cache_transaction(struct kunit *test)
{
	struct mc16_ct *f = mc16_ct_begin(test);
	struct mc16_ct_snapshot snapshot;
	bool equal;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_ct_store(f));
	KUNIT_ASSERT_NE(test, f->token, 0UL);
	KUNIT_EXPECT_TRUE(test, memcomp_entry_present(f->token));
	KUNIT_ASSERT_TRUE(test, memcomp_load_folio_pin(f->token, &f->guard));
	KUNIT_ASSERT_TRUE(test, mc16_ct_snapshot(&f->guard, &snapshot));
	KUNIT_EXPECT_FALSE(test, snapshot.attached);
	KUNIT_EXPECT_EQ(test, snapshot.refs, 2U);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(snapshot.flags), MC16_STORED);
	mc16_ct_publish(f);
	ret = mc16_ct_load(f, &equal);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, equal);
	KUNIT_EXPECT_NOT_NULL(test, f->guard.slot);
	memcomp_load_folio_abort(&f->guard);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_TRUE(test, memcomp_entry_present(f->token));
	KUNIT_ASSERT_TRUE(test, memcomp_load_folio_pin(f->token, &f->guard));
	ret = mc16_ct_load(f, &equal);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, equal);
	if (ret)
		return;
	memcomp_load_folio_commit(&f->guard);
	f->cache_owner = false;
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(f->token));
}

static void memcomp_test_public_ref_domains(struct kunit *test)
{
	struct mc16_ct *f = mc16_ct_begin(test);
	struct mc16_ct_snapshot snapshot;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_ct_store(f));
	KUNIT_ASSERT_NE(test, f->token, 0UL);
	KUNIT_ASSERT_TRUE(test, memcomp_entry_get_ref(f->token, &f->guard));
	KUNIT_ASSERT_TRUE(test, memcomp_entry_get(f->token));
	f->pte_owner = true;
	KUNIT_ASSERT_TRUE(test, mc16_ct_snapshot(&f->guard, &snapshot));
	KUNIT_EXPECT_EQ(test, snapshot.refs, 3U);
	memcomp_invalidate(f->token);
	f->pte_owner = false;
	KUNIT_ASSERT_TRUE(test, mc16_ct_snapshot(&f->guard, &snapshot));
	KUNIT_EXPECT_EQ(test, snapshot.refs, 2U);
	memcomp_entry_put_ref(&f->guard);
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.cluster, NULL);
	KUNIT_EXPECT_EQ(test, f->guard.token, 0U);
	KUNIT_EXPECT_TRUE(test, memcomp_entry_present(f->token));
	memcomp_invalidate_cache(f->token);
	f->cache_owner = false;
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(f->token));
}

static void memcomp_test_public_query_busy(struct kunit *test)
{
	struct mc16_ct *f = mc16_ct_begin(test);
	struct memcomp_ref resident = { };
	unsigned long compressed = ULONG_MAX, base = 0;
	int maps = -1, diagnostic_maps = -1, busy;
	bool refs_ok, diagnostic_ok, present;
	unsigned int nr;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_ct_store(f));
	KUNIT_ASSERT_NE(test, f->token, 0UL);
	KUNIT_ASSERT_TRUE(test, memcomp_entry_get_ref(f->token, &f->guard));
	nr = memcomp_entry_nr_pages(f->token, &base);
	KUNIT_EXPECT_EQ(test, nr, 1U);
	KUNIT_EXPECT_EQ(test, base, f->token);
	mutex_lock(memcomp_lock_of(f->token));
	refs_ok = memcomp_entry_stat(f->token, NULL, &maps);
	diagnostic_ok = memcomp_entry_stat(f->token, &compressed, &diagnostic_maps);
	present = memcomp_entry_present(f->token);
	busy = memcomp_entry_resident_get(f->token, &resident);
	mutex_unlock(memcomp_lock_of(f->token));
	/* Do not leave an unexpected result's guard outside cleanup ownership. */
	if (resident.slot)
		memcomp_entry_resident_put(&resident);
	KUNIT_EXPECT_TRUE(test, refs_ok);
	KUNIT_EXPECT_GE(test, maps, 1);
	KUNIT_EXPECT_FALSE(test, diagnostic_ok);
	KUNIT_EXPECT_EQ(test, compressed, 0UL);
	KUNIT_EXPECT_EQ(test, diagnostic_maps, 0);
	KUNIT_EXPECT_TRUE(test, present);
	KUNIT_EXPECT_EQ(test, busy, -EAGAIN);
	KUNIT_EXPECT_NULL(test, resident.slot);
}

static void memcomp_test_public_refresh_retry(struct kunit *test)
{
	struct mc16_ct *f = mc16_ct_begin(test);
	struct mc16_ct_snapshot snapshot;
	bool equal;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_ct_store(f));
	KUNIT_ASSERT_NE(test, f->token, 0UL);
	KUNIT_ASSERT_TRUE(test, memcomp_load_folio_pin(f->token, &f->guard));
	mc16_ct_publish(f);
	KUNIT_ASSERT_TRUE(test, memcomp_entry_set_refresh_pending(f->token));
	ret = mc16_ct_load(f, &equal);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_FALSE(test, equal);
	KUNIT_EXPECT_NOT_NULL(test, f->guard.slot);
	KUNIT_ASSERT_TRUE(test, mc16_ct_snapshot(&f->guard, &snapshot));
	KUNIT_EXPECT_TRUE(test, snapshot.flags & MC16_F_REFRESH_PENDING);
	KUNIT_EXPECT_EQ(test, snapshot.refs, 2U);
	memcomp_entry_clear_refresh_pending(f->token);
	ret = mc16_ct_load(f, &equal);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, equal);
}

static void memcomp_test_public_cache_charge_failure(struct kunit *test)
{
	struct mc16_ct *f = mc16_ct_begin(test);
	long charged;
	bool stored;

	KUNIT_ASSERT_NOT_NULL(test, f);
	charged = atomic_long_read(&f->base->model.charged);
	f->base->model.fail_at = atomic_read(&f->base->model.charge_calls) + 1;
	stored = mc16_ct_store(f);
	f->base->model.fail_at = 0;
	KUNIT_EXPECT_FALSE(test, stored);
	KUNIT_EXPECT_FALSE(test, f->cache_owner);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->base->model.charged), charged);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
}

static void memcomp_test_public_invalid_refs(struct kunit *test)
{
	struct mc16_ct *f = mc16_ct_begin(test);
	unsigned long compressed = ULONG_MAX;
	int mapcount = -1;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_get_ref(0, &f->guard));
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_get_ref(U32_MAX, &f->guard));
	KUNIT_EXPECT_NULL(test, f->guard.slot);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(U32_MAX));
	memcomp_invalidate_nr(U32_MAX, 2);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_stat(U32_MAX, &compressed, &mapcount));
	KUNIT_EXPECT_EQ(test, compressed, 0UL);
	KUNIT_EXPECT_EQ(test, mapcount, 0);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
}

#ifdef MC16_FAULT_MM_TEST_ENABLED
struct mc16_ct_extra {
	u32 tokens[MC16_FT_PAGES];
	unsigned int nr;
};

static void mc16_ct_extra_cleanup(void *data)
{
	struct mc16_ct_extra *extra = data;
	unsigned int i;

	for (i = 0; i < extra->nr; i++)
		memcomp_invalidate(extra->tokens[i]);
}

static void mc16_ct_madv_free(struct kunit *test, bool partial, bool shared)
{
	struct mc16_ft *f = mc16_ft_begin(test, MC16_FT_PAGES, false, true);
	struct mc16_ct_extra *extra;
	unsigned short owner_id[MC16_FT_PAGES];
	unsigned int i, first = partial ? 1 : 0;
	unsigned int last = partial ? 3 : MC16_FT_PAGES;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	extra = kunit_kzalloc(test, sizeof(*extra), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, extra);
	ret = kunit_add_action_or_reset(test, mc16_ct_extra_cleanup, extra);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < f->nr; i++) {
		pte_t pte = mc16_ft_orig(f, i);
		struct memcomp_ref guard;

		if (i == 2)
			pte = pte_swp_mksoft_dirty(pte);
		if (i == 3)
			pte = pte_swp_mkuffd_wp(pte);
		mmap_read_lock(f->mm);
		ret = mc16_ft_change(f, i, pte);
		mmap_read_unlock(f->mm);
		KUNIT_ASSERT_EQ(test, ret, 0);
		if (shared) {
			/* Same extra-PTE-owner model as the old shared oracle. */
			KUNIT_ASSERT_TRUE(test, memcomp_entry_get(f->tokens[i]));
			extra->tokens[extra->nr++] = f->tokens[i];
		}
		/* Opaque lifetime data: this case never calls stock refault. */
		KUNIT_ASSERT_TRUE(test, mc16_get(f->tokens[i], &guard));
		mutex_lock(memcomp_lock_of(guard.token));
		*mc16_shadow_ptr(&guard) = (unsigned long)xa_mk_value(i + 1);
		owner_id[i] = mc16_owner_id(&guard);
		mutex_unlock(memcomp_lock_of(guard.token));
		mc16_ref_put(&guard);
	}
	ret = do_madvise(f->mm, f->addr + first * PAGE_SIZE,
			 (last - first) * PAGE_SIZE, MADV_FREE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	mc16_ft_counts(f, partial ? 2 : 0, 0);
	for (i = 0; i < f->nr; i++) {
		bool discarded = i >= first && i < last;
		struct memcomp_ref guard;
		unsigned int refs = 0;
		unsigned short id = 0;
		u64 shadow = 0;
		bool found;

		KUNIT_EXPECT_EQ(test, pte_none(mc16_ft_read(f, i)), discarded);
		KUNIT_EXPECT_EQ(test, memcomp_entry_present(f->tokens[i]),
				shared || !discarded);
		/* Short observations must not keep the final discarded owner alive. */
		found = mc16_get(f->tokens[i], &guard);
		if (found) {
			mutex_lock(memcomp_lock_of(guard.token));
			shadow = *mc16_shadow_ptr(&guard);
			id = mc16_owner_id(&guard);
			refs = refcount_read(&guard.slot->refs);
			mutex_unlock(memcomp_lock_of(guard.token));
			mc16_ref_put(&guard);
		}
		KUNIT_EXPECT_EQ(test, found, shared || !discarded);
		if (found) {
			KUNIT_EXPECT_EQ(test, shadow,
					(u64)(unsigned long)xa_mk_value(i + 1));
			KUNIT_EXPECT_EQ(test, id, owner_id[i]);
			/* Include the just-released observer in the sampled count. */
			KUNIT_EXPECT_EQ(test, refs, 1U + shared + !discarded);
		}
	}
	/* Both extra owners and surviving PTEs are covered by fixture actions. */
}

static void memcomp_test_public_madv_free(struct kunit *test)
{
	mc16_ct_madv_free(test, false, false);
}

static void memcomp_test_public_madv_free_partial(struct kunit *test)
{
	mc16_ct_madv_free(test, true, false);
}

static void memcomp_test_public_madv_free_shared(struct kunit *test)
{
	mc16_ct_madv_free(test, false, true);
}

static void memcomp_test_public_fault(struct kunit *test)
{
	struct mc16_ft *f = mc16_ft_begin(test, 1, false, true);
	struct vm_area_struct *vma;
	pte_t *returned_pte = NULL;
	vm_fault_t ret = VM_FAULT_SIGBUS;
	bool locked = true;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mmap_read_lock(f->mm);
	vma = vma_lookup(f->mm, f->addr);
	if (vma) {
		struct vm_fault vmf = {
			.vma = vma,
			.address = f->addr,
			.real_address = f->addr,
			.flags = FAULT_FLAG_WRITE | FAULT_FLAG_ORIG_PTE_VALID,
			.pmd = mm_find_pmd(f->mm, f->addr),
			.orig_pte = mc16_ft_orig(f, 0),
			.gfp_mask = GFP_HIGHUSER_MOVABLE,
		};

		ret = memcomp_load(&vmf, &locked);
		returned_pte = vmf.pte;
	}
	mmap_read_unlock(f->mm);
	KUNIT_ASSERT_NOT_NULL(test, vma);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	KUNIT_EXPECT_FALSE(test, locked);
	KUNIT_EXPECT_PTR_EQ(test, returned_pte, NULL);
	mc16_ft_counts(f, 0, 1);
	mc16_ft_present(f, 0, 0, 1);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(f->tokens[0]));
}
#endif /* MC16_FAULT_MM_TEST_ENABLED */

#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_PUBLIC_API_TEST_H */
