/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_STORAGE_H
#define _MM_MEMCOMP_STORAGE_H

/*
 * Include in memcomp.c after its accounting wrappers. Required static hooks:
 * void memcomp_objcg_get_ref(struct obj_cgroup *); (test-model aware)
 * void memcomp_objcg_put(struct obj_cgroup *);
 * int memcomp_objcg_charge(struct obj_cgroup *, gfp_t, size_t);
 * void memcomp_objcg_uncharge(struct obj_cgroup *, size_t);
 *
 * Record allocation/member lifetime, payload, T and L belong to the caller.
 * reserve failure never consumes record members or resident folio references.
 * Successful resident refs[] hold initial OWNER ownership. Cache refs[] are
 * private, not-yet-activated reservations with zero refs; activate infallibly
 * after accounting, before exposing tokens. They are never lookup guards.
 * Cache reservation does not allocate a record.
 * After refs reaches zero, release payload and complete L eligibility removal
 * before mc16_retire_slot(). A guard is put by the caller without XA relookup.
 *
 * Metadata is allocated against the source owner and freed after RCU.
 */
#include <linux/err.h>
#include <linux/atomic.h>
#include <linux/hashtable.h>
#include <linux/log2.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/shrinker.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "format.h"

#define MC16_CLUSTER_SHIFT	9
#define MC16_CLUSTER_SLOTS	BIT(MC16_CLUSTER_SHIFT)
#define MC16_SEGMENT_SHIFT	6
#define MC16_SEGMENT_SLOTS	BIT(MC16_SEGMENT_SHIFT)
#define MC16_SEGMENTS		(MC16_CLUSTER_SLOTS / MC16_SEGMENT_SLOTS)
#define MC16_MAX_CLUSTER_ID	(U32_MAX >> MC16_CLUSTER_SHIFT)
#define MC16_KEY_HASH_BITS	8
#define MC16_CLUSTER_AGED	BIT(0)
#define MC16_AGING_BATCH	32

struct mc16_key;

struct mc16_cluster {
	struct mc16_slot *segments[MC16_SEGMENTS];
	struct mc16_key *key;
	union {
		struct list_head fifo;
		struct rcu_head rcu;
	};
	u32 id;
	u32 live_slots;
	u16 next;
	u16 wb_cursor;
	u16 flags;
	u16 eligible_slots;
	u64 metadata_bytes;
	u8 segment_live[MC16_SEGMENTS];
	u8 reserved[8];
};

enum mc16_key_kind {
	MC16_KEY_ANON,
	MC16_KEY_SHMEM,
};

struct mc16_key {
	struct hlist_node node;
	struct obj_cgroup *objcg;
	struct mc16_cluster *current_cluster;
	u64 metadata_bytes;
	u32 clusters;		/* X protected, including queued header callbacks. */
	u16 nid;
	u8 kind;
	u8 zero_hint;
};

struct mc16_segment_retire {
	struct rcu_head rcu;
	struct mc16_slot *segment;
};

static_assert(sizeof(struct mc16_cluster) == 128);
static_assert(offsetof(struct mc16_cluster, segments) == 0);
static_assert(offsetof(struct mc16_cluster, key) == 64);
static_assert(offsetof(struct mc16_cluster, fifo) == 72);
static_assert(offsetof(struct mc16_cluster, rcu) == 72);
static_assert(offsetof(struct mc16_cluster, id) == 88);
static_assert(offsetof(struct mc16_cluster, live_slots) == 92);
static_assert(offsetof(struct mc16_cluster, next) == 96);
static_assert(offsetof(struct mc16_cluster, wb_cursor) == 98);
static_assert(offsetof(struct mc16_cluster, flags) == 100);
static_assert(offsetof(struct mc16_cluster, eligible_slots) == 102);
static_assert(offsetof(struct mc16_cluster, metadata_bytes) == 104);
static_assert(offsetof(struct mc16_cluster, segment_live) == 112);
static_assert(offsetof(struct mc16_cluster, reserved) == 120);
static_assert(sizeof(struct memcomp_ref) == 24);
static_assert(MC16_SEGMENT_SLOTS * sizeof(struct mc16_slot) == 1024);

enum mc16_storage_class {
	MC16_META_SEGMENT,
	MC16_META_CLUSTER,
	MC16_META_KEY,
	MC16_META_DESCRIPTOR,
	MC16_META_CLASSES,
};

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
struct mc16_storage_debug {
	u64 allocated[MC16_META_CLASSES];
	u64 freed[MC16_META_CLASSES];
	u64 allocated_bytes[MC16_META_CLASSES];
	u64 freed_bytes[MC16_META_CLASSES];
	u64 descriptor_failures;
	u64 meta_count_calls;
	u64 meta_scan_calls;
	u64 meta_detached;
	u64 aging_work_calls;
	u64 aging_detached;
};

