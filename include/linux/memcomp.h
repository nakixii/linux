/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMCOMP_H
#define _LINUX_MEMCOMP_H

#include <linux/errno.h>
#include <linux/mm_types.h>
#include <linux/stddef.h>
#include <linux/types.h>

struct folio;
struct mc16_slot;
struct mc16_cluster;
struct memcomp_reclaim_ctx;
struct vm_fault;

/*
 * Caller-owned lookup guard.  Initialize to zero and test only @slot for a
 * held guard; storage fields are opaque to callers.  Release consumes and
 * clears the whole guard, including when the underlying put is not final.
 */
struct memcomp_ref {
	struct mc16_slot *slot;
	struct mc16_cluster *cluster;
	u32 token;
};

enum memcomp_reclaim_source {
	MEMCOMP_RECLAIM_DIRECT,
	MEMCOMP_RECLAIM_KSWAPD,
	MEMCOMP_RECLAIM_SOURCE_NR,
};

enum memcomp_anon_fail_event {
	MEMCOMP_ANON_UNMAP_FAIL,
	MEMCOMP_ANON_FINISH_FAIL,
	/* Not tried, for having not compressed before. */
	MEMCOMP_ANON_INCOMPRESSIBLE_SKIP,
};

#ifdef CONFIG_MEMCOMP

bool memcomp_available(void);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
void memcomp_count_initial_compress(void);
void memcomp_count_normal_swap_fallback(void);
#else
static inline void memcomp_count_initial_compress(void)
{
}

static inline void memcomp_count_normal_swap_fallback(void)
{
}
#endif
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
void memcomp_count_anon_fail(enum memcomp_anon_fail_event event,
			     unsigned int nr_pages);
void memcomp_count_split_refused(struct folio *folio);
#else

static inline void
memcomp_count_anon_fail(enum memcomp_anon_fail_event event,
			unsigned int nr_pages)
{
}

static inline void memcomp_count_split_refused(struct folio *folio)
{
}
#endif
/*
 * Reject fresh foreground reservations early when no context can currently
 * be admitted.  A true result is advisory; acquisition can still race.
 */
bool memcomp_reclaim_ctx_precheck(unsigned int nr_pages,
				  enum memcomp_reclaim_source source);
/* Acquire after reserving tokens and release before rollback or finish. */
struct memcomp_reclaim_ctx *
memcomp_reclaim_ctx_get(unsigned int nr_pages,
			enum memcomp_reclaim_source source);
void memcomp_reclaim_ctx_put(struct memcomp_reclaim_ctx *ctx);
/* Read-only reservation preflight; allocation can still fail afterwards. */
bool memcomp_reserve_eligible(struct folio *folio);
/* Reserve tokens for an eligible folio before unmapping it. */
bool memcomp_reserve(struct folio *folio);
/* A non-NULL *@ctxp lease is consumed and cleared on every return. */
bool memcomp_store_after_unmap(struct folio *folio,
			       struct memcomp_reclaim_ctx **ctxp);
void memcomp_rollback_folio(struct folio *folio);
bool memcomp_folio_dirty(struct folio *folio);
bool memcomp_folio_incompressible(struct folio *folio);
/* Return fault flags (or zero to retry), never negative errno. */
vm_fault_t memcomp_load(struct vm_fault *vmf, bool *locked);
/* Return 1 if resident, 0 if movable/absent, or -EAGAIN if busy. */
int memcomp_entry_resident(unsigned long token);
/*
 * Return 1 if resident, 0 if movable/absent, or -EAGAIN if busy.
 * A successful lookup holds a guard even for a nonresident token; absent
 * and busy lookups clear it.  The caller must put any held guard.
 */
int memcomp_entry_resident_get(unsigned long token, struct memcomp_ref *ref);
void memcomp_entry_resident_put(struct memcomp_ref *ref);
/* Consumes and clears the held guard on every return. */
int memcomp_restore_resident(struct vm_fault *vmf, struct memcomp_ref *ref);
void memcomp_invalidate(unsigned long token);
void memcomp_invalidate_cache(unsigned long token);
/* Called only by the winning zero-cache XA replacement while locked. */
void memcomp_publish_cache_zero(void);
void memcomp_invalidate_nr(unsigned long token, unsigned int nr_pages);
void memcomp_invalidate_folio(struct folio *folio);
bool memcomp_release_folio(struct folio *folio);
int memcomp_migrate_folio(struct folio *dst, struct folio *src);
void memcomp_migrate_folio_rollback(struct folio *dst, struct folio *src);
unsigned int memcomp_entry_nr_pages(unsigned long token,
				    unsigned long *base_token);
/*
 * Mapcount approximates unified references, including temporary guards.
 * A compressed-size query may return false when busy and clears outputs;
 * compressed == NULL is a refs-only observation permitted under PTL.
 */
bool memcomp_entry_stat(unsigned long token, unsigned long *compressed,
			int *mapcount);
/* Internal guards, not PTE references; a failed get clears the output. */
bool memcomp_entry_get_ref(unsigned long token, struct memcomp_ref *ref);
void memcomp_entry_put_ref(struct memcomp_ref *ref);
bool memcomp_entry_set_refresh_pending(unsigned long token);
void memcomp_entry_clear_refresh_pending(unsigned long token);
bool memcomp_entry_get(unsigned long token);
bool memcomp_entry_present(unsigned long token);

