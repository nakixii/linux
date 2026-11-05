/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_LIFETIME_H
#define _MM_MEMCOMP_LIFETIME_H

/* Included after memcomp's accounting wrappers. No payload is freed here. */
#include <linux/err.h>
#include <linux/log2.h>
#include <linux/overflow.h>
#include <linux/slab.h>

static inline void mc16_record_destroy(struct mc16_record *record)
{
	struct obj_cgroup *objcg = record->objcg;
	u64 charge = record->charge_bytes;

	/* A detached tail may outlive the key and every cluster callback. */
	memcomp_objcg_uncharge(objcg, charge);
	if (objcg)
		memcomp_objcg_put(objcg);
	kfree(record);
}

static inline void mc16_record_member_put(struct mc16_record *record)
{
	/* The caller must not touch record after returning its own member. */
	if (refcount_sub_and_test(1, &record->members))
		mc16_record_destroy(record);
}

/* The failed reserve owns all members privately; no member was published. */
static inline void mc16_record_abort(struct mc16_record *record)
{
	unsigned int nr_pages = record->nr_pages;

	if (WARN_ON_ONCE(!refcount_sub_and_test(nr_pages, &record->members)))
		return;
	mc16_record_destroy(record);
}

static inline struct mc16_record *
mc16_record_alloc(struct folio *folio, unsigned int nr_pages, bool shmem)
{
	struct mc16_record *record;
	struct obj_cgroup *objcg;
	unsigned int order, i;
	size_t bytes;
	int ret;

	if (!folio || !nr_pages || nr_pages > MEMCOMP_MAX_COMPRESS_PAGES ||
	    !is_power_of_2(nr_pages))
		return ERR_PTR(-EINVAL);
	bytes = struct_size(record, member, nr_pages);
	record = kzalloc(bytes, GFP_NOIO | __GFP_NOWARN);
	if (!record)
		return ERR_PTR(-ENOMEM);

	/* Charge the actual slab size, not the flexible-array requested size. */
	record->alloc_bytes = ksize(record);
	objcg = memcomp_objcg_get(folio);
	if (objcg) {
		ret = memcomp_objcg_charge(objcg, GFP_NOWAIT | __GFP_NOWARN,
					   record->alloc_bytes);
		if (ret) {
			memcomp_objcg_put(objcg);
			kfree(record);
			return ERR_PTR(ret);
		}
		record->charge_bytes = record->alloc_bytes;
	}
	record->objcg = objcg;
	record->producer = folio;
	record->nid = folio_nid(folio);
	record->nr_pages = nr_pages;
	if (shmem)
		record->record_flags |= MC16_RECORD_SHMEM;
	refcount_set(&record->members, nr_pages);
	order = ilog2(nr_pages);
	for (i = 0; i < nr_pages; i++) {
		record->member[i].owner_folio = folio;
		record->member[i].flags = MC16_PENDING | MC16_F_OWNER |
			MC16_F_RESIDENT | (order << MC16_ORDER_SHIFT);
	}
	/* Reserve acquires resident folio refs only when slots are committed. */
	return record;
}

#endif /* _MM_MEMCOMP_LIFETIME_H */
