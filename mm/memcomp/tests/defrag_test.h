/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_DEFRAG_TEST_H
#define _MM_MEMCOMP_TESTS_DEFRAG_TEST_H

#include <kunit/static_stub.h>
#include "../defrag.h"

/*
 * Include after consumers_test.h for its unlocked accounting/probe fixture.
 * Backing IO and slot counts are modeled. Encodings, zs objects, byte copies,
 * decompression, guards, charging and the scanner are real. No device is used.
 */
#if defined(CONFIG_MEMCG) && defined(CONFIG_SWAP)
#define MC16_DEFRAG_TEST_SLOTS (MEMCOMP_WRITEBACK_DEFRAG_SCAN + 4)

struct mc16_defrag_test {
	struct mc16_consumer_test *base;
	struct memcomp_ref refs[MC16_DEFRAG_TEST_SLOTS];
	struct memcomp_ref guard;
	struct memcomp_kunit_async_probe payload;
	unsigned int nr;
	u32 markers[2];
	u8 *image;
	u8 *original;
	u32 bytes;
	int count;
	int read_error;
	bool fail_buffer;
	bool fail_alloc;
	bool fail_charge;
	bool drop_owner;
	unsigned int race;
	unsigned int reads;
	unsigned int copies;
	bool callback_under_t;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	long swapped_before;
#endif
};

static struct mc16_defrag_test *mc16_defrag_test_current;

static void mc16_defrag_test_cleanup(void *data)
{
	struct mc16_defrag_test *f = data;
	unsigned int i;

	f->base->model.fail_at = 0;
	if (mc16_defrag_test_current == f)
		mc16_defrag_test_current = NULL;
	if (f->guard.slot)
		mc16_ref_put(&f->guard);
	for (i = 0; i < f->nr; i++)
		if (f->refs[i].slot)
			mc16_ref_put(&f->refs[i]);
	for (i = 0; i < ARRAY_SIZE(f->markers); i++)
		if (f->markers[i])
			xa_erase_irq(&mc16_clusters, f->markers[i]);
	rcu_barrier();
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	KUNIT_EXPECT_EQ(f->base->test, atomic_long_read(&memcomp_swapped_entries),
			f->swapped_before);
#endif
	if (rcu_access_pointer(memcomp_kunit_async_probe) == &f->payload) {
		rcu_assign_pointer(memcomp_kunit_async_probe, NULL);
		synchronize_rcu();
	}
}

static struct mc16_defrag_test *mc16_defrag_test_begin(struct kunit *test)
{
	struct mc16_consumer_test *base = mc16_consumer_test_begin(test);
	struct mc16_defrag_test *f;
	unsigned long handle = 0;
	unsigned int i;
	void *mapped;
	u8 pool = 0;
	int ret;

	if (!base)
		return NULL;
	if (rcu_access_pointer(memcomp_kunit_async_probe)) {
		KUNIT_FAIL(test, "defrag fixture requires an idle payload probe");
		return NULL;
	}
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->base = base;
	f->count = 1;
	f->payload.task = current;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	f->swapped_before = atomic_long_read(&memcomp_swapped_entries);
#endif
	if (kunit_add_action_or_reset(test, mc16_defrag_test_cleanup, f))
		return NULL;
	f->image = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	f->original = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->image || !f->original)
		return NULL;
	for (i = 0; i < PAGE_SIZE; i++)
		f->original[i] = i % 53;
	ret = memcomp_prepare_payload(f->original, PAGE_SIZE, &f->bytes, &handle,
				      &pool, MEMCOMP_PAYLOAD_MUST_SUCCEED, false);
	if (ret)
		return NULL;
	if (!f->bytes || f->bytes >= PAGE_SIZE - 38 || pool) {
		if (f->bytes)
			memcomp_zs_free(pool, handle);
		return NULL;
	}
	mapped = zs_map_object(memcomp_pools[0], handle, ZS_MM_RO);
	if (mapped) {
		memcpy(f->image + 37, mapped, f->bytes);
		zs_unmap_object(memcomp_pools[0], handle);
	}
	memcomp_zs_free(0, handle);
	return mapped ? f : NULL;
}

