/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_SCAN_H
#define _MM_MEMCOMP_SCAN_H

/* Include after storage.h. No token lookup, T, W or record access here. */
enum mc16_scan_result {
	MC16_SCAN_GUARD,
	MC16_SCAN_BUDGET,
	MC16_SCAN_END,
};

#define MC16_SCAN_END_CURSOR	((u64)U32_MAX + 1)

/*
 * cursor/ref/scanned are non-NULL; ref must not own a guard on entry.
 * Start at cursor 0 (or any token). END canonicalizes cursor to 2^32.
 * A zero budget does no lookup and leaves a non-END cursor unchanged.
 *
 * Each initialized slot position costs one probe, including alignment holes,
 * zero refs and absent segments. A reserved XA_ZERO_ENTRY cluster ID and
 * an empty published header each cost one probe. Sparse absent IDs and a
 * known unpublished tail are skipped for free. After a tail skip the next
 * found entry consumes a probe or ends the walk, so skips cannot evade it.
 * XA_RETRY_ENTRY follows xas_retry/reset semantics without consuming a probe;
 * concurrent tree retries have no fixed scheduler/latency bound. The budget
 * bounds examined positions/markers, not these primitive-internal restarts.
 *
 * GUARD owns exactly one reference, cursor is past its token, and scanned
 * includes that probe. BUDGET is distinct from END; neither owns a guard.
 * scanned is per call and never exceeds budget. No state filter is applied:
 * the caller checks the effective state under T, then puts the guard before
 * dropping W. Across W or between calls retain only the numeric cursor.
 * This is a forward walk, not a snapshot of concurrent publications.
 */
static inline enum mc16_scan_result
mc16_scan_next(u64 *cursor, unsigned int budget, struct memcomp_ref *ref,
	       unsigned int *scanned)
{
	const u64 end = MC16_SCAN_END_CURSOR;
	enum mc16_scan_result result;
	unsigned int used = 0;
	u64 pos = *cursor;

	*ref = (struct memcomp_ref) { };
	*scanned = 0;
	if (pos >= end) {
		*cursor = end;
		return MC16_SCAN_END;
	}
	if (!budget)
		return MC16_SCAN_BUDGET;
	if (pos < MC16_CLUSTER_SLOTS)
		pos = MC16_CLUSTER_SLOTS;

	rcu_read_lock();
	while (used < budget && pos < end) {
		struct mc16_cluster *cluster;
		struct mc16_slot *segment, *slot;
		XA_STATE(xas, &mc16_clusters, pos >> MC16_CLUSTER_SHIFT);
		unsigned long id;
		unsigned int index, next;
		void *entry;
		u64 base;

		/* Retain zero entries for accounting, unlike xa_find(). */
		do {
			entry = xas_find(&xas, MC16_MAX_CLUSTER_ID);
		} while (!xa_is_zero(entry) && xas_retry(&xas, entry));
		if (!entry) {
			pos = end;
			break;
		}
		id = xas.xa_index;
		if (xa_is_zero(entry)) {
			used++;
			pos = ((u64)id + 1) << MC16_CLUSTER_SHIFT;
			continue;
		}
		cluster = entry;
		base = (u64)id << MC16_CLUSTER_SHIFT;
		if (pos < base)
			pos = base;
		index = pos - base;
		/* Initialized slots and holes precede release-next. */
		next = smp_load_acquire(&cluster->next);
		if (index >= next) {
			if (!next)
				used++;
			pos = ((u64)id + 1) << MC16_CLUSTER_SHIFT;
			continue;
		}
		used++;
		pos++;
		/* A detached segment stays allocated until its RCU grace. */
		segment = smp_load_acquire(&cluster->segments[index >> MC16_SEGMENT_SHIFT]);
		if (!segment)
			continue;
		slot = &segment[index & (MC16_SEGMENT_SLOTS - 1)];
		if (!refcount_inc_not_zero(&slot->refs))
			continue;
		*ref = (struct memcomp_ref) {
			.slot = slot, .cluster = cluster,
			.token = (u32)(base + index),
		};
		result = MC16_SCAN_GUARD;
		goto out;
	}
	result = pos == end ? MC16_SCAN_END : MC16_SCAN_BUDGET;
out:
	rcu_read_unlock();
	*cursor = pos;
	*scanned = used;
	return result;
}
#endif /* _MM_MEMCOMP_SCAN_H */