static DEFINE_PER_CPU(struct mc16_storage_debug, mc16_storage_debug);
#endif

static inline void mc16_meta_alloc(enum mc16_storage_class type, void *ptr)
{
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	this_cpu_inc(mc16_storage_debug.allocated[type]);
	this_cpu_add(mc16_storage_debug.allocated_bytes[type], ksize(ptr));
#endif
}

static inline void mc16_meta_free(enum mc16_storage_class type, void *ptr)
{
	if (!ptr)
		return;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	this_cpu_inc(mc16_storage_debug.freed[type]);
	this_cpu_add(mc16_storage_debug.freed_bytes[type], ksize(ptr));
#endif
	kfree(ptr);
}

static DEFINE_XARRAY_FLAGS(mc16_clusters,
			   XA_FLAGS_ALLOC1 | XA_FLAGS_LOCK_IRQ);
static DEFINE_HASHTABLE(mc16_keys, MC16_KEY_HASH_BITS);
static unsigned long mc16_retained_empty_headers;
static unsigned long mc16_meta_shrinker_cursor = 1;
static void mc16_retained_age_work(struct work_struct *work);
static DECLARE_DEFERRABLE_WORK(mc16_retained_aging, mc16_retained_age_work);

/* X protects the numeric sweep state; no iterator survives an unlock. */
static struct {
	unsigned long next_id;
	unsigned long not_before;
	bool active;
	bool deadline_valid;
	bool requested;
} mc16_aging = { .next_id = 1 };

static unsigned long mc16_aging_now(void)
{
	return jiffies;
}

static void mc16_queue_aging(unsigned long delay)
{
	mod_delayed_work(system_unbound_wq, &mc16_retained_aging, delay);
}

static void mc16_schedule_aging(void)
{
	unsigned long irqflags, now, delay;

	xa_lock_irqsave(&mc16_clusters, irqflags);
	if (mc16_aging.requested) {
		xa_unlock_irqrestore(&mc16_clusters, irqflags);
		return;
	}
	now = mc16_aging_now();
	mc16_aging.requested = true;
	if (!mc16_aging.deadline_valid) {
		mc16_aging.not_before = now + HZ;
		mc16_aging.deadline_valid = true;
	}
	delay = !mc16_aging.active &&
		time_before(now, mc16_aging.not_before) ?
		mc16_aging.not_before - now : 0;
	xa_unlock_irqrestore(&mc16_clusters, irqflags);
	/* An early concurrent kick cannot bypass the callback's deadline. */
	mc16_queue_aging(delay);
}

static inline bool mc16_retained_empty_inc_locked(void)
{
	unsigned long count;

	lockdep_assert_held(&mc16_clusters.xa_lock);
	count = READ_ONCE(mc16_retained_empty_headers);
	WRITE_ONCE(mc16_retained_empty_headers, count + 1);
	return !count;
}

static inline void mc16_retained_empty_dec_locked(void)
{
	unsigned long count;

	lockdep_assert_held(&mc16_clusters.xa_lock);
	count = READ_ONCE(mc16_retained_empty_headers);
	if (WARN_ON_ONCE(!count))
		return;
	WRITE_ONCE(mc16_retained_empty_headers, count - 1);
}

/* A real source objcg reference is held by record/caller/key in this scope. */
static inline void *mc16_kmalloc_owner(size_t bytes, gfp_t gfp,
				       struct obj_cgroup *objcg)
{
	/* Metadata remains unaccounted until a source owner is introduced. */
	return kmalloc(bytes, gfp & ~__GFP_ACCOUNT);
}

static inline void *mc16_owner_alloc(size_t bytes, gfp_t gfp,
				     struct obj_cgroup *objcg)
{
	return mc16_kmalloc_owner(bytes, gfp, objcg);
}

static inline void mc16_queue_rcu(struct rcu_head *head, rcu_callback_t func)
{
	call_rcu(head, func);
}

static inline void *mc16_alloc_zero(enum mc16_storage_class type,
				    size_t bytes, struct obj_cgroup *objcg)
{
	void *ptr;

	ptr = mc16_owner_alloc(bytes, GFP_NOIO | __GFP_NOWARN, objcg);
	if (ptr) {
		memset(ptr, 0, bytes);
		mc16_meta_alloc(type, ptr);
	}
	return ptr;
}