/* 0 samefill/noncandidate, 1 stable packed, 2 ordinary, 3 attached packed. */
static int mc16_defrag_test_insert(struct mc16_defrag_test *f, unsigned int mode)
{
	struct mc16_initial_payload initial = { .word = 0x12345678 };
	struct mc16_consumer_test *b = f->base;
	struct memcomp_ref *ref;
	struct mc16_view view;
	u64 word;
	u16 length;
	int ret;

	if (f->nr == ARRAY_SIZE(f->refs))
		return -ENOSPC;
	ref = &f->refs[f->nr];
	if (mode == 3) {
		ret = mc16_consumer_test_reserve(b, 0);
		if (ret)
			return ret;
		*ref = b->owners[0];
		b->owners[0] = (struct memcomp_ref) { };
	} else {
		ret = mc16_reserve_cache(&b->model.objcg, folio_nid(b->account),
					 &initial, 1, ref);
		if (ret)
			return ret;
		mc16_cache_publish(ref, 0);
	}
	f->nr++;
	if (!mode)
		return 0;
	if (!mc16_swap_encode(0, 0x567 + f->nr, mode == 2 ? 0 : 37,
			      f->bytes, mode != 2, &word, &length))
		return -EINVAL;
	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view)) {
		ret = -EIO;
		goto unlock;
	}
	mc16_account_state(ref, false);
	if (*view.flags & MC16_F_RESIDENT) {
		folio_put(view.member->owner_folio);
		memcomp_debug_dec(&memcomp_pending_pages);
	}
	*view.word = word;
	*view.length = length;
	*view.flags = MC16_SWAPPED | (*view.flags & MC16_F_OWNER) |
		      MC16_F_REFERENCED | MC16_F_INCOMPRESSIBLE;
	memcomp_compressed_bytes_add(f->bytes);
	memcomp_debug_long_inc(&memcomp_swapped_entries);
	mc16_account_state(ref, true);
	ret = 0;
unlock:
	mutex_unlock(memcomp_lock_of(ref->token));
	return ret;
}

static int mc16_defrag_test_count(swp_entry_t swap)
{
	return swap.val ? mc16_defrag_test_current->count : 0;
}

static int mc16_defrag_test_read(swp_entry_t swap, void *dst)
{
	struct mc16_defrag_test *f = mc16_defrag_test_current;

	f->reads++;
	if (!swap.val)
		return -EIO;
	if (f->guard.slot && mutex_is_locked(memcomp_lock_of(f->guard.token)))
		f->callback_under_t = true;
	if (f->read_error)
		return f->read_error;
	memcpy(dst, f->image, PAGE_SIZE);
	return 0;
}

static const struct memcomp_defrag_ops mc16_defrag_test_ops = {
	.slot_count = mc16_defrag_test_count,
	.read_page = mc16_defrag_test_read,
};

static void *mc16_defrag_test_buffer(void)
{
	if (mc16_defrag_test_current->fail_buffer)
		return NULL;
	return kmalloc(PAGE_SIZE, GFP_NOWAIT | __GFP_NOWARN);
}

static int mc16_defrag_test_copy(const void *src, unsigned int bytes,
				 struct mc16_payload *payload)
{
	struct mc16_defrag_test *f = mc16_defrag_test_current;
	struct memcomp_store_ctx store = { .kunit_async_probe = &f->payload };
	unsigned int copied = 0;
	unsigned long handle;
	int ret;

	f->copies++;
	if (f->fail_alloc)
		return -ENOMEM;
	ret = memcomp_alloc_raw_payload(0, src, bytes, &copied, &handle, false,
					f->guard.slot ? &store : NULL);
	if (ret)
		return ret;
	*payload = (struct mc16_payload) { .word = handle, .bytes = copied };
	if (f->fail_charge)
		f->base->model.fail_at = atomic_read(&f->base->model.charge_calls) + 1;
	if (f->race) {
		struct mc16_swap_fields fields;
		struct mc16_view view;
		u64 word;
		u16 length;

		mutex_lock(memcomp_lock_of(f->guard.token));
		ret = mc16_view_locked(&f->guard, &view) &&
			mc16_swap_decode(*view.word, *view.length, &fields) ? 0 : -EIO;
		if (!ret) {
			if (f->race == 1)
				fields.byte_offset++;
			else
				fields.length++;
			if (!mc16_swap_encode(fields.type, fields.swap_offset,
					      fields.byte_offset, fields.length,
					      fields.packed, &word, &length)) {
				ret = -EINVAL;
			} else {
				*view.word = word;
				*view.length = length;
				if (f->race == 2)
					memcomp_compressed_bytes_add(1);
			}
		}
		mutex_unlock(memcomp_lock_of(f->guard.token));
		if (ret)
			return ret;
	}
	if (f->drop_owner) {
		struct memcomp_ref owner;
		bool dropped = false;

		folio_lock(f->base->account);
		if (mc16_get(f->guard.token, &owner))
			dropped = mc16_owner_drop_folio_held(&owner,
							     f->base->account, 1, 0);
		folio_unlock(f->base->account);
		if (!dropped)
			return -EIO;
		f->refs[0] = (struct memcomp_ref) { };
	}
	return 0;
}

