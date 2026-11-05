/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_REPLACEMENT_TEST_H
#define _MM_MEMCOMP_TESTS_REPLACEMENT_TEST_H

/*
 * Author: nakixii
 *
 * Include after store_transaction_test.h in suite.h.
 * Register the four cases below under MC16_REPLACEMENT_TEST_ENABLED.
 * Inherits MEMCOMP_KUNIT_TEST (DEBUG_STATS), MEMCG, MMU, ADVISE_SYSCALLS.
 *
 * Old oracles: 6cc25de18927:mm/suite.h:
 * refresh_rejects_mixed_state: -EAGAIN, one state event/two pages, unchanged
 * PENDING+RESIDENT and SAMEFILL members, one pending page, balanced rollback.
 * reserve_replaces_stored_span: a different reservation, one pending page,
 * old token absent after its final reference, balanced reservation teardown.
 * Extend replacement to order 1 and retain real old payloads with guards.
 *
 * Public reserve/store/load/release and real MM/PTE/rmap/codec/zs execute.
 * A sibling SAMEFILL uses the existing real payload commit/finish fixture;
 * the reverse mixed order uses an injected second-commit store failure.
 * No fabricated entry state or mocked successful store is used.
 * There is no public refresh entry point: call the production mc16 refresh
 * transaction for its state-event oracle, and separately check public store's
 * earlier mixed-span PREFLIGHT rejection. These counters are not interchangeable.
 * Objcg/slot allocation and commit failure routing use the serial test models.
 * No concurrent race, device IO or reclaim scheduling coverage is claimed.
 * All actors release locks/leases before returning to any KUnit assertion.
 */
#ifdef MC16_STORE_TRANSACTION_TEST_ENABLED
#define MC16_REPLACEMENT_TEST_ENABLED

struct mc16_rct_member {
	u64 word;
	u16 length;
	u16 flags;
	unsigned int refs;
	bool valid;
};

/* Only scalar effective state crosses T; the caller owns the lookup guard. */
static struct mc16_rct_member mc16_rct_member(struct memcomp_ref *ref)
{
	struct mc16_rct_member s = { };
	struct mc16_view view;

	mutex_lock(memcomp_lock_of(ref->token));
	if (mc16_view_locked(ref, &view)) {
		s.word = *view.word;
		s.length = *view.length;
		s.flags = *view.flags;
		s.refs = refcount_read(&ref->slot->refs);
		s.valid = mc16_flags_valid(s.flags);
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	return s;
}

/* Drop the temporary public lookup before returning or asserting. */
static struct mc16_rct_member mc16_rct_lookup(unsigned long token)
{
	struct mc16_rct_member s = { };
	struct memcomp_ref ref = { };

	if (memcomp_entry_get_ref(token, &ref)) {
		s = mc16_rct_member(&ref);
		/* Report references excluding this temporary lookup. */
		if (s.refs)
			s.refs--;
		memcomp_entry_put_ref(&ref);
	}
	return s;
}

static void mc16_rct_expect_unchanged(struct mc16_flt *f,
				      struct mc16_rct_member before[2])
{
	struct mc16_rct_member after;
	unsigned int i;

