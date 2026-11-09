/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_WRITEBACK_TEST_H
#define _MM_MEMCOMP_TESTS_WRITEBACK_TEST_H

#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)
/*
 * Include after refs/payload/cache_store test fixtures and the WB helpers.
 * Payload allocation, compression, load and freeing are real zsmalloc paths.
 * objcg uses the existing identity-only charge model (not memcg enforcement);
 * backing ownership uses memcomp_slot_* KUnit tallies, never a swap device.
 * Interleavings below are scheduled serially at their actual T boundaries.
 */
struct mc16_test_writeback {
	struct mc16_test_refs *refs;
	struct mc16_payload prepared;
	struct mc16_payload_tail prepare_tail;
	struct mc16_test_cache_producer *cache;
	struct memcomp_ref writer;
	struct memcomp_ref pte;
	struct memcomp_ref lookup;
	struct mc16_writeback snapshot;
	struct mc16_writeback_tail tail;
	swp_entry_t backing;
	bool backing_owned;
	u32 bytes;
	u8 *src;
};

/* Registered last: finish the payload tail before consuming any guard. */
static void mc16_test_writeback_cleanup(void *data)
{
	struct mc16_test_writeback *f = data;
	struct memcomp_ref *entry = f->writer.slot ? &f->writer :
		&f->refs->storage->refs[0];

	if (entry->slot) {
		mc16_payload_finish(entry, &f->prepare_tail);
		mc16_payload_abort(entry, &f->prepared);
	}
	if (f->writer.slot)
		mc16_writeback_finish(&f->writer, &f->tail);
	if (f->lookup.slot)
		mc16_ref_put(&f->lookup);
	if (f->pte.slot)
		mc16_ref_put(&f->pte);
	if (f->writer.slot)
		mc16_ref_put(&f->writer);
	if (f->backing_owned)
		memcomp_slot_put(f->backing);
}

static struct mc16_test_writeback *
mc16_test_writeback_begin(struct kunit *test, bool attached)
{
	struct mc16_test_writeback *f;
	struct mc16_test_storage *s;
	struct memcomp_ref *entry;
	struct mc16_view view;
	unsigned long handle = 0;
	unsigned int bytes = 0, i;
	u8 pool = 0;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	if (attached) {
		f->refs = mc16_test_refs_begin(test);
		if (!f->refs)
			return NULL;
		entry = &f->refs->storage->refs[0];
	} else {
		f->cache = mc16_test_cache_producer_begin(test);
		if (!f->cache)
			return NULL;
		f->refs = f->cache->refs;
		entry = &f->cache->cache;
	}
	if (kunit_add_action_or_reset(test, mc16_test_writeback_cleanup, f))
		return NULL;
	s = f->refs->storage;
	if (attached && mc16_test_refs_reserve(f->refs, 1))
		return NULL;
	f->src = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->src)
		return NULL;
	for (i = 0; i < PAGE_SIZE; i++)
		f->src[i] = i % 53;
	if (attached) {
		ret = memcomp_prepare_payload(f->src, PAGE_SIZE, &bytes, &handle,
					      &pool, MEMCOMP_PAYLOAD_MUST_SUCCEED,
					      false);
		if (ret)
			return NULL;
		f->prepared = (struct mc16_payload) {
			.word = handle, .bytes = bytes,
		};
		if (pool || !bytes || bytes >= PAGE_SIZE ||
		    mc16_payload_charge(entry, &f->prepared))
			return NULL;
		mutex_lock(memcomp_lock_of(entry->token));
		ret = mc16_payload_commit_locked(entry, s->owner->folio, 1, 0,
						 &f->prepared, &f->prepare_tail);
		mutex_unlock(memcomp_lock_of(entry->token));
		if (ret)
			return NULL;
		mc16_payload_finish(entry, &f->prepare_tail);
	} else if (mc16_cache_store(f->src, s->owner->folio, entry)) {
		return NULL;
	}
	if (!mc16_get(entry->token, &f->writer))
		return NULL;
	mutex_lock(memcomp_lock_of(entry->token));
	ret = mc16_view_locked(entry, &view) &&
		mc16_length_decode(*view.length, &f->bytes);
	if (ret) {
		s->payload.handle = *view.word;
		s->payload.pool = memcomp_pools[0];
		s->payload.task = current;
		s->payload.live = true;
		/* Opaque shadow contents must survive backing-only changes. */
		*mc16_shadow_ptr(entry) = (unsigned long)xa_mk_value(123);
	}
	mutex_unlock(memcomp_lock_of(entry->token));
	if (!ret || !f->bytes || f->bytes >= PAGE_SIZE)
		return NULL;
	rcu_assign_pointer(memcomp_kunit_async_probe, &s->payload);
	return f;
}

