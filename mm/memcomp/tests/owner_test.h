/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_OWNER_TEST_H
#define _MM_MEMCOMP_TESTS_OWNER_TEST_H
#ifdef CONFIG_MEMCG
/* Actions run before refs/storage cleanup, including after a fatal assert. */
struct mc16_test_owner {
	struct mc16_test_refs *refs;
	struct memcomp_ref lookup;
	struct memcomp_ref survivor[2];
	struct mc16_owner_tail tail[2];
};

static void mc16_test_owner_cleanup(void *data)
{
	struct mc16_test_owner *f = data;
	unsigned int i;

	if (f->lookup.slot)
		mc16_ref_put(&f->lookup);
	for (i = 0; i < ARRAY_SIZE(f->survivor); i++) {
		if (f->survivor[i].slot)
			mc16_ref_put(&f->survivor[i]);
	}
	/* Siblings first, producer/head last, also on assertion failure. */
	for (i = ARRAY_SIZE(f->tail); i; i--)
		mc16_owner_tail_put(&f->tail[i - 1]);
}

static struct mc16_test_owner *
mc16_test_owner_begin(struct kunit *test, unsigned int nr)
{
	struct mc16_test_owner *f;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs)
		return NULL;
	ret = kunit_add_action_or_reset(test, mc16_test_owner_cleanup, f);
	if (ret || mc16_test_refs_reserve(f->refs, nr))
		return NULL;
	return f;
}

static void mc16_test_owner_normalize(struct kunit *test, unsigned int state)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 2);
	struct mc16_test_storage *storage;
	struct mc16_record *record;
	struct memcomp_ref *ref;
	struct mc16_view view;
	unsigned int i;
	long record_charge;
	u64 word;
	u16 length, flags;
	u32 base;
	unsigned int index;
	bool dropped;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	struct memcomp_state_stats before, after;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	for (i = 0; i < 2; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f->refs, i, state), 0);
	record = (void *)(unsigned long)storage->refs[0].slot->word;
	record_charge = record->charge_bytes;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	memcomp_state_read(&before);
#endif
	/* Exercise producer/head-last, using real lookups for surviving holds. */
	for (i = 2; i; ) {
		i--;
		ref = &storage->refs[i];
		KUNIT_ASSERT_TRUE(test, mc16_get(ref->token, &f->survivor[i]));
		KUNIT_ASSERT_TRUE(test, mc16_get(ref->token, &f->lookup));
		mutex_lock(memcomp_lock_of(ref->token));
		mc16_view_locked(ref, &view);
		word = *view.word;
		length = *view.length;
		flags = *view.flags & ~MC16_F_OWNER;
		dropped = mc16_owner_drop_locked(&f->lookup, storage->owner->folio,
						 2, i, &f->tail[i]);
		mutex_unlock(memcomp_lock_of(ref->token));
		/* Complete consumption before any fatal assertion can unwind. */
		mc16_ref_put_count(&f->lookup, dropped ? 2 : 1);
		if (dropped)
			*ref = (struct memcomp_ref) { };
		KUNIT_ASSERT_TRUE(test, dropped);
		ref = &f->survivor[i];
		KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, NULL);
		KUNIT_ASSERT_PTR_EQ(test, f->tail[i].record, record);
		KUNIT_EXPECT_FALSE(test, f->tail[i].resident);
		KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 1U);
		KUNIT_EXPECT_EQ(test, ref->slot->word, word);
		KUNIT_EXPECT_EQ(test, ref->slot->length, length);
		/* Pair with normalize's release publication of word/length. */
		KUNIT_EXPECT_EQ(test, smp_load_acquire(&ref->slot->flags), flags);
		KUNIT_EXPECT_EQ(test, mc16_ref_geometry(ref, &base, &index), 2U);
		KUNIT_EXPECT_EQ(test, base, record->token_base);
		KUNIT_EXPECT_EQ(test, index, i);
		KUNIT_EXPECT_EQ(test, record->member[i].payload_charge, 0U);
		KUNIT_EXPECT_PTR_EQ(test, record->member[i].owner_folio, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&record->members), 2U);
	}
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	memcomp_state_read(&after);
	KUNIT_EXPECT_EQ(test, after.folio_owner_pages, before.folio_owner_pages - 2);
