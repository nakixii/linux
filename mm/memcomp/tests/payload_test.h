/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_PAYLOAD_TEST_H
#define _MM_MEMCOMP_TESTS_PAYLOAD_TEST_H
#ifdef CONFIG_MEMCG
struct mc16_test_payload {
	struct mc16_test_refs *refs;
	struct mc16_payload prepared;
	struct mc16_payload_tail tail;
};

static void mc16_test_payload_cleanup(void *data)
{
	struct mc16_test_payload *f = data;
	struct memcomp_ref *ref = &f->refs->storage->refs[0];

	mc16_payload_finish(ref, &f->tail);
	mc16_payload_abort(ref, &f->prepared);
}

static struct mc16_test_payload *mc16_test_payload_begin(struct kunit *test)
{
	struct mc16_test_payload *f;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->refs = mc16_test_refs_begin(test);
	if (!f->refs)
		return NULL;
	ret = mc16_test_refs_reserve(f->refs, 1);
	if (ret)
		return NULL;
	ret = kunit_add_action_or_reset(test, mc16_test_payload_cleanup, f);
	return ret ? NULL : f;
}

static int mc16_test_payload_prepare(struct mc16_test_payload *f, u32 bytes,
				     u64 pattern)
{
	struct memcomp_ref *ref = &f->refs->storage->refs[0];
	unsigned long handle;
	int ret;

	if (bytes) {
		ret = memcomp_zs_malloc(memcomp_pools[0], bytes, &handle, false);
		if (ret)
			return ret;
		pattern = handle;
	}
	f->prepared = (struct mc16_payload) { .word = pattern, .bytes = bytes };
	return mc16_payload_charge(ref, &f->prepared);
}

static int mc16_test_payload_commit(struct mc16_test_payload *f,
				    struct folio *folio)
{
	struct memcomp_ref *ref = &f->refs->storage->refs[0];
	int ret;

	mutex_lock(memcomp_lock_of(ref->token));
	ret = mc16_payload_commit_locked(ref, folio, 1, 0,
					 &f->prepared, &f->tail);
	mutex_unlock(memcomp_lock_of(ref->token));
	return ret;
}

static void memcomp_test_payload_samefill(struct kunit *test)
{
	const u64 pattern = 0xfedcba9876543210ULL;
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	struct mc16_view view;
	u64 word;
	u16 flags;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, 0, pattern), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, storage->owner->folio), 0);
	KUNIT_EXPECT_EQ(test, f->prepared.word, 0ULL);
	mc16_payload_finish(ref, &f->tail);
	mc16_payload_finish(ref, &f->tail); /* Empty tail must be harmless. */
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	word = *view.word;
	flags = *view.flags;
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_EQ(test, word, pattern);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(flags), (unsigned int)MC16_SAMEFILL);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(ref->slot->flags),
			(unsigned int)MC16_PENDING);
	KUNIT_EXPECT_FALSE(test, flags & MC16_F_RESIDENT);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), 1);
	KUNIT_EXPECT_EQ(test, ref->cluster->eligible_slots, (u16)0);
}

static void memcomp_test_payload_replace(struct kunit *test)
{
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	long charged;
	u64 handle;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	charged = atomic_long_read(&storage->owner->model.charged);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, PAGE_SIZE, 0), 0);
	handle = f->prepared.word;
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, storage->owner->folio), 0);
	mc16_payload_finish(ref, &f->tail);
	KUNIT_EXPECT_EQ(test, ref->cluster->eligible_slots, (u16)1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged),
			charged + PAGE_SIZE);

	storage->payload.task = current;
	storage->payload.pool = memcomp_pools[0];
	storage->payload.handle = handle;
	storage->payload.live = true;
	rcu_assign_pointer(memcomp_kunit_async_probe, &storage->payload);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, 0, U64_MAX), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, storage->owner->folio), 0);
	mc16_payload_finish(ref, &f->tail);
	KUNIT_EXPECT_EQ(test, storage->payload.frees, 1U);
	KUNIT_EXPECT_FALSE(test, storage->payload.live);
	KUNIT_EXPECT_FALSE(test, storage->payload.bad_lifetime);
	KUNIT_EXPECT_EQ(test, ref->cluster->eligible_slots, (u16)0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged),
			charged);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), 1);
}

static void memcomp_test_payload_stale(struct kunit *test)
{
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	long charged;
	u64 handle, record;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	charged = atomic_long_read(&storage->owner->model.charged);
	record = ref->slot->word;
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, 128, 0), 0);
	handle = f->prepared.word;
	KUNIT_EXPECT_EQ(test, mc16_test_payload_commit(f, NULL), -ESTALE);
	KUNIT_EXPECT_EQ(test, ref->slot->word, record);
	KUNIT_EXPECT_EQ(test, f->prepared.word, handle);
	KUNIT_EXPECT_EQ(test, f->prepared.charge, 128U);
	KUNIT_EXPECT_EQ(test, f->tail.flags, (u16)0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(storage->owner->folio), 2);
	mc16_payload_abort(ref, &f->prepared);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged),
			charged);
}

static void memcomp_test_payload_charge_failure(struct kunit *test)
{
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	long charged;
	u64 record;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	charged = atomic_long_read(&storage->owner->model.charged);
	record = ref->slot->word;
	storage->owner->model.fail_at =
		atomic_read(&storage->owner->model.charge_calls) + 1;
	KUNIT_EXPECT_EQ(test, mc16_test_payload_prepare(f, 128, 0), -ENOMEM);
	KUNIT_EXPECT_EQ(test, f->prepared.charge, 0U);
	KUNIT_EXPECT_EQ(test, ref->slot->word, record);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&storage->owner->model.charged),
			charged);
	mc16_payload_abort(ref, &f->prepared);
	storage->owner->model.fail_at = 0;
}

static void memcomp_test_payload_writeback(struct kunit *test)
{
	struct mc16_test_payload *f = mc16_test_payload_begin(test);
	struct mc16_test_storage *storage;
	struct memcomp_ref *ref;
	struct mc16_view view;
	u64 word;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->refs->storage;
	ref = &storage->refs[0];
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, PAGE_SIZE, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_payload_commit(f, storage->owner->folio), 0);
	mc16_payload_finish(ref, &f->tail);
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	word = *view.word;
	*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_WRITEBACK;
	mc16_fifo_transition(ref, MC16_STORED, MC16_WRITEBACK);
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_ASSERT_EQ(test, mc16_test_payload_prepare(f, 0, U64_MAX), 0);
	KUNIT_EXPECT_EQ(test, mc16_test_payload_commit(f, storage->owner->folio),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, f->prepared.word, U64_MAX);
	KUNIT_EXPECT_EQ(test, f->tail.flags, (u16)0);
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	KUNIT_EXPECT_EQ(test, *view.word, word);
	KUNIT_EXPECT_EQ(test, mc16_flags_state(*view.flags),
			(unsigned int)MC16_WRITEBACK);
	mutex_unlock(memcomp_lock_of(ref->token));
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_PAYLOAD_TEST_H */