static bool mc16_test_writeback_claim(struct mc16_test_writeback *f)
{
	bool claimed;

	mutex_lock(memcomp_lock_of(f->writer.token));
	claimed = mc16_writeback_claim_locked(&f->writer, &f->snapshot);
	mutex_unlock(memcomp_lock_of(f->writer.token));
	return claimed;
}

static bool mc16_test_writeback_revert(struct mc16_test_writeback *f)
{
	bool reverted;

	mutex_lock(memcomp_lock_of(f->writer.token));
	reverted = mc16_writeback_revert_locked(&f->writer);
	mutex_unlock(memcomp_lock_of(f->writer.token));
	return reverted;
}

static int
mc16_test_writeback_settle(struct mc16_test_writeback *f,
			   const struct mc16_swap_fields *swap)
{
	struct kunit *test = f->refs->storage->test;
	struct mc16_key *key = f->writer.cluster->key;
	u32 token = f->writer.token;
	unsigned short owner_id = key->owner_id;
	u64 shadow;
	int ret;

	mutex_lock(memcomp_lock_of(f->writer.token));
	shadow = *mc16_shadow_ptr(&f->writer);
	ret = mc16_writeback_settle_locked(&f->writer, &f->snapshot, swap,
					   &f->tail);
	KUNIT_EXPECT_EQ(test, *mc16_shadow_ptr(&f->writer), shadow);
	KUNIT_EXPECT_EQ(test, f->writer.token, token);
	KUNIT_EXPECT_PTR_EQ(test, f->writer.cluster->key, key);
	KUNIT_EXPECT_EQ(test, f->writer.cluster->key->owner_id, owner_id);
	mutex_unlock(memcomp_lock_of(f->writer.token));
	if (!ret)
		f->backing_owned = false;
	return ret;
}

static struct mc16_swap_fields
mc16_test_writeback_backing(struct mc16_test_writeback *f, bool packed)
{
	struct mc16_swap_fields swap = {
		.type = MAX_SWAPFILES - 1,
		.swap_offset = mc16_swap_offset_limit(),
		.length = f->bytes,
		.byte_offset = packed ? PAGE_SIZE - f->bytes : 0,
		.packed = packed,
	};

	f->backing = swp_entry(swap.type, swap.swap_offset);
	memcomp_slot_dup(f->backing);
	f->backing_owned = true;
	return swap;
}

static void mc16_test_writeback_load(struct kunit *test,
				     struct mc16_test_writeback *f)
{
	void *dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dst);
	KUNIT_ASSERT_EQ(test, mc16_load_page(&f->writer, dst), 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->src, dst, PAGE_SIZE), 0);
}

static void mc16_test_writeback_swapped(struct kunit *test,
					struct mc16_test_writeback *f,
					const struct mc16_swap_fields *swap,
					bool attached)
{
	struct mc16_swap_fields decoded = { };
	struct mc16_view view;
	bool valid, has_record, owner;
	u32 charge;
	unsigned int state;

	mutex_lock(memcomp_lock_of(f->writer.token));
	valid = mc16_view_locked(&f->writer, &view);
	if (valid) {
		state = mc16_flags_state(*view.flags);
		valid = mc16_swap_decode(*view.word, *view.length, &decoded);
		has_record = !!view.record;
		owner = !!(*view.flags & MC16_F_OWNER);
		charge = view.member ? view.member->payload_charge : 0;
	} else {
		state = MC16_EMPTY;
		has_record = false;
		owner = false;
		charge = 0;
	}
	mutex_unlock(memcomp_lock_of(f->writer.token));
	KUNIT_ASSERT_TRUE(test, valid);
	KUNIT_EXPECT_EQ(test, state, (unsigned int)MC16_SWAPPED);
	KUNIT_EXPECT_EQ(test, has_record, attached);
	KUNIT_EXPECT_EQ(test, owner, attached);
	KUNIT_EXPECT_EQ(test, charge, 0U);
	KUNIT_EXPECT_EQ(test, decoded.type, swap->type);
	KUNIT_EXPECT_EQ(test, decoded.swap_offset, swap->swap_offset);
	KUNIT_EXPECT_EQ(test, decoded.byte_offset, swap->byte_offset);
	KUNIT_EXPECT_EQ(test, decoded.length, swap->length);
	KUNIT_EXPECT_EQ(test, decoded.packed, swap->packed);
	KUNIT_EXPECT_EQ(test, f->writer.cluster->eligible_slots, (u16)0);
}