	for (i = 0; i < f->nr; i++) {
		after = mc16_rct_member(&f->guards[i]);
		KUNIT_EXPECT_TRUE(f->test, after.valid);
		KUNIT_EXPECT_EQ(f->test, after.word, before[i].word);
		KUNIT_EXPECT_EQ(f->test, after.length, before[i].length);
		KUNIT_EXPECT_EQ(f->test, after.flags, before[i].flags);
		KUNIT_EXPECT_EQ(f->test, after.refs, before[i].refs);
	}
}

/* Unlike public store, the refresh transaction retains the caller's ctx. */
static int mc16_rct_refresh(struct mc16_flt *f, const void *src)
{
	struct memcomp_reclaim_ctx *ctx;
	struct memcomp_store_ctx store = { };
	unsigned int flags;
	int ret = -EBUSY;

	flags = memcomp_test_reclaimer_enter();
	ctx = memcomp_reclaim_ctx_get(f->nr, MEMCOMP_RECLAIM_KSWAPD);
	if (!ctx)
		goto out;
	folio_lock(f->folio);
	ret = mc16_anon_refresh(&ctx, src, f->nr, &store, f->guards);
	folio_unlock(f->folio);
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	memcomp_direct_staging_put(store.staging);
out:
	memcomp_test_reclaimer_exit(flags);
	return ret;
}

static void mc16_rct_mixed(struct kunit *test, bool sibling_samefill)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_rct_member members[2];
	struct mc16_sct_stats stats;
	s64 pending;
	long charged;
	u8 *src, *dst;
	unsigned int i, stored = sibling_samefill ? 1 : 0;
	int pins, ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	src = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	mc16_sct_pattern(src, 2, false);
	if (sibling_samefill)
		memset(src + PAGE_SIZE, 0x5a, PAGE_SIZE);
	mc16_sct_copy(f, src);
	pending = atomic64_read(&memcomp_pending_pages);
	pins = atomic_read(&f->anon_vma->refcount);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	stats = mc16_sct_stats_read();
	if (sibling_samefill) {
		/* Actual SAMEFILL commit matches the source bytes above. */
		KUNIT_ASSERT_EQ(test, mc16_flt_commit(f, 1), 0);
		mc16_sct_stats_expect(test, stats, (struct mc16_sct_stats) { });
	} else {
		/* Real codec/zs prefix, then public store fails commit 2. */
		mc16_sct_run(f, (struct mc16_sct_attempt) {
			.fail = MEMCOMP_KUNIT_FAIL_REFRESH_COMMIT, .nth = 2,
		}, false);
		mc16_sct_stats_expect(test, stats, (struct mc16_sct_stats) {
			.state = 1, .state_pages = 2, .bg_fail = 2,
		});
	}
	for (i = 0; i < f->nr; i++) {
		members[i] = mc16_rct_member(&f->guards[i]);
		KUNIT_ASSERT_TRUE(test, members[i].valid);
		KUNIT_EXPECT_TRUE(test, members[i].flags & MC16_F_OWNER);
		KUNIT_EXPECT_FALSE(test, members[i].flags & MC16_F_REFRESH_PENDING);
		KUNIT_EXPECT_EQ(test, members[i].refs, 3U);
		KUNIT_EXPECT_EQ(test, mc16_flags_state(members[i].flags),
				i == stored ? (sibling_samefill ? MC16_SAMEFILL :
				MC16_STORED) : MC16_PENDING);
		KUNIT_EXPECT_EQ(test, !!(members[i].flags & MC16_F_RESIDENT),
				i != stored);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), pending + 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 2);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins + 1);
	charged = atomic_long_read(&f->base->model.charged);
	stats = mc16_sct_stats_read();
	ret = mc16_rct_refresh(f, src);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	mc16_sct_stats_expect(test, stats, (struct mc16_sct_stats) {
		.state = 1, .state_pages = 2,
	});
	mc16_rct_expect_unchanged(f, members);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->base->model.charged), charged);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), pending + 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 2);
	stats = mc16_sct_stats_read();
	/* Public wrapper refuses the mixed span before refresh validation. */
	mc16_sct_run(f, (struct mc16_sct_attempt) { }, false);
	mc16_sct_stats_expect(test, stats, (struct mc16_sct_stats) {
		.preflight = 2, .bg_fail = 2,
	});
	mc16_rct_expect_unchanged(f, members);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->base->model.charged), charged);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), pending + 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins + 1);
	mc16_sct_rollback(f);
	for (i = 0; i < f->nr; i++) {
		mc16_flt_expect_present(f, i, f->addr + i * PAGE_SIZE);
		KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[i], false) &
				   MC16_F_OWNER);
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 1U);
	}
	/* Normalized committed member remains loadable through its guard. */
	mc16_sct_expect_load(f, stored, src + stored * PAGE_SIZE, dst);
	for (i = 0; i < f->nr; i++)
		memcomp_entry_put_ref(&f->guards[i]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), pending);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
}

static void memcomp_test_replacement_mixed_sibling(struct kunit *test)
{
	mc16_rct_mixed(test, true);
}

static void memcomp_test_replacement_mixed_head(struct kunit *test)
{
	mc16_rct_mixed(test, false);
}

static void mc16_rct_load(struct mc16_flt *f, unsigned long token,
			  const void *src, void *dst)
{
	struct memcomp_ref ref = { };
	void *bytes;
	int ret = -ENOENT;

	if (memcomp_load_folio_pin(token, &ref)) {
		folio_lock(f->base->dst);
		ret = memcomp_load_folio(&ref, f->base->dst);
		if (!ret) {
			bytes = kmap_local_folio(f->base->dst, 0);
			memcpy(dst, bytes, PAGE_SIZE);
			kunmap_local(bytes);
		}
		folio_unlock(f->base->dst);
		memcomp_load_folio_abort(&ref);
	}
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	if (!ret)
		KUNIT_EXPECT_EQ(f->test, memcmp(src, dst, PAGE_SIZE), 0);
}

static void mc16_rct_release_guards(void *data)
{
	struct memcomp_ref *refs = data;
	unsigned int i;

	for (i = 0; i < 2; i++)
		memcomp_entry_put_ref(&refs[i]);
}

