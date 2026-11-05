/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SWAPOFF_TEST_H
#define _MM_MEMCOMP_TESTS_SWAPOFF_TEST_H
#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)

struct mc16_test_swap_restore {
	struct mc16_test_refs *refs;
	struct memcomp_ref guard;
	struct mc16_swapoff_tail tail;
	u8 *page;
	u8 *original;
	int read_error;
	int alloc_error;
	unsigned int reads;
	unsigned int allocations;
	unsigned int frees;
	bool interrupted;
	bool fail_charge;
	unsigned int race_kind;
	bool drop_owner;
};

static struct mc16_test_swap_restore *mc16_test_swap_current;

static void mc16_test_swap_restore_cleanup(void *data)
{
	struct mc16_test_swap_restore *f = data;
	bool deferred = !!f->tail.swap.val;
	int puts = atomic_read(&memcomp_kunit_slot_puts);

	f->refs->storage->owner->model.fail_at = 0;
	mc16_test_swap_current = NULL;
	/* A fatal test assertion may interrupt commit-before-finish. */
	mc16_swapoff_finish(&f->tail);
	if (deferred)
		KUNIT_EXPECT_EQ(f->refs->storage->test,
				atomic_read(&memcomp_kunit_slot_puts) - puts, 1);
	if (f->guard.slot)
		mc16_ref_put(&f->guard);
}

static int mc16_test_swap_read(swp_entry_t swap, void *dst)
{
	struct mc16_test_swap_restore *f = mc16_test_swap_current;

	f->reads++;
	if (f->read_error)
		return f->read_error;
	memcpy(dst, f->page, PAGE_SIZE);
	return 0;
}

static int mc16_test_swap_allocate(const void *src, unsigned int *bytes,
				   unsigned long *handle, u8 *pool, void *data)
{
	struct mc16_test_swap_restore *f = data;
	struct mc16_test_storage *s = f->refs->storage;
	struct mc16_view view;
	int ret;

	f->allocations++;
	if (f->alloc_error)
		return f->alloc_error;
	*pool = 0;
	ret = memcomp_prepare_payload(src, PAGE_SIZE, bytes, handle, pool,
				      MEMCOMP_PAYLOAD_MUST_SUCCEED, false);
	if (ret)
		return ret;
	if (f->fail_charge)
		s->owner->model.fail_at = atomic_read(&s->owner->model.charge_calls) + 1;
	if (f->race_kind) {
		struct mc16_swap_fields old;
		u64 word;
		u16 length;

		mutex_lock(memcomp_lock_of(f->guard.token));
		mc16_view_locked(&f->guard, &view);
		mc16_swap_decode(*view.word, *view.length, &old);
		if (f->race_kind == 1) {
			memcomp_slot_put(swp_entry(old.type, old.swap_offset));
			old.swap_offset++;
		} else if (f->race_kind == 2) {
			old.length++;
			memcomp_compressed_bytes_add(1);
		} else if (f->race_kind == 3) {
			old.byte_offset++;
		} else {
			old.packed = false;
		}
		mc16_swap_encode(old.type, old.swap_offset, old.byte_offset,
				 old.length, old.packed, &word, &length);
		*view.word = word;
		*view.length = length;
		mutex_unlock(memcomp_lock_of(f->guard.token));
	}
	if (f->drop_owner) {
		struct memcomp_ref lookup;

		if (!mc16_get(f->guard.token, &lookup))
			return -EIO;
		if (!mc16_owner_drop_folio_held(&lookup, s->owner->folio, 1, 0))
			return -EIO;
		/* The callback consumed the initial OWNER, not the restore guard. */
		s->refs[0] = (struct memcomp_ref) { };
	}
	return 0;
}

static void mc16_test_swap_free(u8 pool, unsigned long handle, void *data)
{
	struct mc16_test_swap_restore *f = data;

	f->frees++;
	memcomp_zs_free(pool, handle);
}

static bool mc16_test_swap_interrupted(void *data)
{
	return ((struct mc16_test_swap_restore *)data)->interrupted;
}

static struct memcomp_swapoff_ops
mc16_test_swap_ops(struct mc16_test_swap_restore *f)
{
	return (struct memcomp_swapoff_ops) {
		.read_page = mc16_test_swap_read,
		.alloc_payload = mc16_test_swap_allocate,
		.free_payload = mc16_test_swap_free,
		.interrupted = mc16_test_swap_interrupted, .ctx = f,
	};
}