/*
 * Page-cache variants for shmem/tmpfs pages. A zero store result prepares
 * a value only: its winning XA replacement must publish the cache owner
 * before unlocking. An aborted zero preparation creates no owner to drop.
 */
bool memcomp_store_cache(struct folio *folio, unsigned long *token);
/*
 * Pin rejects token0 with an empty guard. Explicit zero load consumes no
 * owner; the winning shadow replacement drops its cache owner separately.
 * Pin must be resolved with exactly one abort or commit call.  Commit also
 * drops the shadow owner and is valid only after replacing the expected
 * shadow in the page cache.  Failed pins clear the output; abort and commit
 * consume and clear the guard.  Load keeps the guard on every return.
 */
bool memcomp_load_folio_pin(unsigned long token, struct memcomp_ref *ref);
int memcomp_load_folio(struct memcomp_ref *ref, struct folio *folio);
int memcomp_load_folio_zero(struct folio *folio);
void memcomp_load_folio_abort(struct memcomp_ref *ref);
void memcomp_load_folio_commit(struct memcomp_ref *ref);

/* Check if a page-cache xa_value is a memcomp entry. */
bool is_memcomp_shadow(void *shadow);
unsigned long memcomp_shadow_token(void *shadow);

#else

static inline bool memcomp_available(void)
{
	return false;
}

static inline void memcomp_count_initial_compress(void)
{
}

static inline void memcomp_count_normal_swap_fallback(void)
{
}

static inline void
memcomp_count_anon_fail(enum memcomp_anon_fail_event event,
			unsigned int nr_pages)
{
}

static inline void memcomp_count_split_refused(struct folio *folio)
{
}

static inline struct memcomp_reclaim_ctx *
memcomp_reclaim_ctx_get(unsigned int nr_pages,
			enum memcomp_reclaim_source source)
{
	return NULL;
}

static inline bool
memcomp_reclaim_ctx_precheck(unsigned int nr_pages,
			     enum memcomp_reclaim_source source)
{
	return true;
}

static inline void
memcomp_reclaim_ctx_put(struct memcomp_reclaim_ctx *ctx)
{
}

static inline bool memcomp_reserve_eligible(struct folio *folio)
{
	return false;
}

static inline bool memcomp_reserve(struct folio *folio)
{
	return false;
}

static inline bool
memcomp_store_after_unmap(struct folio *folio,
			  struct memcomp_reclaim_ctx **ctxp)
{
	if (ctxp)
		*ctxp = NULL;
	return false;
}

static inline void memcomp_rollback_folio(struct folio *folio)
{
}

static inline vm_fault_t memcomp_load(struct vm_fault *vmf, bool *locked)
{
	return VM_FAULT_SIGBUS;
}

static inline int memcomp_entry_resident(unsigned long token)
{
	return 0;
}

static inline int memcomp_entry_resident_get(unsigned long token,
					     struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
	return 0;
}

static inline void memcomp_entry_resident_put(struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
}

static inline int memcomp_restore_resident(struct vm_fault *vmf,
					   struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
	return 0;
}

static inline void memcomp_invalidate(unsigned long token)
{
}

static inline void memcomp_invalidate_cache(unsigned long token)
{
}

static inline void memcomp_publish_cache_zero(void)
{
}

static inline void memcomp_invalidate_nr(unsigned long token,
					 unsigned int nr_pages)
{
}

static inline void memcomp_invalidate_folio(struct folio *folio)
{
}

static inline bool memcomp_folio_incompressible(struct folio *folio)
{
	return false;
}

static inline bool memcomp_release_folio(struct folio *folio)
{
	return false;
}

static inline int memcomp_migrate_folio(struct folio *dst,
					struct folio *src)
{
	return 0;
}

static inline void memcomp_migrate_folio_rollback(struct folio *dst,
						  struct folio *src)
{
}

static inline unsigned int memcomp_entry_nr_pages(unsigned long token,
						  unsigned long *base_token)
{
	if (base_token)
		*base_token = token;
	return 0;
}

static inline bool memcomp_entry_stat(unsigned long token,
				      unsigned long *compressed,
					  int *mapcount)
{
	return false;
}

static inline bool memcomp_entry_get_ref(unsigned long token,
					 struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
	return false;
}

static inline void memcomp_entry_put_ref(struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
}

static inline bool memcomp_entry_set_refresh_pending(unsigned long token)
{
	return false;
}

static inline void memcomp_entry_clear_refresh_pending(unsigned long token)
{
}

static inline bool memcomp_entry_get(unsigned long token)
{
	return false;
}

static inline bool memcomp_entry_present(unsigned long token)
{
	return false;
}

static inline bool memcomp_store_cache(struct folio *folio,
				       unsigned long *token)
{
	return false;
}

static inline bool memcomp_load_folio_pin(unsigned long token,
					  struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
	return false;
}

static inline int memcomp_load_folio(struct memcomp_ref *ref,
				     struct folio *folio)
{
	*ref = (struct memcomp_ref) { };
	return -ENOENT;
}

static inline int memcomp_load_folio_zero(struct folio *folio)
{
	return -ENOENT;
}

static inline void memcomp_load_folio_abort(struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
}

static inline void memcomp_load_folio_commit(struct memcomp_ref *ref)
{
	*ref = (struct memcomp_ref) { };
}

static inline bool is_memcomp_shadow(void *shadow)
{
	return false;
}

static inline unsigned long memcomp_shadow_token(void *shadow)
{
	return 0;
}

#endif

#endif