/* Caller holds mc16_clusters.xa_lock for every key/current operation. */
static inline unsigned long mc16_key_tag(struct obj_cgroup *objcg,
					 unsigned int nid, u8 kind,
					 bool zero_hint)
{
	return (unsigned long)objcg ^ ((unsigned long)nid << 1) ^ kind ^
		((unsigned long)zero_hint << 3);
}

static inline struct mc16_key *
mc16_key_find(struct obj_cgroup *objcg, unsigned int nid, u8 kind,
	      bool zero_hint)
{
	struct mc16_key *key;
	unsigned long tag = mc16_key_tag(objcg, nid, kind, zero_hint);

	lockdep_assert_held(&mc16_clusters.xa_lock);
	hash_for_each_possible(mc16_keys, key, node, tag) {
		if (key->objcg == objcg && key->nid == nid && key->kind == kind &&
		    key->zero_hint == zero_hint)
			return key;
	}
	return NULL;
}

static inline int mc16_private_id_alloc(u32 *id)
{
	return __xa_alloc(&mc16_clusters, id, NULL,
			  XA_LIMIT(1, MC16_MAX_CLUSTER_ID),
			  GFP_NOWAIT | __GFP_NOWARN);
}

static inline struct mc16_key *mc16_key_zalloc(void)
{
	return kzalloc(sizeof(struct mc16_key), GFP_NOIO | __GFP_NOWARN);
}

/* Key slab is manually charged, with one independent test-aware objcg hold. */
static inline struct mc16_key *
mc16_key_alloc(struct obj_cgroup *objcg, unsigned int nid, u8 kind,
	       bool zero_hint)
{
	struct mc16_key *key;
	int ret;

	key = mc16_key_zalloc();
	if (!key)
		return ERR_PTR(-ENOMEM);
	mc16_meta_alloc(MC16_META_KEY, key);
	key->metadata_bytes = ksize(key);
	if (objcg) {
		ret = memcomp_objcg_charge(objcg, GFP_NOIO | __GFP_NOWARN,
					   key->metadata_bytes);
		if (ret) {
			mc16_meta_free(MC16_META_KEY, key);
			return ERR_PTR(ret);
		}
		memcomp_objcg_get_ref(objcg);
	}
	key->objcg = objcg;
	key->nid = nid;
	key->kind = kind;
	key->zero_hint = zero_hint;
	return key;
}

static inline void mc16_key_free(struct mc16_key *key)
{
	struct obj_cgroup *objcg;

	if (!key)
		return;
	objcg = key->objcg;
	if (objcg) {
		memcomp_objcg_uncharge(objcg, key->metadata_bytes);
		memcomp_objcg_put(objcg);
	}
	mc16_meta_free(MC16_META_KEY, key);
}

static inline bool mc16_get(unsigned long token, struct memcomp_ref *out)
{
	struct mc16_cluster *cluster;
	struct mc16_slot *segment, *slot;
	unsigned int index, id;
	bool found = false;

	if (!out)
		return false;
	*out = (struct memcomp_ref) { };
	if (!token || token > U32_MAX)
		return false;
	id = token >> MC16_CLUSTER_SHIFT;
	index = token & (MC16_CLUSTER_SLOTS - 1);
	if (!id)
		return false;

	rcu_read_lock();
	cluster = xa_load(&mc16_clusters, id);
	if (!cluster)
		goto out;
	/* Pair with release-next: all slots and alignment holes are initialized. */
	if (index >= smp_load_acquire(&cluster->next))
		goto out;
	/* Pair with segment install; RCU protects a concurrently detached one. */
	segment = smp_load_acquire(&cluster->segments[index >> MC16_SEGMENT_SHIFT]);
	if (!segment)
		goto out;
	slot = &segment[index & (MC16_SEGMENT_SLOTS - 1)];
	if (!refcount_inc_not_zero(&slot->refs))
		goto out;
	out->slot = slot;
	out->cluster = cluster;
	out->token = token;
	found = true;
out:
	rcu_read_unlock();
	return found;
}

static inline void mc16_segment_rcu_free(struct rcu_head *rcu)
{
	struct mc16_segment_retire *retired;

	retired = container_of(rcu, struct mc16_segment_retire, rcu);
	mc16_meta_free(MC16_META_SEGMENT, retired->segment);
	mc16_meta_free(MC16_META_DESCRIPTOR, retired);
}

