/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_SPAN_TEST_H
#define _MM_MEMCOMP_TESTS_SPAN_TEST_H
#ifdef CONFIG_MEMCG
static void memcomp_test_span_geometry(struct kunit *test)
{
	struct mc16_test_refs *f = mc16_test_refs_begin(test);
	struct mc16_test_storage *storage;
	unsigned int nr, first, i, index;
	u32 base;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->storage;
	for (nr = 1; nr <= MEMCOMP_MAX_COMPRESS_PAGES; nr <<= 1) {
		first = storage->nr;
		KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, nr), 0);
		for (i = 0; i < nr; i++) {
			KUNIT_EXPECT_EQ(test, mc16_ref_geometry(&storage->refs[first + i],
								&base, &index), nr);
			KUNIT_EXPECT_EQ(test, base, storage->refs[first].token);
			KUNIT_EXPECT_EQ(test, index, i);
		}
	}
}

static void memcomp_test_span_identity(struct kunit *test)
{
	struct mc16_test_refs *f = mc16_test_refs_begin(test);
	struct memcomp_ref refs[2];
	struct mc16_test_storage *storage;
	unsigned int i;
	u32 token;
	bool found;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 2), 0);
	token = storage->refs[0].token;
	found = mc16_span_get(token, storage->owner->folio, 2, true, refs);
	KUNIT_ASSERT_TRUE(test, found);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_EQ(test, refcount_read(&refs[i].slot->refs), 2U);
		mc16_ref_put(&refs[i]);
	}
	KUNIT_EXPECT_FALSE(test, mc16_span_get(token, NULL, 2, true, refs));
	KUNIT_EXPECT_FALSE(test, mc16_span_get(token, storage->owner->folio,
					       1, true, refs));
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_PTR_EQ(test, refs[i].slot, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&storage->refs[i].slot->refs), 1U);
	}
	/* A changed suffix must consume both temporary lookups on rejection. */
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 1, MC16_SAMEFILL), 0);
	KUNIT_EXPECT_FALSE(test, mc16_span_get(token, storage->owner->folio,
					       2, true, refs));
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_PTR_EQ(test, refs[i].slot, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&storage->refs[i].slot->refs), 1U);
	}
	/* Valid geometry with an invalid token also clears poisoned outputs. */
	memset(refs, 0xa5, sizeof(refs));
	KUNIT_EXPECT_FALSE(test, mc16_span_get(0, storage->owner->folio,
					       2, true, refs));
	for (i = 0; i < 2; i++)
		KUNIT_EXPECT_PTR_EQ(test, refs[i].slot, NULL);
	KUNIT_ASSERT_EQ(test, mc16_test_refs_payload(f, 0, MC16_SAMEFILL), 0);
	found = mc16_span_get(token, storage->owner->folio, 2, false, refs);
	KUNIT_ASSERT_TRUE(test, found);
	for (i = 0; i < 2; i++)
		mc16_ref_put(&refs[i]);

	/* Remove a real suffix slot; the successful lookup prefix must unwind. */
	KUNIT_ASSERT_TRUE(test, mc16_get(token + 1, &refs[1]));
	found = mc16_owner_drop_folio_held(&refs[1], storage->owner->folio, 2, 1);
	if (found)
		storage->refs[1] = (struct memcomp_ref) { };
	KUNIT_ASSERT_TRUE(test, found);
	KUNIT_EXPECT_FALSE(test, mc16_span_get(token, storage->owner->folio,
					       2, false, refs));
	for (i = 0; i < 2; i++)
		KUNIT_EXPECT_PTR_EQ(test, refs[i].slot, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&storage->refs[0].slot->refs), 1U);
}

static void memcomp_test_span_anon_vma(struct kunit *test)
{
	struct mc16_test_refs *f = mc16_test_refs_begin(test);
	struct mc16_test_storage *storage;
	struct mc16_record *record;
	struct anon_vma *anon_vma, *taken, *again;
	struct folio *mapped;
	unsigned long addr;
	struct memcomp_ref *head;

	KUNIT_ASSERT_NOT_NULL(test, f);
	storage = f->storage;
	KUNIT_ASSERT_EQ(test, mc16_test_refs_reserve(f, 2), 0);
	head = &storage->refs[0];
	record = (void *)(unsigned long)head->slot->word;
	mapped = memcomp_test_user_folio(test, &addr);
	KUNIT_ASSERT_NOT_NULL(test, mapped);
	anon_vma = folio_get_anon_vma(mapped);
	folio_unlock(mapped);
	folio_put(mapped);
	KUNIT_ASSERT_NOT_NULL(test, anon_vma);
	/* Own one real reference; the helper must transfer it without a put. */
	record->anon_vma = anon_vma;
	record->record_flags |= MC16_RECORD_ANON_VMA;
	mutex_lock(memcomp_lock_of(head->token));
	taken = mc16_span_take_anon_vma_locked(head, NULL, 2);
	mutex_unlock(memcomp_lock_of(head->token));
	KUNIT_EXPECT_PTR_EQ(test, taken, NULL);
	KUNIT_EXPECT_PTR_EQ(test, record->anon_vma, anon_vma);
	mutex_lock(memcomp_lock_of(head->token));
	taken = mc16_span_take_anon_vma_locked(head, storage->owner->folio, 2);
	again = mc16_span_take_anon_vma_locked(head, storage->owner->folio, 2);
	mutex_unlock(memcomp_lock_of(head->token));
	KUNIT_EXPECT_PTR_EQ(test, taken, anon_vma);
	KUNIT_EXPECT_PTR_EQ(test, again, NULL);
	KUNIT_EXPECT_PTR_EQ(test, record->anon_vma, NULL);
	KUNIT_EXPECT_FALSE(test, record->record_flags & MC16_RECORD_ANON_VMA);
	/* Tests own the transferred reference even if a helper assertion fails. */
	record->anon_vma = NULL;
	record->record_flags &= ~MC16_RECORD_ANON_VMA;
	put_anon_vma(anon_vma);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_SPAN_TEST_H */