#endif
	/* Independent tails survive slot release and explicit key reclaim. */
	mc16_ref_put(&f->survivor[1]);
	mc16_ref_put(&f->survivor[0]);
	/* Exact metadata balance is checked after explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged),
			record_charge);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compressed_bytes),
			f->refs->compressed_before);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), 1);
	mc16_owner_tail_put(&f->tail[1]);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->tail[0].record->members), 1U);
	mc16_owner_tail_put(&f->tail[0]);
	/* No record access after the final put, including cleanup retries. */
	mc16_owner_tail_put(&f->tail[0]);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged), 0L);
}

static void memcomp_test_owner_stored_normalize(struct kunit *test)
{
	mc16_test_owner_normalize(test, MC16_STORED);
}

static void memcomp_test_owner_samefill_normalize(struct kunit *test)
{
	mc16_test_owner_normalize(test, MC16_SAMEFILL);
}

/* A writer's batch guard survives OWNER and the detached record metadata. */
static void memcomp_test_owner_writeback_guard(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	struct mc16_view view;
	struct memcomp_lru *fifo;
	long fifo_before, charged, record_charge;
	unsigned long handle;
	void *data;
	u32 token;
	u16 length;
	u8 marker;
	int puts_before;
	bool dropped;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	token = ref->token;
	fifo = mc16_fifo_of(ref);
	fifo_before = atomic_long_read(&fifo->nr_entries);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f->refs, 0, MC16_STORED), 0);
	KUNIT_EXPECT_EQ(test, ref->cluster->eligible_slots, (u16)1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo->nr_entries), fifo_before + 1);
	/* survivor is the actual extra slot reference owned by the batch. */
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->survivor[0]));
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->lookup));
	mutex_lock(memcomp_lock_of(token));
	mc16_view_locked(ref, &view);
	handle = *view.word;
	length = *view.length;
	record_charge = view.record->charge_bytes;
	/* The claim performs its existing L removal before OWNER is dropped. */
	mc16_fifo_transition(ref, MC16_STORED, MC16_WRITEBACK);
	*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_WRITEBACK;
	mutex_unlock(memcomp_lock_of(token));
	data = zs_map_object(memcomp_pools[0], handle, ZS_MM_WO);
	memset(data, 0x5a, PAGE_SIZE);
	zs_unmap_object(memcomp_pools[0], handle);
	storage->payload.task = current;
	storage->payload.pool = memcomp_pools[0];
	storage->payload.handle = handle;
	storage->payload.live = true;
	rcu_assign_pointer(memcomp_kunit_async_probe, &storage->payload);
	charged = atomic_long_read(&storage->owner->model.charged);
	puts_before = atomic_read(&storage->owner->model.put_calls);

	dropped = mc16_owner_drop_folio_held(&f->lookup, storage->owner->folio,
					     1, 0);
	if (dropped)
		*ref = (struct memcomp_ref) { };
	KUNIT_ASSERT_TRUE(test, dropped);
	KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, NULL);
	ref = &f->survivor[0];
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, ref->slot->word, (u64)handle);
	KUNIT_EXPECT_EQ(test, ref->slot->length, length);
	KUNIT_EXPECT_EQ(test, ref->slot->flags, (u16)MC16_WRITEBACK);
	/* Record's last member was put, without consuming the batch's handle. */
	KUNIT_EXPECT_EQ(test, atomic_read(&storage->owner->model.put_calls),
			puts_before + 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged),
			charged - record_charge);
	KUNIT_EXPECT_EQ(test, storage->payload.frees, 0U);
	KUNIT_ASSERT_TRUE(test, storage->payload.live);
	data = zs_map_object(memcomp_pools[0], handle, ZS_MM_RO);
	marker = *(u8 *)data;
	zs_unmap_object(memcomp_pools[0], handle);
	KUNIT_EXPECT_EQ(test, marker, (u8)0x5a);
	KUNIT_EXPECT_EQ(test, ref->cluster->eligible_slots, (u16)0);
	KUNIT_EXPECT_TRUE(test, list_empty(&ref->cluster->fifo));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo->nr_entries), fifo_before);
	/* WRITEBACK -> EMPTY has no eligibility change and takes no extra L. */
	mc16_ref_put(ref);
	KUNIT_EXPECT_PTR_EQ(test, ref->slot, NULL);
	KUNIT_EXPECT_EQ(test, storage->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, storage->payload.live);
	KUNIT_EXPECT_FALSE(test, storage->payload.bad_lifetime);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo->nr_entries), fifo_before);
	KUNIT_EXPECT_FALSE(test, mc16_get(token, &f->lookup));
	/* Exact metadata balance is checked after explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged), 0L);
}

static void memcomp_test_owner_resident_final_guard(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_storage *storage;
	struct mc16_view view;
	struct memcomp_ref *ref;
	/* Models the PTE lock around the final reference release. */
	spinlock_t ptl;
	unsigned long irqflags;
	u32 token;
	u16 flags;
	bool dropped, attached;
	int folio_refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	token = ref->token;
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->survivor[0]));
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->lookup));
	dropped = mc16_owner_drop_folio_held(&f->lookup, storage->owner->folio,
					     1, 0);
	if (dropped)
		*ref = (struct memcomp_ref) { };
	KUNIT_ASSERT_TRUE(test, dropped);
	KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, NULL);
	ref = &f->survivor[0];
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(ref->slot->flags),
			(unsigned int)MC16_PENDING);
	mutex_lock(memcomp_lock_of(token));
	mc16_view_locked(ref, &view);
	attached = !!view.record;
	flags = *view.flags;
	mutex_unlock(memcomp_lock_of(token));
	KUNIT_EXPECT_TRUE(test, attached);
	KUNIT_EXPECT_FALSE(test, flags & MC16_F_OWNER);
	KUNIT_EXPECT_TRUE(test, flags & MC16_F_RESIDENT);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), 2);
	/* Real refs-zero release while holding a modeled PTE spinlock. */
	spin_lock_init(&ptl);
	spin_lock_irqsave(&ptl, irqflags);
	mc16_ref_put(ref);
	folio_refs = folio_ref_count(storage->owner->folio);
	spin_unlock_irqrestore(&ptl, irqflags);
	KUNIT_EXPECT_EQ(test, folio_refs, 1);
	KUNIT_EXPECT_PTR_EQ(test, ref->slot, NULL);
	KUNIT_EXPECT_FALSE(test, mc16_get(token, &f->lookup));
	/* Exact metadata balance is checked after explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged), 0L);
}

static void memcomp_test_owner_last_pair(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	bool dropped;
	u32 token;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	token = ref->token;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f->refs, 0, MC16_STORED), 0);
	storage->payload.task = current;
	storage->payload.pool = memcomp_pools[0];
	storage->payload.handle = ((struct mc16_record *)
		(unsigned long)ref->slot->word)->member[0].word;
	storage->payload.live = true;
	rcu_assign_pointer(memcomp_kunit_async_probe, &storage->payload);
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->lookup));
	dropped = mc16_owner_drop_folio_held(&f->lookup, storage->owner->folio,
					     1, 0);
	if (dropped)
		*ref = (struct memcomp_ref) { };
	KUNIT_ASSERT_TRUE(test, dropped);
	KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, NULL);
	KUNIT_EXPECT_EQ(test, storage->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, storage->payload.live);
	KUNIT_EXPECT_FALSE(test, storage->payload.bad_lifetime);
	KUNIT_EXPECT_FALSE(test, mc16_get(token, &f->lookup));
	/* Exact metadata balance is checked after explicit pressure reclaim. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged), 0L);
}

/* A stale attempt consumes exactly its lookup; the initial OWNER survives. */
static void
mc16_test_owner_stale(struct kunit *test, struct mc16_test_owner *f,
		      struct folio *folio, unsigned int nr, unsigned int index)
{
	struct mc16_test_storage *storage = f->refs->storage;
	struct memcomp_ref *ref = &storage->refs[0];
	struct mc16_member before;
	struct mc16_record *record;
	bool dropped;
	u64 physical;
	u16 flags;