static inline void mc16_cluster_rcu_free(struct rcu_head *rcu)
{
	struct mc16_cluster *cluster;
	struct mc16_key *key, *last = NULL;
	unsigned int i;
	unsigned long irqflags;

	cluster = container_of(rcu, struct mc16_cluster, rcu);
	key = cluster->key;
	for (i = 0; i < MC16_SEGMENTS; i++)
		mc16_meta_free(MC16_META_SEGMENT, cluster->segments[i]);
	mc16_meta_free(MC16_META_CLUSTER, cluster);

	/* A queued header still owns its key until this exact transaction. */
	xa_lock_irqsave(&mc16_clusters, irqflags);
	if (WARN_ON_ONCE(!key->clusters)) {
		xa_unlock_irqrestore(&mc16_clusters, irqflags);
		return;
	}
	if (!--key->clusters) {
		WARN_ON_ONCE(key->current_cluster);
		hash_del(&key->node);
		last = key;
	}
	xa_unlock_irqrestore(&mc16_clusters, irqflags);
	mc16_key_free(last);
}

/* A detached header owns its key until mc16_cluster_rcu_free(). */
static inline void mc16_queue_cluster(struct mc16_cluster *cluster)
{
	mc16_queue_rcu(&cluster->rcu, mc16_cluster_rcu_free);
}

/* Return -1 to stop, zero to continue, or the next full-pass delay. */
static long mc16_aging_step_locked(unsigned long now,
				   struct mc16_cluster **detached, unsigned int *nr_detached,
				   unsigned int *checked)
{
	bool complete = false;

	lockdep_assert_held(&mc16_clusters.xa_lock);
	*nr_detached = 0;
	*checked = 0;
	mc16_aging.requested = false;
	if (!READ_ONCE(mc16_retained_empty_headers)) {
		mc16_aging.active = false;
		mc16_aging.next_id = 1;
		return -1;
	}
	if (!mc16_aging.active && mc16_aging.deadline_valid &&
	    time_before(now, mc16_aging.not_before))
		return mc16_aging.not_before - now;
	mc16_aging.active = true;

	while (*checked < MC16_AGING_BATCH) {
		XA_STATE(xas, &mc16_clusters, mc16_aging.next_id);
		struct mc16_cluster *cluster;
		unsigned long id;
		void *entry;

		/* Raw lookup exposes private reservations hidden by xa_find(). */
		entry = xas_find(&xas, MC16_MAX_CLUSTER_ID);
		if (!entry) {
			complete = true;
			break;
		}
		(*checked)++;
		id = xas.xa_index;
		if (id == MC16_MAX_CLUSTER_ID)
			complete = true;
		else
			mc16_aging.next_id = id + 1;
		if (xa_is_value(entry) || xa_is_internal(entry))
			goto next;
		cluster = entry;
		if (cluster->id != id ||
		    cluster->key->current_cluster != cluster ||
		    cluster->live_slots || !cluster->next ||
		    cluster->next >= MC16_CLUSTER_SLOTS ||
		    cluster->eligible_slots || !list_empty(&cluster->fifo))
			goto next;
		if (!(cluster->flags & MC16_CLUSTER_AGED)) {
			cluster->flags |= MC16_CLUSTER_AGED;
			goto next;
		}
		mc16_retained_empty_dec_locked();
		cluster->key->current_cluster = NULL;
		__xa_erase(&mc16_clusters, cluster->id);
		detached[(*nr_detached)++] = cluster;
next:
		/* Rebuild XA_STATE before the next lookup, including after erase. */
		if (complete)
			break;
	}
	if (complete) {
		mc16_aging.next_id = 1;
		mc16_aging.active = false;
		mc16_aging.not_before = mc16_aging_now() + HZ;
		mc16_aging.deadline_valid = true;
	}
	if (!READ_ONCE(mc16_retained_empty_headers)) {
		mc16_aging.next_id = 1;
		mc16_aging.active = false;
		return -1;
	}
	return mc16_aging.active ? 0 : HZ;
}

/* Detached headers retain their keys until the existing RCU callback. */
static void mc16_retained_age_work(struct work_struct *work)
{
	struct mc16_cluster *detached[MC16_AGING_BATCH];
	unsigned long irqflags;
	unsigned int checked, nr_detached, i;
	long delay;

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	this_cpu_inc(mc16_storage_debug.aging_work_calls);
#endif
	xa_lock_irqsave(&mc16_clusters, irqflags);
	delay = mc16_aging_step_locked(mc16_aging_now(), detached,
				       &nr_detached, &checked);
	xa_unlock_irqrestore(&mc16_clusters, irqflags);

	for (i = 0; i < nr_detached; i++) {
		mc16_queue_cluster(detached[i]);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		this_cpu_inc(mc16_storage_debug.aging_detached);
#endif
	}
	if (delay >= 0)
		mc16_queue_aging(delay);
}

