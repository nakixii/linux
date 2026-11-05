/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MEMCOMP_TESTS_SUITE_H
#define _MM_MEMCOMP_TESTS_SUITE_H

#ifdef CONFIG_MEMCOMP_KUNIT_TEST

#include <kunit/test.h>
#include <crypto/internal/acompress.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mman.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>

#include "shared_test.h"
#include "async_test.h"

#include "format_test.h"
#include "core_test.h"
#include "storage_test.h"
#include "workingset_storage_test.h"
#include "cache_reserve_test.h"
#include "access_test.h"
#include "refs_test.h"
#include "payload_test.h"
#include "cache_store_test.h"
#include "owner_test.h"
#include "span_test.h"
#include "load_test.h"
#include "swapoff_test.h"
#include "writeback_test.h"
#include "zero_test.h"
#include "query_test.h"
#include "anon_store_test.h"
#include "scan_test.h"
#include "zero_load_test.h"
#include "consumers_test.h"
#include "cache_folio_test.h"
#include "shmem_retry_test.h"
#include "cache_xarray_race_test.h"
#include "migrate_test.h"
#include "writeback_batch_test.h"
#include "drain_test.h"
#include "swapoff_walk_test.h"
#include "defrag_test.h"
#include "resident_test.h"
#include "folio_test.h"
#include "store_transaction_test.h"
#include "workingset_store_test.h"
#include "replacement_test.h"
#include "budget_test.h"
#include "fault_test.h"
#include "workingset_load_test.h"
#include "workingset_mthp_error_test.h"
#include "shmem_zero_fault_test.h"
#include "budget_swapped_test.h"
#include "budget_packed_test.h"
#include "zero_convert_test.h"
#include "zero_carrier_test.h"
#include "zero_callers_test.h"
#include "zero_hint_test.h"
#include "aging_test.h"
#include "enospc_test.h"
#include "fault_alloc_test.h"
#include "mincore_test.h"
#include "transport_test.h"

#include "public_api_test.h"
#include "public_driver_test.h"
#include "rollback_test.h"

/* Registered first so it runs after every fixture-specific cleanup action. */
static void memcomp_test_case_cleanup(void *data)
{
	struct kunit *test = data;

	mc16_test_reclaim_retained();
	mc16_test_aging_reset();
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
}

static int memcomp_test_case_init(struct kunit *test)
{
	mc16_test_aging_reset();
	atomic_set(&mc16_test_aging_schedules, 0);
	kunit_activate_static_stub(test, mc16_schedule_aging,
				   mc16_test_schedule_aging);
	return kunit_add_action_or_reset(test, memcomp_test_case_cleanup, test);
}

static struct kunit_case memcomp_test_cases[] = {
#ifdef MC16_SHMEM_RETRY_TEST_ENABLED
	KUNIT_CASE(memcomp_test_shmem_publish_retry),
	KUNIT_CASE(memcomp_test_shmem_retry_eio),
	KUNIT_CASE(memcomp_test_shmem_retry_enomem),
	KUNIT_CASE(memcomp_test_cache_xarray_race_no_restore),
#ifdef MC16_SHMEM_ZERO_FAULT_TEST_ENABLED
	KUNIT_CASE(memcomp_test_shmem_token0_not_major),
#endif
#endif
#ifdef MC16_WORKINGSET_LOAD_TEST_ENABLED
	KUNIT_CASE(memcomp_test_workingset_load_charge_failure),
	KUNIT_CASE(memcomp_test_workingset_load_take_once),
	KUNIT_CASE(memcomp_test_workingset_load_ineligible),
	KUNIT_CASE(memcomp_test_workingset_load_read_error),
	KUNIT_CASE(memcomp_test_workingset_load_pte_race),
#endif
#ifdef MC16_WORKINGSET_MTHP_TEST_ENABLED
	KUNIT_CASE_PARAM(memcomp_test_workingset_load_mthp,
			 mc16_wl_batch_gen_params),
	KUNIT_CASE_PARAM(memcomp_test_workingset_load_mthp_read_error,
			 mc16_wl_mthp_error_gen_params),
#endif
#ifdef MC16_WORKINGSET_MAJOR_TEST_ENABLED
	KUNIT_CASE(memcomp_test_workingset_load_major_shared),
	KUNIT_CASE(memcomp_test_workingset_load_zero_not_major),
#endif
#ifdef MC16_WORKINGSET_RESIDENT_MAJOR_TEST_ENABLED
	KUNIT_CASE(memcomp_test_workingset_load_resident_not_major),
#endif
#ifdef MC16_WORKINGSET_SCOPE_TEST_ENABLED
	KUNIT_CASE_PARAM(memcomp_test_workingset_load_scope,
			 mc16_wl_scope_gen_params),
	KUNIT_CASE_PARAM(memcomp_test_workingset_load_cache_scope,
			 mc16_wl_cache_gen_params),
	KUNIT_CASE(memcomp_test_workingset_load_scope_cold),
	KUNIT_CASE(memcomp_test_workingset_load_scope_hot),
#endif
#ifdef MC16_WORKINGSET_TRANSPORT_TEST_ENABLED
	KUNIT_CASE(memcomp_test_workingset_load_transport),
#endif

#ifdef CONFIG_SWAP
	KUNIT_CASE(memcomp_test_transport_bitmap_lifecycle),
	KUNIT_CASE(memcomp_test_transport_bitmap_range),
	KUNIT_CASE(memcomp_test_transport_bitmap_devices),
	KUNIT_CASE(memcomp_test_transport_bitmap_budget),
#endif