static struct mc16_test_swap_restore *
mc16_test_swap_restore_begin(struct kunit *test, bool packed, bool samefill)
{
	struct mc16_test_swap_restore *f;
	struct mc16_test_storage *s;
	struct memcomp_ref *ref;
	struct mc16_view view;
	unsigned long handle = 0;
	unsigned int bytes = PAGE_SIZE, i;
	u64 word;
	u16 length;
	u8 pool = 0;
	void *mapped;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs || mc16_test_refs_reserve(f->refs, 1))
		return NULL;
	if (kunit_add_action_or_reset(test, mc16_test_swap_restore_cleanup, f))
		return NULL;
	s = f->refs->storage;
	ref = &s->refs[0];
	f->page = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	f->original = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->page || !f->original)
		return NULL;
	for (i = 0; i < PAGE_SIZE; i++)
		f->original[i] = samefill ? 0xab : i % 53;
	if (packed) {
		ret = memcomp_prepare_payload(f->original, PAGE_SIZE, &bytes,
					      &handle, &pool,
					      MEMCOMP_PAYLOAD_MUST_SUCCEED, false);
		if (ret)
			return NULL;
		if (!bytes || bytes >= PAGE_SIZE - 37) {
			if (bytes)
				memcomp_zs_free(pool, handle);
			return NULL;
		}
		mapped = zs_map_object(memcomp_pools[0], handle, ZS_MM_RO);
		memcpy(f->page + 37, mapped, bytes);
		zs_unmap_object(memcomp_pools[0], handle);
		memcomp_zs_free(pool, handle);
	} else {
		memcpy(f->page, f->original, PAGE_SIZE);
	}
	if (!mc16_swap_encode(0, 0x567, packed ? 37 : 0, bytes,
			      packed, &word, &length))
		return NULL;
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	mc16_account_state(ref, false);
	folio_put(view.member->owner_folio);
	*view.word = word;
	*view.length = length;
	*view.flags = MC16_SWAPPED | MC16_F_OWNER |
		      MC16_F_REFERENCED | MC16_F_INCOMPRESSIBLE;
	memcomp_debug_dec(&memcomp_pending_pages);
	memcomp_debug_long_inc(&memcomp_swapped_entries);
	memcomp_compressed_bytes_add(bytes);
	mc16_account_state(ref, true);
	/* A pure pool restore must not consume the user shadow. */
	*mc16_shadow_ptr(ref) = (unsigned long)xa_mk_value(123);
	mutex_unlock(memcomp_lock_of(ref->token));
	if (!mc16_get(ref->token, &f->guard))
		return NULL;
	mc16_test_swap_current = f;
	return f;
}

static void mc16_test_swap_restore_roundtrip(struct kunit *test, bool packed,
					     bool samefill, bool drop_owner)
{
	struct mc16_test_swap_restore *f;
	struct memcomp_swapoff_ops ops;
	struct mc16_view view;
	struct mc16_key *key;
	u32 token;
	unsigned short owner_id;
	u64 shadow;
	u8 *dst;
	int puts, dups;

	f = mc16_test_swap_restore_begin(test, packed, samefill);
	KUNIT_ASSERT_NOT_NULL(test, f);
	ops = mc16_test_swap_ops(f);
	f->drop_owner = drop_owner;
	puts = atomic_read(&memcomp_kunit_slot_puts);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	key = f->guard.cluster->key;
	owner_id = key->owner_id;
	token = f->guard.token;
	mutex_lock(memcomp_lock_of(token));
	shadow = *mc16_shadow_ptr(&f->guard);
	mutex_unlock(memcomp_lock_of(token));
	KUNIT_ASSERT_EQ(test, mc16_swapoff_entry(&f->guard, 0, &ops), 1);
	mutex_lock(memcomp_lock_of(token));
	KUNIT_EXPECT_EQ(test, *mc16_shadow_ptr(&f->guard), shadow);
	KUNIT_EXPECT_EQ(test, f->guard.token, token);
	KUNIT_EXPECT_PTR_EQ(test, f->guard.cluster->key, key);
	KUNIT_EXPECT_EQ(test, f->guard.cluster->key->owner_id, owner_id);
	mutex_unlock(memcomp_lock_of(token));
	KUNIT_EXPECT_EQ(test, f->reads, 1U);
	KUNIT_EXPECT_EQ(test, f->allocations, 1U);
	KUNIT_EXPECT_EQ(test, f->frees, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts, 2);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups) - dups, 1);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	KUNIT_ASSERT_EQ(test, mc16_load_page(&f->guard, dst), 0);
	KUNIT_EXPECT_EQ(test, memcmp(dst, f->original, PAGE_SIZE), 0);
	mutex_lock(memcomp_lock_of(f->guard.token));
	if (mc16_view_locked(&f->guard, &view)) {
		KUNIT_EXPECT_EQ(test, mc16_flags_state(*view.flags),
				(unsigned int)(samefill ? MC16_SAMEFILL : MC16_STORED));
		KUNIT_EXPECT_EQ(test, !!view.record, !drop_owner);
		KUNIT_EXPECT_EQ(test, *view.flags & (MC16_F_REFERENCED |
				 MC16_F_INCOMPRESSIBLE),
				(u16)(MC16_F_REFERENCED | MC16_F_INCOMPRESSIBLE));
	}
	mutex_unlock(memcomp_lock_of(f->guard.token));
}

