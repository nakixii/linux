/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_QUERY_TEST_H
#define _MM_MEMCOMP_TESTS_QUERY_TEST_H

/*
 * Include after cache_reserve/refs/payload/owner test fixtures and query.h.
 * These call the slot-storage adapters directly; public dispatch stays separate.
 * All cases use real slots/guards and the existing modeled objcg fixture.
 */
#ifdef CONFIG_MEMCG
struct mc16_test_query_observation {
	unsigned int allocations;
	int gets;
	int puts;
	int charges;
	int uncharges;
	int folio_refs;
	long charged;
	s64 stored;
	s64 compressed;
};

static void
mc16_test_query_observe(struct mc16_test_storage *s,
			struct mc16_test_query_observation *out)
{
	out->allocations = s->probe.allocations;
	out->gets = atomic_read(&s->owner->model.get_calls);
	out->puts = atomic_read(&s->owner->model.put_calls);
	out->charges = atomic_read(&s->owner->model.charge_calls);
	out->uncharges = atomic_read(&s->owner->model.uncharge_calls);
	out->folio_refs = folio_ref_count(s->owner->folio);
	out->charged = atomic_long_read(&s->owner->model.charged);
	out->stored = atomic64_read(&memcomp_stored_bytes);
	out->compressed = atomic64_read(&memcomp_compressed_bytes);
}

static void
mc16_test_query_unchanged(struct kunit *test, struct mc16_test_storage *s,
			  const struct mc16_test_query_observation *before)
{
	struct mc16_test_query_observation after;

	mc16_test_query_observe(s, &after);
	KUNIT_EXPECT_EQ(test, after.allocations, before->allocations);
	KUNIT_EXPECT_EQ(test, after.gets, before->gets);
	KUNIT_EXPECT_EQ(test, after.puts, before->puts);
	KUNIT_EXPECT_EQ(test, after.charges, before->charges);
	KUNIT_EXPECT_EQ(test, after.uncharges, before->uncharges);
	KUNIT_EXPECT_EQ(test, after.folio_refs, before->folio_refs);
	KUNIT_EXPECT_EQ(test, after.charged, before->charged);
	KUNIT_EXPECT_EQ(test, after.stored, before->stored);
	KUNIT_EXPECT_EQ(test, after.compressed, before->compressed);
}

static void mc16_test_query_absent(struct kunit *test, unsigned long token)
{
	struct memcomp_ref ref = { };
	unsigned long compressed = ULONG_MAX, base = ULONG_MAX, index = ULONG_MAX;
	unsigned int nr;
	int count = -1, resident;
	bool stat;

	KUNIT_EXPECT_FALSE(test, mc16_entry_present(token));
	stat = mc16_entry_stat(token, &compressed, &count);
	KUNIT_EXPECT_FALSE(test, stat);
	KUNIT_EXPECT_EQ(test, compressed, 0UL);
	KUNIT_EXPECT_EQ(test, count, 0);
	nr = mc16_entry_geometry(token, &base, &index);
	KUNIT_EXPECT_EQ(test, nr, 0U);
	KUNIT_EXPECT_EQ(test, base, token);
	KUNIT_EXPECT_EQ(test, index, 0UL);
	resident = mc16_entry_resident_get(token, &ref);
	KUNIT_EXPECT_EQ(test, resident, 0);
	KUNIT_EXPECT_PTR_EQ(test, ref.slot, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ref.cluster, NULL);
	KUNIT_EXPECT_EQ(test, ref.token, 0U);
	/* Return any unexpected successful guard, even on a failing expectation. */
	mc16_entry_resident_put(&ref);
}

/*
 * Presence must work with T already held and under PTL/RCU. Check storage
 * side effects and reference balance; the storage probe does not count slot
 * ref RMW events, so unchanged refs alone do not prove absence of a get/put.
 */