/* Exercise real cache compressed data, including reads while claimed. */
static void memcomp_test_writeback_cache_raw(struct kunit *test)
{
	struct mc16_test_writeback *f = mc16_test_writeback_begin(test, false);
	struct mc16_test_storage *s;
	struct mc16_swap_fields swap;
	long charged;
	s64 compressed, stored;
	int dups, puts, uncharges;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	long swapped;
	s64 packed;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	compressed = atomic64_read(&memcomp_compressed_bytes);
	stored = atomic64_read(&memcomp_stored_bytes);
	charged = atomic_long_read(&s->owner->model.charged);
	KUNIT_ASSERT_TRUE(test, mc16_test_writeback_claim(f));
	KUNIT_EXPECT_EQ(test, f->writer.cluster->eligible_slots, (u16)0);
	mc16_test_writeback_load(test, f);
	swap = mc16_test_writeback_backing(f, false);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	uncharges = atomic_read(&s->owner->model.uncharge_calls);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	swapped = atomic_long_read(&memcomp_swapped_entries);
	packed = atomic64_read(&memcomp_writeback_packed_payloads);
#endif
	KUNIT_ASSERT_EQ(test, mc16_test_writeback_settle(f, &swap), 0);
	KUNIT_EXPECT_FALSE(test, f->backing_owned);
	mc16_test_writeback_swapped(test, f, &swap, false);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups), dups);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 0U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), charged);
	mc16_writeback_finish(&f->writer, &f->tail);
	mc16_writeback_finish(&f->writer, &f->tail);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, s->payload.bad_lifetime);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.uncharge_calls),
			uncharges + 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			charged - f->bytes);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compressed_bytes), compressed);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_stored_bytes), stored);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, atomic_long_read(&memcomp_swapped_entries), swapped + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_writeback_packed_payloads),
			packed);
#endif
	mc16_ref_put(&f->cache->cache);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts);
	mc16_ref_put(&f->writer);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts + 1);
	/* Final guard release retains metadata until explicit pressure. */
	mc16_test_reclaim_retained();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), 0L);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 1U);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, atomic_long_read(&memcomp_swapped_entries), swapped);
#endif
}

static void memcomp_test_writeback_owner_packed(struct kunit *test)
{
	struct mc16_test_writeback *f = mc16_test_writeback_begin(test, true);
	struct mc16_test_storage *s;
	struct mc16_swap_fields swap;
	struct mc16_view view;
	u64 physical;
	long charged;
	int reclaims, uncharges;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	s64 packed;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	/* Remember only an opaque comparison value, never dereference after T. */
	physical = f->writer.slot->word;
	charged = atomic_long_read(&s->owner->model.charged);
	KUNIT_ASSERT_TRUE(test, mc16_test_writeback_claim(f));
	mc16_test_writeback_load(test, f);
	swap = mc16_test_writeback_backing(f, true);
	reclaims = atomic_read(&memcomp_kunit_slot_reclaims);
	uncharges = atomic_read(&s->owner->model.uncharge_calls);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	packed = atomic64_read(&memcomp_writeback_packed_payloads);
#endif
	KUNIT_ASSERT_EQ(test, mc16_test_writeback_settle(f, &swap), 0);
	KUNIT_EXPECT_EQ(test, f->writer.slot->word, physical);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(f->writer.slot->flags),
			(unsigned int)MC16_PENDING);
	mc16_test_writeback_swapped(test, f, &swap, true);
	mc16_writeback_finish(&f->writer, &f->tail);
	mc16_writeback_finish(&f->writer, &f->tail);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			charged - f->bytes);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.uncharge_calls),
			uncharges + 1);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 1U);
	mutex_lock(memcomp_lock_of(f->writer.token));
	mc16_view_locked(&f->writer, &view);
	KUNIT_EXPECT_EQ(test, view.member->payload_charge, 0U);
	mutex_unlock(memcomp_lock_of(f->writer.token));
	mc16_ref_put(&f->writer);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims), reclaims);
	mc16_ref_put(&s->refs[0]);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims), reclaims + 1);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, s->payload.bad_lifetime);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_writeback_packed_payloads),
			packed + 1);