static void memcomp_test_swapoff_raw(struct kunit *test)
{
	mc16_test_swap_restore_roundtrip(test, false, false, false);
}

static void memcomp_test_swapoff_packed(struct kunit *test)
{
	mc16_test_swap_restore_roundtrip(test, true, false, false);
}

static void memcomp_test_swapoff_samefill(struct kunit *test)
{
	mc16_test_swap_restore_roundtrip(test, false, true, false);
}

static void memcomp_test_swapoff_owner_race(struct kunit *test)
{
	mc16_test_swap_restore_roundtrip(test, true, false, true);
}

static void memcomp_test_swapoff_failures(struct kunit *test)
{
	struct mc16_test_swap_restore *f = mc16_test_swap_restore_begin(test, false, false);
	struct memcomp_swapoff_ops ops;
	struct mc16_view view;
	u64 word;
	u16 length;
	int puts, dups;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ops = mc16_test_swap_ops(f);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	f->interrupted = true;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_entry(&f->guard, 0, &ops), -EINTR);
	f->interrupted = false;
	f->read_error = -EIO;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_entry(&f->guard, 0, &ops), -EIO);
	f->read_error = 0;
	f->alloc_error = -ENOMEM;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_entry(&f->guard, 0, &ops), -ENOMEM);
	f->alloc_error = 0;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_entry(&f->guard, 1, &ops), 0);
	f->fail_charge = true;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_entry(&f->guard, 0, &ops), -ENOMEM);
	f->fail_charge = false;
	f->refs->storage->owner->model.fail_at = 0;
	KUNIT_EXPECT_EQ(test, f->frees, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups) - dups, 5);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts, 5);
	mutex_lock(memcomp_lock_of(f->guard.token));
	mc16_view_locked(&f->guard, &view);
	word = *view.word;
	length = *view.length;
	mutex_unlock(memcomp_lock_of(f->guard.token));
	KUNIT_EXPECT_EQ(test, mc16_flags_state(f->guard.slot->flags),
			(unsigned int)MC16_PENDING);
	KUNIT_EXPECT_TRUE(test, mc16_swap_decode(word, length,
						 &(struct mc16_swap_fields) { }));
	KUNIT_EXPECT_EQ(test, f->guard.cluster->eligible_slots, (u16)0);
}

static void mc16_test_swapoff_backing_race(struct kunit *test, unsigned int kind)
{
	struct mc16_test_swap_restore *f = mc16_test_swap_restore_begin(test, true, false);
	struct memcomp_swapoff_ops ops;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ops = mc16_test_swap_ops(f);
	f->race_kind = kind;
	if (kind == 4) {
		struct mc16_view view;
		struct mc16_swap_fields fields;

		mutex_lock(memcomp_lock_of(f->guard.token));
		mc16_view_locked(&f->guard, &view);
		mc16_swap_decode(*view.word, *view.length, &fields);
		memmove(f->page, f->page + fields.byte_offset, fields.length);
		mc16_swap_encode(fields.type, fields.swap_offset, 0, fields.length,
				 true, view.word, view.length);
		mutex_unlock(memcomp_lock_of(f->guard.token));
	}
	KUNIT_EXPECT_EQ(test, mc16_swapoff_entry(&f->guard, 0, &ops), 0);
	KUNIT_EXPECT_EQ(test, f->frees, 1U);
	KUNIT_EXPECT_EQ(test, f->guard.cluster->eligible_slots, (u16)0);
	KUNIT_EXPECT_EQ(test, f->refs->storage->owner->model.fail_at, 0);
}

static void memcomp_test_swapoff_changed_backing(struct kunit *test)
{
	mc16_test_swapoff_backing_race(test, 1);
}

