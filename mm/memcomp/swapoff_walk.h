/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_SWAPOFF_WALK_H
#define _MM_MEMCOMP_SWAPOFF_WALK_H

#include "scan.h"

#ifdef CONFIG_SWAP
#define MC16_SWAPOFF_SCAN_BATCH 32U

/*
 * All required ops and ctx remain valid until the walk returns. flush returns
 * zero or -errno and settles all staging under W. restore borrows ref; it
 * owns state/type validation and must return with all actor-local resources
 * released, without consuming or retaining the scanner's guard.
 *
 * interrupted may be NULL to use signal_pending(current). The optional test
 * observers borrow their arguments only for the call: scanned runs under W,
 * unlocked runs with no W and no scanner/actor guard. Neither may retain a
 * slot, cluster, record or backing snapshot across W. Production bindings
 * (mc16_swapoff_entry and the slot batch flush) are supplied by the caller.
 */
struct mc16_swapoff_walk_ops {
	struct mutex *lock; /* Shared writeback mutex W. */
	int (*flush)(void *ctx);
	int (*restore)(struct memcomp_ref *ref, unsigned int type, void *ctx);
	bool (*interrupted)(void *ctx);
	void (*scanned)(u64 cursor, unsigned int used,
			enum mc16_scan_result result,
			const struct memcomp_ref *ref, void *ctx);
	void (*unlocked)(void *ctx);
	void *ctx;
};

static inline bool
mc16_swapoff_walk_interrupted(const struct mc16_swapoff_walk_ops *ops)
{
	return ops->interrupted ? ops->interrupted(ops->ctx) :
		signal_pending(current);
}

/*
 * Caller cancels any W-taking drain work before entry. The same W serializes
 * every flush and restore. Only a u64 forward cursor survives a batch unlock;
 * no saved registry, new lookup or T acquisition is needed.
 * The 32-position bound inherits scan.h's XA_RETRY_ENTRY retry exception and
 * is not a fixed scheduling/latency bound. A full slice yields even when EOF
 * is discovered only by the next scan; a known end never needs another flush.
 */
static inline int
mc16_swapoff_walk(unsigned int type, const struct mc16_swapoff_walk_ops *ops)
{
	unsigned int nofs_flag, noreclaim_flag, scanned = 0;
	u64 cursor = 0;
	int ret;

	nofs_flag = memalloc_nofs_save();
	noreclaim_flag = memalloc_noreclaim_save();
	mutex_lock(ops->lock);
	ret = ops->flush(ops->ctx);
	if (ret)
		goto out_unlock;
	for (;;) {
		struct memcomp_ref ref = { };
		enum mc16_scan_result result;
		unsigned int used;

		if (mc16_swapoff_walk_interrupted(ops)) {
			ret = -EINTR;
			break;
		}
		result = mc16_scan_next(&cursor,
					MC16_SWAPOFF_SCAN_BATCH - scanned, &ref, &used);
		scanned += used;
		if (ops->scanned)
			ops->scanned(cursor, used, result, &ref, ops->ctx);
		/* As in the old walk, an empty EOF ends before another signal check. */
		if (result == MC16_SCAN_END && !used)
			break;
		if (mc16_swapoff_walk_interrupted(ops)) {
			if (ref.slot)
				mc16_ref_put(&ref);
			ret = -EINTR;
			break;
		}
		if (result == MC16_SCAN_GUARD) {
			ret = ops->restore(&ref, type, ops->ctx);
			mc16_ref_put(&ref);
			if (ret < 0)
				break;
			/* Counts restored by the actor must not reach try_to_unuse. */
			ret = 0;
		}
		if (mc16_swapoff_walk_interrupted(ops)) {
			ret = -EINTR;
			break;
		}
		if (result == MC16_SCAN_END || cursor == MC16_SCAN_END_CURSOR)
			break;
		if (scanned == MC16_SWAPOFF_SCAN_BATCH) {
			/* ref is empty; no scanner or actor resource survives W. */
			mutex_unlock(ops->lock);
			if (ops->unlocked)
				ops->unlocked(ops->ctx);
			cond_resched();
			mutex_lock(ops->lock);
			/* A writer may have retired slots and staged new writes. */
			ret = ops->flush(ops->ctx);
			if (ret)
				break;
			scanned = 0;
		} else {
			cond_resched();
		}
	}
out_unlock:
	mutex_unlock(ops->lock);
	memalloc_noreclaim_restore(noreclaim_flag);
	memalloc_nofs_restore(nofs_flag);
	return ret;
}
#endif /* CONFIG_SWAP */
#endif /* _MM_MEMCOMP_SWAPOFF_WALK_H */
