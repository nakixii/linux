/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ACCESS_TEST_H
#define _MM_MEMCOMP_TESTS_ACCESS_TEST_H
#ifdef CONFIG_MEMCG
static void memcomp_test_pending_final_guard(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_view view;
	struct memcomp_ref guard;
	spinlock_t ptl; /* Models the caller holding a PTE lock. */
	unsigned long irqflags;
	long record_charge, before, charged_after;
	int puts_after, folio_refs_after;
	bool last;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[0].token, &guard));
	refcount_inc(&guard.slot->refs); /* persistent PTE ownership */
	mutex_lock(memcomp_lock_of(guard.token));
	mc16_view_locked(&guard, &view);
	record_charge = view.record->charge_bytes;
	*view.flags &= ~MC16_F_OWNER;
	mutex_unlock(memcomp_lock_of(guard.token));
	/* Owner + PTE are gone; only the guard still owns this member. */
	KUNIT_ASSERT_FALSE(test, refcount_sub_and_test(2, &guard.slot->refs));
	f->refs[0] = (struct memcomp_ref) { };
	before = atomic_long_read(&f->owner->model.charged);
	spin_lock_init(&ptl);
	rcu_read_lock();
	spin_lock_irqsave(&ptl, irqflags);
	last = refcount_dec_and_test(&guard.slot->refs);
	if (last) {
		mc16_release_resources(&guard);
		mc16_retire_slot(&guard);
	}
	spin_unlock_irqrestore(&ptl, irqflags);
	/* Capture before grace; report only after leaving atomic/RCU scope. */
	charged_after = atomic_long_read(&f->owner->model.charged);
	puts_after = atomic_read(&f->owner->model.put_calls);
	folio_refs_after = folio_ref_count(f->owner->folio);
	rcu_read_unlock();
	KUNIT_EXPECT_EQ(test, charged_after, before - record_charge);
	KUNIT_EXPECT_EQ(test, puts_after, 1);
	KUNIT_EXPECT_EQ(test, folio_refs_after, 1);
	KUNIT_EXPECT_TRUE(test, last);
	rcu_barrier();
	KUNIT_EXPECT_FALSE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_FALSE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_GT(test, atomic_long_read(&f->owner->model.charged), 0L);
	/* Exact metadata balance is checked after explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
}

static void memcomp_test_samefill_normalize(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_record *tail[2];
	struct mc16_view view;
	const u64 pattern = 0xfedcba9876543210ULL;
	unsigned int i;
	u16 flags;
	bool matches;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 2), 0);
	for (i = 0; i < 2; i++) {
		mutex_lock(memcomp_lock_of(f->refs[i].token));
		mc16_view_locked(&f->refs[i], &view);
		matches = mc16_owner_matches_locked(&f->refs[i], f->owner->folio, 2, i);
		KUNIT_EXPECT_TRUE(test, matches);
		matches = mc16_owner_matches_locked(&f->refs[i], NULL, 2, i);
		KUNIT_EXPECT_FALSE(test, matches);
		matches = mc16_owner_matches_locked(&f->refs[i], f->owner->folio, 1, i);
		KUNIT_EXPECT_FALSE(test, matches);
		folio_put(view.member->owner_folio);
		*view.word = pattern;
		*view.length = 0;
		*view.flags = MC16_SAMEFILL | (1 << MC16_ORDER_SHIFT);
		tail[i] = mc16_normalize_locked(&f->refs[i]);
		mutex_unlock(memcomp_lock_of(f->refs[i].token));
		KUNIT_ASSERT_NOT_NULL(test, tail[i]);
		KUNIT_EXPECT_EQ(test, f->refs[i].slot->word, pattern);
		/* Pair with normalize release to read the complete fill word. */
		flags = smp_load_acquire(&f->refs[i].slot->flags);
		KUNIT_EXPECT_EQ(test, mc16_flags_state(flags),
				(unsigned int)MC16_SAMEFILL);
	}
	KUNIT_EXPECT_PTR_EQ(test, tail[0], tail[1]);
	mc16_record_member_put(tail[1]);
	KUNIT_EXPECT_EQ(test, refcount_read(&tail[0]->members), 1U);
	mc16_record_member_put(tail[0]); /* producer/head detaches last */
	KUNIT_EXPECT_EQ(test, atomic_read(&f->owner->model.put_calls), 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->owner->folio), 1);
	for (i = 0; i < 2; i++)
		mc16_test_cancel_pending(&f->refs[i]);
	/* Exact metadata balance is checked after explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
}

static void memcomp_test_payload_final_release(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct mc16_view view;
	struct mc16_record *tail;
	unsigned long handle;
	spinlock_t ptl; /* Models the caller holding a PTE lock. */
	unsigned long irqflags;
	int ret;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	ret = memcomp_zs_malloc(memcomp_pools[0], PAGE_SIZE, &handle, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = memcomp_objcg_charge(&f->owner->model.objcg, GFP_KERNEL, PAGE_SIZE);
	if (ret) {
		memcomp_zs_free(0, handle);
		KUNIT_FAIL(test, "payload charge failed: %d", ret);
		return;
	}
	mutex_lock(memcomp_lock_of(f->refs[0].token));
	mc16_view_locked(&f->refs[0], &view);
	folio_put(view.member->owner_folio);
	*view.word = handle;
	*view.length = PAGE_SIZE - 1;
	*view.flags = MC16_STORED;
	view.member->payload_charge = PAGE_SIZE;
	tail = mc16_normalize_locked(&f->refs[0]);
	mutex_unlock(memcomp_lock_of(f->refs[0].token));
	mc16_record_member_put(tail);
	f->payload.task = current;
	f->payload.pool = memcomp_pools[0];
	f->payload.handle = handle;
	f->payload.live = true;
	rcu_assign_pointer(memcomp_kunit_async_probe, &f->payload);
	spin_lock_init(&ptl);
	spin_lock_irqsave(&ptl, irqflags);
	mc16_test_cancel_pending(&f->refs[0]);
	spin_unlock_irqrestore(&ptl, irqflags);
	KUNIT_EXPECT_EQ(test, f->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, f->payload.live);
	KUNIT_EXPECT_FALSE(test, f->payload.bad_lifetime);
	/* Exact metadata balance is checked after explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
}

static void memcomp_test_record_outlives_key(struct kunit *test)
{
	struct mc16_test_storage *f;
	struct memcomp_test_owner *tail;
	struct mc16_view view;
	u64 charge;
	int ret;

	f = mc16_test_storage_begin(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	tail = kunit_kzalloc(test, sizeof(*tail), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tail);
	ret = kunit_add_action_or_reset(test, memcomp_test_cleanup, tail);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(f, 1), 0);
	mutex_lock(memcomp_lock_of(f->refs[0].token));
	mc16_view_locked(&f->refs[0], &view);
	charge = view.record->charge_bytes;
	folio_put(view.member->owner_folio);
	*view.word = 0xfefefefefefefefeULL;
	*view.length = 0;
	*view.flags = MC16_SAMEFILL;
	tail->record = mc16_normalize_locked(&f->refs[0]);
	tail->remaining = 1;
	mutex_unlock(memcomp_lock_of(f->refs[0].token));
	mc16_test_cancel_pending(&f->refs[0]);
	KUNIT_EXPECT_FALSE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_GT(test, atomic_long_read(&f->owner->model.charged),
			(long)charge);
	/* The independent record survives explicit header/key reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged),
			(long)charge);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->owner->model.put_calls), 1);
	tail->remaining = 0;
	memcomp_test_record_put(tail->record);
	tail->record = NULL;
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->owner->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->owner->model.put_calls), 2);
}

#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_ACCESS_TEST_H */
