/* SPDX-License-Identifier: GPL-2.0-only */
/* Included once inside workingset_load_test.h's existing swap/scope guard. */
struct mc16_tr {
	struct kunit *test;
	struct folio *folio[2]; /* Allocation refs only; [1] is the keeper. */
	struct swap_info_struct *si[2];
	swp_entry_t swap[2];
	struct xa_node *leaf; /* Compare only; dereference current cursors under XA. */
	bool reserved[2], held[2], handed;
	bool shadow, empty;
};

static void mc16_tr_cleanup(void *data)
{
	struct mc16_tr *f = data;
	unsigned char map[2] = { };
	bool checked[2] = { }, marked[2] = { };
	unsigned int i;

	if (!f->swap[0].val && !f->swap[1].val)
		return;
	lru_add_drain_all();
	disable_swap_slots_cache_lock();
	/* Target first: keep the keeper and its slot alive through the oracle. */
	for (i = 0; i < 2; i++) {
		struct folio *folio = f->folio[i];
		swp_entry_t entry = f->swap[i];

		f->folio[i] = NULL;
		/* Reclaim consumed the old isolation ref, even on putback. */
		if (!folio && !i && f->handed)
			folio = swap_cache_get_folio(entry, NULL, 0);
		if (folio) {
			folio_lock(folio);
			folio_wait_writeback(folio);
			if (folio_test_swapcache(folio))
				delete_from_swap_cache(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
		if (f->reserved[i])
			swapcache_free_entries(&entry, 1);
		if (f->held[i])
			swap_free(f->swap[i]);
		if (f->si[i]) {
			map[i] = READ_ONCE(f->si[i]->swap_map[swp_offset(f->swap[i])]);
			marked[i] = swap_memcomp_transport_test(f->si[i],
								swp_offset(f->swap[i]));
			checked[i] = true;
			put_swap_device(f->si[i]);
			f->si[i] = NULL;
		}
		f->reserved[i] = false;
		f->held[i] = false;
		f->swap[i].val = 0;
	}
	f->handed = false;
	reenable_swap_slots_cache_unlock();
	for (i = 0; i < 2; i++) {
		if (!checked[i])
			continue;
		kunit_info(f->test, "transport reclaim cleanup: index=%u map=%u marked=%d",
			   i, map[i], marked[i]);
		KUNIT_EXPECT_EQ(f->test, map[i], (unsigned char)0);
		KUNIT_EXPECT_FALSE(f->test, marked[i]);
	}
}

static int mc16_tr_alloc(struct mc16_tr *f, bool transport)
{
	unsigned int i;
	int nr, ret = 0;

	if (!READ_ONCE(mc16_kunit_real_swap))
		return -EINVAL;
	if (transport) {
		mutex_lock(&memcomp_writeback_lock);
		if (memcomp_writeback_slots_free) {
			mutex_unlock(&memcomp_writeback_lock);
			return -EBUSY;
		}
		for (i = 0; i < 2; i++) {
			ret = memcomp_swap_folio_alloc(&f->folio[i], &f->swap[i]);
			if (ret)
				break;
			f->held[i] = true;
		}
		memcomp_writeback_slots_return();
		mutex_unlock(&memcomp_writeback_lock);
		for (i = 0; i < 2; i++)
			if (f->folio[i])
				folio_unlock(f->folio[i]);
		if (ret)
			return ret;
	} else {
		nr = get_swap_pages(2, f->swap, 0);
		for (i = 0; i < nr; i++)
			f->reserved[i] = true;
		if (nr != 2)
			return -ENOSPC;
	}
	for (i = 0; i < 2; i++) {
		f->si[i] = get_swap_device(f->swap[i]);
		if (!f->si[i])
			return -ENODEV;
		if (transport)
			continue;
		f->folio[i] = folio_alloc(GFP_HIGHUSER_MOVABLE, 0);
		if (!f->folio[i])
			return -ENOMEM;
		folio_lock(f->folio[i]);
		__folio_set_swapbacked(f->folio[i]);
		ret = add_to_swap_cache(f->folio[i], f->swap[i], GFP_KERNEL, NULL);
		if (!ret) {
			f->reserved[i] = false;
			ret = swap_duplicate(f->swap[i]);
			if (!ret)
				f->held[i] = true;
		}
		folio_unlock(f->folio[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int mc16_tr_prepare(struct mc16_tr *f, const void *src, unsigned int i)
{
	struct folio *folio = f->folio[i];
	struct writeback_control wbc = { .sync_mode = WB_SYNC_ALL };
	void *dst;
	int ret;

	folio_lock(folio);
	/* Balance the initial uncharged cache stats before charging. */
	delete_from_swap_cache(folio); /* The held slot ref stays live. */
	ret = mem_cgroup_charge(folio, &init_mm, GFP_KERNEL);
	if (ret)
		goto unlock;
	if (!mem_cgroup_disabled() && folio_memcg(folio) != root_mem_cgroup) {
		ret = -EINVAL;
		goto unlock;
	}
	ret = mem_cgroup_try_charge_swap(folio, f->swap[i]);
	if (ret)
		goto unlock;
	ret = swapcache_prepare(f->swap[i], 1);
	if (ret)
		goto unlock;
	ret = add_to_swap_cache(folio, f->swap[i], GFP_KERNEL, NULL);
	if (ret) {
		put_swap_folio(folio, f->swap[i]);
		goto unlock;
	}
	dst = kmap_local_folio(folio, 0);
	memcpy(dst, src, PAGE_SIZE);
	kunmap_local(dst);
	__folio_mark_uptodate(folio);
	if (i)
		goto unlock;
	ret = swap_writepage(&folio->page, &wbc);
	if (ret) {
		if (folio_test_locked(folio))
			folio_unlock(folio);
		return ret;
	}
	folio_lock(folio);
	folio_wait_writeback(folio);
	if (folio_test_dirty(folio) || !folio_test_swapcache(folio) ||
	    !folio_test_uptodate(folio))
		ret = -EIO;
unlock:
	folio_unlock(folio);
	return ret;
}

/* Actual cursors, not just adjacent offsets; never write XA internals. */
static bool mc16_tr_leaf(struct mc16_tr *f, bool after)
{
	struct address_space *mapping = swap_address_space(f->swap[0]);
	XA_STATE(target, &mapping->i_pages, swap_cache_index(f->swap[0]));
	XA_STATE(keeper, &mapping->i_pages, swap_cache_index(f->swap[1]));
	void *entry, *kept;
	bool same;

	if (mapping != swap_address_space(f->swap[1]))
		return false;
	xa_lock_irq(&mapping->i_pages);
	entry = xas_load(&target);
	kept = xas_load(&keeper);
	same = xas_is_node(&target) && xas_is_node(&keeper) &&
	       target.xa_node == keeper.xa_node && !target.xa_node->shift &&
	       kept == f->folio[1] &&
	       target.xa_node->count > target.xa_node->nr_values &&
	       list_empty(&target.xa_node->private_list) && !mapping_exiting(mapping);
	if (after) {
		same = same && target.xa_node == f->leaf;
		f->shadow = xa_is_value(entry);
		f->empty = !entry;
	} else {
		same = same && entry == f->folio[0];
		if (same)
			f->leaf = target.xa_node;
	}
	xa_unlock_irq(&mapping->i_pages);
	return same;
}

static void mc16_tr_one(struct kunit *test, const void *src, bool transport)
{
	struct mc16_tr *f;
	struct folio *folio;
	unsigned long reclaimed = 0;
	unsigned int i;
	unsigned char target_map = 0;
	int ret, refs = 0;
	bool before = false, after = false, marked;
	LIST_HEAD(list);

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->test = test;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mc16_tr_cleanup, f), 0);
	ret = mc16_tr_alloc(f, transport);
	if (ret)
		goto out;
	ret = mc16_tr_prepare(f, src, 0);
	if (!ret)
		ret = mc16_tr_prepare(f, src, 1);
	if (ret)
		goto out;
	for (i = 0; i < 2; i++) {
		if (READ_ONCE(f->si[i]->swap_map[swp_offset(f->swap[i])]) !=
		    (SWAP_HAS_CACHE | 1)) {
			ret = -EINVAL;
			goto out;
		}
	}
	marked = swap_memcomp_transport_test(f->si[0], swp_offset(f->swap[0]));
	if (marked != transport) {
		ret = -EINVAL;
		goto out;
	}
	before = mc16_tr_leaf(f, false);
	kunit_info(test,
		   "transport reclaim pair: transport=%d target=%#lx keeper=%#lx leaf=%p same=%d",
		   transport, f->swap[0].val, f->swap[1].val, f->leaf, before);
	kunit_info(test, "transport reclaim indexes: target=%lu keeper=%lu marked=%d",
		   swap_cache_index(f->swap[0]), swap_cache_index(f->swap[1]), marked);
	if (!before) {
		ret = -EXDEV;
		goto out;
	}
	folio = f->folio[0];
	folio_add_lru(folio);
	lru_add_drain_all();
	if (!folio_isolate_lru(folio)) {
		ret = -EBUSY;
		goto out;
	}
	/* Allocation + cache + isolation. No extra target pin may survive. */
	refs = folio_ref_count(folio);
	if (refs != 3) {
		folio_putback_lru(folio);
		ret = -EBUSY;
		goto out;
	}
	f->folio[0] = NULL;
	f->handed = true;
	list_add(&folio->lru, &list);
	folio_put(folio); /* Drop only allocation; list owns isolation. */
	folio = NULL;
	reclaimed = reclaim_pages(&list);
	/* Neither success nor putback leaves a fixture-owned target pointer. */
	after = mc16_tr_leaf(f, true);
	target_map = READ_ONCE(f->si[0]->swap_map[swp_offset(f->swap[0])]);
	if (reclaimed != 1 || !after || target_map != 1) {
		ret = -ESTALE;
		goto out;
	}
	kunit_info(test, "transport reclaim transfer: refs=%d leaf=%d map=%u",
		   refs, after, target_map);
	kunit_info(test,
		   "transport reclaim oracle: transport=%d reclaimed=%lu shadow=%d empty=%d",
		   transport, reclaimed, f->shadow, f->empty);
	if (transport) {
		KUNIT_EXPECT_FALSE(test, f->shadow);
		KUNIT_EXPECT_TRUE(test, f->empty);
	} else {
		KUNIT_EXPECT_TRUE(test, f->shadow);
		KUNIT_EXPECT_FALSE(test, f->empty);
	}
out:
	kunit_info(test, "transport reclaim result: transport=%d ret=%d reclaimed=%lu",
		   transport, ret, reclaimed);
	KUNIT_EXPECT_EQ(test, ret, 0);
	kunit_release_action(test, mc16_tr_cleanup, f);
}
