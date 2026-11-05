/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ZERO_HINT_TEST_H
#define _MM_MEMCOMP_TESTS_ZERO_HINT_TEST_H

/* Include after budget_test.h and zero_convert_test.h. */
#include <asm/sections.h>
#ifdef CONFIG_MEMCG
static int mc16_zh_reserve(struct mc16_test_storage *s, unsigned int nr,
			   bool hint)
{
	struct mc16_record *record;
	int ret;

	if (s->nr + nr > MC16_TEST_REFS)
		return -ENOSPC;
	record = mc16_record_alloc(s->owner->folio, nr, false);
	if (IS_ERR(record))
		return PTR_ERR(record);
	ret = mc16_reserve_hint(record, nr, MC16_KEY_ANON, hint,
				&s->refs[s->nr]);
	if (ret)
		mc16_record_abort(record);
	else
		s->nr += nr;
	return ret;
}

static void memcomp_test_retained_x1(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);
	struct shrink_control sc = { };
	unsigned int i;
	u32 token = 0;

	KUNIT_ASSERT_NOT_NULL(test, s);
	for (i = 0; i < 32; i++) {
		KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc),
				i ? 1UL : SHRINK_EMPTY);
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(s, 1), 0);
		if (!i)
			token = s->refs[i].token;
		KUNIT_EXPECT_EQ(test, s->refs[i].token, token + i);
		KUNIT_EXPECT_EQ(test, s->refs[i].cluster->next, (u16)(i + 1));
		KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
		mc16_test_cancel_pending(&s->refs[i]);
		KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	}
	/* Each immediate refault reuses the header and the unsealed segment. */
}

static void memcomp_test_retained_x2_segment(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);
	struct mc16_cluster *cluster;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, s);
	for (i = 0; i < MC16_SEGMENT_SLOTS; i++) {
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(s, 1), 0);
		mc16_test_cancel_pending(&s->refs[i]);
	}
	cluster = xa_load(&mc16_clusters, 1);
	KUNIT_ASSERT_NOT_NULL(test, cluster);
	KUNIT_EXPECT_PTR_EQ(test, cluster->segments[0], NULL);
	KUNIT_EXPECT_PTR_EQ(test, cluster->segments[1], NULL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	/* A failed private segment allocation must leave the retained count. */
	s->probe.fail_at = s->probe.allocations + 1;
	KUNIT_EXPECT_EQ(test, mc16_test_reserve(s, 1), -ENOMEM);
	KUNIT_EXPECT_EQ(test, cluster->next, (u16)64);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	s->probe.fail_at = 0;
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(s, 1), 0);
	KUNIT_EXPECT_PTR_EQ(test, s->refs[64].cluster, cluster);
	KUNIT_EXPECT_EQ(test, s->refs[64].token, (u32)(BIT(9) + 64));
	KUNIT_EXPECT_NOT_NULL(test, cluster->segments[1]);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	mc16_test_cancel_pending(&s->refs[64]);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
}

static void memcomp_test_retained_x2_replace(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);
	unsigned int i;
	u32 old_id = 0;

	KUNIT_ASSERT_NOT_NULL(test, s);
	for (i = 0; i < MC16_CLUSTER_SLOTS - 1; i++) {
		KUNIT_ASSERT_EQ(test, mc16_test_reserve(s, 1), 0);
		if (!i)
			old_id = s->refs[0].cluster->id;
		mc16_test_cancel_pending(&s->refs[i]);
	}
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	/* ALIGN(511, 2) cannot fit: replace an empty, appendable current. */
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(s, 2), 0);
	KUNIT_EXPECT_NE(test, s->refs[511].cluster->id, old_id);
	KUNIT_EXPECT_PTR_EQ(test, xa_load(&mc16_clusters, old_id), NULL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
}

static void memcomp_test_meta_shrinker_exact(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);
	struct shrink_control sc = { .nr_to_scan = 1 };
	u32 ordinary, zero;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, mc16_zh_reserve(s, 1, false), 0);
	KUNIT_ASSERT_EQ(test, mc16_zh_reserve(s, 1, true), 0);
	ordinary = s->refs[0].cluster->id;
	zero = s->refs[1].cluster->id;
	KUNIT_ASSERT_NE(test, ordinary, zero);
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), SHRINK_EMPTY);
	mc16_test_cancel_pending(&s->refs[0]);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), 1UL);
	mc16_test_cancel_pending(&s->refs[1]);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), 2UL);
	WRITE_ONCE(mc16_meta_shrinker_cursor, ordinary);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 1UL);
	KUNIT_EXPECT_EQ(test, sc.nr_scanned, 1UL);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), 1UL);
	KUNIT_EXPECT_NOT_NULL(test, xa_load(&mc16_clusters, zero));
	WRITE_ONCE(mc16_meta_shrinker_cursor, zero);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 1UL);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), SHRINK_EMPTY);
	rcu_barrier();
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
}

