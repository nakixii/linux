/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_WORKINGSET_STORE_H
#define _MM_MEMCOMP_WORKINGSET_STORE_H

/*
 * Author: nakixii
 *
 * Include after storage/access/refs. A slot guard protects the side word;
 * its token mutex T serializes every change. PUBLISHING excludes payload
 * readers and background actors, not token reference removal by PTE zap.
 * Only a source OWNER in its first PENDING epoch can open this gate.
 */
#ifdef CONFIG_MEMCOMP_KUNIT_TEST
#include <kunit/static_stub.h>
static void *mc16_store_eviction(struct folio *folio,
				 struct mem_cgroup *target_memcg)
{
	KUNIT_STATIC_STUB_REDIRECT(mc16_store_eviction, folio, target_memcg);
	return workingset_memcomp_eviction(folio, target_memcg);
}
#else
#define mc16_store_eviction workingset_memcomp_eviction
#endif

static inline int
mc16_store_begin_locked(struct memcomp_ref *ref, struct folio *folio,
			unsigned int nr, unsigned int index)
{
	struct mc16_view view;

	lockdep_assert_held(memcomp_lock_of(ref->token));
	if (!mc16_owner_matches_locked(ref, folio, nr, index) ||
	    !mc16_view_locked(ref, &view) || !mc16_flags_valid(*view.flags) ||
	    mc16_flags_state(*view.flags) != MC16_PENDING ||
	    !(*view.flags & MC16_F_RESIDENT) || *mc16_shadow_ptr(ref))
		return -EAGAIN;
	*mc16_shadow_ptr(ref) = MC16_SHADOW_PUBLISHING;
	return 0;
}

/*
 * The caller has crossed its last fallible point with all span guards held.
 * The source folio lock, source OWNER and PUBLISHING prevent any actor from
 * changing this epoch. No T, PTL or XArray lock is held during eviction.
 * Each base page gets the same stock shadow, with one folio-sized delta.
 */
static inline void
mc16_store_publish(struct memcomp_ref *refs, unsigned int nr, void *shadow)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		mutex_lock(memcomp_lock_of(refs[i].token));
		VM_WARN_ON_ONCE(*mc16_shadow_ptr(&refs[i]) != MC16_SHADOW_PUBLISHING);
		/* Make this epoch's shadow visible before admitting user reads. */
		smp_store_release(mc16_shadow_ptr(&refs[i]),
				  (u64)(unsigned long)shadow);
		mutex_unlock(memcomp_lock_of(refs[i].token));
	}
}

#endif /* _MM_MEMCOMP_WORKINGSET_STORE_H */
