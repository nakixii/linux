/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_MIGRATE_H
#define _MM_MEMCOMP_MIGRATE_H

/* Include after access.h, span.h and refs.h; no public dispatch is changed. */
static inline bool
mc16_migrate_owner_locked(struct memcomp_ref *ref, struct folio *folio,
			  unsigned int nr_pages, unsigned int index,
			  struct mc16_view *view)
{
	unsigned int offset;

	lockdep_assert_held(memcomp_lock_of(ref->token));
	if (!mc16_owner_matches_locked(ref, folio, nr_pages, index) ||
	    !mc16_view_locked(ref, view) || !mc16_flags_valid(*view->flags) ||
	    mc16_flags_state(*view->flags) == MC16_PENDING ||
	    mc16_flags_state(*view->flags) == MC16_EMPTY ||
	    (*view->flags & MC16_F_RESIDENT))
		return false;
	return mc16_ref_geometry(ref, NULL, &offset) == nr_pages && offset == index;
}

/* Failure window only: successful ownership changes always execute below. */
static inline int
mc16_migrate_move_error(struct memcomp_ref *ref, struct folio *from,
			struct folio *to, unsigned int index)
{
	return 0;
}

static inline bool
mc16_migrate_move_owner(struct memcomp_ref *ref, struct folio *from,
			struct folio *to, unsigned int nr_pages,
			unsigned int index)
{
	struct mc16_view view;
	bool move;

	if (mc16_migrate_move_error(ref, from, to, index))
		return false;
	mutex_lock(memcomp_lock_of(ref->token));
	move = mc16_migrate_owner_locked(ref, from, nr_pages, index, &view);
	if (move)
		view.member->owner_folio = to;
	mutex_unlock(memcomp_lock_of(ref->token));
	return move;
}

/*
 * Both folios stay locked throughout the transaction and any core rollback.
 * Get every slot before preflight; the guards protect slots, never records.
 * An attached member's OWNER cannot detach while its owner folio is locked.
 * All record/member access is under its own T; no borrowed view escapes T.
 *
 * The record's producer is its immutable reservation anchor, only dereferenced
 * during reserve/publication. Post-publication ownership uses owner_folio.
 * Moving a nonresident OWNER transfers no resident folio reference or charge.
 * key->nid and FIFO eligibility stay at the reservation node (approved N6).
 */
static inline int mc16_migrate_folio(struct folio *dst, struct folio *src)
{
	struct memcomp_ref refs[MEMCOMP_MAX_COMPRESS_PAGES] = { };
	unsigned int nr_pages = folio_nr_pages(src), i;
	unsigned long token;
	struct mc16_view view;
	swp_entry_t swap;
	int ret = -EAGAIN;
	bool match;

	if (src == dst || !folio_test_locked(src) || !folio_test_locked(dst))
		return -EAGAIN;
	if (!folio_test_anon(src) || !folio_test_swapbacked(src) ||
	    folio_mapping(src) || folio_test_swapcache(src))
		return 0;
	swap = src->swap;
	if (!folio_memcomp_reserved(src)) {
		if (folio_test_incompressible(src))
			dst->swap = swap;
		return 0;
	}
	token = swp_offset(swap);
	if (!token || !nr_pages || nr_pages > MEMCOMP_MAX_COMPRESS_PAGES ||
	    token > U32_MAX - (nr_pages - 1) || folio_nr_pages(dst) != nr_pages)
		return -EAGAIN;
	for (i = 0; i < nr_pages; i++) {
		if (!mc16_get(token + i, &refs[i]))
			goto out;
	}
	for (i = 0; i < nr_pages; i++) {
		mutex_lock(memcomp_lock_of(refs[i].token));
		match = mc16_migrate_owner_locked(&refs[i], src, nr_pages, i, &view);
		mutex_unlock(memcomp_lock_of(refs[i].token));
		if (!match)
			goto out;
	}
	for (i = 0; i < nr_pages; i++) {
		if (mc16_migrate_move_owner(&refs[i], src, dst, nr_pages, i))
			continue;
		while (i) {
			i--;
			WARN_ON_ONCE(!mc16_migrate_move_owner(&refs[i], dst, src,
							      nr_pages, i));
		}
		goto out;
	}
	dst->swap = swap;
	ret = 1;
out:
	for (i = 0; i < nr_pages; i++) {
		if (refs[i].slot)
			mc16_ref_put(&refs[i]);
	}
	return ret;
}

/*
 * Core migration failed after a successful handoff. Prefetch all available
 * guards, then restore owners in reverse order, skipping missing/nonmatching
 * members. Clear only a reserved destination mark, including
 * malformed ranges; an incompressible mark remains the core's responsibility.
 * Invalid lock callers retain the mark so they can retry while correctly held.
 */
static inline void
mc16_migrate_folio_rollback(struct folio *dst, struct folio *src)
{
	struct memcomp_ref refs[MEMCOMP_MAX_COMPRESS_PAGES] = { };
	unsigned int nr_pages = folio_nr_pages(src), i;
	unsigned long token;

	if (src == dst || !folio_test_locked(src) || !folio_test_locked(dst))
		return;
	if (!folio_memcomp_reserved(dst))
		return;
	token = swp_offset(dst->swap);
	if (!token || !nr_pages || nr_pages > MEMCOMP_MAX_COMPRESS_PAGES ||
	    token > U32_MAX - (nr_pages - 1))
		goto clear;
	for (i = 0; i < nr_pages; i++)
		mc16_get(token + i, &refs[i]);
	for (i = nr_pages; i; ) {
		i--;
		if (refs[i].slot)
			mc16_migrate_move_owner(&refs[i], dst, src, nr_pages, i);
	}
	for (i = 0; i < nr_pages; i++) {
		if (refs[i].slot)
			mc16_ref_put(&refs[i]);
	}
clear:
	dst->swap.val = 0;
}
#endif /* _MM_MEMCOMP_MIGRATE_H */