static unsigned long mc16_meta_shrinker_count(struct shrinker *shrinker,
					      struct shrink_control *sc)
{
	unsigned long count = READ_ONCE(mc16_retained_empty_headers);

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	this_cpu_inc(mc16_storage_debug.meta_count_calls);
#endif
	return count ? count : SHRINK_EMPTY;
}

static unsigned long mc16_meta_shrinker_scan(struct shrinker *shrinker,
					     struct shrink_control *sc)
{
	struct mc16_cluster *detached[64];
	unsigned long irqflags, start, id, max = MC16_MAX_CLUSTER_ID;
	unsigned long limit = min_t(unsigned long, sc->nr_to_scan,
				    ARRAY_SIZE(detached));
	unsigned int checked = 0, nr_detached = 0, i;
	bool wrapped = false;

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	this_cpu_inc(mc16_storage_debug.meta_scan_calls);
#endif
	sc->nr_scanned = 0;
	if (!limit)
		return 0;

	xa_lock_irqsave(&mc16_clusters, irqflags);
	start = mc16_meta_shrinker_cursor;
	id = start;
	while (checked < limit) {
		struct mc16_cluster *cluster;

		/* The normal API skips private zero entries as well as holes. */
		cluster = xa_find(&mc16_clusters, &id, max, XA_PRESENT);
		if (!cluster) {
			if (wrapped || start == 1) {
				id = 1;
				break;
			}
			/* The disjoint second range cannot revisit an entry. */
			id = 1;
			max = start - 1;
			wrapped = true;
			continue;
		}
		if (xa_is_value(cluster) || xa_is_internal(cluster)) {
			id++;
			continue;
		}
		checked++;
		if (cluster->id == id &&
		    cluster->key->current_cluster == cluster &&
		    !cluster->live_slots && cluster->next &&
		    cluster->next < MC16_CLUSTER_SLOTS &&
		    !cluster->eligible_slots && list_empty(&cluster->fifo)) {
			mc16_retained_empty_dec_locked();
			cluster->key->current_cluster = NULL;
			__xa_erase(&mc16_clusters, id);
			detached[nr_detached++] = cluster;
		}
		id++;
	}
	mc16_meta_shrinker_cursor = id > MC16_MAX_CLUSTER_ID ? 1 : id;
	xa_unlock_irqrestore(&mc16_clusters, irqflags);
	sc->nr_scanned = checked;

	for (i = 0; i < nr_detached; i++)
		mc16_queue_cluster(detached[i]);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	this_cpu_add(mc16_storage_debug.meta_detached, nr_detached);
#endif
	/* do_shrink_slab() repeats a pass that scanned nothing until it stops. */
	return checked ? nr_detached : SHRINK_STOP;
}