#endif
}

static void memcomp_test_writeback_revert(struct kunit *test)
{
	struct mc16_test_writeback *f = mc16_test_writeback_begin(test, false);
	struct mc16_test_storage *s;
	struct mc16_writeback snapshot;
	struct memcomp_lru *fifo;
	long charged, eligible;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	fifo = mc16_fifo_of(&f->writer);
	eligible = atomic_long_read(&fifo->nr_entries);
	charged = atomic_long_read(&s->owner->model.charged);
	KUNIT_ASSERT_TRUE(test, mc16_test_writeback_claim(f));
	snapshot = f->snapshot;
	KUNIT_EXPECT_FALSE(test, mc16_test_writeback_claim(f));
	KUNIT_EXPECT_PTR_EQ(test, f->snapshot.slot, snapshot.slot);
	KUNIT_EXPECT_EQ(test, f->snapshot.word, snapshot.word);
	KUNIT_EXPECT_EQ(test, f->snapshot.length, snapshot.length);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo->nr_entries), eligible - 1);
	KUNIT_EXPECT_TRUE(test, mc16_test_writeback_revert(f));
	KUNIT_EXPECT_FALSE(test, mc16_test_writeback_revert(f));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fifo->nr_entries), eligible);
	KUNIT_EXPECT_EQ(test, f->writer.cluster->eligible_slots, (u16)1);
	KUNIT_EXPECT_EQ(test, f->writer.slot->word, snapshot.word);
	KUNIT_EXPECT_EQ(test, f->writer.slot->length, snapshot.length);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 0U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged), charged);
	mc16_test_writeback_load(test, f);
}

/* Mutate the staged identity, keeping the actual zs handle valid throughout. */
static void memcomp_test_writeback_mismatch(struct kunit *test)
{
	struct mc16_test_writeback *f = mc16_test_writeback_begin(test, false);
	struct mc16_test_storage *s;
	struct mc16_swap_fields swap;
	int puts, dups, uncharges;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	swap = mc16_test_writeback_backing(f, true);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	uncharges = atomic_read(&s->owner->model.uncharge_calls);
	for (i = 0; i < 3; i++) {
		KUNIT_ASSERT_TRUE(test, mc16_test_writeback_claim(f));
		if (i == 0)
			f->snapshot.word ^= 1;
		else if (i == 1)
			f->snapshot.length ^= 1;
		else
			KUNIT_ASSERT_TRUE(test, mc16_test_writeback_revert(f));
		KUNIT_EXPECT_EQ(test, mc16_test_writeback_settle(f, &swap), -ESTALE);
		KUNIT_EXPECT_TRUE(test, f->backing_owned);
		KUNIT_EXPECT_EQ(test, f->tail.word, 0ULL);
		KUNIT_EXPECT_EQ(test, f->tail.charge, 0U);
		KUNIT_EXPECT_EQ(test, f->writer.cluster->eligible_slots, (u16)1);
		mc16_test_writeback_load(test, f);
	}
	KUNIT_EXPECT_EQ(test, s->payload.frees, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.uncharge_calls), uncharges);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups), dups);
	/* Caller can still return exactly its own unconsumed reference. */
	memcomp_slot_put(f->backing);
	f->backing_owned = false;
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts + 1);
}

