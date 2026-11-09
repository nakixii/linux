/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_CONSUMERS_H
#define _MM_MEMCOMP_CONSUMERS_H

#include <linux/highmem.h>

/*
 * Include after load.h and zero.h, with memcomp_arch_restore() available.
 * load.h must touch MC16_F_REFERENCED in its existing T critical section.
 * These adapters do not publish PTEs or replace page-cache xarray entries.
 */

/* Parent PTL protects its owner; the new reference becomes the child PTE's. */
static inline bool mc16_pte_get(unsigned long token)
{
	struct memcomp_ref ref;

	if (!token)
		return mc16_zero_pte_get(1);
	/* Successful lookup ownership transfers directly to the new PTE. */
	return mc16_get(token, &ref);
}

/*
 * Caller already removed nr PTE owners. Positive tokens name consecutive
 * slots; zero names nr logical PTEs in the zero domain. Reject an invalid
 * positive range in full before consuming anything; absent slots are skipped.
 */
static inline void mc16_pte_put_nr(unsigned long token, unsigned int nr)
{
	struct memcomp_ref ref;
	unsigned int i;

	if (!nr)
		return;
	if (!token) {
		mc16_zero_pte_put(nr);
		return;
	}
	if (token > U32_MAX || nr - 1 > U32_MAX - token)
		return;
	for (i = 0; i < nr; i++) {
		if (mc16_get(token + i, &ref))
			mc16_ref_put_count(&ref, 2); /* lookup + removed PTE */
	}
}

/* The caller removed one cache owner, independently of any PTE owner. */
static inline void mc16_cache_drop(unsigned long token)
{
	struct memcomp_ref ref;

	if (!token) {
		mc16_zero_cache_put(1);
		return;
	}
	if (mc16_get(token, &ref))
		mc16_ref_put_count(&ref, 2); /* lookup + removed cache owner */
}

/* Empty output on failure, including token0; caller handles zero explicitly. */
static inline bool mc16_cache_pin(unsigned long token, struct memcomp_ref *ref)
{
	return mc16_get(token, ref);
}

/*
 * Keep this same guard through load and the caller's xarray transaction.
 * Failure leaves both the guard and cache owner intact for abort/retry.
 */
static inline int mc16_cache_load(struct memcomp_ref *ref, struct folio *folio)
{
	struct mc16_user_read scope;
	void *dst;
	int ret;

	if (!ref || !ref->slot)
		return -ENOENT;
	if (!folio || folio_test_large(folio) || !folio_test_locked(folio))
		return -EINVAL;
	ret = mc16_user_refault(ref, 1, folio);
	if (ret)
		return ret;
	mc16_user_read_start(&scope, folio);
	dst = kmap_local_folio(folio, 0);
	ret = mc16_load_page(ref, dst);
	kunmap_local(dst);
	mc16_user_read_end(&scope);
	if (!ret) {
		__folio_mark_uptodate(folio);
		flush_dcache_folio(folio);
		memcomp_arch_restore(ref->token, folio);
	}
	return ret;
}

static inline void mc16_cache_abort(struct memcomp_ref *ref)
{
	if (ref && ref->slot)
		mc16_ref_put(ref);
}

/* Only the winner replacing the expected cache shadow may consume its owner. */
static inline void mc16_cache_commit(struct memcomp_ref *ref)
{
	if (ref && ref->slot)
		mc16_ref_put_count(ref, 2); /* same guard + cache owner */
}

/*
 * Explicit token0 branch: no guard, lookup or owner consumption. After the
 * expected zero shadow is replaced successfully, caller must cache_drop(0).
 * An aborted xarray transaction leaves the zero cache owner unchanged.
 */
static inline int mc16_cache_load_zero(struct folio *folio)
{
	if (!folio || folio_test_large(folio) || !folio_test_locked(folio))
		return -EINVAL;
	clear_highpage(folio_page(folio, 0));
	__folio_mark_uptodate(folio);
	flush_dcache_folio(folio);
	memcomp_arch_restore(0, folio);
	return 0;
}
#endif /* _MM_MEMCOMP_CONSUMERS_H */