/* refs is already zero; no caller may use slot/cluster after this function. */
static inline void mc16_retire_slot(struct memcomp_ref *ref)
{
	struct mc16_segment_retire *retired = NULL;
	struct mc16_cluster *cluster, *last = NULL;
	bool schedule_aging = false;
	struct mc16_slot *segment;
	unsigned int seg;
	unsigned long irqflags;

	if (WARN_ON_ONCE(!ref || !ref->slot || !ref->cluster))
		return;
	cluster = ref->cluster;
	seg = (ref->token & (MC16_CLUSTER_SLOTS - 1)) >> MC16_SEGMENT_SHIFT;
	if (WARN_ON_ONCE(refcount_read(&ref->slot->refs)))
		return;

	xa_lock_irqsave(&mc16_clusters, irqflags);
	if (WARN_ON_ONCE(!cluster->live_slots || !cluster->segment_live[seg]) ||
	    WARN_ON_ONCE(cluster->live_slots == 1 && cluster->eligible_slots)) {
		xa_unlock_irqrestore(&mc16_clusters, irqflags);
		return;
	}
	cluster->live_slots--;
	cluster->segment_live[seg]--;
	segment = cluster->segments[seg];
	if (!cluster->segment_live[seg] &&
	    cluster->next >= (seg + 1) * MC16_SEGMENT_SLOTS) {
		retired = mc16_owner_alloc(sizeof(*retired),
					   GFP_NOWAIT | __GFP_NOWARN, cluster->key->objcg);
		if (retired) {
			mc16_meta_alloc(MC16_META_DESCRIPTOR, retired);
			retired->segment = segment;
			/* Old RCU readers retain permanently zero refs until grace. */
			smp_store_release(&cluster->segments[seg], NULL);
		} else {
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
			this_cpu_inc(mc16_storage_debug.descriptor_failures);
#endif
		}
	}
	if (!cluster->live_slots) {
		WARN_ON_ONCE(!list_empty(&cluster->fifo));
		if (cluster->key->current_cluster == cluster &&
		    cluster->next < MC16_CLUSTER_SLOTS) {
			schedule_aging = mc16_retained_empty_inc_locked();
		} else {
			if (cluster->key->current_cluster == cluster)
				cluster->key->current_cluster = NULL;
			__xa_erase(&mc16_clusters, cluster->id);
			last = cluster;
		}
	}
	xa_unlock_irqrestore(&mc16_clusters, irqflags);
	/* Non-winners never dereference cluster or slot after releasing X. */
	*ref = (struct memcomp_ref) { };
	if (schedule_aging)
		mc16_schedule_aging();
	if (retired)
		mc16_queue_rcu(&retired->rcu, mc16_segment_rcu_free);
	if (last)
		mc16_queue_rcu(&last->rcu, mc16_cluster_rcu_free);
}

/* Under X, return an aligned range wholly inside one segment/cluster. */
static inline bool mc16_range(struct mc16_cluster *cluster, unsigned int nr,
			      unsigned int *index)
{
	unsigned int start;

	if (!cluster)
		return false;
	start = ALIGN(cluster->next, nr);
	if (start + nr > MC16_CLUSTER_SLOTS)
		return false;
	*index = start;
	return true;
}

/* Independent of the later payload/T helpers; bytes zero means SAMEFILL. */
struct mc16_initial_payload {
	u64 word;
	u32 bytes;
};

/* Stack-only identity; record or caller keeps objcg alive across X1/X2. */
struct mc16_reserve_request {
	struct mc16_record *record;
	const struct mc16_initial_payload *payloads;
	struct obj_cgroup *objcg;
	unsigned int nid;
	unsigned int nr;
	u8 kind;
	bool zero_hint;
};

/* Infallible commit: all input validation and allocation precedes this. */
static inline void
mc16_publish_slots(struct mc16_cluster *cluster,
		   const struct mc16_reserve_request *request,
		   unsigned int index, struct memcomp_ref refs[])
{
	struct mc16_slot *segment = cluster->segments[index >> MC16_SEGMENT_SHIFT];
	struct mc16_record *record = request->record;
	unsigned int i, nr = request->nr, order = ilog2(nr);
	u32 base = (cluster->id << MC16_CLUSTER_SHIFT) | index;

	cluster->flags &= ~MC16_CLUSTER_AGED;
	if (record)
		record->token_base = base;
	for (i = 0; i < nr; i++) {
		struct mc16_slot *slot;
		u16 flags = order << MC16_ORDER_SHIFT;

		slot = &segment[(index + i) & (MC16_SEGMENT_SLOTS - 1)];
		if (record) {
			folio_get(record->producer);
			slot->word = (u64)(unsigned long)record;
			slot->length = 0;
			flags |= MC16_PENDING | MC16_F_OWNER | MC16_F_RESIDENT;
		} else {
			const struct mc16_initial_payload *initial;

			initial = &request->payloads[i];
			slot->word = initial->word;
			slot->length = initial->bytes ? initial->bytes - 1 : 0;
			flags |= initial->bytes ? MC16_STORED : MC16_SAMEFILL;
		}
		/* Cache initialization completes only after caller accounting. */
		refcount_set(&slot->refs, record ? 1 : 0);
		/* Stable SAMEFILL readers acquire flags before reading word. */
		smp_store_release(&slot->flags, flags);
		refs[i] = (struct memcomp_ref) {
			.slot = slot, .cluster = cluster, .token = base + i,
		};
	}
	cluster->live_slots += nr;
	cluster->segment_live[index >> MC16_SEGMENT_SHIFT] += nr;
	/* Includes permanently zero, never-reused alignment holes. */
	smp_store_release(&cluster->next, index + nr);
}

/*
 * P0: one X. P1: X1 preflight, source-owned allocation, X2 commit/cancel.
 * No A/T/L, no reserve-local retry, no third X, no record-member put here.
 * Validated request identity is independently owned across both sections.
 */
