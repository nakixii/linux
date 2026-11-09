/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_QUERY_H
#define _MM_MEMCOMP_QUERY_H

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>

/* Include after zero/storage/access/refs/span. Public dispatch is separate. */

/* RCU-only observation: no reference acquisition and no lifetime guarantee. */
static inline bool mc16_entry_present(unsigned long token)
{
	struct mc16_cluster *cluster;
	struct mc16_slot *segment;
	unsigned int id, index;
	bool present = false;

	if (mc16_zero_entry_stat(token, NULL, NULL))
		return true;
	if (token > U32_MAX)
		return false;
	id = token >> MC16_CLUSTER_SHIFT;
	index = token & (MC16_CLUSTER_SLOTS - 1);
	if (!id)
		return false;

	rcu_read_lock();
	cluster = xa_load(&mc16_clusters, id);
	/* Pair with reserve's release-next before reading initialized slots. */
	if (!cluster || index >= smp_load_acquire(&cluster->next))
		goto out;
	/* Pair with segment publication; RCU protects a detached segment. */
	segment = smp_load_acquire(&cluster->segments[index >> MC16_SEGMENT_SHIFT]);
	if (!segment)
		goto out;
	present = refcount_read(&segment[index & (MC16_SEGMENT_SLOTS - 1)].refs) != 0;
out:
	rcu_read_unlock();
	return present;
}

/*
 * compressed == NULL: refs-only query, no T attempt or record access, valid
 * under PTL/RCU. Mapcount approximates unified refs minus this lookup and the
 * mirrored OWNER; temporary/WB refs and concurrent changes remain included.
 *
 * compressed != NULL: process-context diagnostic with one T trylock. False
 * also means busy/invalid payload, not just absent; every requested output is
 * zero on failure. Successful lengths are decoded effective-state snapshots.
 */
static inline bool mc16_entry_stat(unsigned long token,
				   unsigned long *compressed, int *mapcount)
{
	struct memcomp_ref ref;
	struct mc16_view view;
	struct mutex *lock; /* Existing hashed token mutex T. */
	unsigned int refs, state;
	u32 bytes = 0;
	u16 flags;
	bool valid;

	if (compressed)
		*compressed = 0;
	if (mapcount)
		*mapcount = 0;
	if (mc16_zero_entry_stat(token, compressed, mapcount))
		return true;
	if (!mc16_get(token, &ref))
		return false;

	if (compressed) {
		lock = memcomp_lock_of(token);
		if (!mutex_trylock(lock))
			goto fail;
		valid = mc16_view_locked(&ref, &view);
		if (valid) {
			flags = *view.flags;
			state = mc16_flags_state(flags);
			valid = mc16_flags_valid(flags);
			if (valid && state == MC16_PENDING)
				valid = flags & MC16_F_RESIDENT;
			else if (valid)
				valid = !(flags & MC16_F_RESIDENT) &&
					mc16_payload_length_decode(state, *view.length,
								   &bytes);
		}
		mutex_unlock(lock);
		if (!valid)
			goto fail;
	}
	if (mapcount) {
		refs = refcount_read(&ref.slot->refs);
		if (refs)
			refs--;
		if (refs && (READ_ONCE(ref.slot->flags) & MC16_F_OWNER))
			refs--;
		*mapcount = clamp_t(unsigned int, refs, 1, INT_MAX);
	}
	if (compressed)
		*compressed = bytes;
	mc16_ref_put(&ref);
	return true;
fail:
	mc16_ref_put(&ref);
	return false;
}

/* Zero is one pure-value page; a repeated zero token does not encode a span. */
static inline unsigned int
mc16_entry_geometry(unsigned long token, unsigned long *base_token,
		    unsigned long *page_idx)
{
	struct memcomp_ref ref;
	unsigned int nr, index;
	u32 base;

	if (base_token)
		*base_token = token;
	if (page_idx)
		*page_idx = 0;
	if (mc16_zero_entry_stat(token, NULL, NULL))
		return 1;
	if (!mc16_get(token, &ref))
		return 0;
	nr = mc16_ref_geometry(&ref, &base, &index);
	mc16_ref_put(&ref);
	if (nr) {
		if (base_token)
			*base_token = base;
		if (page_idx)
			*page_idx = index;
	}
	return nr;
}

static inline unsigned int
mc16_entry_nr_pages(unsigned long token, unsigned long *base_token)
{
	return mc16_entry_geometry(token, base_token, NULL);
}

/*
 * Return 1 for resident, 0 for nonresident/absent, -EAGAIN for busy/unreadable.
 * ref must be non-NULL. A successful positive lookup retains its guard even
 * when nonresident; zero, absent and negative returns leave an empty guard.
 */
static inline int mc16_entry_resident_get(unsigned long token,
					  struct memcomp_ref *ref)
{
	struct mc16_view view;
	struct mutex *lock; /* Existing hashed token mutex T. */
	u16 flags;
	bool valid, resident = false;

	*ref = (struct memcomp_ref) { };
	if (mc16_zero_entry_stat(token, NULL, NULL))
		return 0;
	if (!mc16_get(token, ref))
		return 0;
	lock = memcomp_lock_of(token);
	if (!mutex_trylock(lock)) {
		mc16_ref_put(ref);
		return -EAGAIN;
	}
	valid = mc16_view_locked(ref, &view);
	if (valid) {
		flags = *view.flags;
		valid = mc16_flags_valid(flags);
		resident = mc16_flags_state(flags) == MC16_PENDING &&
			   (flags & MC16_F_RESIDENT);
	}
	mutex_unlock(lock);
	if (!valid) {
		mc16_ref_put(ref);
		return -EAGAIN;
	}
	return resident;
}

static inline void mc16_entry_resident_put(struct memcomp_ref *ref)
{
	if (ref->slot)
		mc16_ref_put(ref);
}

#endif /* _MM_MEMCOMP_QUERY_H */
