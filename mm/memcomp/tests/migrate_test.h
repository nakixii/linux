/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_MIGRATE_TEST_H
#define _MM_MEMCOMP_TESTS_MIGRATE_TEST_H

/* Include after consumers_test.h; register these CONFIG_MEMCG cases in main. */
#ifdef CONFIG_MEMCG
#define MC16_MIGT_NR 4

struct mc16_migt_saved {
	u64 word;
	u16 flags;
	u16 length;
};

struct mc16_migt_snapshot {
	struct mc16_migt_saved payload[MC16_MIGT_NR];
	u32 charge[MC16_MIGT_NR];
	int refs[MC16_MIGT_NR];
	int members[MC16_MIGT_NR];
	u16 nid[MC16_MIGT_NR];
	long charged;
	int charges;
	int uncharges;
	int gets;
	int puts;
	int src_refs;
	int dst_refs;
	s64 stored;
	s64 compressed;
	long eligible;
};

struct mc16_migt {
	struct mc16_consumer_test *base;
	struct memcomp_ref refs[MC16_MIGT_NR]; /* Initial OWNERs, not lookups. */
	u32 tokens[MC16_MIGT_NR];
	struct mc16_migt_saved saved[MC16_MIGT_NR];
	bool swapped[MC16_MIGT_NR];
	struct mc16_migt_snapshot before;
	unsigned int nr;
	u8 *data;
	u8 *loaded;
	unsigned int fail_index;
	bool race;
	bool changed;
	u16 race_flags;
	unsigned int trace[2 * MC16_MIGT_NR + 1];
	unsigned int nr_trace;
	bool all_pinned;
	bool old_owner_seen;
};

static struct mc16_migt *mc16_migt_current;

/* No swap IO is executed: temporarily install only its encoded representation.
 * The real zs payload is retained privately and restored before any release.
 * This checks that migration never touches backing ownership, not swap IO.
 */
static int mc16_migt_swapped(struct mc16_migt *f, unsigned int index, bool set)
{
	struct memcomp_ref *ref = &f->refs[index];
	struct mc16_view view;
	unsigned int old_state, new_state;
	u64 word = 0;
	u16 length = 0;
	int ret = 0;

	if (set && !mc16_swap_encode(0, 128 + index, 0,
				     f->before.payload[index].length + 1,
				     true, &word, &length))
		return -EINVAL;
	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view) || !view.member) {
		ret = -EINVAL;
		goto out;
	}
	old_state = mc16_flags_state(*view.flags);
	mc16_account_state(ref, false);
	if (set) {
		f->saved[index] = (struct mc16_migt_saved) {
			.word = *view.word, .length = *view.length, .flags = *view.flags,
		};
		*view.word = word;
		*view.length = length;
		*view.flags = (*view.flags & ~MC16_STATE_MASK) | MC16_SWAPPED;
		memcomp_debug_long_inc(&memcomp_swapped_entries);
	} else {
		*view.word = f->saved[index].word;
		*view.length = f->saved[index].length;
		*view.flags = f->saved[index].flags;
		memcomp_debug_long_dec(&memcomp_swapped_entries);
	}
	new_state = mc16_flags_state(*view.flags);
	mc16_fifo_transition(ref, old_state, new_state);
	mc16_account_state(ref, true);
	f->swapped[index] = set;
out:
	mutex_unlock(memcomp_lock_of(ref->token));
	return ret;
}

static void mc16_migt_restore_race(struct mc16_migt *f)
{
	struct memcomp_ref *ref;
	struct mc16_view view;

	if (!f->changed)
		return;
	ref = &f->refs[f->fail_index];
	mutex_lock(memcomp_lock_of(ref->token));
	if (mc16_view_locked(ref, &view))
		*view.flags = f->race_flags;
	mutex_unlock(memcomp_lock_of(ref->token));
	f->changed = false;
}

