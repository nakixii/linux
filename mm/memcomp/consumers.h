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

/* Empty output on failure, including token0; caller handles zero explicitly. */

/*
 * Keep this same guard through load and the caller's xarray transaction.
 * Failure leaves both the guard and cache owner intact for abort/retry.
 */

/* Only the winner replacing the expected cache shadow may consume its owner. */

/*
 * Explicit token0 branch: no guard, lookup or owner consumption. After the
 * expected zero shadow is replaced successfully, caller must cache_drop(0).
 * An aborted xarray transaction leaves the zero cache owner unchanged.
 */
#endif /* _MM_MEMCOMP_CONSUMERS_H */
