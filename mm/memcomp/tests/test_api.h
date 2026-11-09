/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_TEST_API_H
#define _MM_MEMCOMP_TESTS_TEST_API_H

#ifdef CONFIG_MEMCOMP_KUNIT_TEST
struct memcomp_record_info {
	u64 allocated_bytes;
	u64 charged_bytes;
	unsigned int members;
};

static inline void *
memcomp_test_record_alloc(struct folio *folio, unsigned int nr_pages)
{
	return mc16_record_alloc(folio, nr_pages, false);
}

static inline struct memcomp_record_info
memcomp_test_record_info(void *handle)
{
	struct mc16_record *record = handle;

	return (struct memcomp_record_info) {
		.allocated_bytes = record->alloc_bytes,
		.charged_bytes = record->charge_bytes,
		.members = refcount_read(&record->members),
	};
}

static inline void memcomp_test_record_put(void *handle)
{
	mc16_record_member_put(handle);
}

static inline void memcomp_test_record_handle_abort(void *handle)
{
	mc16_record_abort(handle);
}
#endif

#endif /* _MM_MEMCOMP_TESTS_TEST_API_H */