/* Stubs are disabled, and every T/folio lock is gone, before any assertion. */
static int mc16_defrag_test_run(struct mc16_defrag_test *f, u64 *cursor)
{
	struct kunit *test = f->base->test;
	int ret;

	mc16_defrag_test_current = f;
	if (f->guard.slot)
		rcu_assign_pointer(memcomp_kunit_async_probe, &f->payload);
	kunit_activate_static_stub(test, mc16_defrag_buffer_alloc,
				   mc16_defrag_test_buffer);
	kunit_activate_static_stub(test, mc16_defrag_copy_payload,
				   mc16_defrag_test_copy);
	ret = cursor ? mc16_defrag_scan(cursor, &mc16_defrag_test_ops) :
		mc16_defrag_entry(&f->guard, &mc16_defrag_test_ops);
	kunit_deactivate_static_stub(test, mc16_defrag_copy_payload);
	kunit_deactivate_static_stub(test, mc16_defrag_buffer_alloc);
	mc16_defrag_test_current = NULL;
	return ret;
}

static unsigned int mc16_defrag_test_state(struct memcomp_ref *ref,
					   u64 *word, u16 *length, bool *attached)
{
	struct mc16_view view;
	unsigned int state = MC16_EMPTY;

	mutex_lock(memcomp_lock_of(ref->token));
	if (mc16_view_locked(ref, &view)) {
		state = mc16_flags_state(*view.flags);
		if (word)
			*word = *view.word;
		if (length)
			*length = *view.length;
		if (attached)
			*attached = !!view.record;
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	return state;
}

static void memcomp_test_defrag_roundtrip(struct kunit *test)
{
	struct mc16_defrag_test *f = mc16_defrag_test_begin(test);
	u64 word;
	u16 length;
	void *mapped;
	bool same;
	s64 compressed;
	int puts, packed, dups, ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 1), 0);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[0].token, &f->guard));
	puts = atomic_read(&memcomp_kunit_slot_puts);
	packed = atomic_read(&memcomp_kunit_slot_reclaims);
	dups = atomic_read(&memcomp_kunit_slot_dups);
	compressed = atomic64_read(&memcomp_compressed_bytes);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_run(f, NULL), 1);
	KUNIT_EXPECT_FALSE(test, f->callback_under_t);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups) - dups, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims) - packed, 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_compressed_bytes), compressed);
	KUNIT_EXPECT_EQ(test, f->guard.cluster->eligible_slots, (u16)1);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_state(&f->guard, &word, &length, NULL),
			(unsigned int)MC16_STORED);
	KUNIT_EXPECT_EQ(test, (u32)length + 1, f->bytes);
	mapped = zs_map_object(memcomp_pools[0], word, ZS_MM_RO);
	if (!mapped) {
		KUNIT_FAIL(test, "copied payload did not map");
		return;
	}
	same = !memcmp(mapped, f->image + 37, f->bytes);
	zs_unmap_object(memcomp_pools[0], word);
	KUNIT_EXPECT_TRUE(test, same);
	ret = mc16_load_page(&f->guard, f->image);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->image, f->original, PAGE_SIZE), 0);
	mc16_ref_put(&f->guard);
	mc16_ref_put(&f->refs[0]);
	KUNIT_EXPECT_EQ(test, f->payload.allocs, 1U);
	KUNIT_EXPECT_EQ(test, f->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, f->payload.live);
	KUNIT_EXPECT_FALSE(test, f->payload.bad_lifetime);
}

static void memcomp_test_defrag_noncandidates(struct kunit *test)
{
	struct mc16_defrag_test *f = mc16_defrag_test_begin(test);
	int dups;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 1), 0);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[0].token, &f->guard));
	dups = atomic_read(&memcomp_kunit_slot_dups);
	f->count = 2;
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, NULL), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups), dups);
	mc16_ref_put(&f->guard);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 2), 0);
	f->count = 1;
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[1].token, &f->guard));
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, NULL), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_dups), dups);
}