static void mc16_migt_cleanup(void *data)
{
	struct mc16_migt *f = data;
	struct folio *src = f->base->account, *dst = f->base->dst;
	unsigned int i;

	mc16_migt_current = NULL;
	mc16_migt_restore_race(f);
	for (i = 0; i < f->nr; i++) {
		if (!f->refs[i].slot)
			continue;
		if (f->swapped[i])
			mc16_migt_swapped(f, i, false);
		mc16_ref_put(&f->refs[i]);
	}
	if (src) {
		src->swap.val = 0;
		src->mapping = NULL;
		folio_clear_swapbacked(src);
	}
	if (dst) {
		dst->swap.val = 0;
		dst->mapping = NULL;
		folio_clear_swapbacked(dst);
	}
}

static int mc16_migt_payload(struct mc16_migt *f, unsigned int index)
{
	struct folio *src = f->base->account;
	struct memcomp_ref *ref = &f->refs[index];
	struct mc16_payload payload = { };
	struct mc16_payload_tail tail = { };
	unsigned long handle;
	unsigned int bytes;
	u8 pool = 0;
	int ret;

	ret = memcomp_prepare_payload(f->data + index * PAGE_SIZE, PAGE_SIZE,
				      &bytes, &handle, &pool,
				      MEMCOMP_PAYLOAD_MUST_SUCCEED, false);
	if (ret)
		return ret;
	if (pool) {
		memcomp_zs_free(pool, handle);
		return -EINVAL;
	}
	payload = (struct mc16_payload) { .word = handle, .bytes = bytes };
	ret = mc16_payload_charge(ref, &payload);
	if (ret)
		goto abort;
	folio_lock(src);
	mutex_lock(memcomp_lock_of(ref->token));
	ret = mc16_payload_commit_locked(ref, src, f->nr, index, &payload, &tail);
	mutex_unlock(memcomp_lock_of(ref->token));
	mc16_payload_finish(ref, &tail);
	folio_unlock(src);
abort:
	mc16_payload_abort(ref, &payload);
	return ret;
}

static struct mc16_migt *
mc16_migt_begin(struct kunit *test, unsigned int nr, unsigned int state)
{
	struct mc16_migt *f;
	struct mc16_record *record;
	struct folio *src, *dst;
	unsigned int i;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->base = mc16_consumer_test_begin(test);
	if (!f->base)
		return NULL;
	f->fail_index = UINT_MAX;
	if (nr > 1) {
		src = folio_alloc(GFP_KERNEL, ilog2(nr));
		dst = folio_alloc(GFP_KERNEL, ilog2(nr));
		if (!src || !dst) {
			if (src)
				folio_put(src);
			if (dst)
				folio_put(dst);
			return NULL;
		}
		folio_put(f->base->account);
		folio_put(f->base->dst);
		f->base->account = src;
		f->base->dst = dst;
		f->base->model.folio = src;
	}
	if (kunit_add_action_or_reset(test, mc16_migt_cleanup, f))
		return NULL;
	src = f->base->account;
	dst = f->base->dst;
	src->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	dst->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(src);
	__folio_set_swapbacked(dst);
	f->data = kunit_kmalloc(test, nr * PAGE_SIZE, GFP_KERNEL);
	f->loaded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->data || !f->loaded)
		return NULL;
	for (i = 0; i < nr * PAGE_SIZE; i++)
		f->data[i] = state == MC16_SAMEFILL ? 0x6e : (i + i / PAGE_SIZE) % 47;
	for (i = 0; i < nr; i++) {
		void *mapped = kmap_local_folio(src, i * PAGE_SIZE);

		memcpy(mapped, f->data + i * PAGE_SIZE, PAGE_SIZE);
		kunmap_local(mapped);
	}
	record = mc16_record_alloc(src, nr, false);
	if (IS_ERR(record))
		return NULL;
	ret = mc16_reserve(record, nr, MC16_KEY_ANON, f->refs);
	if (ret) {
		mc16_record_abort(record);
		return NULL;
	}
	f->nr = nr;
	for (i = 0; i < nr; i++) {
		mc16_account_reservation(&f->refs[i]);
		f->tokens[i] = f->refs[i].token;
	}
	src->swap = make_memcomp_entry(f->tokens[0]);
	if (state == MC16_PENDING)
		return f;
	for (i = 0; i < nr; i++) {
		if (mc16_migt_payload(f, i))
			return NULL;
	}
	return f;
}

