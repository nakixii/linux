/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_MINCORE_H
#define _MM_MEMCOMP_MINCORE_H

#include <linux/pagemap.h>
#include <linux/swap.h>
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
#include <kunit/static_stub.h>
#endif

/* Include after storage/access/refs and the memcomp_slot_* helpers. */
#ifdef CONFIG_SWAP
static struct folio *mc16_mincore_swap_folio(swp_entry_t swap)
{
	struct swap_info_struct *si;
	struct folio *folio;

#ifdef CONFIG_MEMCOMP_KUNIT_TEST
	KUNIT_STATIC_STUB_REDIRECT(mc16_mincore_swap_folio, swap);
#endif
	si = get_swap_device(swap);
	if (!si)
		return ERR_PTR(-ENOENT);
	/* No allocation, read, readahead or swap-cache hit accounting. */
	folio = filemap_get_folio(swap_address_space(swap), swap_cache_index(swap));
	put_swap_device(si);
	return folio;
}
#endif

/* Process context only: the caller pins its PTE/XA owner across this query. */
static inline bool mc16_mincore(unsigned long token)
{
	struct memcomp_ref ref;
	struct mc16_view view;
	unsigned int state;
	bool present = false;
#ifdef CONFIG_SWAP
	struct mc16_swap_fields fields;
	struct folio *folio;
	swp_entry_t swap = { };
#endif

	might_sleep();
	if (!token)
		return true;
	if (!mc16_get(token, &ref))
		return false;
	mutex_lock(memcomp_lock_of(token));
	if (!mc16_view_locked(&ref, &view) || !mc16_flags_valid(*view.flags))
		goto unlock;
	state = mc16_flags_state(*view.flags);
	if ((*view.flags & MC16_F_RESIDENT) || state == MC16_PENDING ||
	    state == MC16_STORED || state == MC16_WRITEBACK ||
	    state == MC16_SAMEFILL) {
		present = true;
		goto unlock;
	}
#ifdef CONFIG_SWAP
	if (state == MC16_SWAPPED &&
	    mc16_swap_decode(*view.word, *view.length, &fields)) {
		swap = swp_entry(fields.type, fields.swap_offset);
		/* A token guard alone does not pin a backing changed by swapoff. */
		if (!swap.val || memcomp_slot_dup(swap))
			swap.val = 0;
	}
#endif
unlock:
	mutex_unlock(memcomp_lock_of(token));
#ifdef CONFIG_SWAP
	if (swap.val) {
		folio = mc16_mincore_swap_folio(swap);
		if (!IS_ERR_OR_NULL(folio)) {
			present = folio_test_uptodate(folio);
			folio_put(folio);
		}
		memcomp_slot_put(swap);
	}
#endif
	mc16_ref_put(&ref);
	return present;
}
#endif /* _MM_MEMCOMP_MINCORE_H */
