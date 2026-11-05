/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ROLLBACK_TEST_H
#define _MM_MEMCOMP_TESTS_ROLLBACK_TEST_H

/* Real VM/PTE fixture; only allocation ownership uses the existing objcg model. */
#if defined(MC16_FOLIO_TEST_ENABLED) && defined(CONFIG_MEMCOMP_DEBUG_STATS)
#define MC16_ROLLBACK_TEST_ENABLED

struct mc16_rb_counts {
	s64 spans;
	s64 ptes;
	s64 rewalks;
	s64 missed;
	s64 pending;
};

static struct mc16_rb_counts mc16_rb_counts(void)
{
	return (struct mc16_rb_counts) {
		.spans = atomic64_read(&memcomp_restore_spans),
		.ptes = atomic64_read(&memcomp_restore_ptes),
		.rewalks = atomic64_read(&memcomp_restore_rewalks),
		.missed = atomic64_read(&memcomp_restore_missed_pages),
		.pending = atomic64_read(&memcomp_pending_pages),
	};
}

static bool mc16_rb_reserve(struct mc16_flt *f)
{
	bool reserved;

	folio_lock(f->folio);
	reserved = memcomp_reserve(f->folio);
	folio_unlock(f->folio);
	return reserved;
}

static void mc16_rb_rollback(struct mc16_flt *f)
{
	mmap_read_lock(f->mm);
	folio_lock(f->folio);
	memcomp_rollback_folio(f->folio);
	folio_unlock(f->folio);
	mmap_read_unlock(f->mm);
}

static void mc16_rb_expect_bytes(struct mc16_flt *f)
{
	unsigned int i;
	void *bytes;

	for (i = 0; i < f->nr; i++) {
		bytes = kmap_local_folio(f->folio, i * PAGE_SIZE);
		KUNIT_EXPECT_PTR_EQ(f->test, memchr_inv(bytes, 0x6b, PAGE_SIZE), NULL);
		kunmap_local(bytes);
	}
}

/* A fully restored span without a lookup guard must not start another walk. */
static void memcomp_test_public_rollback_restored(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 0);
	struct mc16_rb_counts before, after;
	unsigned long token;
	unsigned int first, second;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_rb_reserve(f));
	token = swp_offset(f->folio->swap);
	KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, 0), 0);
	mmap_read_lock(f->mm);
	folio_lock(f->folio);
	first = mc16_folio_restore_span(f->folio, token, false);
	second = mc16_folio_restore_span(f->folio, token, false);
	folio_unlock(f->folio);
	mmap_read_unlock(f->mm);
	KUNIT_ASSERT_EQ(test, first, 1U);
	KUNIT_ASSERT_EQ(test, second, 0U);
	before = mc16_rb_counts();
	mc16_rb_rollback(f);
	after = mc16_rb_counts();
	KUNIT_EXPECT_EQ(test, after.spans, before.spans);
	KUNIT_EXPECT_EQ(test, after.ptes, before.ptes);
	KUNIT_EXPECT_EQ(test, after.rewalks, before.rewalks);
	KUNIT_EXPECT_EQ(test, after.missed, before.missed);
	KUNIT_EXPECT_EQ(test, after.pending, before.pending - 1);
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token));
	mc16_flt_expect_present(f, 0, f->addr);
	mc16_rb_expect_bytes(f);
}

/* Temporary guards may request a bounded rewalk, but are not missed PTEs. */
static void memcomp_test_public_rollback_lookup_only(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_rb_counts before, after;
	unsigned long token;
	unsigned int i;
	int pins;

	KUNIT_ASSERT_NOT_NULL(test, f);
	pins = atomic_read(&f->anon_vma->refcount);
	KUNIT_ASSERT_TRUE(test, mc16_rb_reserve(f));
	token = swp_offset(f->folio->swap);
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	before = mc16_rb_counts();
	mc16_rb_rollback(f);
	after = mc16_rb_counts();
	KUNIT_EXPECT_EQ(test, after.spans, before.spans + 1);
	KUNIT_EXPECT_EQ(test, after.rewalks, before.rewalks + 1);
	KUNIT_EXPECT_EQ(test, after.ptes, before.ptes);
	KUNIT_EXPECT_EQ(test, after.missed, before.missed);
	/* Pending slots remain live while the two lookup guards are held. */
	KUNIT_EXPECT_EQ(test, after.pending, before.pending);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->anon_vma->refcount), pins);
	KUNIT_EXPECT_EQ(test, f->folio->swap.val, 0UL);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 1U);
		KUNIT_EXPECT_FALSE(test, mc16_flt_flags(&f->guards[i], false) & MC16_F_OWNER);
		memcomp_entry_put_ref(&f->guards[i]);
		KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token + i));
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), before.pending - 2);
	mc16_rb_expect_bytes(f);
}

/* Both PTEs restore on the first pass; held guards cause exactly one rewalk. */
static void memcomp_test_public_rollback_rewalk(struct kunit *test)
{
	struct mc16_flt *f = mc16_flt_begin(test, 1);
	struct mc16_rb_counts before, after;
	unsigned long token;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_TRUE(test, mc16_rb_reserve(f));
	token = swp_offset(f->folio->swap);
	KUNIT_ASSERT_EQ(test, mc16_flt_hold(f), 0);
	for (i = 0; i < f->nr; i++)
		KUNIT_ASSERT_EQ(test, mc16_flt_escape(f, i), 0);
	before = mc16_rb_counts();
	mc16_rb_rollback(f);
	after = mc16_rb_counts();
	KUNIT_EXPECT_EQ(test, after.spans, before.spans + 1);
	KUNIT_EXPECT_EQ(test, after.ptes, before.ptes + 2);
	KUNIT_EXPECT_EQ(test, after.rewalks, before.rewalks + 1);
	KUNIT_EXPECT_EQ(test, after.missed, before.missed);
	/* Pending slots remain live while the two lookup guards are held. */
	KUNIT_EXPECT_EQ(test, after.pending, before.pending);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_SWAPENTS), f->swapents);
	KUNIT_EXPECT_EQ(test, mc16_flt_rss(f, MM_ANONPAGES), f->anonpages + 2);
	for (i = 0; i < f->nr; i++) {
		mc16_flt_expect_present(f, i, f->addr + i * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, refcount_read(&f->guards[i].slot->refs), 1U);
		memcomp_entry_put_ref(&f->guards[i]);
		KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token + i));
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&memcomp_pending_pages), before.pending - 2);
	mc16_rb_expect_bytes(f);
}
#endif /* MC16_FOLIO_TEST_ENABLED && CONFIG_MEMCOMP_DEBUG_STATS */
#endif /* _MM_MEMCOMP_TESTS_ROLLBACK_TEST_H */