/* Snapshot scalars under T; no record/member pointer is kept across T. */
static void mc16_migt_snapshot(struct mc16_migt *f,
			       struct mc16_migt_snapshot *s, struct folio *owner)
{
	struct memcomp_kunit_objcg *model = &f->base->model;
	struct kunit *test = f->base->test;
	struct mc16_view view;
	unsigned int i;
	bool valid;

	for (i = 0; i < f->nr; i++) {
		struct memcomp_ref *ref = &f->refs[i];

		if (!ref->slot)
			continue;
		mutex_lock(memcomp_lock_of(ref->token));
		valid = mc16_view_locked(ref, &view) && view.record;
		if (valid) {
			s->payload[i] = (struct mc16_migt_saved) {
				.word = *view.word, .flags = *view.flags,
				.length = *view.length,
			};
			s->charge[i] = view.member->payload_charge;
			s->members[i] = refcount_read(&view.record->members);
			valid = view.member->owner_folio == owner &&
				view.record->producer == f->base->account &&
				view.record->nid == ref->cluster->key->nid;
		}
		mutex_unlock(memcomp_lock_of(ref->token));
		KUNIT_EXPECT_TRUE(test, valid);
		s->refs[i] = refcount_read(&ref->slot->refs);
		s->nid[i] = ref->cluster->key->nid;
	}
	s->charged = atomic_long_read(&model->charged);
	s->charges = atomic_read(&model->charge_calls);
	s->uncharges = atomic_read(&model->uncharge_calls);
	s->gets = atomic_read(&model->get_calls);
	s->puts = atomic_read(&model->put_calls);
	s->src_refs = folio_ref_count(f->base->account);
	s->dst_refs = folio_ref_count(f->base->dst);
	s->stored = atomic64_read(&memcomp_stored_bytes);
	s->compressed = atomic64_read(&memcomp_compressed_bytes);
	s->eligible = atomic_long_read(&mc16_fifo_of(&f->refs[0])->nr_entries);
}

static void mc16_migt_unchanged(struct mc16_migt *f, struct folio *owner)
{
	struct mc16_migt_snapshot after = { };
	unsigned int i;

	mc16_migt_snapshot(f, &after, owner);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(f->base->test, after.payload[i].word,
				f->before.payload[i].word);
		KUNIT_EXPECT_EQ(f->base->test, after.payload[i].flags,
				f->before.payload[i].flags);
		KUNIT_EXPECT_EQ(f->base->test, after.payload[i].length,
				f->before.payload[i].length);
		KUNIT_EXPECT_EQ(f->base->test, after.charge[i],
				f->before.charge[i]);
		KUNIT_EXPECT_EQ(f->base->test, after.refs[i],
				f->before.refs[i]);
		KUNIT_EXPECT_EQ(f->base->test, after.members[i],
				f->before.members[i]);
		KUNIT_EXPECT_EQ(f->base->test, after.nid[i],
				f->before.nid[i]);
	}
	KUNIT_EXPECT_EQ(f->base->test, after.charged, f->before.charged);
	KUNIT_EXPECT_EQ(f->base->test, after.charges, f->before.charges);
	KUNIT_EXPECT_EQ(f->base->test, after.uncharges, f->before.uncharges);
	KUNIT_EXPECT_EQ(f->base->test, after.gets, f->before.gets);
	KUNIT_EXPECT_EQ(f->base->test, after.puts, f->before.puts);
	KUNIT_EXPECT_EQ(f->base->test, after.src_refs, f->before.src_refs);
	KUNIT_EXPECT_EQ(f->base->test, after.dst_refs, f->before.dst_refs);
	KUNIT_EXPECT_EQ(f->base->test, after.stored, f->before.stored);
	KUNIT_EXPECT_EQ(f->base->test, after.compressed, f->before.compressed);
	KUNIT_EXPECT_EQ(f->base->test, after.eligible, f->before.eligible);
}