	KUNIT_ASSERT_TRUE(test, mc16_get(ref->token, &f->lookup));
	mutex_lock(memcomp_lock_of(ref->token));
	record = (void *)(unsigned long)ref->slot->word;
	before = record->member[0];
	physical = ref->slot->word;
	flags = ref->slot->flags;
	mutex_unlock(memcomp_lock_of(ref->token));
	dropped = mc16_owner_drop_folio_held(&f->lookup, folio, nr, index);
	/* Retain cleanup ownership even if an unexpected success consumed it. */
	if (dropped)
		*ref = (struct memcomp_ref) { };
	KUNIT_ASSERT_FALSE(test, dropped);
	KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, ref->slot->word, physical);
	KUNIT_EXPECT_EQ(test, ref->slot->flags, flags);
	KUNIT_EXPECT_EQ(test, record->member[0].word, before.word);
	KUNIT_EXPECT_EQ(test, record->member[0].length, before.length);
	KUNIT_EXPECT_EQ(test, record->member[0].flags, before.flags);
	KUNIT_EXPECT_EQ(test, record->member[0].payload_charge, before.payload_charge);
	KUNIT_EXPECT_PTR_EQ(test, record->member[0].owner_folio, before.owner_folio);
	KUNIT_EXPECT_EQ(test, refcount_read(&record->members), 1U);
}