static void memcomp_test_query_present(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_query_observation before;
	struct mc16_test_storage *s;
	struct memcomp_ref *ref;
	spinlock_t ptl; /* Models a PTL-only query caller. */
	unsigned long irqflags;
	unsigned int refs, i;
	bool present = true;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	ref = &s->refs[0];
	refs = refcount_read(&ref->slot->refs);
	mc16_test_query_observe(s, &before);
	spin_lock_init(&ptl);
	mutex_lock(memcomp_lock_of(ref->token));
	present = mc16_entry_present(ref->token);
	mutex_unlock(memcomp_lock_of(ref->token));
	rcu_read_lock();
	spin_lock_irqsave(&ptl, irqflags);
	for (i = 0; i < 8; i++)
		present &= mc16_entry_present(ref->token);
	spin_unlock_irqrestore(&ptl, irqflags);
	rcu_read_unlock();
	KUNIT_EXPECT_TRUE(test, present);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);
	mc16_test_query_unchanged(test, s, &before);
}

static void memcomp_test_query_absent(struct kunit *test)
{
	struct mc16_test_cache_reserve *f = mc16_test_cache_begin(test);
	struct mc16_test_query_observation before;
	struct mc16_test_storage *s;
	struct memcomp_ref *ref;
	unsigned long token;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_prepare(f, 0, 0, U64_MAX), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_cache_reserve(f, 1), 0);
	s = f->storage;
	ref = &s->refs[0];
	token = ref->token;
	KUNIT_ASSERT_EQ(test, refcount_read(&ref->slot->refs), 0U);
	mc16_test_query_observe(s, &before);
	/* Private reservation is published structurally but has no owner yet. */
	mc16_test_query_absent(test, token);
	mc16_test_query_absent(test, token + 1); /* beyond release-published next */
	mc16_test_query_absent(test, token + MC16_SEGMENT_SLOTS);
	mc16_test_query_absent(test, 1); /* cluster id zero */
	mc16_test_query_absent(test, U32_MAX);
#if BITS_PER_LONG > 32
	mc16_test_query_absent(test, (1UL << 32) | token);
#endif
	mc16_test_query_absent(test, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 0U);
	mc16_test_query_unchanged(test, s, &before);
	/* Same numeric token must remain absent before and after RCU retirement. */
	mc16_release_resources(ref);
	mc16_retire_slot(ref);
	*ref = (struct memcomp_ref) { };
	mc16_test_query_absent(test, token);
	rcu_barrier();
	mc16_test_query_absent(test, token);
}

static void memcomp_test_query_zero(struct kunit *test)
{
	struct mc16_test_refs *f = mc16_test_refs_begin(test);
	struct mc16_test_query_observation before;
	struct mc16_zero_owners owners, after;
	struct memcomp_ref ref = { .token = U32_MAX };
	unsigned long bytes = ULONG_MAX, base = ULONG_MAX, index = ULONG_MAX;
	int count = -1, resident;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_test_query_observe(f->storage, &before);
	mc16_zero_owners_read(&owners);
	KUNIT_EXPECT_TRUE(test, mc16_entry_present(0));
	KUNIT_EXPECT_TRUE(test, mc16_entry_stat(0, &bytes, &count));
	KUNIT_EXPECT_EQ(test, bytes, 0UL);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_TRUE(test, mc16_entry_stat(0, NULL, NULL));
	KUNIT_EXPECT_EQ(test, mc16_entry_geometry(0, &base, &index), 1U);
	KUNIT_EXPECT_EQ(test, base, 0UL);
	KUNIT_EXPECT_EQ(test, index, 0UL);
	KUNIT_EXPECT_EQ(test, mc16_entry_nr_pages(0, NULL), 1U);
	resident = mc16_entry_resident_get(0, &ref);
	KUNIT_EXPECT_EQ(test, resident, 0);
	KUNIT_EXPECT_PTR_EQ(test, ref.slot, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ref.cluster, NULL);
	KUNIT_EXPECT_EQ(test, ref.token, 0U);
	mc16_entry_resident_put(&ref);
	mc16_entry_resident_put(&ref);
	mc16_zero_owners_read(&after);
	KUNIT_EXPECT_EQ(test, after.pte_pages, owners.pte_pages);
	KUNIT_EXPECT_EQ(test, after.cache_pages, owners.cache_pages);
	mc16_test_query_unchanged(test, f->storage, &before);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
}