static void mc16_rct_replace(struct kunit *test, unsigned int order)
{
	struct mc16_flt *f = mc16_flt_begin(test, order);
	struct mc16_rct_member old, fresh;
	struct memcomp_ref *new_guards;
	struct mc16_sct_stats stats;
	unsigned long token, old_tokens[2];
	u8 *src, *next, *dst;
	s64 pending;
	unsigned int i;
	int pins;

	KUNIT_ASSERT_NOT_NULL(test, f);
	new_guards = kunit_kcalloc(test, 2, sizeof(*new_guards), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, new_guards);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							mc16_rct_release_guards, new_guards), 0);
	src = kunit_kmalloc(test, f->nr * PAGE_SIZE, GFP_KERNEL);
	next = kunit_kmalloc(test, f->nr * PAGE_SIZE, GFP_KERNEL);
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, next);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	mc16_sct_pattern(src, f->nr, false);
	for (i = 0; i < f->nr * PAGE_SIZE; i++)
		next[i] = src[i] ^ 0x73;
	mc16_sct_copy(f, src);
	pending = atomic64_read(&memcomp_pending_pages);
	pins = atomic_read(&f->anon_vma->refcount);
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++) {
		old_tokens[i] = f->guards[i].token;
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	}
	stats = mc16_sct_stats_read();
	mc16_sct_run(f, (struct mc16_sct_attempt) { }, true);
	mc16_sct_stats_expect(test, stats, (struct mc16_sct_stats) {
		.bg_ok = f->nr,
	});
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), pending);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
	for (i = 0; i < f->nr; i++) {
		old = mc16_rct_member(&f->guards[i]);
		KUNIT_ASSERT_TRUE(test, old.valid);
		KUNIT_ASSERT_EQ(test, mc16_flags_state(old.flags), MC16_STORED);
		/* Remove actual old PTE ownership, retaining OWNER + guard. */
		KUNIT_ASSERT_EQ(test, mc16_flt_zap(f, f->addr + i * PAGE_SIZE), 0);
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 2U);
	}
	KUNIT_ASSERT_TRUE(test, mc16_sct_reserve(f));
	token = swp_offset(f->folio->swap);
	KUNIT_EXPECT_NE(test, token, old_tokens[0]);
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 1 + f->nr);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages),
			pending + f->nr);
	/* The unmapped source takes no new folio_get_anon_vma() pin. */
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
	for (i = 0; i < f->nr; i++) {
		old = mc16_rct_member(&f->guards[i]);
		fresh = mc16_rct_lookup(token + i);
		KUNIT_EXPECT_TRUE(test, old.valid);
		KUNIT_EXPECT_EQ(test, mc16_flags_state(old.flags), MC16_STORED);
		KUNIT_EXPECT_FALSE(test, old.flags & MC16_F_OWNER);
		KUNIT_EXPECT_EQ(test, old.refs, 1U);
		KUNIT_EXPECT_TRUE(test, memcomp_entry_present(old_tokens[i]));
		KUNIT_EXPECT_TRUE(test, fresh.valid);
		KUNIT_EXPECT_EQ(test, mc16_flags_state(fresh.flags), MC16_PENDING);
		KUNIT_EXPECT_TRUE(test, fresh.flags & MC16_F_OWNER);
		KUNIT_EXPECT_TRUE(test, fresh.flags & MC16_F_RESIDENT);
		KUNIT_EXPECT_FALSE(test, fresh.flags & MC16_F_REFRESH_PENDING);
		KUNIT_EXPECT_EQ(test, fresh.refs, 1U);
		mc16_sct_expect_load(f, i, src + i * PAGE_SIZE, dst);
	}
	/* New stores change bytes without touching the guard-held old payloads. */
	mc16_sct_copy(f, next);
	stats = mc16_sct_stats_read();
	mc16_sct_run(f, (struct mc16_sct_attempt) { }, true);
	mc16_sct_stats_expect(test, stats, (struct mc16_sct_stats) {
		.bg_ok = f->nr,
	});
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), pending);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
	/* A fresh span without PTE owners needs guards across real source exit. */
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_TRUE(test, memcomp_entry_get_ref(token + i,
							      &new_guards[i]));
	KUNIT_ASSERT_TRUE(test, mc16_sct_release(f));
	for (i = 0; i < f->nr; i++) {
		mc16_sct_expect_load(f, i, src + i * PAGE_SIZE, dst);
		mc16_rct_load(f, token + i, next + i * PAGE_SIZE, dst);
		memcomp_entry_put_ref(&f->guards[i]);
		KUNIT_EXPECT_FALSE(test, memcomp_entry_present(old_tokens[i]));
		KUNIT_EXPECT_TRUE(test, memcomp_entry_present(token + i));
	}
	for (i = 0; i < f->nr; i++)
		memcomp_entry_put_ref(&new_guards[i]);
	for (i = 0; i < f->nr; i++)
		KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token + i));
	KUNIT_EXPECT_EQ(test, folio_ref_count(f->folio), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), pending);
}

static void memcomp_test_replacement_stored_order0(struct kunit *test)
{
	mc16_rct_replace(test, 0);
}

static void memcomp_test_replacement_stored_order1(struct kunit *test)
{
	mc16_rct_replace(test, 1);
}
#endif /* MC16_STORE_TRANSACTION_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_REPLACEMENT_TEST_H */