static void mc16_migt_bytes(struct mc16_migt *f)
{
	unsigned int i;
	int ret;

	for (i = 0; i < f->nr; i++) {
		if (!f->refs[i].slot)
			continue;
		if (f->swapped[i]) {
			ret = mc16_migt_swapped(f, i, false);
			KUNIT_ASSERT_EQ(f->base->test, ret, 0);
		}
		ret = mc16_load_page(&f->refs[i], f->loaded);
		KUNIT_ASSERT_EQ(f->base->test, ret, 0);
		KUNIT_EXPECT_EQ(f->base->test,
				memcmp(f->loaded, f->data + i * PAGE_SIZE, PAGE_SIZE), 0);
	}
}

static int mc16_migt_move_window(struct memcomp_ref *ref, struct folio *from,
				 struct folio *to, unsigned int index)
{
	struct mc16_migt *f = mc16_migt_current;
	struct mc16_view view;
	unsigned int i;
	bool forward = from == f->base->account;

	if (f->nr_trace < ARRAY_SIZE(f->trace))
		f->trace[f->nr_trace++] = index | (forward ? 0 : 0x80);
	if (!index && forward) {
		f->all_pinned = true;
		for (i = 0; i < f->nr; i++)
			f->all_pinned &= refcount_read(&f->refs[i].slot->refs) == 2;
	}
	if (!forward || index != f->fail_index)
		return 0;
	mutex_lock(memcomp_lock_of(ref->token));
	if (mc16_view_locked(ref, &view)) {
		f->old_owner_seen = view.member->owner_folio == from;
		if (f->race) {
			f->race_flags = *view.flags;
			*view.flags &= ~MC16_F_OWNER;
			f->changed = true;
		}
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	return f->race ? 0 : -EAGAIN;
}

/* Every acquired folio lock is released before any fatal test assertion. */
static int mc16_migt_call(struct mc16_migt *f, bool rollback,
			  bool lock_src, bool lock_dst, bool inject)
{
	struct folio *src = f->base->account, *dst = f->base->dst;
	int ret = 0;

	if (inject) {
		mc16_migt_current = f;
		kunit_activate_static_stub(f->base->test, mc16_migrate_move_error,
					   mc16_migt_move_window);
	}
	if (lock_src)
		folio_lock(src);
	if (lock_dst)
		folio_lock(dst);
	if (rollback)
		mc16_migrate_folio_rollback(dst, src);
	else
		ret = mc16_migrate_folio(dst, src);
	if (lock_dst)
		folio_unlock(dst);
	if (lock_src)
		folio_unlock(src);
	if (inject) {
		kunit_deactivate_static_stub(f->base->test, mc16_migrate_move_error);
		mc16_migt_current = NULL;
	}
	mc16_migt_restore_race(f);
	return ret;
}

struct mc16_migt_metadata {
	unsigned long entry;
	u32 token[MC16_MIGT_NR];
	unsigned short source_id[MC16_MIGT_NR];
	u64 shadow[MC16_MIGT_NR];
};

/* Existing OWNER refs pin slots/keys; only scalars leave the lock scope. */
static bool
mc16_migt_metadata_snapshot(struct mc16_migt *f, struct folio *owner,
			    struct mc16_migt_metadata *s, bool seed)
{
	unsigned long token;
	unsigned int i;
	bool valid;