static void memcomp_test_query_stat_ptl(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct memcomp_ref *ref;
	spinlock_t ptl; /* Models a PTL-only query caller. */
	unsigned long irqflags;
	unsigned int refs;
	int count = -1, held_count = -1;
	bool stat, optional, held_stat;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ref = &f->refs->storage->refs[0];
	/* OWNER plus two persistent PTEs; query's own lookup must disappear. */
	KUNIT_ASSERT_TRUE(test, mc16_get(ref->token, &f->survivor[0]));
	KUNIT_ASSERT_TRUE(test, mc16_get(ref->token, &f->survivor[1]));
	refs = refcount_read(&ref->slot->refs);
	spin_lock_init(&ptl);
	/* Separate contexts: never manufacture a forbidden T -> PTL edge. */
	mutex_lock(memcomp_lock_of(ref->token));
	held_stat = mc16_entry_stat(ref->token, NULL, &held_count);
	mutex_unlock(memcomp_lock_of(ref->token));
	rcu_read_lock();
	spin_lock_irqsave(&ptl, irqflags);
	stat = mc16_entry_stat(ref->token, NULL, &count);
	optional = mc16_entry_stat(ref->token, NULL, NULL);
	spin_unlock_irqrestore(&ptl, irqflags);
	rcu_read_unlock();
	KUNIT_EXPECT_TRUE(test, held_stat);
	KUNIT_EXPECT_EQ(test, held_count, 2);
	KUNIT_EXPECT_TRUE(test, stat);
	KUNIT_EXPECT_TRUE(test, optional);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);
}

static void memcomp_test_query_stat_busy_invalid(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct memcomp_ref *ref;
	struct mc16_view view;
	unsigned long bytes = ULONG_MAX;
	unsigned int refs, i;
	u16 flags, length;
	int count = -1;
	bool stat;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f->refs, 0, MC16_STORED), 0);
	ref = &f->refs->storage->refs[0];
	refs = refcount_read(&ref->slot->refs);
	mutex_lock(memcomp_lock_of(ref->token));
	stat = mc16_entry_stat(ref->token, &bytes, &count);
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_FALSE(test, stat);
	KUNIT_EXPECT_EQ(test, bytes, 0UL);
	KUNIT_EXPECT_EQ(test, count, 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);
	KUNIT_ASSERT_TRUE(test, mc16_entry_stat(ref->token, &bytes, &count));
	KUNIT_EXPECT_EQ(test, bytes, (unsigned long)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, count, 1);

	/* Restore every corrupted field before any fatal assertion or teardown. */
	for (i = 0; i < 3; i++) {
		mutex_lock(memcomp_lock_of(ref->token));
		mc16_view_locked(ref, &view);
		flags = *view.flags;
		length = *view.length;
		if (i == 0)
			*view.flags |= BIT(15);
		else if (i == 1)
			*view.length = U16_MAX;
		else
			*view.flags |= MC16_F_RESIDENT;
		mutex_unlock(memcomp_lock_of(ref->token));
		bytes = ULONG_MAX;
		count = -1;
		stat = mc16_entry_stat(ref->token, &bytes, &count);
		mutex_lock(memcomp_lock_of(ref->token));
		mc16_view_locked(ref, &view);
		*view.flags = flags;
		*view.length = length;
		mutex_unlock(memcomp_lock_of(ref->token));
		KUNIT_EXPECT_FALSE(test, stat);
		KUNIT_EXPECT_EQ(test, bytes, 0UL);
		KUNIT_EXPECT_EQ(test, count, 0);
		KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);
	}
}

