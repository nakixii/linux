/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_DRAIN_H
#define _MM_MEMCOMP_DRAIN_H

#ifdef CONFIG_SWAP
/* Include after the mc16 batch and the existing writeback pressure helpers. */

/* Caller owns W in its nofs/noreclaim scope; every return drains staging. */
static inline unsigned long
mc16_writeback_drain_queue(int nid, struct memcomp_lru *fifo,
			   unsigned long limit, unsigned long *freedp,
			   int *write_retp, bool *attemptedp)
{
	unsigned long scanned = 0;

	lockdep_assert_held(&memcomp_writeback_lock);
	while (scanned < limit) {
		struct mc16_writeback_member staged = { };
		struct memcomp_ref ref;
		unsigned int budget, before;
		bool found;
		int ret;

		/* Leave headroom before claiming ownership under T. */
		if (mc16_writeback_batch_full()) {
			ret = mc16_writeback_batch_flush(freedp);
			if (ret) {
				if (ret == -ENOSPC)
					memcomp_reclaim_writeback_stop(nid);
				if (!*write_retp)
					*write_retp = ret;
				break;
			}
		}
		budget = min_t(unsigned long, limit - scanned, UINT_MAX);
		before = budget;
		found = mc16_fifo_select(fifo, &budget, &ref);
		scanned += before - budget;
		if (!found) {
			/* A limit wider than u32 may need another bounded slice. */
			if (!budget && scanned < limit) {
				cond_resched();
				continue;
			}
			break;
		}

		/* No L or X survives the selector; reuse this one T for claim. */
		mutex_lock(memcomp_lock_of(ref.token));
		if (mc16_fifo_second_chance_locked(&ref))
			ret = -EAGAIN;
		else
			ret = mc16_writeback_stage_locked(&ref, &staged);
		mutex_unlock(memcomp_lock_of(ref.token));
		if (!ret) {
			*attemptedp = true;
			ret = mc16_writeback_stage(&staged);
		}
		/* stage consumes its own guard; this is the selector's guard. */
		mc16_ref_put(&ref);
		if (ret && ret != -EAGAIN) {
			if (!*write_retp)
				*write_retp = ret;
			if (ret == -ENOSPC)
				memcomp_reclaim_writeback_stop(nid);
			break;
		}
		cond_resched();
	}

	/* Also refund prefetched reservations after an allocation failure. */
	{
		int ret = mc16_writeback_batch_flush(freedp);

		if (ret && !*write_retp)
			*write_retp = ret;
		if (ret == -ENOSPC)
			memcomp_reclaim_writeback_stop(nid);
	}
	return scanned;
}
#endif /* CONFIG_SWAP */
#endif /* _MM_MEMCOMP_DRAIN_H */
