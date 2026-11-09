/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_ANON_FOLIO_H
#define _MM_MEMCOMP_ANON_FOLIO_H

/* Caller owns the context lease; the shared finish tail consumes it. */
static inline bool
mc16_anon_store_internal(struct folio *folio,
			 struct memcomp_reclaim_ctx **ctxp,
			 struct memcomp_store_ctx *store,
			 enum memcomp_anon_store_fail *failure)
{
	struct memcomp_ref refs[MEMCOMP_MAX_COMPRESS_PAGES];
	unsigned int nr_pages = folio_nr_pages(folio), old_nr = 1, i;
	struct memcomp_ref head;
	unsigned long token;
	u32 base;
	bool has_anon_vma;
	void *src;
	int ret;

	lockdep_assert_held(&(*ctxp)->lock);
	if (store)
		store->zero = (struct mc16_zero_carrier) { };
	*failure = MEMCOMP_ANON_STORE_PREFLIGHT;
	if (!folio_test_locked(folio))
		return false;
	if (folio_mapped(folio)) {
		*failure = MEMCOMP_ANON_STORE_FOLIO_BUSY;
		return false;
	}
	if (!nr_pages || nr_pages > MEMCOMP_MAX_COMPRESS_PAGES ||
	    !folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
	    folio_test_mlocked(folio) || folio_mapping(folio) ||
	    folio_test_swapcache(folio) || !is_memcomp_entry(folio->swap))
		return false;
	token = swp_offset(folio->swap);
	if (!token || token > U32_MAX)
		return false;
	if (folio_test_large(folio) && mc16_get(token, &head)) {
		old_nr = mc16_ref_geometry(&head, &base, NULL);
		if (old_nr)
			token = base;
		mc16_ref_put(&head);
	}
	if (old_nr != nr_pages ||
	    !mc16_span_get(token, folio, nr_pages, true, refs))
		return false;
	if (folio_maybe_dma_pinned(folio) ||
	    folio_ref_count(folio) != 1 + nr_pages || folio_mapped(folio)) {
		*failure = MEMCOMP_ANON_STORE_FOLIO_BUSY;
		ret = -EBUSY;
		goto out;
	}
	if (memcomp_arch_prepare_to_swap(folio)) {
		*failure = MEMCOMP_ANON_STORE_ARCH;
		ret = -EIO;
		goto out;
	}
	src = kmap_local_folio(folio, 0);
	ret = mc16_anon_refresh_full(ctxp, src, nr_pages, store, refs,
				     &has_anon_vma);
	kunmap_local(src);
	if (!ret && has_anon_vma) {
		mutex_lock(memcomp_lock_of(refs[0].token));
		store->anon_vma = mc16_span_take_anon_vma_locked(&refs[0], folio,
								 nr_pages);
		mutex_unlock(memcomp_lock_of(refs[0].token));
	} else if (ret) {
		*failure = MEMCOMP_ANON_STORE_REFRESH;
		if (ret == -E2BIG) {
			struct mc16_view view;

			mutex_lock(memcomp_lock_of(refs[0].token));
			if (mc16_view_locked(&refs[0], &view) && view.record)
				*view.flags |= MC16_F_INCOMPRESSIBLE;
			mutex_unlock(memcomp_lock_of(refs[0].token));
		}
	}
out:
	for (i = 0; i < nr_pages; i++)
		mc16_ref_put(&refs[i]);
	return !ret;
}
#endif /* _MM_MEMCOMP_ANON_FOLIO_H */