	KUNIT_CASE(memcomp_test_workingset_storage_marker),
#ifdef CONFIG_MEMCG
	KUNIT_CASE(memcomp_test_workingset_storage_words),
	KUNIT_CASE(memcomp_test_workingset_storage_segments),
	KUNIT_CASE(memcomp_test_workingset_storage_nomem),
	KUNIT_CASE(memcomp_test_workingset_storage_retire),
	KUNIT_CASE(memcomp_test_workingset_storage_retire_nomem),
	KUNIT_CASE(memcomp_test_workingset_storage_key_identity),
	KUNIT_CASE(memcomp_test_workingset_storage_key_ref),
	KUNIT_CASE(memcomp_test_workingset_storage_key_ref_failure),
	KUNIT_CASE(memcomp_test_workingset_storage_source_id_failure),
	KUNIT_CASE(memcomp_test_workingset_storage_source_id),
	KUNIT_CASE(memcomp_test_workingset_storage_root_source),
	KUNIT_CASE(memcomp_test_workingset_storage_charge_fallback),
#endif
#ifdef MC16_WORKINGSET_STORE_TEST_ENABLED
	KUNIT_CASE(memcomp_test_workingset_store_single),
	KUNIT_CASE(memcomp_test_workingset_store_mthp),
	KUNIT_CASE(memcomp_test_workingset_store_freeze_failure),
	KUNIT_CASE(memcomp_test_workingset_store_bad_sibling),
	KUNIT_CASE(memcomp_test_workingset_store_gate_race),
	KUNIT_CASE(memcomp_test_workingset_store_prepared),
	KUNIT_CASE(memcomp_test_workingset_store_cancel_before_commit),
	KUNIT_CASE(memcomp_test_workingset_store_cache),
	KUNIT_CASE(memcomp_test_workingset_store_cache_abort),
	KUNIT_CASE(memcomp_test_workingset_store_legacy),
	KUNIT_CASE(memcomp_test_workingset_store_zero_excluded),
	KUNIT_CASE(memcomp_test_workingset_store_partial),
#endif
	KUNIT_CASE(memcomp_test_mincore_zero),
#ifdef MC16_MINCORE_PENDING_TEST_ENABLED
	KUNIT_CASE(memcomp_test_mincore_pending),
#endif
#ifdef MC16_MINCORE_SWAP_TEST_ENABLED
	KUNIT_CASE(memcomp_test_mincore_pool),
	KUNIT_CASE(memcomp_test_mincore_swapped_absent),
	KUNIT_CASE(memcomp_test_mincore_swapped_cached),
	KUNIT_CASE(memcomp_test_mincore_swapped_incomplete),
#endif
#ifdef MC16_MINCORE_SHMEM_TEST_ENABLED
	KUNIT_CASE(memcomp_test_mincore_shmem),
	KUNIT_CASE(memcomp_test_mincore_shmem_replaced),
#endif
#ifdef MC16_MINCORE_MM_TEST_ENABLED
	KUNIT_CASE(memcomp_test_mincore_mthp),
	KUNIT_CASE(memcomp_test_mincore_pte_replaced),
#endif

#ifdef MC16_ZERO_CARRIER_TEST_ENABLED
	KUNIT_CASE(memcomp_test_zero_carrier_ready),
	KUNIT_CASE(memcomp_test_zero_carrier_commit_failure),
	KUNIT_CASE(memcomp_test_zero_carrier_refresh_preflight),
	KUNIT_CASE(memcomp_test_zero_carrier_store_preflight),
	KUNIT_CASE(memcomp_test_zero_carrier_nonzero),
	KUNIT_CASE(memcomp_test_zero_carrier_mthp),
	KUNIT_CASE(memcomp_test_zero_carrier_token_mismatch),
	KUNIT_CASE(memcomp_test_zero_carrier_invalid),
	KUNIT_CASE(memcomp_test_zero_carrier_borrow_owner),
	KUNIT_CASE(memcomp_test_zero_carrier_lifecycle_refault1),
	KUNIT_CASE(memcomp_test_zero_carrier_lifecycle_exit),
#endif
#ifdef CONFIG_MEMCG
	KUNIT_CASE(memcomp_test_aging_batch_bounds),
	KUNIT_CASE(memcomp_test_aging_deadlines),
	KUNIT_CASE(memcomp_test_aging_mutation),
	KUNIT_CASE(memcomp_test_aging_eligibility),
	KUNIT_CASE(memcomp_test_aging_requests),
	KUNIT_CASE(memcomp_test_aging_rcu_ownership),
	KUNIT_CASE(memcomp_test_aging_teardown),
	KUNIT_CASE(memcomp_test_aging_reuse),
	KUNIT_CASE(memcomp_test_aging_stops),
	KUNIT_CASE(memcomp_test_aging_shrinker),
	KUNIT_CASE(memcomp_test_aging_real_work),
	KUNIT_CASE(memcomp_test_retained_x1),
	KUNIT_CASE(memcomp_test_retained_x2_segment),
	KUNIT_CASE(memcomp_test_retained_x2_replace),
	KUNIT_CASE(memcomp_test_meta_shrinker_exact),
	KUNIT_CASE(memcomp_test_meta_shrinker_wrap),
	KUNIT_CASE(memcomp_test_meta_shrinker_progress),
	KUNIT_CASE(memcomp_test_meta_shrinker_drained),
#endif
#ifdef MC16_ZERO_HINT_DENSITY_TEST_ENABLED
	KUNIT_CASE(memcomp_test_zero_hint_ordinary_refault),
	KUNIT_CASE(memcomp_test_zero_hint_serial533),
	KUNIT_CASE(memcomp_test_zero_hint_route512),
	KUNIT_CASE(memcomp_test_zero_hint_density65536),
#endif
#ifdef MC16_FOLIO_TEST_ENABLED
	KUNIT_CASE(memcomp_test_budget_pageout_stored),
	KUNIT_CASE(memcomp_test_budget_pageout_samefill),
	KUNIT_CASE(memcomp_test_budget_refault1_stored),
	KUNIT_CASE(memcomp_test_budget_refault1_samefill),
	KUNIT_CASE(memcomp_test_budget_fork_stored),
	KUNIT_CASE(memcomp_test_budget_fork_samefill),
	KUNIT_CASE(memcomp_test_budget_exit_stored),
	KUNIT_CASE(memcomp_test_budget_exit_samefill),
	KUNIT_CASE(memcomp_test_budget_refault4_stored),
	KUNIT_CASE(memcomp_test_budget_refault4_samefill),
	KUNIT_CASE(memcomp_test_budget_fixed512),
#endif
#ifdef MC16_REPLACEMENT_TEST_ENABLED
	KUNIT_CASE(memcomp_test_replacement_mixed_sibling),
	KUNIT_CASE(memcomp_test_replacement_mixed_head),
	KUNIT_CASE(memcomp_test_replacement_stored_order0),
	KUNIT_CASE(memcomp_test_replacement_stored_order1),
#endif
#ifdef MC16_STORE_TRANSACTION_TEST_ENABLED
	KUNIT_CASE(memcomp_test_store_transaction_success),
	KUNIT_CASE(memcomp_test_store_transaction_preflight_lookup),
	KUNIT_CASE(memcomp_test_store_transaction_busy),
	KUNIT_CASE(memcomp_test_store_transaction_arch_failure),
	KUNIT_CASE(memcomp_test_store_transaction_payload_pages),
	KUNIT_CASE(memcomp_test_store_transaction_partial_commit),
	KUNIT_CASE(memcomp_test_store_transaction_incompressible),
	KUNIT_CASE(memcomp_test_store_transaction_outcomes),
#endif
#ifdef MC16_DRIVER_RESIDENT_TEST_ENABLED
	KUNIT_CASE(memcomp_test_public_resident_restore),
	KUNIT_CASE(memcomp_test_public_restore_error),
#endif
#ifdef MC16_DRIVER_SWAPOFF_TEST_ENABLED
	KUNIT_CASE(memcomp_test_public_swapoff_type_miss),
	KUNIT_CASE(memcomp_test_public_swapoff_error),
#endif
#ifdef CONFIG_MEMCG
	KUNIT_CASE(memcomp_test_async_registered_submit),
	KUNIT_CASE(memcomp_test_async_registered_backlog),
	KUNIT_CASE(memcomp_test_async_registered_error),
	KUNIT_CASE(memcomp_test_async_registered_finish_fail),
	KUNIT_CASE(memcomp_test_async_software_samefill),
	KUNIT_CASE(memcomp_test_async_lease_identity),
	KUNIT_CASE(memcomp_test_async_precomp_offset),
	KUNIT_CASE(memcomp_test_async_precomp_mixed),
#endif
#ifdef MC16_ROLLBACK_TEST_ENABLED
	KUNIT_CASE(memcomp_test_public_rollback_restored),
	KUNIT_CASE(memcomp_test_public_rollback_lookup_only),
	KUNIT_CASE(memcomp_test_public_rollback_rewalk),
#endif
#ifdef MC16_FAULT_MM_TEST_ENABLED
	KUNIT_CASE(memcomp_test_public_madv_free),
	KUNIT_CASE(memcomp_test_public_madv_free_partial),
	KUNIT_CASE(memcomp_test_public_madv_free_shared),
	KUNIT_CASE(memcomp_test_public_fault),
#endif
#ifdef CONFIG_MEMCG
	KUNIT_CASE(memcomp_test_public_cache_transaction),
	KUNIT_CASE(memcomp_test_public_ref_domains),
	KUNIT_CASE(memcomp_test_public_query_busy),
	KUNIT_CASE(memcomp_test_public_refresh_retry),
	KUNIT_CASE(memcomp_test_public_cache_charge_failure),
	KUNIT_CASE(memcomp_test_public_invalid_refs),
#endif
	KUNIT_CASE(memcomp_test_cache_folio_compressed),
	KUNIT_CASE(memcomp_test_cache_folio_samefill),
	KUNIT_CASE(memcomp_test_cache_folio_last_byte),
	KUNIT_CASE(memcomp_test_cache_folio_zero_abort),
	KUNIT_CASE(memcomp_test_cache_folio_zero_publish),
	KUNIT_CASE(memcomp_test_cache_folio_arch_failure),
	KUNIT_CASE(memcomp_test_cache_folio_arch_seam),
	KUNIT_CASE(memcomp_test_cache_folio_samefill_arch_failure),
	KUNIT_CASE(memcomp_test_cache_folio_disabled),
	KUNIT_CASE(memcomp_test_cache_folio_preflight),
	KUNIT_CASE(memcomp_test_cache_folio_large),
	KUNIT_CASE(memcomp_test_cache_folio_metadata_failure),
#ifdef CONFIG_MEMCG
	KUNIT_CASE(memcomp_test_cache_folio_charge_failure),
	KUNIT_CASE(memcomp_test_cache_folio_key_charge_failure),
#endif
#if defined(CONFIG_ARM64) && defined(CONFIG_ARM64_MTE)
	KUNIT_CASE(memcomp_test_cache_folio_tagged_zero),
#endif
#ifdef MC16_FOLIO_TEST_ENABLED
	KUNIT_CASE(memcomp_test_folio_reserve),
	KUNIT_CASE(memcomp_test_folio_reserve_failure),
	KUNIT_CASE(memcomp_test_folio_rollback_rmap),
	KUNIT_CASE(memcomp_test_folio_partial_rollback),
	KUNIT_CASE(memcomp_test_folio_partial_invalidate),
	KUNIT_CASE(memcomp_test_folio_release),
	KUNIT_CASE(memcomp_test_folio_release_bad_head),
	KUNIT_CASE(memcomp_test_folio_release_bad_sibling),
	KUNIT_CASE(memcomp_test_folio_transport),
	KUNIT_CASE(memcomp_test_folio_missed_rmap),
#endif
#ifdef CONFIG_MEMCG
	KUNIT_CASE(memcomp_test_migrate_stored),
	KUNIT_CASE(memcomp_test_migrate_samefill),
	KUNIT_CASE(memcomp_test_migrate_swapped),
	KUNIT_CASE(memcomp_test_migrate_rollback),
	KUNIT_CASE(memcomp_test_migrate_partial),
	KUNIT_CASE(memcomp_test_migrate_revalidate),
	KUNIT_CASE(memcomp_test_migrate_pending),
	KUNIT_CASE(memcomp_test_migrate_geometry),
	KUNIT_CASE(memcomp_test_migrate_owner),
	KUNIT_CASE(memcomp_test_migrate_missing),
	KUNIT_CASE(memcomp_test_migrate_locks),
	KUNIT_CASE(memcomp_test_migrate_rollback_missing),
	KUNIT_CASE(memcomp_test_migrate_rollback_invalid),
	KUNIT_CASE(memcomp_test_migrate_incompressible),
	KUNIT_CASE(memcomp_test_migrate_single),
	KUNIT_CASE(memcomp_test_migrate_dst_geometry),
	KUNIT_CASE(memcomp_test_migrate_range),
	KUNIT_CASE(memcomp_test_migrate_record_geometry),
#endif
	KUNIT_CASE(memcomp_test_fault_alloc_single_page),
	KUNIT_CASE(memcomp_test_fault_alloc_defrag_policy),
	KUNIT_CASE(memcomp_test_fault_alloc_single_fallback),
	KUNIT_CASE(memcomp_test_format_layout),
	KUNIT_CASE(memcomp_test_zero_page),
	KUNIT_CASE(memcomp_test_zero_owners),
	KUNIT_CASE(memcomp_test_zero_batch_bounds),
	KUNIT_CASE(memcomp_test_zero_stat),
	KUNIT_CASE(memcomp_test_zero_cross_cpu),
#ifdef MC16_ZERO_CALLERS_TEST_ENABLED
	KUNIT_CASE(memcomp_test_zero_callers_fork_get),
	KUNIT_CASE(memcomp_test_zero_callers_put_domains),
	KUNIT_CASE(memcomp_test_zero_callers_put_bounds),
	KUNIT_CASE(memcomp_test_zero_callers_query),
	KUNIT_CASE(memcomp_test_zero_callers_stat),
	KUNIT_CASE(memcomp_test_zero_callers_batch_zero),
	KUNIT_CASE(memcomp_test_zero_callers_batch_positive),
	KUNIT_CASE(memcomp_test_zero_callers_batch_exclusive),
	KUNIT_CASE(memcomp_test_zero_callers_batch_soft_dirty),
	KUNIT_CASE(memcomp_test_zero_callers_batch_uffd_wp),
	KUNIT_CASE(memcomp_test_zero_callers_cache_publish),
	KUNIT_CASE(memcomp_test_zero_callers_cache_load),
	KUNIT_CASE(memcomp_test_zero_callers_cache_invalid),
	KUNIT_CASE(memcomp_test_zero_callers_release),
	KUNIT_CASE(memcomp_test_zero_callers_release_bounds),
	KUNIT_CASE(memcomp_test_zero_callers_load_read),
	KUNIT_CASE(memcomp_test_zero_callers_load_write),
	KUNIT_CASE(memcomp_test_zero_callers_load_oom),
#endif
#ifdef MC16_ZERO_LOAD_MM_TEST_ENABLED
	KUNIT_CASE(memcomp_test_zero_load_write),
	KUNIT_CASE(memcomp_test_zero_load_softdirty),
	KUNIT_CASE(memcomp_test_zero_load_softdirty_wp),
	KUNIT_CASE(memcomp_test_zero_load_uffd_wp),
	KUNIT_CASE(memcomp_test_zero_load_readonly),
	KUNIT_CASE(memcomp_test_zero_load_changed),
	KUNIT_CASE(memcomp_test_zero_load_zapped),
	KUNIT_CASE(memcomp_test_zero_load_alloc_oom),
	KUNIT_CASE(memcomp_test_zero_load_charge_oom),
	KUNIT_CASE(memcomp_test_zero_load_retry),
#endif
	KUNIT_CASE(memcomp_test_format_states_orders),
	KUNIT_CASE(memcomp_test_format_flags_reject),
	KUNIT_CASE(memcomp_test_format_length_bounds),
	KUNIT_CASE(memcomp_test_format_payload_states),
	KUNIT_CASE(memcomp_test_format_full_words),
	KUNIT_CASE(memcomp_test_format_swap_raw),
	KUNIT_CASE(memcomp_test_format_swap_packed),
	KUNIT_CASE(memcomp_test_format_swap_encode_reject),
	KUNIT_CASE(memcomp_test_format_swap_decode_reject),
	KUNIT_CASE(memcomp_test_format_pending_record),
	KUNIT_CASE(memcomp_test_format_member_index_orders),
	KUNIT_CASE(memcomp_test_format_member_index_reject),
	KUNIT_CASE(memcomp_test_format_null_outputs),
#ifdef CONFIG_SWAP
#ifdef CONFIG_XARRAY_MULTI
#endif
#endif
	KUNIT_CASE(memcomp_test_initial_compression_placement),
	KUNIT_CASE(memcomp_test_writeback_pressure_scope),
	KUNIT_CASE(memcomp_test_writeback_feedback),
	KUNIT_CASE(memcomp_test_writeback_budget_values),
	KUNIT_CASE(memcomp_test_writeback_level_floor),
	KUNIT_CASE(memcomp_test_writeback_gate_values),
	KUNIT_CASE(memcomp_test_writeback_drain_loop),
	KUNIT_CASE(memcomp_test_writeback_drain_transient),
	KUNIT_CASE(memcomp_test_writeback_drain_recovery_applied),
	KUNIT_CASE(memcomp_test_writeback_drain_handoff),
	KUNIT_CASE(memcomp_test_writeback_drain_slice),
	KUNIT_CASE(memcomp_test_writeback_pass_bounded),
#ifdef CONFIG_SWAP
#endif
	KUNIT_CASE(memcomp_test_writeback_budget_vs_legacy),
	KUNIT_CASE(memcomp_test_comp_ctx_bitmap),
	KUNIT_CASE(memcomp_test_comp_budget),
	KUNIT_CASE(memcomp_test_direct_staging_policy),
	KUNIT_CASE(memcomp_test_direct_staging_pool),
	KUNIT_CASE(memcomp_test_zstd_real_page_roundtrip),
	KUNIT_CASE(memcomp_test_zstd_page_ratio_bench),
	KUNIT_CASE(memcomp_test_zstd_huf_cache_frames),
	KUNIT_CASE(memcomp_test_zstd_huf_cache_shift),
	KUNIT_CASE(memcomp_test_direct_staging_execution),
	KUNIT_CASE(memcomp_test_store_consumes_context),
	KUNIT_CASE(memcomp_test_async_pool),
	KUNIT_CASE(memcomp_test_async_late_registration),
	KUNIT_CASE(memcomp_test_same_filled_plausible),
	KUNIT_CASE(memcomp_test_ctx_timing_helpers),
	KUNIT_CASE(memcomp_test_comp_ctx_configuration),
	KUNIT_CASE(memcomp_test_stock_transforms),
	KUNIT_CASE(memcomp_test_zs_gfp),
	KUNIT_CASE(memcomp_test_reclaim_ctx_admission),
	KUNIT_CASE(memcomp_test_reclaim_ctx_precheck),
	KUNIT_CASE(memcomp_test_reserve_eligibility),
	KUNIT_CASE(memcomp_test_admission_rollback_unescaped),
	KUNIT_CASE(memcomp_test_samefill),
	KUNIT_CASE(memcomp_test_raw_payload_threshold),
	KUNIT_CASE(memcomp_test_compressor_e2big),
	KUNIT_CASE(memcomp_test_mixed_raw_net_reclaim),
	KUNIT_CASE(memcomp_test_state_signed_sum),
	KUNIT_CASE(memcomp_test_incompressible_mark_lifetime),
#ifdef CONFIG_ADVISE_SYSCALLS
	KUNIT_CASE(memcomp_test_lazyfree_mark_discarded),
#endif
	KUNIT_CASE(memcomp_test_rollback_restores_ptes),
	KUNIT_CASE(memcomp_test_store_drops_anon_vma),
	KUNIT_CASE(memcomp_test_invalidate_drops_anon_vma),
#ifdef CONFIG_MEMCG
	KUNIT_CASE(memcomp_test_record_members),
	KUNIT_CASE(memcomp_test_record_abort),
	KUNIT_CASE(memcomp_test_record_charge_failure),
	KUNIT_CASE(memcomp_test_reserve_boundaries),
	KUNIT_CASE(memcomp_test_reserve_holes),
	KUNIT_CASE(memcomp_test_segment_retire),
	KUNIT_CASE(memcomp_test_segment_retire_nomem),
	KUNIT_CASE(memcomp_test_reserve_nomem),
	KUNIT_CASE(memcomp_test_reserve_race),
	KUNIT_CASE(memcomp_test_cluster_key_lifetime),
	KUNIT_CASE(memcomp_test_pending_final_guard),
	KUNIT_CASE(memcomp_test_samefill_normalize),
	KUNIT_CASE(memcomp_test_payload_final_release),
	KUNIT_CASE(memcomp_test_segment_reserve_race),
	KUNIT_CASE(memcomp_test_rollover_retire_race),
	KUNIT_CASE(memcomp_test_record_outlives_key),
	KUNIT_CASE(memcomp_test_header_before_segment_callback),
	KUNIT_CASE(memcomp_test_reserve_id_failure),
	KUNIT_CASE(memcomp_test_reserve_key_failure),
	KUNIT_CASE(memcomp_test_put_pair),
	KUNIT_CASE(memcomp_test_payload_samefill),
	KUNIT_CASE(memcomp_test_payload_replace),
	KUNIT_CASE(memcomp_test_payload_stale),
	KUNIT_CASE(memcomp_test_payload_charge_failure),
	KUNIT_CASE(memcomp_test_payload_writeback),
	KUNIT_CASE(memcomp_test_load_pending),
	KUNIT_CASE(memcomp_test_load_samefill),
	KUNIT_CASE(memcomp_test_load_raw),
	KUNIT_CASE(memcomp_test_load_compressed),
	KUNIT_CASE(memcomp_test_load_writeback),
#ifdef CONFIG_SWAP
	KUNIT_CASE(memcomp_test_swapoff_raw),
	KUNIT_CASE(memcomp_test_swapoff_packed),
	KUNIT_CASE(memcomp_test_swapoff_samefill),
	KUNIT_CASE(memcomp_test_swapoff_owner_race),
	KUNIT_CASE(memcomp_test_swapoff_failures),
	KUNIT_CASE(memcomp_test_swapoff_changed_backing),
	KUNIT_CASE(memcomp_test_swapoff_invalid_commit),
	KUNIT_CASE(memcomp_test_swapoff_raw_commit),
	KUNIT_CASE(memcomp_test_swapoff_deferred_tail),
	KUNIT_CASE(memcomp_test_swapoff_changed_length),
	KUNIT_CASE(memcomp_test_swapoff_changed_offset),
	KUNIT_CASE(memcomp_test_swapoff_changed_packed),
	KUNIT_CASE(memcomp_test_swapoff_bad_member_flags),
	KUNIT_CASE(memcomp_test_swapoff_bad_stable_flags),
	KUNIT_CASE(memcomp_test_writeback_cache_raw),
	KUNIT_CASE(memcomp_test_writeback_owner_packed),
	KUNIT_CASE(memcomp_test_writeback_revert),
	KUNIT_CASE(memcomp_test_writeback_mismatch),
	KUNIT_CASE(memcomp_test_writeback_encoding),
	KUNIT_CASE(memcomp_test_writeback_last_pte),
#endif
	KUNIT_CASE(memcomp_test_owner_stored_normalize),
	KUNIT_CASE(memcomp_test_owner_samefill_normalize),
	KUNIT_CASE(memcomp_test_owner_writeback_guard),
	KUNIT_CASE(memcomp_test_owner_resident_final_guard),
	KUNIT_CASE(memcomp_test_owner_last_pair),
	KUNIT_CASE(memcomp_test_owner_stale),
	KUNIT_CASE(memcomp_test_span_geometry),
	KUNIT_CASE(memcomp_test_span_identity),
	KUNIT_CASE(memcomp_test_span_anon_vma),
	KUNIT_CASE(memcomp_test_cache_reserve_payloads),
	KUNIT_CASE(memcomp_test_cache_reserve_boundaries),
	KUNIT_CASE(memcomp_test_cache_reserve_invalid),
	KUNIT_CASE(memcomp_test_cache_reserve_nomem),
	KUNIT_CASE(memcomp_test_cache_reserve_race),
	KUNIT_CASE(memcomp_test_cache_reserve_retire),
	KUNIT_CASE(memcomp_test_cache_reserve_root),
	KUNIT_CASE(memcomp_test_cache_store_samefill),
	KUNIT_CASE(memcomp_test_cache_store_compressed),
	KUNIT_CASE(memcomp_test_cache_publish_window),
	KUNIT_CASE(memcomp_test_zero_no_storage),
	KUNIT_CASE(memcomp_test_scan_empty_budget),
	KUNIT_CASE(memcomp_test_scan_reserved_markers),
	KUNIT_CASE(memcomp_test_scan_reserved_last),
	KUNIT_CASE(memcomp_test_scan_first),
	KUNIT_CASE(memcomp_test_scan_last_guard),
	KUNIT_CASE(memcomp_test_scan_alignment),
	KUNIT_CASE(memcomp_test_scan_refs_zero),
	KUNIT_CASE(memcomp_test_scan_segment_retired),
	KUNIT_CASE(memcomp_test_scan_private),
	KUNIT_CASE(memcomp_test_scan_sparse),
	KUNIT_CASE(memcomp_test_scan_u32_max),
	KUNIT_CASE(memcomp_test_scan_w_relookup),
	KUNIT_CASE(memcomp_test_query_present),
	KUNIT_CASE(memcomp_test_query_absent),
	KUNIT_CASE(memcomp_test_query_zero),
	KUNIT_CASE(memcomp_test_query_stat_ptl),
	KUNIT_CASE(memcomp_test_query_stat_busy_invalid),
	KUNIT_CASE(memcomp_test_query_stat_compressed),
	KUNIT_CASE(memcomp_test_query_pending_owner_drop),
	KUNIT_CASE(memcomp_test_query_geometry),
	KUNIT_CASE(memcomp_test_query_resident_busy),
	KUNIT_CASE(memcomp_test_query_nonresident_guard),
	KUNIT_CASE(memcomp_test_anon_store_samefill),
	KUNIT_CASE(memcomp_test_anon_store_compressed),
	KUNIT_CASE(memcomp_test_anon_store_mixed_raw),
	KUNIT_CASE(memcomp_test_anon_store_reserve_failure),
	KUNIT_CASE(memcomp_test_anon_store_charge_failure),
	KUNIT_CASE(memcomp_test_anon_store_partial_commit),
	KUNIT_CASE(memcomp_test_anon_store_invalid_refs),
	KUNIT_CASE(memcomp_test_anon_store_precomp),
#if defined(CONFIG_MEMCG) && defined(CONFIG_MEMCOMP_DEBUG_STATS)
	KUNIT_CASE(memcomp_test_anon_store_cross_node_refresh),
#endif
	KUNIT_CASE(memcomp_test_wbb_raw),
	KUNIT_CASE(memcomp_test_wbb_unpacked_compressed),
	KUNIT_CASE(memcomp_test_wbb_packed),
	KUNIT_CASE(memcomp_test_wbb_owner_detach),
	KUNIT_CASE(memcomp_test_wbb_alloc_failure),
	KUNIT_CASE(memcomp_test_wbb_dup_failure),
	KUNIT_CASE(memcomp_test_wbb_dirty_io),
	KUNIT_CASE(memcomp_test_wbb_settle_reject),
	KUNIT_CASE(memcomp_test_swapoff_walk_empty),
	KUNIT_CASE(memcomp_test_swapoff_walk_31),
	KUNIT_CASE(memcomp_test_swapoff_walk_32),
	KUNIT_CASE(memcomp_test_swapoff_walk_33),
	KUNIT_CASE(memcomp_test_swapoff_walk_64),
	KUNIT_CASE(memcomp_test_swapoff_walk_non_target),
	KUNIT_CASE(memcomp_test_swapoff_walk_hole),
	KUNIT_CASE(memcomp_test_swapoff_walk_retired),
	KUNIT_CASE(memcomp_test_swapoff_walk_segment_hole),
	KUNIT_CASE(memcomp_test_swapoff_walk_private),
	KUNIT_CASE(memcomp_test_swapoff_walk_markers),
	KUNIT_CASE(memcomp_test_swapoff_walk_reflush),
	KUNIT_CASE(memcomp_test_swapoff_walk_u32_max),
	KUNIT_CASE(memcomp_test_swapoff_walk_exits),
	KUNIT_CASE(memcomp_test_defrag_roundtrip),
	KUNIT_CASE(memcomp_test_defrag_noncandidates),
	KUNIT_CASE(memcomp_test_defrag_buffer_failure),
	KUNIT_CASE(memcomp_test_defrag_read_failure),
	KUNIT_CASE(memcomp_test_defrag_alloc_failure),
	KUNIT_CASE(memcomp_test_defrag_charge_failure),
	KUNIT_CASE(memcomp_test_defrag_stale_word),
	KUNIT_CASE(memcomp_test_defrag_stale_length),
	KUNIT_CASE(memcomp_test_defrag_owner_normalize),
	KUNIT_CASE(memcomp_test_defrag_scan_budget),
	KUNIT_CASE(memcomp_test_defrag_scan_moves),
	KUNIT_CASE(memcomp_test_defrag_scan_error),
	KUNIT_CASE(memcomp_test_drain_cold_packed),
	KUNIT_CASE(memcomp_test_drain_cold_raw),
	KUNIT_CASE(memcomp_test_drain_hot_second_chance),
	KUNIT_CASE(memcomp_test_drain_zero_limit),
	KUNIT_CASE(memcomp_test_drain_empty_fifo),
	KUNIT_CASE(memcomp_test_drain_ineligible_budget),
	KUNIT_CASE(memcomp_test_drain_alloc_failure),
	KUNIT_CASE(memcomp_test_drain_stage_alloc_failure),
	KUNIT_CASE(memcomp_test_drain_dirty_io),
	KUNIT_CASE(memcomp_test_drain_enospc),
	KUNIT_CASE(memcomp_test_drain_stage_enospc),
	KUNIT_CASE(memcomp_test_drain_zero_limit_flush),
	KUNIT_CASE(memcomp_test_drain_partial_flush_failure),
	KUNIT_CASE(memcomp_test_drain_first_error),
#ifdef MC16_RESIDENT_TEST_ENABLED
	KUNIT_CASE(memcomp_test_resident_write),
	KUNIT_CASE(memcomp_test_resident_readonly),
	KUNIT_CASE(memcomp_test_resident_shared),
	KUNIT_CASE(memcomp_test_resident_softdirty_wp),
	KUNIT_CASE(memcomp_test_resident_uffd_wp),
	KUNIT_CASE(memcomp_test_resident_changed_pte),
	KUNIT_CASE(memcomp_test_resident_zapped),
	KUNIT_CASE(memcomp_test_resident_unhandled),
	KUNIT_CASE(memcomp_test_resident_state_race),
	KUNIT_CASE(memcomp_test_resident_folio_race),
	KUNIT_CASE(memcomp_test_resident_flag_race),
	KUNIT_CASE(memcomp_test_resident_bad_index),
	KUNIT_CASE(memcomp_test_resident_retry_nowait),
	KUNIT_CASE(memcomp_test_resident_retry),
#endif
#ifdef MC16_FAULT_MM_TEST_ENABLED
	KUNIT_CASE(memcomp_test_fault_single),
	KUNIT_CASE(memcomp_test_fault_raw),
	KUNIT_CASE(memcomp_test_fault_attached),
	KUNIT_CASE(memcomp_test_fault_shared),
	KUNIT_CASE(memcomp_test_fault_softdirty),
	KUNIT_CASE(memcomp_test_fault_softdirty_wp),
	KUNIT_CASE(memcomp_test_fault_uffd_wp),
	KUNIT_CASE(memcomp_test_fault_readonly),
	KUNIT_CASE(memcomp_test_fault_changed),
	KUNIT_CASE(memcomp_test_fault_alloc_oom),
	KUNIT_CASE(memcomp_test_fault_charge_oom),
	KUNIT_CASE(memcomp_test_fault_payload_retry),
	KUNIT_CASE(memcomp_test_fault_payload_invalid),
	KUNIT_CASE(memcomp_test_fault_thp),
	KUNIT_CASE(memcomp_test_fault_thp_alloc_fallback),
	KUNIT_CASE(memcomp_test_fault_thp_charge_fallback),
	KUNIT_CASE(memcomp_test_fault_thp_pte_fallback),
	KUNIT_CASE(memcomp_test_fault_thp_sibling_gone),
	KUNIT_CASE(memcomp_test_fault_zero),
#endif
#ifdef MC16_ZERO_CONVERT_TEST_ENABLED
	KUNIT_CASE(memcomp_test_zero_convert_flags),
	KUNIT_CASE(memcomp_test_zero_convert_stale),
	KUNIT_CASE(memcomp_test_zero_convert_rmap),
	KUNIT_CASE(memcomp_test_zero_convert_move_miss),
#endif
	KUNIT_CASE(memcomp_test_consumer_pte_shared),
	KUNIT_CASE(memcomp_test_consumer_pte_bounds),
	KUNIT_CASE(memcomp_test_consumer_cache_abort),
	KUNIT_CASE(memcomp_test_consumer_cache_samefill),
	KUNIT_CASE(memcomp_test_consumer_cache_commit),
	KUNIT_CASE(memcomp_test_consumer_cache_retry),
	KUNIT_CASE(memcomp_test_consumer_zero_domains),
	KUNIT_CASE(memcomp_test_consumer_load_bounds),
	KUNIT_CASE(memcomp_test_cache_store_charge_failure),
	KUNIT_CASE(memcomp_test_cache_store_reserve_failure),
	KUNIT_CASE(memcomp_test_fifo_eligibility),
	KUNIT_CASE(memcomp_test_fifo_scan_budget),
	KUNIT_CASE(memcomp_test_writeback_last_guard),
	KUNIT_CASE(memcomp_test_enospc_result),
	KUNIT_CASE(memcomp_test_enospc_sync),
	KUNIT_CASE(memcomp_test_enospc_precomp),
	KUNIT_CASE(memcomp_test_enospc_async_events),
	KUNIT_CASE(memcomp_test_objcg_source_identity),
#endif
	KUNIT_CASE(memcomp_test_store_cache_rejects_mapped),
#ifdef CONFIG_SWAP
	KUNIT_CASE(memcomp_test_writeback_pack_fits),
	KUNIT_CASE(memcomp_test_writeback_decode_slot),
	KUNIT_CASE(memcomp_test_writeback_defrag_wanted),
	KUNIT_CASE(memcomp_test_sysfs_attrs_render),
#endif
	{}
};

static struct kunit_suite memcomp_test_suite = {
	.name = "memcomp",
	.init = memcomp_test_case_init,
	.test_cases = memcomp_test_cases,
};

kunit_test_suite(memcomp_test_suite);

#endif /* CONFIG_MEMCOMP_KUNIT_TEST */

#endif /* _MM_MEMCOMP_TESTS_SUITE_H */