static int mc16_zh_high_id(u32 *id)
{
	return __xa_alloc(&mc16_clusters, id, NULL,
			XA_LIMIT(MC16_MAX_CLUSTER_ID, MC16_MAX_CLUSTER_ID),
			GFP_NOWAIT | __GFP_NOWARN);
}

static void memcomp_test_meta_shrinker_wrap(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);
	struct shrink_control sc = { .nr_to_scan = 4096 };
	unsigned long start = MC16_MAX_CLUSTER_ID - 31;
	u32 high, private;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, mc16_zh_reserve(s, 1, false), 0);
	KUNIT_EXPECT_EQ(test, s->refs[0].cluster->id, 1U);
	kunit_activate_static_stub(test, mc16_private_id_alloc, mc16_zh_high_id);
	KUNIT_EXPECT_EQ(test, mc16_zh_reserve(s, 1, true), 0);
	kunit_deactivate_static_stub(test, mc16_private_id_alloc);
	KUNIT_ASSERT_EQ(test, s->nr, 2U);
	high = s->refs[1].cluster->id;
	KUNIT_ASSERT_EQ(test, high, (u32)MC16_MAX_CLUSTER_ID);
	/* A private zero entry must not consume a published-entry budget. */
	xa_lock_irq(&mc16_clusters);
	ret = __xa_alloc(&mc16_clusters, &private, NULL,
			 XA_LIMIT(start, start), GFP_NOWAIT | __GFP_NOWARN);
	xa_unlock_irq(&mc16_clusters);
	KUNIT_ASSERT_EQ(test, ret, 0);
	WRITE_ONCE(mc16_meta_shrinker_cursor, start);
	/* Keep both entries live: a full wrap checks each exactly once. */
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 0UL);
	KUNIT_EXPECT_EQ(test, sc.nr_scanned, 2UL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_meta_shrinker_cursor), 1UL);
	mc16_test_cancel_pending(&s->refs[0]);
	mc16_test_cancel_pending(&s->refs[1]);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 2UL);
	KUNIT_EXPECT_EQ(test, sc.nr_scanned, 2UL);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), SHRINK_EMPTY);
	sc.nr_to_scan = 0;
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 0UL);
	KUNIT_EXPECT_EQ(test, sc.nr_scanned, 0UL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_meta_shrinker_cursor), 1UL);
	xa_lock_irq(&mc16_clusters);
	__xa_erase(&mc16_clusters, private);
	xa_unlock_irq(&mc16_clusters);
	rcu_barrier();
}

static void memcomp_test_meta_shrinker_progress(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);
	struct shrink_control sc = { .nr_to_scan = 1 };
	unsigned long detached = 0;
	unsigned int calls;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, mc16_zh_reserve(s, 1, false), 0);
	/* A live entry naturally advances the cursor past all used IDs. */
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 0UL);
	KUNIT_EXPECT_EQ(test, sc.nr_scanned, 1UL);
	KUNIT_ASSERT_GT(test, READ_ONCE(mc16_meta_shrinker_cursor),
			(unsigned long)s->refs[0].cluster->id);
	mc16_test_cancel_pending(&s->refs[0]);
	sc.nr_to_scan = 64;
	for (calls = 0; calls < 2 && !detached; calls++)
		detached += mc16_meta_shrinker_scan(NULL, &sc);
	KUNIT_EXPECT_EQ(test, detached, 1UL);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), SHRINK_EMPTY);
	rcu_barrier();
}

static void memcomp_test_meta_shrinker_drained(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);
	struct shrink_control sc = { .nr_to_scan = 1 };

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, mc16_zh_reserve(s, 1, false), 0);
	mc16_test_cancel_pending(&s->refs[0]);
	KUNIT_ASSERT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), 1UL);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 1UL);
	KUNIT_EXPECT_EQ(test, sc.nr_scanned, 1UL);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_count(NULL, &sc), SHRINK_EMPTY);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), SHRINK_STOP);
	KUNIT_EXPECT_EQ(test, sc.nr_scanned, 0UL);
	rcu_barrier();
}

