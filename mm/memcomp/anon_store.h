/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_ANON_STORE_H
#define _MM_MEMCOMP_ANON_STORE_H

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static DEFINE_PER_CPU(u64, mc16_refresh_cross_node_pages);
#endif

/* Placement hint only: store remains responsible for payload validation. */
static inline bool mc16_folio_zero_hint(struct folio *folio)
{
	unsigned int i;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	for (i = 0; i < folio_nr_pages(folio); i++) {
		void *src = kmap_local_folio(folio, i * PAGE_SIZE);
		bool zero = !memchr_inv(src, 0, PAGE_SIZE);

		kunmap_local(src);
		if (!zero)
			return false;
	}
	return true;
}

/* Called in the existing successful payload-commit T critical section. */
static inline void
mc16_zero_carrier_prepare_locked(struct memcomp_store_ctx *store,
				 struct memcomp_ref *ref, unsigned int nr_pages)
{
	struct mc16_view view;

	if (!store)
		return;
	store->zero = (struct mc16_zero_carrier) { };
	lockdep_assert_held(memcomp_lock_of(ref->token));
	if (nr_pages != 1 || !mc16_view_locked(ref, &view) ||
	    !mc16_flags_valid(*view.flags) ||
	    mc16_flags_state(*view.flags) != MC16_SAMEFILL ||
	    (*view.flags & (MC16_F_RESIDENT | MC16_F_REFRESH_PENDING)) ||
	    *view.word || *view.length)
		return;
	store->zero = (struct mc16_zero_carrier) {
		.ref = *ref, .token = ref->token, .valid = true, .ready = true,
	};
}

/* Success transfers anon_vma; failure leaves its hold with the caller. */
static inline int
mc16_anon_reserve(struct folio *folio, unsigned int nr_pages,
		  struct anon_vma *anon_vma, unsigned long *token)
{
	struct memcomp_ref refs[MEMCOMP_MAX_COMPRESS_PAGES];
	struct mc16_record *record;
	unsigned int i;
	bool zero_hint;
	int ret;

	if (!token || !folio || nr_pages != folio_nr_pages(folio))
		return -EINVAL;
	if (!memcomp_is_enabled())
		return -EOPNOTSUPP;
	zero_hint = mc16_folio_zero_hint(folio);
	record = mc16_record_alloc(folio, nr_pages, false);
	if (IS_ERR(record)) {
		memcomp_debug_inc(&memcomp_reserve_entry_alloc_fail);
		return PTR_ERR(record);
	}
	record->anon_vma = anon_vma;
	if (anon_vma)
		record->record_flags |= MC16_RECORD_ANON_VMA;
	ret = mc16_reserve_hint(record, nr_pages, MC16_KEY_ANON,
				zero_hint, refs);
	if (ret) {
		/* No published member and no transfer of the caller's hold. */
		record->anon_vma = NULL;
		record->record_flags &= ~MC16_RECORD_ANON_VMA;
		mc16_record_abort(record);
		return ret;
	}
	for (i = 0; i < nr_pages; i++)
		mc16_account_reservation(&refs[i]);
	/* These are the initial folio-owner references, not lookup guards. */
	*token = refs[0].token;
	return 0;
}

static inline int
mc16_refresh_validate(struct memcomp_ref *ref, struct folio **folio,
		      unsigned int nr_pages, unsigned int index,
		      bool *has_anon_vma)
{
	struct mc16_view view;
	int ret = 0;

	mutex_lock(memcomp_lock_of(ref->token));
	if (!mc16_view_locked(ref, &view) || !view.record ||
	    view.record->nr_pages != nr_pages ||
	    ref->token - view.record->token_base != index) {
		ret = -EIO;
	} else if (!mc16_flags_valid(*view.flags) ||
		   mc16_flags_state(*view.flags) != MC16_PENDING ||
		   (*view.flags & (MC16_F_RESIDENT | MC16_F_OWNER)) !=
				 (MC16_F_RESIDENT | MC16_F_OWNER) ||
		   !view.member->owner_folio) {
		ret = -EAGAIN;
	} else if (*folio && view.member->owner_folio != *folio) {
		ret = -EIO;
	} else {
		*folio = view.member->owner_folio;
		if (has_anon_vma)
			*has_anon_vma = view.record->record_flags &
					MC16_RECORD_ANON_VMA;
	}
	mutex_unlock(memcomp_lock_of(ref->token));
	return ret;
}

static inline int mc16_refresh_commit_error(struct memcomp_store_ctx *store)
{
	return 0;
}

/*
 * The caller owns one lookup guard per member and the locked source folio.
 * The shared codec helper retains its context/staging lease-transfer rules.
 * Validation and each commit hold T; all
 * preparation/charge/old-payload release runs outside T. A partial commit
 * leaves completed members in place for the existing folio rollback walk.
 */