static void memcomp_test_query_stat_compressed(struct kunit *test)
{
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct memcomp_ref *ref;
	unsigned long handle = 0, bytes;
	unsigned int compressed = 0, i;
	u8 pool = 0, *src;
	int count, ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ref = &f->refs->storage->refs[0];
	src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = i % 53;
	ret = memcomp_prepare_payload(src, PAGE_SIZE, &compressed, &handle, &pool,
				      MEMCOMP_PAYLOAD_MUST_SUCCEED, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->prepared = (struct mc16_payload) { .word = handle, .bytes = compressed };
	KUNIT_ASSERT_EQ(test, pool, (u8)0);
	KUNIT_ASSERT_GT(test, compressed, 0U);
	KUNIT_ASSERT_LT(test, compressed, (unsigned int)PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, mc16_payload_charge(ref, &f->prepared), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, f->refs->storage->owner->folio), 0);
	mc16_payload_finish(ref, &f->tail);
	/* Expected byte count comes from the real compressor's output. */
	KUNIT_ASSERT_TRUE(test, mc16_entry_stat(ref->token, &bytes, &count));
	KUNIT_EXPECT_EQ(test, bytes, (unsigned long)compressed);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), 1U);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, 0, U64_MAX), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, f->refs->storage->owner->folio), 0);
	mc16_payload_finish(ref, &f->tail);
	bytes = ULONG_MAX;
	KUNIT_ASSERT_TRUE(test, mc16_entry_stat(ref->token, &bytes, NULL));
	KUNIT_EXPECT_EQ(test, bytes, 0UL);
}

static void memcomp_test_query_pending_owner_drop(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_storage *s;
	struct mc16_view view;
	unsigned long bytes = ULONG_MAX;
	u32 token;
	u16 physical, effective;
	int count = -1;
	bool dropped;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	token = s->refs[0].token;
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->survivor[0]));
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->survivor[1]));
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->lookup));
	dropped = mc16_owner_drop_folio_held(&f->lookup, s->owner->folio, 1, 0);
	if (dropped)
		s->refs[0] = (struct memcomp_ref) { };
	KUNIT_ASSERT_TRUE(test, dropped);
	mutex_lock(memcomp_lock_of(token));
	physical = f->survivor[0].slot->flags;
	mc16_view_locked(&f->survivor[0], &view);
	effective = *view.flags;
	mutex_unlock(memcomp_lock_of(token));
	KUNIT_EXPECT_FALSE(test, physical & MC16_F_OWNER);
	KUNIT_EXPECT_FALSE(test, effective & MC16_F_OWNER);
	KUNIT_EXPECT_TRUE(test, effective & MC16_F_RESIDENT);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(physical), (unsigned int)MC16_PENDING);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->survivor[0].slot->refs), 2U);
	KUNIT_ASSERT_TRUE(test, mc16_entry_stat(token, NULL, &count));
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_ASSERT_TRUE(test, mc16_entry_stat(token, &bytes, &count));
	KUNIT_EXPECT_EQ(test, bytes, 0UL);
	KUNIT_EXPECT_EQ(test, count, 2);
}

