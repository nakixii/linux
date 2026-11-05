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

static inline int mc16_cache_store(const void *src, struct folio *account,
				   struct memcomp_ref *out)
{
	struct mc16_initial_payload payload = { };
	struct obj_cgroup *objcg;
	unsigned long handle = 0;
	unsigned int bytes = 0;
	u8 pool = 0;
	int ret;

	if (!out)
		return -EINVAL;
	*out = (struct memcomp_ref) { };
	if (!src || !account)
		return -EINVAL;
	if (!memcomp_is_enabled())
		return -EOPNOTSUPP;
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
	ret = mc16_reserve_cache(objcg, folio_nid(account), &payload, 1, out);
	if (ret) {
		memcomp_objcg_uncharge(objcg, bytes);
		goto out_payload;
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
	return ret;
}
#endif /* _MM_MEMCOMP_CACHE_STORE_H */