#endif /* CONFIG_MEMCG */

#if defined(MC16_FOLIO_TEST_ENABLED) && \
	defined(MC16_ZERO_CONVERT_TEST_ENABLED) && \
	defined(CONFIG_MEMCOMP_DEBUG_STATS)
#define MC16_ZERO_HINT_DENSITY_TEST_ENABLED

struct mc16_zh_fixture {
	bool age_between_zeros;
	struct kunit *test;
	struct mc16_test_storage *storage;
	struct memcomp_ref *live;
	void *loaded;
	struct mm_struct *mm;
	unsigned long address;
	unsigned int nr, capacity;
	unsigned int issued[2];
	struct mc16_key *key[2];
	u32 zero_token;
	u32 failed_owner_token;
	unsigned int route_mismatches;
	unsigned int key_mismatches;
	unsigned int index_mismatches;
	s64 swapents;
	struct mc16_zero_owners zeros;
};

struct mc16_zh_ledger {
	u64 count[MC16_META_CLASSES];
	u64 bytes[MC16_META_CLASSES];
	u64 attached[MC16_META_CLASSES];
	u64 attached_bytes;
	u64 retained_bytes;
	u64 record_bytes;
	u64 descriptor_tail, segment_tail, shadow_tail;
	unsigned int unpaired_segments;
};

static u64 mc16_zh_allocated(struct mc16_zh_fixture *f,
			     enum mc16_storage_class type)
{
	struct mc16_storage_debug now;

	mc16_test_metadata_snapshot(&now);
	return now.allocated[type] - f->storage->baseline.allocated[type];
}