/* Buffer/read/copy/charge errors and independent full-word/length races. */
static void mc16_defrag_test_failure(struct kunit *test, unsigned int failure)
{
	struct mc16_defrag_test *f = mc16_defrag_test_begin(test);
	u64 before_word, word;
	u16 before_length, length;
	long charge;
	int puts, packed, expected;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 1), 0);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[0].token, &f->guard));
	mc16_defrag_test_state(&f->guard, &before_word, &before_length, NULL);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	packed = atomic_read(&memcomp_kunit_slot_reclaims);
	charge = atomic_long_read(&f->base->model.charged);
	f->fail_buffer = failure == 0;
	f->read_error = failure == 1 ? -EIO : 0;
	f->fail_alloc = failure == 2;
	f->fail_charge = failure == 3;
	f->race = failure >= 4 ? failure - 3 : 0;
	expected = failure >= 4 ? 0 : failure == 1 ? -EIO : -ENOMEM;
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, NULL), expected);
	f->base->model.fail_at = 0;
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->base->model.charged), charge);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims) - packed, 0);
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_state(&f->guard, &word, &length, NULL),
			(unsigned int)MC16_SWAPPED);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), 2U);
	if (failure < 4) {
		KUNIT_EXPECT_EQ(test, word, before_word);
		KUNIT_EXPECT_EQ(test, length, before_length);
	} else if (failure == 4) {
		KUNIT_EXPECT_NE(test, word, before_word);
		KUNIT_EXPECT_EQ(test, length, before_length);
	} else {
		KUNIT_EXPECT_EQ(test, word, before_word);
		KUNIT_EXPECT_NE(test, length, before_length);
	}
	KUNIT_EXPECT_EQ(test, f->payload.allocs, failure >= 3 ? 1U : 0U);
	KUNIT_EXPECT_EQ(test, f->payload.frees, f->payload.allocs);
	KUNIT_EXPECT_FALSE(test, f->payload.live);
	KUNIT_EXPECT_FALSE(test, f->payload.bad_lifetime);
}

static void memcomp_test_defrag_buffer_failure(struct kunit *test)
{
	mc16_defrag_test_failure(test, 0);
}

static void memcomp_test_defrag_read_failure(struct kunit *test)
{
	mc16_defrag_test_failure(test, 1);
}

static void memcomp_test_defrag_alloc_failure(struct kunit *test)
{
	mc16_defrag_test_failure(test, 2);
}

static void memcomp_test_defrag_charge_failure(struct kunit *test)
{
	mc16_defrag_test_failure(test, 3);
}

static void memcomp_test_defrag_stale_word(struct kunit *test)
{
	mc16_defrag_test_failure(test, 4);
}

static void memcomp_test_defrag_stale_length(struct kunit *test)
{
	mc16_defrag_test_failure(test, 5);
}

static void memcomp_test_defrag_owner_normalize(struct kunit *test)
{
	struct mc16_defrag_test *f = mc16_defrag_test_begin(test);
	bool attached = false;
	int puts, packed;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 3), 0);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[0].token, &f->guard));
	mc16_defrag_test_state(&f->guard, NULL, NULL, &attached);
	KUNIT_ASSERT_TRUE(test, attached);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	packed = atomic_read(&memcomp_kunit_slot_reclaims);
	f->drop_owner = true;
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_run(f, NULL), 1);
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_state(&f->guard, NULL, NULL, &attached),
			(unsigned int)MC16_STORED);
	KUNIT_EXPECT_FALSE(test, attached);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->guard.slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->base->account), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims) - packed, 1);
	KUNIT_ASSERT_EQ(test, mc16_load_page(&f->guard, f->image), 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->image, f->original, PAGE_SIZE), 0);
}

static int mc16_defrag_test_marker(struct mc16_defrag_test *f, unsigned int i,
				   u32 id)
{
	unsigned long flags;
	u32 reserved;
	int ret;

	xa_lock_irqsave(&mc16_clusters, flags);
	ret = __xa_alloc(&mc16_clusters, &reserved, NULL, XA_LIMIT(id, id),
			 GFP_NOWAIT | __GFP_NOWARN);
	if (!ret)
		f->markers[i] = reserved;
	xa_unlock_irqrestore(&mc16_clusters, flags);
	return ret;
}