	folio_lock(owner);
	s->entry = owner->swap.val;
	token = swp_offset(owner->swap);
	valid = folio_memcomp_reserved(owner);
	for (i = 0; valid && i < f->nr; i++) {
		struct memcomp_ref *ref = &f->refs[i];
		u64 *shadow;

		mutex_lock(memcomp_lock_of(ref->token));
		shadow = mc16_shadow_ptr(ref);
		valid = shadow && ref->token == token + i &&
			mc16_owner_matches_locked(ref, owner, f->nr, i);
		if (valid && seed) {
			valid = !*shadow;
			/* Opaque words; never passed to stock refault. */
			if (valid)
				*shadow = (unsigned long)xa_mk_value(0x100 + i);
		}
		if (valid) {
			s->token[i] = ref->token;
			s->source_id[i] = mc16_owner_id(ref);
			s->shadow[i] = *shadow;
		}
		mutex_unlock(memcomp_lock_of(ref->token));
	}
	folio_unlock(owner);
	return valid;
}

static void mc16_migt_success(struct kunit *test, unsigned int state)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, state);
	struct mc16_migt_metadata before = { }, after = { };
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	if (state == MC16_SWAPPED) {
		for (i = 0; i < f->nr; i++)
			KUNIT_ASSERT_EQ(test, mc16_migt_swapped(f, i, true), 0);
		mc16_migt_snapshot(f, &f->before, f->base->account);
	}
	KUNIT_ASSERT_TRUE(test,
			  mc16_migt_metadata_snapshot(f, f->base->account,
						      &before, true));
	ret = mc16_migt_call(f, false, true, true, false);
	KUNIT_ASSERT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, f->base->account->swap.val);
	mc16_migt_unchanged(f, f->base->dst);
	KUNIT_ASSERT_TRUE(test,
			  mc16_migt_metadata_snapshot(f, f->base->dst,
						      &after, false));
	KUNIT_EXPECT_EQ(test, after.entry, before.entry);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, after.token[i], before.token[i]);
		KUNIT_EXPECT_EQ(test, after.source_id[i],
				before.source_id[i]);
		KUNIT_EXPECT_EQ(test, after.shadow[i], before.shadow[i]);
	}
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_stored(struct kunit *test)
{
	mc16_migt_success(test, MC16_STORED);
}

static void memcomp_test_migrate_samefill(struct kunit *test)
{
	mc16_migt_success(test, MC16_SAMEFILL);
}

static void memcomp_test_migrate_swapped(struct kunit *test)
{
	mc16_migt_success(test, MC16_SWAPPED);
}

static void memcomp_test_migrate_rollback(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	ret = mc16_migt_call(f, false, true, true, true);
	KUNIT_ASSERT_EQ(test, ret, 1);
	KUNIT_EXPECT_TRUE(test, f->all_pinned);
	mc16_migt_unchanged(f, f->base->dst);
	mc16_migt_call(f, true, true, true, true);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	KUNIT_ASSERT_EQ(test, f->nr_trace, 2 * f->nr);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, f->trace[i], i);
		KUNIT_EXPECT_EQ(test, f->trace[f->nr + i], 0x80 + f->nr - i - 1);
	}
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}

static void mc16_migt_partial(struct kunit *test, bool race)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->fail_index = 2;
	f->race = race;
	f->base->dst->swap = swp_entry(0, 777);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	ret = mc16_migt_call(f, false, true, true, true);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, swp_entry(0, 777).val);
	KUNIT_EXPECT_TRUE(test, f->all_pinned);
	KUNIT_EXPECT_TRUE(test, f->old_owner_seen);
	KUNIT_ASSERT_EQ(test, f->nr_trace, 5U);
	KUNIT_EXPECT_EQ(test, f->trace[0], 0U);
	KUNIT_EXPECT_EQ(test, f->trace[1], 1U);
	KUNIT_EXPECT_EQ(test, f->trace[2], 2U);
	KUNIT_EXPECT_EQ(test, f->trace[3], 0x81U);
	KUNIT_EXPECT_EQ(test, f->trace[4], 0x80U);
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_partial(struct kunit *test)
{
	mc16_migt_partial(test, false);
}

static void memcomp_test_migrate_revalidate(struct kunit *test)
{
	mc16_migt_partial(test, true);
}

static void memcomp_test_migrate_pending(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_PENDING);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	ret = mc16_migt_call(f, false, true, true, true);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->nr_trace, 0U);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
}

static void mc16_migt_reject(struct kunit *test, bool wrong_owner)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	struct memcomp_ref *ref;
	struct mc16_view view;
	u16 flags;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ref = &f->refs[1];
	mc16_migt_snapshot(f, &f->before, f->base->account);
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	flags = ref->slot->flags;
	if (wrong_owner)
		view.member->owner_folio = f->base->dst;
	else
		ref->slot->flags &= ~MC16_ORDER_MASK;
	mutex_unlock(memcomp_lock_of(ref->token));
	ret = mc16_migt_call(f, false, true, true, true);
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	if (wrong_owner)
		view.member->owner_folio = f->base->account;
	else
		ref->slot->flags = flags;
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->nr_trace, 0U);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_geometry(struct kunit *test)
{
	mc16_migt_reject(test, false);
}