static void memcomp_test_swapoff_changed_length(struct kunit *test)
{
	mc16_test_swapoff_backing_race(test, 2);
}

static void memcomp_test_swapoff_changed_offset(struct kunit *test)
{
	mc16_test_swapoff_backing_race(test, 3);
}

static void memcomp_test_swapoff_changed_packed(struct kunit *test)
{
	mc16_test_swapoff_backing_race(test, 4);
}

static void memcomp_test_swapoff_invalid_commit(struct kunit *test)
{
	struct mc16_test_swap_restore *f = mc16_test_swap_restore_begin(test, false, false);
	struct mc16_swapoff_tail tail = { .swap = { .val = 123 } };
	struct mc16_payload payload = { .word = 1, .bytes = PAGE_SIZE + 1 };
	struct mc16_view view;
	u64 word;
	u16 length;
	int charges;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mutex_lock(memcomp_lock_of(f->guard.token));
	mc16_view_locked(&f->guard, &view);
	word = *view.word;
	length = *view.length;
	mutex_unlock(memcomp_lock_of(f->guard.token));
	charges = atomic_read(&f->refs->storage->owner->model.charge_calls);
	KUNIT_EXPECT_EQ(test, mc16_swapoff_commit(&f->guard, word, length,
						  &payload, GFP_KERNEL, &tail), -EINVAL);
	payload.bytes = 1;
	payload.word = 0;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_commit(&f->guard, word, length,
						  &payload, GFP_KERNEL, &tail), -EINVAL);
	payload.word = 1;
	payload.charge = 1;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_commit(&f->guard, word, length,
						  &payload, GFP_KERNEL, &tail), -EINVAL);
	payload.charge = 0;
	KUNIT_EXPECT_EQ(test, mc16_swapoff_commit(&f->guard, 0, length,
						  &payload, GFP_KERNEL, &tail), -EINVAL);
	KUNIT_EXPECT_EQ(test, mc16_swapoff_commit(&f->guard, word, U16_MAX,
						  &payload, GFP_KERNEL, &tail), -EINVAL);
	KUNIT_EXPECT_EQ(test, tail.swap.val, 123UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->refs->storage->owner->model.charge_calls),
			charges);
}

static void memcomp_test_swapoff_raw_commit(struct kunit *test)
{
	struct mc16_test_swap_restore *f = mc16_test_swap_restore_begin(test, true, false);
	struct mc16_swapoff_tail *tail;
	struct mc16_payload payload = { };
	struct mc16_view view;
	unsigned long handle;
	unsigned int bytes;
	u64 word;
	u16 length;
	int ret, puts;

	KUNIT_ASSERT_NOT_NULL(test, f);
	tail = &f->tail;
	KUNIT_ASSERT_EQ(test, memcomp_alloc_raw_payload(0, f->original, PAGE_SIZE,
							&bytes, &handle, false, NULL), 0);
	payload = (struct mc16_payload) { .word = handle, .bytes = bytes };
	mutex_lock(memcomp_lock_of(f->guard.token));
	mc16_view_locked(&f->guard, &view);
	word = *view.word;
	length = *view.length;
	mutex_unlock(memcomp_lock_of(f->guard.token));
	ret = mc16_swapoff_commit(&f->guard, word, length, &payload,
				  GFP_NOIO | __GFP_NOWARN, tail);
	if (ret != 1) {
		memcomp_zs_free(0, handle);
		KUNIT_FAIL(test, "raw commit failed: %d", ret);
		return;
	}
	KUNIT_EXPECT_EQ(test, payload.word, 0ULL);
	{
		struct memcomp_ref lookup;
		bool dropped;

		KUNIT_ASSERT_TRUE(test, mc16_get(f->guard.token, &lookup));
		dropped = mc16_owner_drop_folio_held(&lookup,
						     f->refs->storage->owner->folio, 1, 0);
		KUNIT_ASSERT_TRUE(test, dropped);
		f->refs->storage->refs[0] = (struct memcomp_ref) { };
	}
	puts = atomic_read(&memcomp_kunit_slot_puts);
	mc16_swapoff_finish(tail);
	mc16_swapoff_finish(tail);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts, 1);
	KUNIT_EXPECT_EQ(test, tail->swap.val, 0UL);
	KUNIT_EXPECT_EQ(test, f->guard.cluster->eligible_slots, (u16)1);
	KUNIT_ASSERT_EQ(test, mc16_load_page(&f->guard, f->page), 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->page, f->original, PAGE_SIZE), 0);
}