static inline int
mc16_reserve_request(const struct mc16_reserve_request *request,
		     struct memcomp_ref refs[])
{
	struct mc16_cluster *cluster = NULL, *spare_cluster = NULL;
	struct mc16_cluster *replaced = NULL;
	struct mc16_slot *spare_segment = NULL;
	struct mc16_key *key, *spare_key = NULL;
	struct obj_cgroup *objcg = request->objcg;
	unsigned int nr = request->nr, nid = request->nid;
	unsigned int index = 0, target_next = 0, target_id = 0, seg;
	u32 private_id = 0;
	unsigned long irqflags;
	bool new_cluster, new_key, reserved = false;
	u8 kind = request->kind;
	int ret = 0;
	void *old;

	memset(refs, 0, sizeof(*refs) * nr);

	xa_lock_irqsave(&mc16_clusters, irqflags);
	key = mc16_key_find(objcg, nid, kind, request->zero_hint);
	cluster = key ? key->current_cluster : NULL;
	if (mc16_range(cluster, nr, &index)) {
		seg = index >> MC16_SEGMENT_SHIFT;
		if (cluster->segments[seg]) {
			if (!cluster->live_slots && cluster->next)
				mc16_retained_empty_dec_locked();
			mc16_publish_slots(cluster, request, index, refs);
			xa_unlock_irqrestore(&mc16_clusters, irqflags);
			return 0;
		}
		new_cluster = false;
		target_id = cluster->id;
		target_next = cluster->next;
	} else {
		new_cluster = true;
		/* NULL is converted internally to a private reserved marker. */
		ret = mc16_private_id_alloc(&private_id);
		if (ret) {
			xa_unlock_irqrestore(&mc16_clusters, irqflags);
			return ret;
		}
		reserved = true;
		if (cluster) {
			target_id = cluster->id;
			target_next = cluster->next;
		}
	}
	new_key = !key;
	/* Carry only scalar geometry and independently owned source identity. */
	cluster = NULL;
	key = NULL;
	xa_unlock_irqrestore(&mc16_clusters, irqflags);

	spare_segment = mc16_alloc_zero(MC16_META_SEGMENT,
					MC16_SEGMENT_SLOTS * sizeof(*spare_segment), objcg);
	if (!spare_segment) {
		ret = -ENOMEM;
		goto recheck;
	}
	if (new_cluster) {
		spare_cluster = mc16_alloc_zero(MC16_META_CLUSTER,
						sizeof(*spare_cluster), objcg);
		if (!spare_cluster) {
			ret = -ENOMEM;
			goto recheck;
		}
		spare_cluster->metadata_bytes = ksize(spare_cluster);
		INIT_LIST_HEAD(&spare_cluster->fifo);
	}
	if (new_key) {
		spare_key = mc16_key_alloc(objcg, nid, kind,
					   request->zero_hint);
		if (IS_ERR(spare_key)) {
			ret = PTR_ERR(spare_key);
			spare_key = NULL;
		}
	}
recheck:
	xa_lock_irqsave(&mc16_clusters, irqflags);
	if (ret)
		goto cancel;
	key = mc16_key_find(objcg, nid, kind, request->zero_hint);
	cluster = key ? key->current_cluster : NULL;
	/* Reject competing consumption/retirement without another allocation. */
	if ((cluster ? cluster->id : 0) != target_id ||
	    (cluster && cluster->next != target_next)) {
		ret = -EAGAIN;
		goto cancel;
	}
	if (!new_cluster) {
		if (!key || !mc16_range(cluster, nr, &index) ||
		    cluster->segments[index >> MC16_SEGMENT_SHIFT]) {
			ret = -EAGAIN;
			goto cancel;
		}
	} else {
		if (!key && !spare_key) {
			ret = -EAGAIN;
			goto cancel;
		}
		if (!key)
			key = spare_key;
		replaced = cluster && !cluster->live_slots ? cluster : NULL;
		cluster = spare_cluster;
		cluster->id = private_id;
		cluster->key = key;
		old = __xa_store(&mc16_clusters, private_id, cluster,
				 GFP_NOWAIT | __GFP_NOWARN);
		if (WARN_ON_ONCE(xa_is_err(old))) {
			ret = xa_err(old);
			replaced = NULL;
			goto cancel;
		}
		WARN_ON_ONCE(old);
		reserved = false;
		spare_cluster = NULL;
		if (key == spare_key) {
			hash_add(mc16_keys, &key->node,
				 mc16_key_tag(objcg, nid, kind,
					      request->zero_hint));
			spare_key = NULL;
		}
		key->clusters++;
		if (replaced) {
			mc16_retained_empty_dec_locked();
			__xa_erase(&mc16_clusters, replaced->id);
		}
		key->current_cluster = cluster;
		index = 0;
	}
	seg = index >> MC16_SEGMENT_SHIFT;
	/* Pair with lookup acquire; only new, zeroed storage is installed. */
	smp_store_release(&cluster->segments[seg], spare_segment);
	spare_segment = NULL;
	if (!new_cluster && !cluster->live_slots && cluster->next)
		mc16_retained_empty_dec_locked();
	mc16_publish_slots(cluster, request, index, refs);
cancel:
	if (reserved)
		__xa_erase(&mc16_clusters, private_id);
	xa_unlock_irqrestore(&mc16_clusters, irqflags);
	/* Unpublished resources only; source/payload ownership stays with caller. */
	mc16_meta_free(MC16_META_SEGMENT, spare_segment);
	mc16_meta_free(MC16_META_CLUSTER, spare_cluster);
	mc16_key_free(spare_key);
	if (replaced)
		mc16_queue_cluster(replaced);
	return ret;
}