static inline int
mc16_anon_refresh_full(struct memcomp_reclaim_ctx **ctxp, const void *src,
		       unsigned int nr_pages, struct memcomp_store_ctx *store,
		       struct memcomp_ref *refs, bool *has_anon_vma)
{
	struct mc16_payload payloads[MEMCOMP_MAX_COMPRESS_PAGES] = { };
	unsigned int lengths[MEMCOMP_MAX_COMPRESS_PAGES] = { };
	struct mc16_payload_tail tail = { };
	struct folio *folio = NULL;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	unsigned int nr_raw = 0;
#endif
	unsigned int i;
	bool held_anon_vma = false;
	int ret;

	if (store)
		store->zero = (struct mc16_zero_carrier) { };
	if (has_anon_vma)
		*has_anon_vma = false;
	if (!ctxp || !*ctxp || !src || !refs || !nr_pages ||
	    nr_pages > MEMCOMP_MAX_COMPRESS_PAGES || !is_power_of_2(nr_pages))
		return -EINVAL;
	if (!memcomp_is_enabled())
		return -EOPNOTSUPP;
	lockdep_assert_held(&(*ctxp)->lock);
	/* Check the complete caller-owned array before any cleanup can use it. */
	for (i = 0; i < nr_pages; i++) {
		if (!refs[i].slot)
			return -EINVAL;
	}
	for (i = 0; i < nr_pages; i++) {
		ret = mc16_refresh_validate(&refs[i], &folio, nr_pages, i,
					    i ? NULL : &held_anon_vma);
		if (ret)
			goto state_fail;
	}
	for (i = 0; i < nr_pages; i++) {
		unsigned long handle = 0;
		u8 pool = 0;

		ret = memcomp_prepare_payload_locked(ctxp, src + i * PAGE_SIZE,
						     PAGE_SIZE, &lengths[i], &handle,
						     &pool, nr_pages > 1,
						     nr_pages == 1 ? store : NULL,
						     memcomp_store_precomp(store, i));
		if (ret)
			goto payload_fail;
		payloads[i].word = handle;
		payloads[i].bytes = lengths[i];
	}
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	nr_raw = memcomp_raw_payload_count(lengths, nr_pages);
#endif
	if (!memcomp_payloads_net_reclaimable(lengths, nr_pages)) {
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		memcomp_debug_add(nr_raw, &memcomp_raw_reject_net);
#endif
		ret = -E2BIG;
		goto payload_fail;
	}
	for (i = 0; i < nr_pages; i++) {
		ret = mc16_payload_charge(&refs[i], &payloads[i]);
		if (ret)
			goto out;
	}
	for (i = 0; i < nr_pages; i++) {
		ret = mc16_refresh_commit_error(store);
		if (ret)
			goto state_fail;
		mutex_lock(memcomp_lock_of(refs[i].token));
		/* Close the gate before STORED/FIFO becomes visible under this T. */
		ret = mc16_store_begin_locked(&refs[i], folio, nr_pages, i);
		if (!ret) {
			ret = mc16_payload_commit_locked(&refs[i], folio, nr_pages, i,
							 &payloads[i], &tail);
			if (ret)
				*mc16_shadow_ptr(&refs[i]) = 0;
		}
		if (!ret)
			mc16_zero_carrier_prepare_locked(store, &refs[i], nr_pages);
		mutex_unlock(memcomp_lock_of(refs[i].token));
		if (ret)
			goto state_fail;
		/* The reserved node is immutable even if the folio migrated. */
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		if (refs[i].cluster->key->nid != folio_nid(folio))
			this_cpu_inc(mc16_refresh_cross_node_pages);
#endif
		mc16_payload_finish(&refs[i], &tail);
	}
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	memcomp_debug_add(nr_raw, &memcomp_raw_store);
#endif
	if (has_anon_vma)
		*has_anon_vma = held_anon_vma;
	ret = 0;
	goto out;
payload_fail:
	memcomp_debug_inc(&memcomp_refresh_payload_fail);
	memcomp_debug_add(nr_pages, &memcomp_refresh_payload_fail_pages);
	goto out;
state_fail:
	memcomp_debug_inc(&memcomp_refresh_state_fail);
	memcomp_debug_add(nr_pages, &memcomp_refresh_state_fail_pages);
out:
	if (ret && store)
		store->zero = (struct mc16_zero_carrier) { };
	for (i = 0; i < nr_pages; i++)
		mc16_payload_abort(&refs[i], &payloads[i]);
	return ret;
}

/* Direct refresh callers do not transfer the producer's anon_vma hold. */
static inline int
mc16_anon_refresh(struct memcomp_reclaim_ctx **ctxp, const void *src,
		  unsigned int nr_pages, struct memcomp_store_ctx *store,
		  struct memcomp_ref *refs)
{
	return mc16_anon_refresh_full(ctxp, src, nr_pages, store, refs, NULL);
}
#endif /* _MM_MEMCOMP_ANON_STORE_H */