static void memcomp_test_query_geometry(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 2);
	struct mc16_test_storage *s;
	unsigned long base, index;
	unsigned int i, nr, refs;
	u32 first, token;
	bool dropped;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	first = s->refs[0].token;
	for (i = 0; i < 2; i++) {
		token = s->refs[i].token;
		refs = refcount_read(&s->refs[i].slot->refs);
		mutex_lock(memcomp_lock_of(token));
		nr = mc16_entry_geometry(token, &base, &index);
		mutex_unlock(memcomp_lock_of(token));
		KUNIT_EXPECT_EQ(test, nr, 2U);
		KUNIT_EXPECT_EQ(test, base, (unsigned long)first);
		KUNIT_EXPECT_EQ(test, index, (unsigned long)i);
		KUNIT_EXPECT_EQ(test, mc16_entry_nr_pages(token, NULL), 2U);
		KUNIT_EXPECT_EQ(test, refcount_read(&s->refs[i].slot->refs), refs);
		KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f->refs, i, MC16_SAMEFILL), 0);
		KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->survivor[i]));
	}
	/* Normalize siblings first; drop the producer's head last. */
	for (i = 2; i; ) {
		i--;
		token = f->survivor[i].token;
		KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->lookup));
		dropped = mc16_owner_drop_folio_held(&f->lookup, s->owner->folio, 2, i);
		if (dropped)
			s->refs[i] = (struct memcomp_ref) { };
		KUNIT_ASSERT_TRUE(test, dropped);
		KUNIT_EXPECT_EQ(test, mc16_flags_state(f->survivor[i].slot->flags),
				(unsigned int)MC16_SAMEFILL);
	}
	/* The record is gone; geometry must still work from the stable slots. */
	for (i = 0; i < 2; i++) {
		nr = mc16_entry_geometry(f->survivor[i].token, &base, &index);
		KUNIT_EXPECT_EQ(test, nr, 2U);
		KUNIT_EXPECT_EQ(test, base, (unsigned long)first);
		KUNIT_EXPECT_EQ(test, index, (unsigned long)i);
	}
}

static void memcomp_test_query_resident_busy(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_storage *s;
	struct memcomp_ref *ref;
	struct mc16_view view;
	unsigned int refs;
	u16 flags;
	int ret, folio_refs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	ref = &s->refs[0];
	refs = refcount_read(&ref->slot->refs);
	folio_refs = folio_ref_count(s->owner->folio);
	mutex_lock(memcomp_lock_of(ref->token));
	ret = mc16_entry_resident_get(ref->token, &f->lookup);
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);
	mc16_entry_resident_put(&f->lookup);

	ret = mc16_entry_resident_get(ref->token, &f->lookup);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, ref->slot);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs + 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(s->owner->folio), folio_refs);
	mc16_entry_resident_put(&f->lookup);
	mc16_entry_resident_put(&f->lookup);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);

	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	flags = *view.flags;
	*view.flags |= BIT(15);
	mutex_unlock(memcomp_lock_of(ref->token));
	ret = mc16_entry_resident_get(ref->token, &f->lookup);
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	*view.flags = flags;
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_PTR_EQ(test, f->lookup.slot, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref->slot->refs), refs);
}

static void memcomp_test_query_nonresident_guard(struct kunit *test)
{
	struct mc16_test_owner *f = mc16_test_owner_begin(test, 1);
	struct mc16_test_storage *s;
	u32 token;
	int ret;
	bool dropped;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	token = s->refs[0].token;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f->refs, 0, MC16_SAMEFILL), 0);
	ret = mc16_entry_resident_get(token, &f->survivor[0]);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->survivor[0].slot);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->survivor[0].slot->refs), 2U);
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->lookup));
	dropped = mc16_owner_drop_folio_held(&f->lookup, s->owner->folio, 1, 0);
	if (dropped)
		s->refs[0] = (struct memcomp_ref) { };
	KUNIT_ASSERT_TRUE(test, dropped);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->survivor[0].slot->refs), 1U);
	KUNIT_EXPECT_TRUE(test, mc16_entry_present(token));
	KUNIT_EXPECT_EQ(test, mc16_flags_state(f->survivor[0].slot->flags),
			(unsigned int)MC16_SAMEFILL);
	mc16_entry_resident_put(&f->survivor[0]);
	KUNIT_EXPECT_PTR_EQ(test, f->survivor[0].slot, NULL);
	KUNIT_EXPECT_FALSE(test, mc16_entry_present(token));
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_QUERY_TEST_H */