/* Existing resident reserve API and its record/member ownership are unchanged. */
static inline int mc16_reserve_hint(struct mc16_record *record, unsigned int nr,
				    u8 kind, bool zero_hint,
				    struct memcomp_ref refs[])
{
	struct mc16_reserve_request request;

	if (!record || !refs || !record->producer || !nr ||
	    nr > MC16_MAX_PAGES || !is_power_of_2(nr) ||
	    record->nr_pages != nr ||
	    kind != !!(record->record_flags & MC16_RECORD_SHMEM))
		return -EINVAL;
	request = (struct mc16_reserve_request) {
		.record = record, .objcg = record->objcg,
		.nid = record->nid, .nr = nr, .kind = kind,
		.zero_hint = zero_hint,
	};
	return mc16_reserve_request(&request, refs);
}

static inline int mc16_reserve(struct mc16_record *record, unsigned int nr,
			       u8 kind, struct memcomp_ref refs[])
{
	return mc16_reserve_hint(record, nr, kind, false, refs);
}

/*
 * Direct SHMEM cache ownership: no record, resident folio hold or lookup ref.
 * Caller holds objcg (NULL for root), zs handles and exact bytes charges before
 * entry, keeps objcg alive until return, and keeps payloads immutable throughout.
 * Validate the entire span before X1: no error may follow partial publication.
 * bytes == 0 is a full-width SAMEFILL word; otherwise word is a nonzero zs handle.
 * Success transfers payload/charge ownership to stable slots, backed by the key's
 * independent objcg hold. The source array is unchanged: caller must not abort it.
 * Failure consumes nothing. refs[] is untouched on invalid input, zeroed after
 * valid input reaches reserve. Caller must finish stats and infallible first
 * activation before exposing tokens. Until then live_slots protects storage,
 * but refs stay zero so FIFO/lookup cannot acquire a guard. Never get/put this
 * private reservation or confuse its initial zero with a retired index.
 */
static inline int
mc16_reserve_cache(struct obj_cgroup *objcg, unsigned int nid,
		   const struct mc16_initial_payload payloads[],
		   unsigned int nr, struct memcomp_ref refs[])
{
	struct mc16_reserve_request request;
	unsigned int i, order, state;
	u16 length, flags;

	/* Check wide inputs before any narrowing, array access or output clearing. */
	if (!payloads || !refs || nid > U16_MAX || nid >= MAX_NUMNODES ||
	    !nr || nr > MC16_MAX_PAGES || !is_power_of_2(nr))
		return -EINVAL;
	order = ilog2(nr);
	for (i = 0; i < nr; i++) {
		state = payloads[i].bytes ? MC16_STORED : MC16_SAMEFILL;
		if ((payloads[i].bytes && !payloads[i].word) ||
		    !mc16_payload_length_encode(state, payloads[i].bytes, &length) ||
		    !mc16_flags_encode(state, order, 0, &flags))
			return -EINVAL;
	}
	request = (struct mc16_reserve_request) {
		.payloads = payloads, .objcg = objcg,
		.nid = nid, .nr = nr, .kind = MC16_KEY_SHMEM,
		.zero_hint = false,
	};
	return mc16_reserve_request(&request, refs);
}

#endif /* _MM_MEMCOMP_STORAGE_H */
