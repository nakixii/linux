/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_CACHE_FOLIO_H
#define _MM_MEMCOMP_CACHE_FOLIO_H

#include <linux/highmem.h>

/* Include after cache_store.h and memcomp_arch_prepare_to_swap(). */
#define mc16_cache_folio_arch_prepare memcomp_arch_prepare_to_swap

/*
 * Caller holds the order-0 source folio lock throughout preparation and XA
 * replacement. Output is valid only on success and unchanged on failure.
 * Positive success transfers the initial cache ref, without another get.
 * Caller drops it if the expected XA replacement loses.
 * Zero success is preparation only: the winning XA replacement must publish
 * one zero cache owner under its XA lock. A losing zero preparation owes no put.
 */
static inline bool mc16_store_cache_folio(struct folio *folio,
					  unsigned long *token)
{
	struct memcomp_ref initial = { };
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	unsigned int nr_pages = folio ? folio_nr_pages(folio) : 1;
#endif
	void *src;
	int ret;

	if (!token || !folio || !folio_test_locked(folio) ||
	    folio_test_large(folio) || folio_mapped(folio) ||
	    folio_test_mlocked(folio) || folio_test_writeback(folio) ||
	    folio_maybe_dma_pinned(folio))
		goto fail_eligibility;
	if (!memcomp_is_enabled())
		goto fail_payload;
	/* The arch predicate includes tags, unlike a byte-only zero scan. */
	if (pages_identical(&folio->page, ZERO_PAGE(0))) {
		*token = 0;
		goto success;
	}
	src = kmap_local_folio(folio, 0);
	ret = mc16_cache_store_prepared(src, folio, &initial);
	kunmap_local(src);
	if (ret)
		goto fail_payload;
	/* The arch hook keys metadata off page_swap_entry(). */
	folio->swap = make_memcomp_entry(initial.token);
	ret = mc16_cache_folio_arch_prepare(folio);
	folio->swap.val = 0;
	if (ret) {
		mc16_ref_put(&initial);
		goto fail_arch;
	}
	*token = initial.token;
success:
	memcomp_debug_add(nr_pages, &memcomp_shmem_store_ok_pages);
	return true;

fail_eligibility:
	memcomp_debug_add(nr_pages, &memcomp_shmem_eligibility_fail_pages);
	goto fail;
fail_payload:
	memcomp_debug_add(nr_pages, &memcomp_shmem_payload_fail_pages);
	goto fail;
fail_arch:
	memcomp_debug_add(nr_pages, &memcomp_shmem_arch_fail_pages);
fail:
	memcomp_debug_add(nr_pages, &memcomp_shmem_store_fail_pages);
	return false;
}

/*
 * The successful XA replacer borrows a guard acquired before taking XA.
 * Cache preparation set PUBLISHING before the initial reference became
 * selectable. That marker is never reopened by a cache reader/writer.
 * The locked source pins its original folio_memcg until eviction completes;
 * truncate may remove the cache owner, but cannot retire our guarded slot.
 */
static inline void
mc16_publish_cache_eviction(struct folio *folio, struct memcomp_ref *ref)
{
	void *shadow;
	bool prepared;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	mutex_lock(memcomp_lock_of(ref->token));
	prepared = *mc16_shadow_ptr(ref) == MC16_SHADOW_PUBLISHING;
	mutex_unlock(memcomp_lock_of(ref->token));
	/* Idempotent: an already consumed shadow does not reopen its epoch. */
	if (!prepared)
		return;
	shadow = mc16_store_eviction(folio, NULL);
	mc16_store_publish(ref, 1, shadow);
}
#endif /* _MM_MEMCOMP_CACHE_FOLIO_H */