static void memcomp_test_owner_stale(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_storage *storage;
	struct mc16_record *record;
	struct memcomp_ref *ref;
	u16 flags;
	bool dropped;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	mc16_test_owner_stale(test, f, NULL, 1, 0);
	KUNIT_ASSERT_NOT_NULL(test, storage->refs[0].slot);
	mc16_test_owner_stale(test, f, storage->owner->folio, 2, 0);
	KUNIT_ASSERT_NOT_NULL(test, storage->refs[0].slot);
	mc16_test_owner_stale(test, f, storage->owner->folio, 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, storage->refs[0].slot);
	ref = &storage->refs[0];
	KUNIT_ASSERT_TRUE(test, mc16_get(ref->token, &f->lookup));
	/* OWNER absent and nonresident PENDING are independently rejected. */
	mutex_lock(memcomp_lock_of(ref->token));
	record = (void *)(unsigned long)ref->slot->word;
	flags = record->member[0].flags;
	record->member[0].flags = flags & ~MC16_F_OWNER;
	dropped = mc16_owner_drop_locked(&f->lookup, storage->owner->folio,
					 1, 0, &f->tail[0]);
	record->member[0].flags = flags;
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_FALSE(test, dropped);
	KUNIT_EXPECT_PTR_EQ(test, f->tail[0].record, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 2U);
	mutex_lock(memcomp_lock_of(ref->token));
	record->member[0].flags = flags & ~MC16_F_RESIDENT;
	dropped = mc16_owner_drop_locked(&f->lookup, storage->owner->folio,
					 1, 0, &f->tail[0]);
	record->member[0].flags = flags;
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_FALSE(test, dropped);
	KUNIT_EXPECT_PTR_EQ(test, f->tail[0].record, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 2U);
	mc16_ref_put(&f->lookup);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), 2);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_OWNER_TEST_H */