static void mc16_test_swapoff_bad_flags(struct kunit *test, bool stable)
{
	struct mc16_test_swap_restore *f = mc16_test_swap_restore_begin(test, false, false);
	struct mc16_swapoff_tail tail = { };
	struct mc16_payload payload;
	struct memcomp_swapoff_ops ops;
	struct mc16_view view;
	unsigned long handle;
	unsigned int bytes, i;
	u64 word;
	u16 length, saved;
	long charged;
	int ret, dups, puts;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ops = mc16_test_swap_ops(f);
	if (stable) {
		struct memcomp_ref lookup;
		bool dropped;

		KUNIT_ASSERT_TRUE(test, mc16_get(f->guard.token, &lookup));
		dropped = mc16_owner_drop_folio_held(&lookup,
						     f->refs->storage->owner->folio, 1, 0);
		KUNIT_ASSERT_TRUE(test, dropped);
		f->refs->storage->refs[0] = (struct memcomp_ref) { };
	}
	KUNIT_ASSERT_EQ(test, memcomp_alloc_raw_payload(0, f->original, PAGE_SIZE,
							&bytes, &handle, false, NULL), 0);
	payload = (struct mc16_payload) { .word = handle, .bytes = bytes };
	charged = atomic_long_read(&f->refs->storage->owner->model.charged);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	for (i = 0; i < 2; i++) {
		mutex_lock(memcomp_lock_of(f->guard.token));
		mc16_view_locked(&f->guard, &view);
		word = *view.word;
		length = *view.length;
		saved = *view.flags;
		*view.flags = i ? (saved & ~MC16_ORDER_MASK) | MC16_ORDER_MASK :
				saved | BIT(15);
		mutex_unlock(memcomp_lock_of(f->guard.token));
		KUNIT_EXPECT_EQ(test, mc16_swapoff_entry(&f->guard, 0, &ops), -EIO);
		ret = mc16_swapoff_commit(&f->guard, word, length, &payload,
					  GFP_KERNEL, &tail);
		KUNIT_EXPECT_EQ(test, ret, -EIO);
		KUNIT_EXPECT_EQ(test, payload.word, (u64)handle);
		KUNIT_EXPECT_EQ(test, tail.swap.val, 0UL);
		KUNIT_EXPECT_EQ(test,
				atomic_long_read(&f->refs->storage->owner->model.charged), charged);
		mutex_lock(memcomp_lock_of(f->guard.token));
		mc16_view_locked(&f->guard, &view);
		*view.flags = saved;
		mutex_unlock(memcomp_lock_of(f->guard.token));
	}
	memcomp_zs_free(0, handle);
	KUNIT_EXPECT_EQ(test, f->reads, 0U);
	KUNIT_EXPECT_EQ(test, f->allocations, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups), dups);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts), puts);
	KUNIT_EXPECT_EQ(test, f->guard.cluster->eligible_slots, (u16)0);
}

static void memcomp_test_swapoff_bad_member_flags(struct kunit *test)
{
	mc16_test_swapoff_bad_flags(test, false);
}

static void memcomp_test_swapoff_bad_stable_flags(struct kunit *test)
{
	mc16_test_swapoff_bad_flags(test, true);
}

/* Return without finishing: cleanup must settle tail before dropping guards. */
static void memcomp_test_swapoff_deferred_tail(struct kunit *test)
{
	struct mc16_test_swap_restore *f = mc16_test_swap_restore_begin(test, true, false);
	struct mc16_payload payload;
	struct mc16_view view;
	unsigned long handle;
	unsigned int bytes;
	u64 word;
	u16 length;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, memcomp_alloc_raw_payload(0, f->original, PAGE_SIZE,
							&bytes, &handle, false, NULL), 0);
	payload = (struct mc16_payload) { .word = handle, .bytes = bytes };
	mutex_lock(memcomp_lock_of(f->guard.token));
	mc16_view_locked(&f->guard, &view);
	word = *view.word;
	length = *view.length;
	mutex_unlock(memcomp_lock_of(f->guard.token));
	ret = mc16_swapoff_commit(&f->guard, word, length, &payload,
				  GFP_KERNEL, &f->tail);
	if (ret != 1) {
		memcomp_zs_free(0, handle);
		KUNIT_FAIL(test, "deferred commit failed: %d", ret);
		return;
	}
	KUNIT_EXPECT_NE(test, f->tail.swap.val, 0UL);
	/* Exactly the same cleanup path as a fatal assertion after commit. */
}
#endif /* CONFIG_MEMCG && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_SWAPOFF_TEST_H */