/* One real PTE, recycled after every canonical-zero transaction. */
static int mc16_zh_clear_pte(struct mc16_zh_fixture *f)
{
	struct memcomp_ref positive = { };
	/* Protects the mapped PTE while its entry is cleared. */
	spinlock_t *ptl;
	pte_t *ptep, pte;
	pmd_t *pmd;
	int ret = 0;

	mmap_read_lock(f->mm);
	pmd = mm_find_pmd(f->mm, f->address);
	if (!pmd)
		goto out;
	ptep = pte_offset_map_lock(f->mm, pmd, f->address, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	if (is_swap_pte(pte) && is_memcomp_entry(pte_to_swp_entry(pte))) {
		unsigned long token = swp_offset(pte_to_swp_entry(pte));

		if (token && !mc16_get(token, &positive)) {
			ret = -ENOENT;
		} else {
			ptep_get_and_clear(f->mm, f->address, ptep);
			add_mm_counter(f->mm, MM_SWAPENTS, -1);
			if (positive.slot)
				mc16_ref_put_count(&positive, 2);
			else
				mc16_zero_pte_put(1);
		}
	}
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(f->mm);
	return ret;
}

/* Recover only the existing initial OWNER, never manufacture a lookup. */
static void mc16_zh_cleanup_failed_owner(struct mc16_zh_fixture *f)
{
	struct folio *folio = f->storage->owner->folio;
	struct memcomp_ref owner = { };
	struct mc16_cluster *cluster;
	struct mc16_slot *segment;
	struct mc16_view view;
	u32 token = f->failed_owner_token;
	unsigned int index = token & (MC16_CLUSTER_SLOTS - 1);
	unsigned long flags;
	bool valid = false;

	if (!token)
		return;
	xa_lock_irqsave(&mc16_clusters, flags);
	cluster = xa_load(&mc16_clusters, token >> MC16_CLUSTER_SHIFT);
	if (cluster && !xa_is_value(cluster) && !xa_is_internal(cluster) &&
	    cluster->id == token >> MC16_CLUSTER_SHIFT && index < cluster->next) {
		segment = cluster->segments[index >> MC16_SEGMENT_SHIFT];
		if (segment &&
		    refcount_read(&segment[index & (MC16_SEGMENT_SLOTS - 1)].refs) == 1) {
			owner = (struct memcomp_ref) {
				.cluster = cluster,
				.slot = &segment[index & (MC16_SEGMENT_SLOTS - 1)],
				.token = token,
			};
		}
	}
	xa_unlock_irqrestore(&mc16_clusters, flags);
	if (!owner.slot) {
		KUNIT_FAIL(f->test, "failed lookup token %u has no sole OWNER", token);
		return;
	}
	/* The outstanding initial OWNER keeps these objects alive after X. */
	mutex_lock(memcomp_lock_of(token));
	if (mc16_view_locked(&owner, &view) && view.record &&
	    mc16_flags_valid(*view.flags) &&
	    mc16_flags_state(*view.flags) == MC16_PENDING &&
	    (*view.flags & (MC16_F_OWNER | MC16_F_RESIDENT)) ==
			  (MC16_F_OWNER | MC16_F_RESIDENT) &&
	    view.member->owner_folio == folio && view.record->producer == folio &&
	    view.record->nr_pages == 1 && view.record->token_base == token &&
	    refcount_read(&owner.slot->refs) == 1)
		valid = true;
	mutex_unlock(memcomp_lock_of(token));
	if (!valid) {
		KUNIT_FAIL(f->test, "failed lookup token %u OWNER identity changed", token);
		return;
	}
	f->failed_owner_token = 0;
	/* anon_reserve already accounted this member; use the full final put. */
	mc16_ref_put(&owner);
}

static void mc16_zh_cleanup(void *data)
{
	struct mc16_zh_fixture *f = data;
	struct mc16_zero_owners after;
	unsigned int i;

	if (f->mm)
		KUNIT_EXPECT_EQ(f->test, mc16_zh_clear_pte(f), 0);
	mc16_zh_cleanup_failed_owner(f);
	for (i = 0; i < f->nr; i++) {
		if (f->live[i].slot)
			mc16_ref_put(&f->live[i]);
	}
	mc16_test_drain_segments(&f->storage->probe);
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(f->test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	if (f->mm) {
		mc16_zero_owners_read(&after);
		KUNIT_EXPECT_EQ(f->test, after.pte_pages, f->zeros.pte_pages);
		KUNIT_EXPECT_EQ(f->test, after.cache_pages, f->zeros.cache_pages);
		KUNIT_EXPECT_EQ(f->test, mc16_test_zc_rss(f->mm, MM_SWAPENTS),
				f->swapents);
		mmput(f->mm);
	}
	kvfree(f->live);
}

static struct mc16_zh_fixture *mc16_zh_begin(struct kunit *test,
					     unsigned int nr)
{
	struct mc16_zh_fixture *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->storage = mc16_test_storage_begin_pages(test, 1);
	if (!f->storage)
		return NULL;
	f->loaded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->loaded)
		return NULL;
	f->capacity = nr;
	f->live = kvcalloc(nr, sizeof(*f->live), GFP_KERNEL);
	if (!f->live)
		return NULL;
	/* Register before activation; partial setup has the same cleanup. */
	if (kunit_add_action_or_reset(test, mc16_zh_cleanup, f))
		return NULL;
	if (!xa_empty(&mc16_clusters) || !hash_empty(mc16_keys))
		return NULL;
	f->address = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (!f->address || IS_ERR_VALUE(f->address))
		return NULL;
	f->mm = current->mm;
	mmget(f->mm);
	f->swapents = mc16_test_zc_rss(f->mm, MM_SWAPENTS);
	mc16_zero_owners_read(&f->zeros);
	/* Run token cleanup before kunit_vm_mmap() removes the VMA. */
	kunit_remove_action(test, mc16_zh_cleanup, f);
	if (kunit_add_action_or_reset(test, mc16_zh_cleanup, f))
		return NULL;
	/* Retain real segment callbacks for the pre-drain ledger. */
	f->storage->probe.hold_segments = true;
	return f;
}

static bool mc16_zh_is_zero(unsigned int page)
{
	unsigned int i = page % 512;

	/* Exactly 42 zeros/block; every unsegregated 64-slot segment has both. */
	return ((i + 1) * 42 / 512) != (i * 42 / 512);
}

static int mc16_zh_convert(struct mc16_zh_fixture *f, struct memcomp_ref *ref)
{
	struct memcomp_ref pte_owner = { };
	struct mc16_zero_walk walk = { .ref = *ref };
	struct vm_area_struct *vma;
	/* Protects the PTE while the memcomp entry is installed. */
	spinlock_t *ptl;
	pte_t *ptep;
	int ret = 0;

	if (!mc16_get(ref->token, &pte_owner))
		return -ENOENT;
	mmap_read_lock(f->mm);
	ptep = get_locked_pte(f->mm, f->address, &ptl);
	if (!ptep) {
		ret = -ENOMEM;
		goto out;
	}
	__acquire(RCU);
	if (!pte_none(ptep_get(ptep))) {
		ret = -EEXIST;
	} else {
		set_pte_at(f->mm, f->address, ptep,
			   swp_entry_to_pte(make_memcomp_entry(ref->token)));
		add_mm_counter(f->mm, MM_SWAPENTS, 1);
		pte_owner = (struct memcomp_ref) { };
	}
	pte_unmap_unlock(ptep, ptl);
	if (ret)
		goto out;
	vma = vma_lookup(f->mm, f->address);
	if (vma)
		mc16_zero_convert_one(f->storage->owner->folio, vma,
				      f->address, &walk);
	if (walk.converted != 1)
		ret = -EIO;
out:
	mmap_read_unlock(f->mm);
	if (pte_owner.slot)
		mc16_ref_put(&pte_owner);
	if (!ret) {
		pte_t pte = mc16_test_zc_read(&(struct mc16_test_zc) {
			.mm = f->mm, .addr = f->address }, f->address);

		if (!is_swap_pte(pte) ||
		    pte_to_swp_entry(pte).val != make_memcomp_entry(0).val)
			ret = -EINVAL;
	}
	if (mc16_zh_clear_pte(f) && !ret)
		ret = -EIO;
	return ret;
}

/* Reserve scans the locked source; refresh uses the real codec and charge. */
static int mc16_zh_page(struct mc16_zh_fixture *f, unsigned int page,
			bool release_ordinary)
{
	struct folio *folio = f->storage->owner->folio;
	struct memcomp_ref ref = { }, drop = { };
	struct memcomp_reclaim_ctx *ctx = NULL;
	unsigned long token;
	unsigned int flags, i, route = mc16_zh_is_zero(page);
	void *src;
	int ret;

	src = kmap_local_folio(folio, 0);
	for (i = 0; i < PAGE_SIZE; i++)
		((u8 *)src)[i] = route ? 0 : i % 53;
	kunmap_local(src);
	if (mc16_folio_zero_hint(folio) != !!route)
		return -EINVAL;
	ret = mc16_anon_reserve(folio, 1, NULL, &token);
	if (ret)
		return ret;
	/* Save a lookup before any operation which may fail. */
	if (!mc16_get(token, &ref)) {
		f->failed_owner_token = token;
		return -ENOENT;
	}
	flags = memcomp_test_reclaimer_enter();
	ctx = memcomp_reclaim_ctx_get(1, MEMCOMP_RECLAIM_KSWAPD);
	if (!ctx) {
		ret = -EBUSY;
		goto out;
	}
	/* GFP_KERNEL order-0 folio: use its stable lowmem address while sleeping. */
	src = page_address(&folio->page);
	ret = mc16_anon_refresh(&ctx, src, 1, NULL, &ref);
	memcomp_reclaim_ctx_put(ctx);
out:
	memcomp_test_reclaimer_exit(flags);
	if (!ret) {
		struct mc16_key *key = ref.cluster->key;
		struct mc16_view view;
		bool valid;
		unsigned int state = MC16_EMPTY;

		mutex_lock(memcomp_lock_of(token));
		valid = mc16_view_locked(&ref, &view);
		if (valid)
			state = mc16_flags_state(*view.flags);
		mutex_unlock(memcomp_lock_of(token));
		KUNIT_EXPECT_TRUE(f->test, valid);
		KUNIT_EXPECT_EQ(f->test, state,
				route ? (unsigned int)MC16_SAMEFILL :
				(unsigned int)MC16_STORED);
		f->route_mismatches += key->zero_hint != (u8)route;
		if (f->issued[route] % MC16_CLUSTER_SLOTS)
			f->key_mismatches += key != f->key[route];
		f->key[route] = key;
		f->index_mismatches += (ref.token & (MC16_CLUSTER_SLOTS - 1)) !=
				       f->issued[route] % MC16_CLUSTER_SLOTS;
		f->issued[route]++;
		if (route) {
			f->zero_token = ref.token;
			ret = mc16_zh_convert(f, &ref);
		}
	}
	/* Drop source OWNER with the same production normalization as release. */
	if (mc16_get(token, &drop)) {
		if (!mc16_owner_drop_folio_held(&drop, folio, 1, 0) && !ret)
			ret = -EIO;
	} else if (!ret) {
		ret = -ENOENT;
	}
	if (!ret && !route && release_ordinary) {
		ret = mc16_load_page(&ref, f->loaded);
		if (!ret && memcmp(f->loaded, page_address(&folio->page), PAGE_SIZE))
			ret = -EILSEQ;
	}
	if (ret || route || release_ordinary) {
		mc16_ref_put(&ref);
	} else if (f->nr < f->capacity) {
		f->live[f->nr++] = ref;
	} else {
		mc16_ref_put(&ref);
		ret = -ENOSPC;
	}
	return ret;
}

/* No constants stand in for slab/XA/infrastructure measurements. */
static void mc16_zh_ledger(struct mc16_zh_fixture *f, const char *phase,
			   struct mc16_zh_ledger *l)
{
	struct mc16_storage_debug now;
	struct mc16_storage_debug *base = &f->storage->baseline;
	struct mc16_cluster *cluster;
	struct mc16_key *key;
	unsigned long id, flags;
	unsigned int type, seg, bucket, i;
	u64 key_bytes = 0;

	memset(l, 0, sizeof(*l));
	mc16_test_metadata_snapshot(&now);
	for (type = 0; type < MC16_META_CLASSES; type++) {
		l->count[type] = now.allocated[type] - base->allocated[type] -
				(now.freed[type] - base->freed[type]);
		l->bytes[type] = now.allocated_bytes[type] - base->allocated_bytes[type] -
				(now.freed_bytes[type] - base->freed_bytes[type]);
	}
	/* Attached pointers only survive X while their real sizes are sampled. */
	xa_lock_irqsave(&mc16_clusters, flags);
	xa_for_each(&mc16_clusters, id, cluster) {
		if (xa_is_value(cluster) || xa_is_internal(cluster))
			continue;
		l->attached[MC16_META_CLUSTER]++;
		l->attached_bytes += ksize(cluster);
		if (!cluster->live_slots && cluster->key->current_cluster == cluster)
			l->retained_bytes += ksize(cluster);
		for (seg = 0; seg < MC16_SEGMENTS; seg++) {
			u64 bytes = 0;

			l->unpaired_segments += !!cluster->segments[seg] !=
						!!cluster->shadows[seg];
			if (cluster->segments[seg]) {
				l->attached[MC16_META_SEGMENT]++;
				bytes += ksize(cluster->segments[seg]);
			}
			if (cluster->shadows[seg]) {
				l->attached[MC16_META_SHADOW]++;
				bytes += ksize(cluster->shadows[seg]);
			}
			l->attached_bytes += bytes;
			if (!cluster->live_slots &&
			    cluster->key->current_cluster == cluster)
				l->retained_bytes += bytes;
		}
	}
	hash_for_each(mc16_keys, bucket, key, node) {
		l->attached[MC16_META_KEY]++;
		key_bytes += ksize(key);
	}
	xa_unlock_irqrestore(&mc16_clusters, flags);
	KUNIT_EXPECT_EQ(f->test, l->unpaired_segments, 0U);
	/* Order-0 live slots have disjoint records, if normalization failed. */
	for (i = 0; i < f->nr; i++) {
		struct mc16_view view;

		if (!f->live[i].slot)
			continue;
		mutex_lock(memcomp_lock_of(f->live[i].token));
		if (mc16_view_locked(&f->live[i], &view) && view.record)
			l->record_bytes += ksize(view.record);
		mutex_unlock(memcomp_lock_of(f->live[i].token));
	}
	l->descriptor_tail = l->count[MC16_META_DESCRIPTOR];
	l->segment_tail = l->count[MC16_META_SEGMENT] -
			  l->attached[MC16_META_SEGMENT];
	l->shadow_tail = l->count[MC16_META_SHADOW] -
			 l->attached[MC16_META_SHADOW];
	kunit_info(f->test,
		   "%s allocated-live segment/header/key/descriptor/shadow=%llu/%llu/%llu/%llu/%llu bytes=%llu/%llu/%llu/%llu/%llu\n",
		   phase, l->count[0], l->count[1], l->count[2], l->count[3],
		   l->count[MC16_META_SHADOW],
		   l->bytes[0], l->bytes[1], l->bytes[2], l->bytes[3],
		   l->bytes[MC16_META_SHADOW]);
	kunit_info(f->test,
		   "%s attached segment/header/key/shadow=%llu/%llu/%llu/%llu attached_bytes=%llu key_bytes=%llu retained_bytes=%llu\n",
		   phase, l->attached[0], l->attached[1], l->attached[2],
		   l->attached[MC16_META_SHADOW],
		   l->attached_bytes, key_bytes, l->retained_bytes);
}

static void mc16_zh_drain(struct mc16_zh_fixture *f)
{
	mc16_test_drain_segments(&f->storage->probe);
	rcu_barrier();
}

static void mc16_zh_expect_drained(struct mc16_zh_fixture *f,
				   struct mc16_zh_ledger *l)
{
	unsigned int i;

	KUNIT_EXPECT_EQ(f->test, l->record_bytes, 0ULL);
	KUNIT_EXPECT_EQ(f->test, l->descriptor_tail, 0ULL);
	KUNIT_EXPECT_EQ(f->test, l->segment_tail, 0ULL);
	KUNIT_EXPECT_EQ(f->test, l->shadow_tail, 0ULL);
	KUNIT_EXPECT_EQ(f->test, l->bytes[MC16_META_SEGMENT] +
			l->bytes[MC16_META_CLUSTER] + l->bytes[MC16_META_SHADOW],
			l->attached_bytes);
	kunit_info(f->test,
		   "post-drain private residual segment/header/key/shadow=%lld/%lld/%lld/%lld bytes=%lld\n",
		   (s64)l->count[0] - (s64)l->attached[0],
		   (s64)l->count[1] - (s64)l->attached[1],
		   (s64)l->count[2] - (s64)l->attached[2],
		   (s64)l->count[MC16_META_SHADOW] -
			(s64)l->attached[MC16_META_SHADOW],
		   (s64)(l->bytes[0] + l->bytes[1] + l->bytes[MC16_META_SHADOW]) -
			(s64)l->attached_bytes);
	/* No unexplained population is silently classified as freed/private. */
	for (i = 0; i < MC16_META_CLASSES; i++)
		KUNIT_EXPECT_EQ(f->test, l->count[i], l->attached[i]);
}

/* Keep per-page checks, but emit bounded diagnostics for routing mutants. */
static void mc16_zh_expect_routes(struct mc16_zh_fixture *f)
{
	KUNIT_EXPECT_EQ(f->test, f->route_mismatches, 0U);
	KUNIT_EXPECT_EQ(f->test, f->key_mismatches, 0U);
	KUNIT_EXPECT_EQ(f->test, f->index_mismatches, 0U);
}

static void mc16_zh_fill(struct kunit *test, struct mc16_zh_fixture *f,
			 unsigned int pages, bool serial)
{
	unsigned int i;

	for (i = 0; i < pages; i++) {
		int ret = mc16_zh_page(f, i, serial);

		if (ret) {
			KUNIT_FAIL(test, "page %u/%u failed: %d", i, pages, ret);
			return;
		}
		if (mc16_zh_is_zero(i)) {
			struct memcomp_ref probe = { };
			bool got = mc16_get(f->zero_token, &probe);

			if (got)
				mc16_ref_put(&probe);
			KUNIT_EXPECT_FALSE(test, got);
			if (f->age_between_zeros)
				mc16_retained_age_work(&mc16_retained_aging.work);
		}
		cond_resched();
	}
	mc16_zh_expect_routes(f);
}

static void memcomp_test_zero_hint_ordinary_refault(struct kunit *test)
{
	struct mc16_zh_fixture *f = mc16_zh_begin(test, 128);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	/* Page 0 is ordinary; store, drop OWNER, decode, and final-put each time. */
	for (i = 0; i < 128; i++)
		KUNIT_ASSERT_EQ(test, mc16_zh_page(f, 0, true), 0);
	mc16_zh_expect_routes(f);
	KUNIT_EXPECT_EQ(test, f->issued[0], 128U);
	KUNIT_EXPECT_EQ(test, f->issued[1], 0U);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_CLUSTER), 1ULL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_SEGMENT), 2ULL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_SHADOW), 2ULL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_KEY), 1ULL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	KUNIT_EXPECT_EQ(test, f->key[0]->current_cluster->next, (u16)128);
}