static void memcomp_test_migrate_owner(struct kunit *test)
{
	mc16_migt_reject(test, true);
}

static void memcomp_test_migrate_missing(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_ref_put(&f->refs[3]);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	ret = mc16_migt_call(f, false, true, true, true);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->nr_trace, 0U);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_locks(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, 1, MC16_STORED);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	ret = mc16_migt_call(f, false, false, true, false);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mc16_migt_call(f, false, true, false, false);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_rollback_missing(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ret = mc16_migt_call(f, false, true, true, false);
	KUNIT_ASSERT_EQ(test, ret, 1);
	/* Actual final member retirement; rollback must restore every survivor. */
	mc16_ref_put(&f->refs[3]);
	mc16_migt_snapshot(f, &f->before, f->base->dst);
	mc16_migt_call(f, true, true, true, false);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_rollback_invalid(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	f->base->dst->swap = make_memcomp_entry(U32_MAX);
	mc16_migt_call(f, true, true, true, false);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
}

static void memcomp_test_migrate_incompressible(struct kunit *test)
{
	struct mc16_consumer_test *f = mc16_consumer_test_begin(test);
	unsigned long marker, moved, rolled_back;
	struct folio *src, *dst;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = f->account;
	dst = f->dst;
	src->mapping = (struct address_space *)PAGE_MAPPING_ANON;
	__folio_set_swapbacked(src);
	folio_lock(src);
	folio_lock(dst);
	folio_set_incompressible(src, memcomp_folio_hash(src));
	marker = src->swap.val;
	ret = mc16_migrate_folio(dst, src);
	moved = dst->swap.val;
	mc16_migrate_folio_rollback(dst, src);
	rolled_back = dst->swap.val;
	src->swap.val = 0;
	dst->swap.val = 0;
	src->mapping = NULL;
	folio_clear_swapbacked(src);
	folio_unlock(dst);
	folio_unlock(src);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, moved, marker);
	KUNIT_EXPECT_EQ(test, rolled_back, marker);
	KUNIT_EXPECT_EQ(test, folio_ref_count(src), 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(dst), 1);
}

static void memcomp_test_migrate_single(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, 1, MC16_STORED);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	ret = mc16_migt_call(f, false, true, true, false);
	KUNIT_ASSERT_EQ(test, ret, 1);
	mc16_migt_unchanged(f, f->base->dst);
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_dst_geometry(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->base->large = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->base->large);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	folio_lock(f->base->account);
	folio_lock(f->base->large);
	ret = mc16_migrate_folio(f->base->large, f->base->account);
	folio_unlock(f->base->large);
	folio_unlock(f->base->account);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->base->large->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}

static void memcomp_test_migrate_range(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	swp_entry_t saved;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	mc16_migt_snapshot(f, &f->before, f->base->account);
	saved = f->base->account->swap;
	f->base->account->swap = make_memcomp_entry(U32_MAX);
	ret = mc16_migt_call(f, false, true, true, false);
	f->base->account->swap = saved;
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
}

static void memcomp_test_migrate_record_geometry(struct kunit *test)
{
	struct mc16_migt *f = mc16_migt_begin(test, MC16_MIGT_NR, MC16_STORED);
	struct memcomp_ref *ref;
	struct mc16_view view;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ref = &f->refs[0];
	mc16_migt_snapshot(f, &f->before, f->base->account);
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	view.record->nr_pages = 2;
	mutex_unlock(memcomp_lock_of(ref->token));
	ret = mc16_migt_call(f, false, true, true, true);
	/* Member zero remains in both geometries; restore before any assertion. */
	mutex_lock(memcomp_lock_of(ref->token));
	mc16_view_locked(ref, &view);
	view.record->nr_pages = f->nr;
	mutex_unlock(memcomp_lock_of(ref->token));
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->nr_trace, 0U);
	KUNIT_EXPECT_EQ(test, f->base->dst->swap.val, 0UL);
	mc16_migt_unchanged(f, f->base->account);
	mc16_migt_bytes(f);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_MIGRATE_TEST_H */
