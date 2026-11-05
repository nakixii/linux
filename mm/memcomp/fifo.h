/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_FIFO_H
#define _MM_MEMCOMP_FIFO_H

/* These lists contain clusters after the production caller switches over. */
static inline struct memcomp_lru *mc16_fifo_of(const struct memcomp_ref *ref)
{
	unsigned int nid = ref->cluster->key->nid;

	if (WARN_ON_ONCE(nid >= MAX_NUMNODES))
		nid = 0;
	return &memcomp_lrus[nid];
}

/* Caller serializes each slot transition with T, or owns refs-zero release. */
static inline void
mc16_fifo_transition(struct memcomp_ref *ref, unsigned int old_state,
		     unsigned int new_state)
{
	struct mc16_cluster *cluster = ref->cluster;
	struct memcomp_lru *fifo;
	unsigned long irqflags;
	bool old_eligible = old_state == MC16_STORED;
	bool new_eligible = new_state == MC16_STORED;

	if (old_eligible == new_eligible)
		return;
	fifo = mc16_fifo_of(ref);
	spin_lock_irqsave(&fifo->lock, irqflags);
	if (new_eligible) {
		if (WARN_ON_ONCE(cluster->eligible_slots == MC16_CLUSTER_SLOTS))
			goto out;
		if (!cluster->eligible_slots++)
			list_add(&cluster->fifo, &fifo->entries);
		atomic_long_inc(&fifo->nr_entries);
	} else {
		if (WARN_ON_ONCE(!cluster->eligible_slots))
			goto out;
		if (!--cluster->eligible_slots)
			list_del_init(&cluster->fifo);
		atomic_long_dec(&fifo->nr_entries);
	}
 out:
	spin_unlock_irqrestore(&fifo->lock, irqflags);
}

/*
 * Finish first cache initialization exactly once; never revive a retired slot.
 * Payload/flags and live protection were published by reserve. STORED becomes
 * selectable only after eligibility is accounted under the selector's L.
 * SAMEFILL needs no L: all data already precedes reserve's next publication,
 * and external tokens are exposed only after this infallible initialization.
 */
static inline void mc16_cache_activate(struct memcomp_ref *ref)
{
	struct mc16_cluster *cluster = ref->cluster;
	struct memcomp_lru *fifo;
	unsigned long irqflags;

	if (mc16_flags_state(ref->slot->flags) == MC16_STORED) {
		fifo = mc16_fifo_of(ref);
		spin_lock_irqsave(&fifo->lock, irqflags);
		if (!cluster->eligible_slots++)
			list_add(&cluster->fifo, &fifo->entries);
		atomic_long_inc(&fifo->nr_entries);
		refcount_set(&ref->slot->refs, 1);
		spin_unlock_irqrestore(&fifo->lock, irqflags);
	} else {
		/* Finish accounting before a scanner can take its first guard. */
		smp_wmb();
		refcount_set(&ref->slot->refs, 1);
	}
}

/*
 * One L scope, bounded slot scanning. Every call reselects from the FIFO,
 * never from a link retained after unlock. The caller validates effective
 * state/hotness under T; an acquired reference alone protects only lifetime.
 * RCU protects a concurrently detached segment while its zero refs are read.
 */
static inline bool mc16_fifo_select(struct memcomp_lru *fifo,
				    unsigned int *budget, struct memcomp_ref *ref)
{
	struct mc16_cluster *cluster;
	struct mc16_slot *segment, *slot;
	unsigned int index, next;
	unsigned long irqflags;
	bool found = false;

	*ref = (struct memcomp_ref) { };
	rcu_read_lock();
	spin_lock_irqsave(&fifo->lock, irqflags);
	while (*budget && !list_empty(&fifo->entries)) {
		cluster = list_last_entry(&fifo->entries, struct mc16_cluster, fifo);
		/* Pair with reserve publication of initialized slots and holes. */
		next = smp_load_acquire(&cluster->next);
		if (WARN_ON_ONCE(!next || !cluster->eligible_slots))
			break;
		if (cluster->wb_cursor >= next) {
			cluster->wb_cursor = 0;
			list_move(&cluster->fifo, &fifo->entries);
		}
		index = cluster->wb_cursor++;
		(*budget)--;
		/* Segment retirement nulls the pointer before a real RCU grace. */
		segment = smp_load_acquire(&cluster->segments[index >> MC16_SEGMENT_SHIFT]);
		if (!segment)
			continue;
		slot = &segment[index & (MC16_SEGMENT_SLOTS - 1)];
		if (!refcount_inc_not_zero(&slot->refs))
			continue;
		*ref = (struct memcomp_ref) {
			.slot = slot, .cluster = cluster,
			.token = (cluster->id << MC16_CLUSTER_SHIFT) + index,
		};
		found = true;
		break;
	}
	spin_unlock_irqrestore(&fifo->lock, irqflags);
	rcu_read_unlock();
	return found;
}

/* Called under T after selecting a held guard and validating its payload. */
static inline bool mc16_fifo_second_chance_locked(struct memcomp_ref *ref)
{
	struct mc16_view view;

	if (!mc16_view_locked(ref, &view))
		return false;
	if (!(*view.flags & MC16_F_REFERENCED))
		return false;
	*view.flags &= ~MC16_F_REFERENCED;
	return true;
}
#endif /* _MM_MEMCOMP_FIFO_H */