static void memcomp_test_zero_hint_serial533(struct kunit *test)
{
	struct mc16_zh_fixture *f = mc16_zh_begin(test, 512);
	struct mc16_zh_ledger l;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->age_between_zeros = true;
	mc16_zh_fill(test, f, 512, true);
	KUNIT_ASSERT_EQ(test, f->issued[0], 470U);
	KUNIT_ASSERT_EQ(test, f->issued[1], 42U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 2UL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_SEGMENT), 9ULL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_SHADOW), 9ULL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_CLUSTER), 2ULL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_KEY), 2ULL);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_DESCRIPTOR), 7ULL);
	mc16_zh_ledger(f, "pre-drain", &l);
	mc16_zh_drain(f);
	mc16_zh_ledger(f, "post-drain", &l);
	mc16_zh_expect_drained(f, &l);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_CLUSTER], 2ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_SEGMENT], 2ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_SHADOW], 2ULL);
	/* A separate pressure checkpoint after the serial lifecycle checks. */
	mc16_test_reclaim_retained();
	mc16_zh_ledger(f, "pressure-cleanup", &l);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_CLUSTER], 0ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_KEY], 0ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_SHADOW], 0ULL);
}

static void memcomp_test_zero_hint_route512(struct kunit *test)
{
	struct mc16_zh_fixture *f = mc16_zh_begin(test, 512);
	struct mc16_zh_ledger l;
	struct shrink_control sc = { .nr_to_scan = 1 };
	struct mc16_cluster *zero;
	unsigned int i, ordinary_holes = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_zh_fill(test, f, 512, false);
	KUNIT_ASSERT_EQ(test, f->issued[0], 470U);
	KUNIT_ASSERT_EQ(test, f->issued[1], 42U);
	KUNIT_ASSERT_EQ(test, f->nr, 470U);
	KUNIT_EXPECT_PTR_NE(test, f->key[0], f->key[1]);
	KUNIT_EXPECT_EQ(test, f->key[0]->current_cluster->next, (u16)470);
	zero = f->key[1]->current_cluster;
	KUNIT_ASSERT_NOT_NULL(test, zero);
	KUNIT_EXPECT_EQ(test, zero->next, (u16)42);
	KUNIT_EXPECT_EQ(test, zero->live_slots, 0U);
	/* A dense ordinary token sequence proves there are no zero holes. */
	for (i = 0; i < f->nr; i++)
		ordinary_holes += f->live[i].token != f->live[0].token + i;
	KUNIT_EXPECT_EQ(test, ordinary_holes, 0U);
	mc16_zh_ledger(f, "pre-drain", &l);
	WRITE_ONCE(mc16_meta_shrinker_cursor, zero->id);
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 1UL);
	mc16_zh_drain(f);
	mc16_zh_ledger(f, "post-drain", &l);
	mc16_zh_expect_drained(f, &l);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_SEGMENT], 8ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_SHADOW], 8ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_CLUSTER], 1ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_KEY], 1ULL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	for (i = 0; i < f->nr; i++)
		KUNIT_EXPECT_EQ(test, refcount_read(&f->live[i].slot->refs), 1U);
	/* Routing gate only; 470 payload pages cannot meet 16.4 B/page. */
}