static int mc16_defrag_test_pair(struct mc16_defrag_test *f)
{
	struct mc16_initial_payload initial[2] = { { .word = 7 }, { .word = 8 } };
	unsigned int index = f->nr;
	int ret;

	ret = mc16_reserve_cache(&f->base->model.objcg,
				 folio_nid(f->base->account), initial, 2, &f->refs[index]);
	if (ret)
		return ret;
	mc16_cache_publish(&f->refs[index], 0);
	mc16_cache_publish(&f->refs[index + 1], 0);
	f->nr += 2;
	return 0;
}

static void memcomp_test_defrag_scan_budget(struct kunit *test)
{
	struct mc16_defrag_test *f = mc16_defrag_test_begin(test);
	u64 cursor = 0, first;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_marker(f, 0, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_marker(f, 1, 2), 0);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 0), 0);
	first = f->refs[0].token;
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_pair(f), 0);
	KUNIT_ASSERT_EQ(test, f->refs[1].token, f->refs[0].token + 2);
	for (i = f->nr; i < MEMCOMP_WRITEBACK_DEFRAG_SCAN + 1; i++)
		KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 0), 0);
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, &cursor), 1);
	/* Two XA markers and the real alignment hole all consume probes. */
	KUNIT_EXPECT_EQ(test, cursor, first + MEMCOMP_WRITEBACK_DEFRAG_SCAN - 2);
	KUNIT_EXPECT_EQ(test, f->reads, 0U);
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, &cursor), 0);
	KUNIT_EXPECT_EQ(test, cursor, 0ULL);
	for (i = 0; i < f->nr; i++)
		KUNIT_EXPECT_EQ(test, refcount_read(&f->refs[i].slot->refs), 1U);
	cursor = U64_MAX;
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, &cursor), 0);
	KUNIT_EXPECT_EQ(test, cursor, 0ULL);
}

static void memcomp_test_defrag_scan_moves(struct kunit *test)
{
	struct mc16_defrag_test *f = mc16_defrag_test_begin(test);
	u64 cursor = 0;
	unsigned int i;
	int puts, packed;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < MEMCOMP_WRITEBACK_DEFRAG_MOVE + 2; i++)
		KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 1), 0);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	packed = atomic_read(&memcomp_kunit_slot_reclaims);
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, &cursor), 1);
	KUNIT_EXPECT_EQ(test, f->reads, MEMCOMP_WRITEBACK_DEFRAG_MOVE);
	KUNIT_EXPECT_EQ(test, cursor,
			(u64)f->refs[MEMCOMP_WRITEBACK_DEFRAG_MOVE - 1].token + 1);
	for (i = 0; i < f->nr; i++)
		KUNIT_EXPECT_EQ(test, mc16_defrag_test_state(&f->refs[i], NULL, NULL, NULL),
				(unsigned int)(i < MEMCOMP_WRITEBACK_DEFRAG_MOVE ?
					       MC16_STORED : MC16_SWAPPED));
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, &cursor), 0);
	KUNIT_EXPECT_EQ(test, cursor, 0ULL);
	KUNIT_EXPECT_EQ(test, f->reads, MEMCOMP_WRITEBACK_DEFRAG_MOVE + 2);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts,
			(int)f->nr);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims) - packed,
			(int)f->nr);
}

static void memcomp_test_defrag_scan_error(struct kunit *test)
{
	struct mc16_defrag_test *f = mc16_defrag_test_begin(test);
	u64 cursor = 0;
	int puts, packed;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_defrag_test_insert(f, 1), 0);
	puts = atomic_read(&memcomp_kunit_slot_puts);
	packed = atomic_read(&memcomp_kunit_slot_reclaims);
	f->read_error = -EIO;
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, &cursor), 0);
	KUNIT_EXPECT_EQ(test, cursor, (u64)f->refs[0].token + 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->refs[0].slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_puts) - puts, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_kunit_slot_reclaims) - packed, 0);
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_state(&f->refs[0], NULL, NULL, NULL),
			(unsigned int)MC16_SWAPPED);
	/* A later outer-policy restart can move the unchanged survivor. */
	f->read_error = 0;
	cursor = 0;
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_run(f, &cursor), 0);
	KUNIT_EXPECT_EQ(test, cursor, 0ULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&f->refs[0].slot->refs), 1U);
	KUNIT_EXPECT_EQ(test, mc16_defrag_test_state(&f->refs[0], NULL, NULL, NULL),
			(unsigned int)MC16_STORED);
}
#endif /* CONFIG_MEMCG && CONFIG_SWAP */
#endif /* _MM_MEMCOMP_TESTS_DEFRAG_TEST_H */
