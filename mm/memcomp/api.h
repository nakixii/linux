/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_API_H
#define _MM_MEMCOMP_API_H

/* Public dispatch uses clustered slots; a miss never probes another table. */
bool memcomp_entry_get_ref(unsigned long token, struct memcomp_ref *ref)
{
	return mc16_get(token, ref);
}

void memcomp_entry_put_ref(struct memcomp_ref *ref)
{
	if (ref->slot)
		mc16_ref_put(ref);
}

bool memcomp_entry_get(unsigned long token)
{
	return mc16_pte_get(token);
}

void memcomp_invalidate(unsigned long token)
{
	mc16_pte_put_nr(token, 1);
}

void memcomp_invalidate_nr(unsigned long token, unsigned int nr)
{
	mc16_pte_put_nr(token, nr);
}

void memcomp_publish_cache_zero(void)
{
	mc16_zero_cache_get(1);
}

void memcomp_invalidate_cache(unsigned long token)
{
	mc16_cache_drop(token);
}

bool memcomp_entry_present(unsigned long token)
{
	return mc16_entry_present(token);
}

bool memcomp_entry_stat(unsigned long token, unsigned long *compressed,
			int *mapcount)
{
	return mc16_entry_stat(token, compressed, mapcount);
}

unsigned int memcomp_entry_nr_pages(unsigned long token, unsigned long *base)
{
	return mc16_entry_nr_pages(token, base);
}

int memcomp_entry_resident_get(unsigned long token, struct memcomp_ref *ref)
{
	return mc16_entry_resident_get(token, ref);
}

void memcomp_entry_resident_put(struct memcomp_ref *ref)
{
	mc16_entry_resident_put(ref);
}

int memcomp_entry_resident(unsigned long token)
{
	struct memcomp_ref ref;
	int ret;

	ret = mc16_entry_resident_get(token, &ref);
	mc16_entry_resident_put(&ref);
	return ret;
}

static bool mc16_refresh_pending(unsigned long token, bool pending)
{
	struct memcomp_ref ref;
	struct mc16_view view;
	bool valid;

	if (!mc16_get(token, &ref))
		return false;
	mutex_lock(memcomp_lock_of(ref.token));
	valid = mc16_view_locked(&ref, &view) && mc16_flags_valid(*view.flags);
	if (valid) {
		if (pending)
			*view.flags |= MC16_F_REFRESH_PENDING;
		else
			*view.flags &= ~MC16_F_REFRESH_PENDING;
	}
	mutex_unlock(memcomp_lock_of(ref.token));
	mc16_ref_put(&ref);
	return valid;
}

bool memcomp_entry_set_refresh_pending(unsigned long token)
{
	return mc16_refresh_pending(token, true);
}

void memcomp_entry_clear_refresh_pending(unsigned long token)
{
	mc16_refresh_pending(token, false);
}

bool memcomp_reserve(struct folio *folio)
{
	return mc16_reserve_folio(folio);
}

void memcomp_invalidate_folio(struct folio *folio)
{
	mc16_invalidate_folio(folio);
}

bool memcomp_release_folio(struct folio *folio)
{
	return mc16_release_folio(folio);
}

bool memcomp_release_folio_reclaim(struct folio *folio,
				   struct mem_cgroup *target_memcg)
{
	return mc16_release_folio_reclaim(folio, target_memcg);
}

void memcomp_rollback_folio(struct folio *folio)
{
	mc16_rollback_folio(folio);
}

int memcomp_migrate_folio(struct folio *dst, struct folio *src)
{
	return mc16_migrate_folio(dst, src);
}

void memcomp_migrate_folio_rollback(struct folio *dst, struct folio *src)
{
	mc16_migrate_folio_rollback(dst, src);
}

vm_fault_t memcomp_load(struct vm_fault *vmf, bool *locked)
{
	return mc16_load(vmf, locked);
}

int memcomp_restore_resident(struct vm_fault *vmf, struct memcomp_ref *ref)
{
	vm_fault_t ret;
	bool handled;

	if (!ref->slot)
		return -EFAULT;
	ret = mc16_load_resident(vmf, ref, &handled);
	if (!handled) {
		mc16_ref_put(ref);
		return 0;
	}
	return (ret & VM_FAULT_SIGBUS) ? -EFAULT : 0;
}

bool memcomp_store_cache(struct folio *folio, unsigned long *token)
{
	return mc16_store_cache_folio(folio, token);
}

void memcomp_publish_cache_eviction(struct folio *folio, struct memcomp_ref *ref)
{
	mc16_publish_cache_eviction(folio, ref);
}

int memcomp_charge_folio(struct memcomp_ref *ref, struct folio *folio,
			 struct mm_struct *mm, gfp_t gfp)
{
	return mc16_user_charge(ref, folio, mm, gfp);
}

bool memcomp_load_folio_pin(unsigned long token, struct memcomp_ref *ref)
{
	return mc16_cache_pin(token, ref);
}

int memcomp_load_folio(struct memcomp_ref *ref, struct folio *folio)
{
	return mc16_cache_load(ref, folio);
}

void memcomp_load_folio_abort(struct memcomp_ref *ref)
{
	mc16_cache_abort(ref);
}

void memcomp_load_folio_commit(struct memcomp_ref *ref)
{
	mc16_cache_commit(ref);
}
#endif /* _MM_MEMCOMP_API_H */
