/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_CACHE_STORE_H
#define _MM_MEMCOMP_CACHE_STORE_H

/*
 * Prepare one cache payload, retaining its initial cache reference in out.
 * The caller keeps the source/account folio alive and transfers that reference
 * only after arch preparation and the expected page-cache replacement succeed.
 * This lower-level helper does not choose the separate slotless-zero path.
 */
/* Private reservation -> counted initial cache ownership, infallibly. */
static inline void mc16_cache_publish(struct memcomp_ref *ref, unsigned int bytes)
{
	memcomp_stored_bytes_inc();
	memcomp_compressed_bytes_add(bytes);
	mc16_account_state(ref, true);
	mc16_cache_activate(ref);
}

static inline int mc16_cache_store_internal(const void *src,
					    struct folio *account,
					    struct memcomp_ref *out,
					    bool prepared)
{
	struct mc16_initial_payload payload = { };
	struct obj_cgroup *objcg;
	unsigned long handle = 0;
	unsigned int bytes = 0;
	unsigned short owner_id;
	u8 pool = 0;
	int ret;

	if (!out)
		return -EINVAL;
	*out = (struct memcomp_ref) { };
	if (!src || !account)
		return -EINVAL;
	if (!memcomp_is_enabled())
		return -EOPNOTSUPP;
	ret = mc16_source_id_get(account, &owner_id);
	if (ret)
		return ret;
	/* Independent source owner, never a borrowed registry key across X1/X2. */
	objcg = memcomp_objcg_get(account);
	ret = memcomp_prepare_payload(src, PAGE_SIZE, &bytes, &handle, &pool,
				      MEMCOMP_PAYLOAD_BEST_EFFORT, false);
	if (ret)
		goto out_owner;
	if (!memcomp_payloads_net_reclaimable(&bytes, 1)) {
		if (bytes == PAGE_SIZE)
			memcomp_debug_inc(&memcomp_raw_reject_net);
		ret = -E2BIG;
		goto out_payload;
	}
	ret = memcomp_objcg_charge(objcg, GFP_NOWAIT | __GFP_NOWARN, bytes);
	if (ret)
		goto out_payload;
	payload = (struct mc16_initial_payload) { .word = handle, .bytes = bytes };
	ret = mc16_reserve_cache_id(objcg, owner_id, folio_nid(account),
				    &payload, 1, out);
	if (ret) {
		memcomp_objcg_uncharge(objcg, bytes);
		goto out_payload;
	}
	/* The private refs-zero reservation is not selectable until activation. */
	if (prepared) {
		mutex_lock(memcomp_lock_of(out->token));
		*mc16_shadow_ptr(out) = MC16_SHADOW_PUBLISHING;
		mutex_unlock(memcomp_lock_of(out->token));
	}
	/* No pending/resident/OWNER transition or record/member RMW occurs. */
	mc16_cache_publish(out, bytes);
	ret = 0;
	goto out_owner;
out_payload:
	if (bytes)
		memcomp_zs_free(pool, handle);
out_owner:
	if (objcg)
		memcomp_objcg_put(objcg);
	mem_cgroup_memcomp_id_put(owner_id);
	return ret;
}

static inline int mc16_cache_store(const void *src, struct folio *account,
				   struct memcomp_ref *out)
{
	return mc16_cache_store_internal(src, account, out, false);
}

static inline int mc16_cache_store_prepared(const void *src,
					    struct folio *account,
					    struct memcomp_ref *out)
{
	return mc16_cache_store_internal(src, account, out, true);
}
#endif /* _MM_MEMCOMP_CACHE_STORE_H */
