/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_WORKINGSET_LOAD_H
#define _MM_MEMCOMP_WORKINGSET_LOAD_H

#include <linux/delayacct.h>
#include <linux/psi.h>

/*
 * The guard pins the immutable source ID independently of objcg reparenting.
 * Charge may reclaim: callers retain no record pointer or T/PTL/XA lock.
 */
static inline int mc16_user_charge(struct memcomp_ref *ref, struct folio *folio,
				   struct mm_struct *mm, gfp_t gfp)
{
	return mem_cgroup_memcomp_charge(folio, mm, gfp,
					 ref->cluster->key->owner_id);
}

/*
 * Charge precedes this transaction. Stable nonresident slots never return to
 * PENDING. First validate the whole guarded span, then take-clear each word
 * under its own T. No T survives the call into the stock workingset policy.
 * Like swapcache insertion, a later read/install failure does not undo it.
 */
static inline int mc16_user_refault(struct memcomp_ref *refs, unsigned int nr,
				    struct folio *folio)
{
	unsigned long shadow = 0;
	unsigned int i;

	for (i = 0; i < nr; i++) {
		struct mc16_view view;
		u64 *word;
		int ret = 0;

		mutex_lock(memcomp_lock_of(refs[i].token));
		word = mc16_shadow_ptr(&refs[i]);
		if (!word || !mc16_view_locked(&refs[i], &view) ||
		    !mc16_flags_valid(*view.flags))
			ret = -EIO;
		else if (mc16_flags_state(*view.flags) == MC16_PENDING ||
			 (*view.flags & MC16_F_RESIDENT) ||
			 *word == MC16_SHADOW_PUBLISHING)
			ret = -EAGAIN;
		else if (*word && !xa_is_value((void *)(unsigned long)*word))
			ret = -EIO;
		mutex_unlock(memcomp_lock_of(refs[i].token));
		if (ret)
			return ret;
	}
	for (i = 0; i < nr; i++) {
		u64 *word;

		mutex_lock(memcomp_lock_of(refs[i].token));
		word = mc16_shadow_ptr(&refs[i]);
		if (!i)
			shadow = *word;
		*word = 0;
		mutex_unlock(memcomp_lock_of(refs[i].token));
	}
	if (shadow)
		workingset_refault(folio, (void *)shadow);
	return 0;
}

struct mc16_user_read {
	unsigned long pflags;
	bool in_thrashing;
	bool workingset;
};

static inline void mc16_user_read_start(struct mc16_user_read *scope,
					struct folio *folio)
{
	scope->workingset = folio_test_workingset(folio);
	if (scope->workingset) {
		delayacct_thrashing_start(&scope->in_thrashing);
		psi_memstall_enter(&scope->pflags);
	}
	delayacct_swapin_start();
}

static inline void mc16_user_read_end(struct mc16_user_read *scope)
{
	if (scope->workingset) {
		delayacct_thrashing_end(&scope->in_thrashing);
		psi_memstall_leave(&scope->pflags);
	}
	delayacct_swapin_end();
}

static inline void mc16_user_major(struct mm_struct *mm)
{
	count_vm_event(PGMAJFAULT);
	count_memcg_event_mm(mm, PGMAJFAULT);
}
#endif /* _MM_MEMCOMP_WORKINGSET_LOAD_H */