static void memcomp_test_zero_hint_density65536(struct kunit *test)
{
	struct mc16_zh_fixture *f = mc16_zh_begin(test, 65536);
	struct mc16_zh_ledger l;
	struct mc16_cluster *zero;
	unsigned int seg;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_zh_fill(test, f, 65536, false);
	KUNIT_ASSERT_EQ(test, f->issued[0], 60160U);
	KUNIT_ASSERT_EQ(test, f->issued[1], 5376U);
	KUNIT_ASSERT_EQ(test, f->nr, 60160U);
	KUNIT_EXPECT_EQ(test, mc16_zh_allocated(f, MC16_META_CLUSTER), 129ULL);
	KUNIT_ASSERT_NOT_NULL(test, memcomp_meta_shrinker);
	zero = f->key[1]->current_cluster;
	KUNIT_ASSERT_NOT_NULL(test, zero);
	KUNIT_EXPECT_EQ(test, zero->next, (u16)256);
	KUNIT_EXPECT_EQ(test, zero->live_slots, 0U);
	/* 5376 is a multiple of 64: no unsealed zero tail is required. */
	for (seg = 0; seg < MC16_SEGMENTS; seg++) {
		KUNIT_EXPECT_PTR_EQ(test, zero->segments[seg], NULL);
		KUNIT_EXPECT_PTR_EQ(test, zero->shadows[seg], NULL);
	}
	mc16_zh_ledger(f, "pre-drain", &l);
	/* Callback drain only. Do not pressure-evict the final zero header. */
	mc16_zh_drain(f);
	mc16_zh_ledger(f, "post-drain", &l);
	mc16_zh_expect_drained(f, &l);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_SEGMENT], 940ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_SHADOW], 940ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_CLUSTER], 119ULL);
	KUNIT_EXPECT_EQ(test, l.attached[MC16_META_KEY], 2ULL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	KUNIT_EXPECT_EQ(test, l.retained_bytes, (u64)ksize(zero));
	/* Population coverage only: XA-inclusive density is not measured here. */
}

#endif /* density fixtures */
#endif /* _MM_MEMCOMP_TESTS_ZERO_HINT_TEST_H */