static void memcomp_test_writeback_encoding(struct kunit *test)
{
	struct mc16_test_writeback *f = mc16_test_writeback_begin(test, false);
	struct mc16_swap_fields swap, bad;
	unsigned int i;
	int puts;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_test_writeback_claim(f));
	swap = mc16_test_writeback_backing(f, true);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	for (i = 0; i < 8; i++) {
		bad = swap;
		switch (i) {
		case 0:
			bad.swap_offset++;
			break;
		case 1:
			bad.type = MAX_SWAPFILES;
			break;
		case 2:
			bad.byte_offset = PAGE_SIZE;
			break;
		case 3:
			bad.byte_offset++; /* packed payload crosses backing page */
			break;
		case 4:
			bad.packed = false; /* raw requires byte_offset == 0 */
			break;
		case 5:
			bad.length = 0;
			break;
		case 6:
			bad.length = PAGE_SIZE;
			break;
		default:
			bad.type = 0;
			bad.swap_offset = 0; /* zero backing is never transferable */
			break;
		}
		KUNIT_EXPECT_EQ(test, mc16_test_writeback_settle(f, &bad), -EINVAL);
		KUNIT_EXPECT_TRUE(test, f->backing_owned);
		KUNIT_EXPECT_EQ(test, f->tail.word, 0ULL);
		KUNIT_EXPECT_EQ(test, f->writer.slot->word, f->snapshot.word);
		KUNIT_EXPECT_EQ(test, mc16_flags_state(f->writer.slot->flags),
				(unsigned int)MC16_WRITEBACK);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts);
	KUNIT_EXPECT_EQ(test, f->refs->storage->payload.frees, 0U);
	/* Maximum type/offset and a payload ending exactly at PAGE_SIZE. */
	KUNIT_ASSERT_EQ(test, mc16_test_writeback_settle(f, &swap), 0);
	mc16_test_writeback_swapped(test, f, &swap, false);
}

/* OWNER normalization frees the record before WB settles using the guard. */
static void memcomp_test_writeback_last_pte(struct kunit *test)
{
	struct mc16_test_writeback *f = mc16_test_writeback_begin(test, true);
	struct mc16_test_storage *s;
	struct mc16_swap_fields swap;
	struct mc16_view view;
	long charged;
	u64 record_charge;
	u32 token;
	int reclaims, uncharges;
	bool dropped;

	KUNIT_ASSERT_NOT_NULL(test, f);
	s = f->refs->storage;
	token = f->writer.token;
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->pte));
	KUNIT_ASSERT_TRUE(test, mc16_get(token, &f->lookup));
	KUNIT_ASSERT_TRUE(test, mc16_test_writeback_claim(f));
	mutex_lock(memcomp_lock_of(token));
	mc16_view_locked(&f->writer, &view);
	record_charge = view.record->charge_bytes;
	mutex_unlock(memcomp_lock_of(token));
	charged = atomic_long_read(&s->owner->model.charged);
	dropped = mc16_owner_drop_folio_held(&f->lookup, s->owner->folio, 1, 0);
	if (dropped)
		s->refs[0] = (struct memcomp_ref) { };
	KUNIT_ASSERT_TRUE(test, dropped);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			charged - record_charge);
	/* This is the last persistent PTE reference; writer is still live. */
	mc16_ref_put(&f->pte);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->writer.slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, f->writer.slot->word, f->snapshot.word);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 0U);
	mc16_test_writeback_load(test, f);
	swap = mc16_test_writeback_backing(f, true);
	reclaims = atomic_read(&memcomp_kunit_slot_reclaims);
	uncharges = atomic_read(&s->owner->model.uncharge_calls);
	KUNIT_ASSERT_EQ(test, mc16_test_writeback_settle(f, &swap), 0);
	mc16_test_writeback_swapped(test, f, &swap, false);
	mc16_writeback_finish(&f->writer, &f->tail);
	mc16_writeback_finish(&f->writer, &f->tail);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.uncharge_calls),
			uncharges + 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&s->owner->model.charged),
			charged - record_charge - f->bytes);
	mc16_ref_put(&f->writer);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims), reclaims + 1);
	KUNIT_EXPECT_EQ(test, s->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, s->payload.bad_lifetime);
	KUNIT_EXPECT_FALSE(test, mc16_get(token, &f->lookup));
}
#endif /* CONFIG_MEMCG && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_WRITEBACK_TEST_H */
