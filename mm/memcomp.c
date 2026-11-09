// SPDX-License-Identifier: GPL-2.0
/*
 * memcomp - direct in-memory compression for reclaimed pages.
 *
 * Reclaim compresses anonymous and shmem pages straight into zsmalloc
 * and installs SWP_MEMCOMP PTEs (or page-cache shadows) that carry a
 * token into a global xarray.  Faults decompress synchronously.  Under
 * continued pressure a shrinker writes cold payloads to real swap.  Compressible
 * payloads share packed slots; raw payloads use individual slots.  Refaults
 * recover the payload according to the entry's backing format.
 *
 * Metadata is kept deliberately small: entries carry no private mutex
 * (payload state is serialized by a hashed lock table), and a swap-backed
 * entry needs no descriptor - its slot lives in the handle word.
 */
#define pr_fmt(fmt) "memcomp: " fmt

#include <crypto/algapi.h>
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/cacheflush.h>
#include <linux/cpumask.h>
#include <linux/crypto.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/local_lock.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/memcomp.h>
#include <linux/mempolicy.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/mmu_notifier.h>
#include <linux/huge_mm.h>
#include <linux/mm_inline.h>
#include <linux/memcontrol.h>
#include <linux/numa.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/rmap.h>
#include <linux/sched/cputime.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/timekeeping.h>
#include <linux/wait.h>
#include <linux/wait_bit.h>
#include <linux/writeback.h>
#include <linux/workqueue.h>
#include <linux/pagemap.h>
#include <linux/pgtable.h>
#include <linux/xarray.h>
#include <linux/zsmalloc.h>

#include "swap.h"
#include "memcomp/fault_alloc.h"
#include "internal.h"

/*
 * A single zsmalloc pool minimizes partially-filled zspages and thus
 * fragmentation; zsmalloc's per-class locks provide the parallelism
 * (zram uses one pool per device the same way).
 */
#define MEMCOMP_NR_POOLS 1
#define MEMCOMP_MAX_COMP_CTXS 32
#define MEMCOMP_MAX_BORROWED_COMP_CTXS 1
#define MEMCOMP_DEFAULT_COMPRESSOR CONFIG_MEMCOMP_DEFAULT_COMPRESSOR
#define MEMCOMP_MAX_COMPRESSED_SIZE (2 * PAGE_SIZE)
#define MEMCOMP_MAX_COMPRESS_PAGES 16
#define MEMCOMP_COMPRESSOR_NAME_LEN 32
#define MEMCOMP_CTX_TIMING_SAMPLE_SHIFT 8
#define MEMCOMP_CTX_TIMING_SAMPLE_INTERVAL \
	(1U << MEMCOMP_CTX_TIMING_SAMPLE_SHIFT)
#define MEMCOMP_CTX_TIMING_SAMPLE_MASK \
	(MEMCOMP_CTX_TIMING_SAMPLE_INTERVAL - 1)
/* Foreground reclaim is limited by contexts, not the background budget. */
#define MEMCOMP_DIRECT_COMP_BUDGET 0

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
#define memcomp_debug_inc(counter)		atomic64_inc(counter)
#define memcomp_debug_dec(counter)		atomic64_dec(counter)
#define memcomp_debug_add(value, counter)	atomic64_add(value, counter)
#define memcomp_debug_long_inc(counter)	atomic_long_inc(counter)
#define memcomp_debug_long_dec(counter)	atomic_long_dec(counter)
#define memcomp_debug_long_add(value, counter) \
	atomic_long_add(value, counter)
#define memcomp_debug_long_sub(value, counter) \
	atomic_long_sub(value, counter)
#define memcomp_debug_this_cpu_inc(field)	this_cpu_inc(field)
#define memcomp_debug_this_cpu_dec(field)	this_cpu_dec(field)

static void memcomp_atomic64_update_max(atomic64_t *counter, u64 value);

static u64 memcomp_debug_clock(void)
{
	return ktime_get_ns();
}

/* Add the time since @start to @total, and keep the longest in @max. */
static void memcomp_debug_elapsed(u64 start, atomic64_t *total,
				  atomic64_t *max)
{
	u64 delta = ktime_get_ns() - start;

	atomic64_add(delta, total);
	memcomp_atomic64_update_max(max, delta);
}
#else
#define memcomp_debug_inc(...)		do { } while (0)
#define memcomp_debug_dec(...)		do { } while (0)
#define memcomp_debug_add(...)		do { } while (0)
#define memcomp_debug_long_inc(...)		do { } while (0)
#define memcomp_debug_long_dec(...)		do { } while (0)
#define memcomp_debug_long_add(...)		do { } while (0)
#define memcomp_debug_long_sub(...)		do { } while (0)
#define memcomp_debug_this_cpu_inc(...)	do { } while (0)
#define memcomp_debug_this_cpu_dec(...)	do { } while (0)

static inline u64 memcomp_debug_clock(void)
{
	return 0;
}

#define memcomp_debug_elapsed(start, ...)		((void)(start))
#endif

#define memcomp_wb_reclaimed(nr) \
	memcomp_debug_add(nr, &memcomp_writeback_zs_reclaimed)

#define MEMCOMP_LOCK_BITS 10
#define MEMCOMP_NR_LOCKS ((unsigned int)BIT(MEMCOMP_LOCK_BITS))

/* Slot layout and effective payload states are defined by storage headers. */

enum memcomp_payload_mode {
	MEMCOMP_PAYLOAD_BEST_EFFORT,
	MEMCOMP_PAYLOAD_MUST_SUCCEED,
};

enum memcomp_comp_ctx_miss {
	MEMCOMP_COMP_CTX_AVAILABLE,
	MEMCOMP_COMP_CTX_BUDGET,
	MEMCOMP_COMP_CTX_RESERVED_FREE,
	MEMCOMP_COMP_CTX_BORROW_LIMIT,
	MEMCOMP_COMP_CTX_BACKGROUND_WAITERS,
	MEMCOMP_COMP_CTX_ALL_BUSY,
};

enum memcomp_compress_timing_outcome {
	MEMCOMP_COMPRESS_TIMING_COMPRESSIBLE,
	MEMCOMP_COMPRESS_TIMING_RAW_HUGE,
	MEMCOMP_COMPRESS_TIMING_RAW_GE_PAGE,
	MEMCOMP_COMPRESS_TIMING_ERROR,
	MEMCOMP_COMPRESS_TIMING_NR,
};

struct memcomp_ctx_timing_record {
	u64 timing_start_ns;
	u64 timing_lease_end_ns;
	u64 timing_store_start_ns;
	u64 timing_store_end_ns;
	u64 timing_compress_ns;
	u64 timing_compress_cpu_ns;
	u64 timing_compress_call_cpu_max_ns;
	u64 timing_compress_outcome_ns[MEMCOMP_COMPRESS_TIMING_NR];
	u64 timing_compress_outcome_cpu_ns[MEMCOMP_COMPRESS_TIMING_NR];
	u64 timing_zs_ns;
	u32 timing_compress_outcome_calls[MEMCOMP_COMPRESS_TIMING_NR];
	u32 timing_pages;
	enum memcomp_reclaim_source timing_source;
	bool timing_active;
	bool timing_store_success;
};

/*
 * A direct single-page store writes into a detached buffer so its compression
 * context can be published before reenabling preemption and beginning zsmalloc pool
 * work.  Slots are preallocated and never waited on from reclaim.
 */
struct memcomp_direct_staging {
	unsigned char *dst;
	struct memcomp_ctx_timing_record timing;
};

/* Borrowed under the locked source folio's OWNER; never put this carrier. */
struct mc16_zero_carrier {
	struct memcomp_ref ref;
	unsigned long token;
	bool valid;
	bool ready;
};

struct memcomp_store_ctx {
	struct memcomp_direct_staging *staging;
	/* The span's anon_vma reference, for the caller to drop once stored. */
	struct anon_vma *anon_vma;
	struct mc16_zero_carrier zero;
};

/*
 * Hashed payload locks.  One entry's state and payload are protected by
 * the mutex its token hashes to.  Never hold two token locks at once.
 */
static struct mutex memcomp_locks[MEMCOMP_NR_LOCKS];

static inline struct mutex *memcomp_lock_of(u32 token)
{
	return &memcomp_locks[hash_32(token, MEMCOMP_LOCK_BITS)];
}

/*
 * Move an entry between the states documented above.
 *
 * @state and @flags are written under the token lock, and lockdep cannot infer
 * that on its own - it reports a lock taken in the wrong order, not one that
 * was never taken.  Routing every transition through here turns the invariant
 * into something CONFIG_PROVE_LOCKING checks on every store.
 */

/*
 * Keep the cold compressed stream local to its MGLRU source node.  MGLRU
 * feeds folios to memcomp from oldest to youngest; insertion and a fault
 * both put an entry at the hot end, so the tail remains the coldest safe
 * candidate for swap writeback on that node.
 */
struct memcomp_lru {
	spinlock_t lock; /* protects entries and on_lru */
	struct list_head entries;
	atomic_long_t nr_entries;
};

static struct memcomp_lru memcomp_lrus[MAX_NUMNODES];

static struct shrinker *memcomp_meta_shrinker;

static struct zs_pool *memcomp_pools[MEMCOMP_NR_POOLS];
static int memcomp_nr_pools;

/* Independently leased compression contexts for the store/refresh paths. */
struct memcomp_reclaim_ctx {
	/* Serializes compression context use. */
	struct mutex lock;
	struct crypto_comp *tfm;
	unsigned char *dst;
	struct memcomp_ctx_timing_record timing;
	u32 timing_seq[MEMCOMP_RECLAIM_SOURCE_NR];
	bool borrowed;
	bool background;
	bool direct_staging;
};

enum memcomp_ctx_timing_kind {
	MEMCOMP_CTX_TIMING_DIRECT,
	MEMCOMP_CTX_TIMING_BACKGROUND,
	MEMCOMP_CTX_TIMING_NR,
};

struct memcomp_compress_timing_stats {
	atomic64_t calls;
	atomic64_t wall_ns;
	atomic64_t cpu_ns;
};

struct memcomp_compress_timing_stamp {
	u64 wall_ns;
	u64 cpu_ns;
};

struct memcomp_ctx_timing_stats {
	atomic64_t samples;
	atomic64_t pages;
	atomic64_t store_samples;
	atomic64_t store_success;
	atomic64_t lease_ns;
	atomic64_t lease_max_ns;
	atomic64_t pre_store_ns;
	atomic64_t pre_store_max_ns;
	atomic64_t compress_ns;
	atomic64_t compress_max_ns;
	atomic64_t compress_cpu_ns;
	atomic64_t compress_call_cpu_max_ns;
	atomic64_t compress_store_success_ns;
	atomic64_t compress_store_success_cpu_ns;
	atomic64_t compress_store_fail_ns;
	atomic64_t compress_store_fail_cpu_ns;
	struct memcomp_compress_timing_stats
		compress_outcome[MEMCOMP_COMPRESS_TIMING_NR];
	atomic64_t zs_ns;
	atomic64_t zs_max_ns;
	atomic64_t store_other_ns;
	atomic64_t store_other_max_ns;
};

struct memcomp_ctx_wait_stats {
	atomic64_t events;
	atomic64_t ns;
	atomic64_t max_ns;
};

struct memcomp_comp_budget {
	unsigned int active;
	unsigned int background_active;
	unsigned int peak;
	unsigned int background_peak;
};

static struct memcomp_reclaim_ctx
	memcomp_comp_ctxs[MEMCOMP_MAX_COMP_CTXS];
/* Each element is protected by the corresponding compression context lock. */
static struct memcomp_direct_staging
	memcomp_direct_staging[MEMCOMP_MAX_COMP_CTXS];
static unsigned int memcomp_nr_comp_ctxs;
static unsigned int memcomp_nr_direct_staging;
static unsigned int memcomp_nr_reserved_comp_ctxs;
static unsigned int memcomp_nr_borrowable_comp_ctxs;
static DEFINE_SPINLOCK(memcomp_comp_ctx_lock);
static unsigned long memcomp_comp_ctx_free;
static unsigned long memcomp_comp_ctx_all;
static unsigned long memcomp_comp_ctx_foreground;
static unsigned long memcomp_comp_ctx_borrowed;
static unsigned int memcomp_comp_ctx_borrowed_active;
static unsigned int memcomp_comp_ctx_borrowed_peak;
static unsigned int memcomp_comp_ctx_waiters;
static struct memcomp_comp_budget memcomp_comp_budget;
static DECLARE_WAIT_QUEUE_HEAD(memcomp_comp_ctx_wait);
static DEFINE_SPINLOCK(memcomp_direct_staging_lock);
static unsigned long memcomp_direct_staging_free;
static unsigned int memcomp_direct_staging_active;
static unsigned int memcomp_direct_staging_peak;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static atomic64_t memcomp_comp_ctx_waits = ATOMIC64_INIT(0);
static atomic64_t memcomp_comp_ctx_wait_ns = ATOMIC64_INIT(0);
static atomic64_t memcomp_comp_ctx_wait_max_ns = ATOMIC64_INIT(0);
static struct memcomp_ctx_timing_stats
	memcomp_ctx_timing[MEMCOMP_CTX_TIMING_NR];
static struct memcomp_ctx_timing_stats
	memcomp_ctx_source_timing[MEMCOMP_RECLAIM_SOURCE_NR];
static struct memcomp_ctx_wait_stats
	memcomp_ctx_source_wait[MEMCOMP_RECLAIM_SOURCE_NR];
static atomic64_t
	memcomp_comp_budget_waits[MEMCOMP_RECLAIM_SOURCE_NR];
#endif

/* Per-CPU decompression contexts for the page-fault load hot path. */
struct memcomp_decomp_ctx {
	local_lock_t lock;
	struct crypto_comp *tfm;
};

static DEFINE_PER_CPU(struct memcomp_decomp_ctx, memcomp_decomp_ctxs) = {
	.lock = INIT_LOCAL_LOCK(lock),
};

static bool memcomp_enabled;
static char memcomp_compressor[MEMCOMP_COMPRESSOR_NAME_LEN] =
	MEMCOMP_DEFAULT_COMPRESSOR;
/* Serializes synchronous compressor selection. */
static DEFINE_MUTEX(memcomp_select_lock);

static bool memcomp_is_enabled(void)
{
	/* Pairs with the init-time store publishing all backing resources. */
	return smp_load_acquire(&memcomp_enabled);
}

static int __init memcomp_compressor_setup(char *str)
{
	if (!str || !*str)
		return 0;

	strscpy(memcomp_compressor, str, sizeof(memcomp_compressor));
	return 1;
}
__setup("memcomp.compressor=", memcomp_compressor_setup);

/* Bytes represented by live memcomp entries (pre-compression payload). */
static atomic64_t memcomp_stored_bytes = ATOMIC64_INIT(0);
/* Compressed bytes represented by live memcomp entries. */
static atomic64_t memcomp_compressed_bytes = ATOMIC64_INIT(0);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
/* Keep debug-only selection diagnostics off a globally contended cacheline. */
enum memcomp_placement_stat {
	MEMCOMP_PLACEMENT_STAT_COMPRESS,
	MEMCOMP_PLACEMENT_STAT_NORMAL_SWAP_FALLBACK,
	MEMCOMP_PLACEMENT_STAT_NR,
};

struct memcomp_placement_stats {
	unsigned long count[MEMCOMP_PLACEMENT_STAT_NR];
};

static DEFINE_PER_CPU(struct memcomp_placement_stats,
			      memcomp_placement_stats);

/* Cumulative hot-path events used to diagnose reclaim capacity. */
static atomic64_t memcomp_ctx_busy = ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_busy_reserved_free_pages =
	ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_busy_all_busy_pages =
	ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_borrow_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_borrow_limit_pages =
	ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_borrow_waiter_pages =
	ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_budget_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_budget_reject = ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_budget_reject_pages =
	ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_early_reject = ATOMIC64_INIT(0);
static atomic64_t memcomp_reclaim_ctx_early_reject_pages =
	ATOMIC64_INIT(0);
static atomic64_t memcomp_direct_staging_miss = ATOMIC64_INIT(0);
static atomic64_t memcomp_direct_staging_detach = ATOMIC64_INIT(0);
static atomic64_t memcomp_compress_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_zs_fast_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_zs_retries = ATOMIC64_INIT(0);
static atomic64_t memcomp_zs_retry_success = ATOMIC64_INIT(0);
static atomic64_t memcomp_zs_retry_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_zs_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_zs_other_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_raw_store = ATOMIC64_INIT(0);
static atomic64_t memcomp_raw_candidate_huge = ATOMIC64_INIT(0);
static atomic64_t memcomp_raw_candidate_ge_page = ATOMIC64_INIT(0);
static atomic64_t memcomp_raw_reject_huge = ATOMIC64_INIT(0);
static atomic64_t memcomp_raw_reject_ge_page = ATOMIC64_INIT(0);
static atomic64_t memcomp_raw_reject_zs_fallback = ATOMIC64_INIT(0);
static atomic64_t memcomp_raw_reject_net = ATOMIC64_INIT(0);
static atomic64_t memcomp_reserve_entry_alloc_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_refresh_state_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_refresh_payload_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_anon_unmap_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_refresh_preflight_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_refresh_folio_busy_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_refresh_arch_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_refresh_state_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_refresh_payload_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_anon_finish_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_incompressible_marked_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_incompressible_changed_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_incompressible_folio_skip_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_shmem_store_ok_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_shmem_store_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_shmem_eligibility_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_shmem_payload_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_shmem_arch_fail_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_pending_pages = ATOMIC64_INIT(0);
/*
 * Rolling a span back once its tokens are in PTEs: the spans whose folio was
 * mapped back, the PTEs that took, the pages a PTE still named after it, and
 * the walks made again for those.
 */
static atomic64_t memcomp_restore_spans = ATOMIC64_INIT(0);
static atomic64_t memcomp_restore_ptes = ATOMIC64_INIT(0);
static atomic64_t memcomp_restore_missed_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_restore_rewalks = ATOMIC64_INIT(0);
static atomic64_t memcomp_restore_ns = ATOMIC64_INIT(0);
static atomic64_t memcomp_restore_max_ns = ATOMIC64_INIT(0);
static atomic64_t memcomp_split_refused = ATOMIC64_INIT(0);
static atomic64_t memcomp_split_refused_pages = ATOMIC64_INIT(0);
static atomic64_t memcomp_bg_store_ok = ATOMIC64_INIT(0);
static atomic64_t memcomp_bg_store_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_direct_store_ok = ATOMIC64_INIT(0);
static atomic64_t memcomp_direct_store_fail = ATOMIC64_INIT(0);
static atomic64_t memcomp_pageout_store_ok = ATOMIC64_INIT(0);
static atomic64_t memcomp_pageout_store_fail = ATOMIC64_INIT(0);
#endif
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static atomic64_t memcomp_swapped_read_io = ATOMIC64_INIT(0);
static atomic64_t memcomp_swapped_read_error = ATOMIC64_INIT(0);
static atomic64_t memcomp_swapped_read_cached = ATOMIC64_INIT(0);
static atomic_long_t memcomp_swapped_entries = ATOMIC_LONG_INIT(0);
static atomic64_t memcomp_swapoff_entries = ATOMIC64_INIT(0);
static atomic64_t memcomp_swapoff_read_io = ATOMIC64_INIT(0);
static atomic64_t memcomp_swapoff_fail = ATOMIC64_INIT(0);
#endif

/*
 * Current entry state gauges.  Updates stay CPU-local because every
 * stored base page otherwise adds several contended global atomics to the
 * reclaim path.  A decrement need not run on the CPU which incremented it,
 * so the per-CPU fields must remain signed; only their global sum is useful.
 */
struct memcomp_state_stats {
	s64 folio_owner_pages;
	s64 samefill_pages;
	s64 raw_pages;
	s64 shmem_pages;
};

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static DEFINE_PER_CPU(struct memcomp_state_stats,
			      memcomp_state_stats);

static void
memcomp_state_add(struct memcomp_state_stats *snapshot,
		  const struct memcomp_state_stats *stats)
{
	snapshot->folio_owner_pages += READ_ONCE(stats->folio_owner_pages);
	snapshot->samefill_pages += READ_ONCE(stats->samefill_pages);
	snapshot->raw_pages += READ_ONCE(stats->raw_pages);
	snapshot->shmem_pages += READ_ONCE(stats->shmem_pages);
}

static void
memcomp_state_clamp(struct memcomp_state_stats *snapshot)
{
	snapshot->folio_owner_pages =
		max_t(s64, 0, snapshot->folio_owner_pages);
	snapshot->samefill_pages = max_t(s64, 0, snapshot->samefill_pages);
	snapshot->raw_pages = max_t(s64, 0, snapshot->raw_pages);
	snapshot->shmem_pages = max_t(s64, 0, snapshot->shmem_pages);
}

static void
memcomp_state_read(struct memcomp_state_stats *snapshot)
{
	int cpu;

	memset(snapshot, 0, sizeof(*snapshot));
	for_each_possible_cpu(cpu) {
		struct memcomp_state_stats *stats;

		stats = per_cpu_ptr(&memcomp_state_stats, cpu);
		memcomp_state_add(snapshot, stats);
	}

	/* Cross-CPU reads are not transactional with concurrent migration. */
	memcomp_state_clamp(snapshot);
}

static void memcomp_state_drop_owner(void)
{
	this_cpu_dec(memcomp_state_stats.folio_owner_pages);
}

/* Caller serializes the payload fields with the token lock. */

#else
static void memcomp_state_drop_owner(void)
{
}

#endif

enum memcomp_anon_store_fail {
	MEMCOMP_ANON_STORE_PREFLIGHT,
	MEMCOMP_ANON_STORE_FOLIO_BUSY,
	MEMCOMP_ANON_STORE_ARCH,
	MEMCOMP_ANON_STORE_REFRESH,
};

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static void memcomp_account_anon_store(bool success,
				       unsigned int nr_pages)
{
	atomic64_t *counter;

	if (current_is_kswapd())
		counter = success ? &memcomp_bg_store_ok :
				    &memcomp_bg_store_fail;
	else if (current->reclaim_state)
		counter = success ? &memcomp_direct_store_ok :
				    &memcomp_direct_store_fail;
	else
		counter = success ? &memcomp_pageout_store_ok :
				    &memcomp_pageout_store_fail;

	atomic64_add(nr_pages, counter);
}

static void
memcomp_account_anon_store_fail(enum memcomp_anon_store_fail failure,
				unsigned int nr_pages)
{
	switch (failure) {
	case MEMCOMP_ANON_STORE_PREFLIGHT:
		atomic64_add(nr_pages,
			     &memcomp_refresh_preflight_fail_pages);
		break;
	case MEMCOMP_ANON_STORE_FOLIO_BUSY:
		atomic64_add(nr_pages,
			     &memcomp_refresh_folio_busy_fail_pages);
		break;
	case MEMCOMP_ANON_STORE_ARCH:
		atomic64_add(nr_pages, &memcomp_refresh_arch_fail_pages);
		break;
	case MEMCOMP_ANON_STORE_REFRESH:
		/* memcomp_do_refresh() accounts its exclusive reason. */
		break;
	}
}

void memcomp_count_anon_fail(enum memcomp_anon_fail_event event,
			     unsigned int nr_pages)
{
	switch (event) {
	case MEMCOMP_ANON_UNMAP_FAIL:
		atomic64_add(nr_pages, &memcomp_anon_unmap_fail_pages);
		break;
	case MEMCOMP_ANON_FINISH_FAIL:
		atomic64_add(nr_pages, &memcomp_anon_finish_fail_pages);
		break;
	case MEMCOMP_ANON_INCOMPRESSIBLE_SKIP:
		atomic64_add(nr_pages,
			     &memcomp_incompressible_folio_skip_pages);
		break;
	}
}

#else
static void memcomp_account_anon_store(bool success,
				       unsigned int nr_pages)
{
}

static void
memcomp_account_anon_store_fail(enum memcomp_anon_store_fail failure,
				unsigned int nr_pages)
{
}
#endif

static inline void memcomp_stored_bytes_inc(void)
{
	atomic64_add(PAGE_SIZE, &memcomp_stored_bytes);
}

static inline void memcomp_stored_bytes_dec(void)
{
	atomic64_sub(PAGE_SIZE, &memcomp_stored_bytes);
}

static inline void memcomp_compressed_bytes_add(unsigned int bytes)
{
	atomic64_add(bytes, &memcomp_compressed_bytes);
}

static inline void memcomp_compressed_bytes_sub(unsigned int bytes)
{
	atomic64_sub(bytes, &memcomp_compressed_bytes);
}

static void memcomp_arch_restore(unsigned long token, struct folio *folio)
{
	arch_swap_restore(make_memcomp_entry(token), folio);
}

/* "Payload can satisfy a fault without swap IO". */

static void memcomp_zs_free(u8 pool_idx, unsigned long handle)
{
	zs_free(memcomp_pools[pool_idx], handle);
}

static struct obj_cgroup *memcomp_objcg_get(struct folio *folio)
{
	return get_obj_cgroup_from_folio(folio);
}

/* Hold an additional metadata owner without bypassing the test model. */
static __maybe_unused void memcomp_objcg_get_ref(struct obj_cgroup *objcg)
{
#ifdef CONFIG_MEMCG
	if (objcg)
		obj_cgroup_get(objcg);
#endif
}

static void memcomp_objcg_put(struct obj_cgroup *objcg)
{
	obj_cgroup_put(objcg);
}

static int memcomp_objcg_charge(struct obj_cgroup *objcg, gfp_t gfp,
				size_t size)
{
#if defined(CONFIG_MEMCG)
	if (!objcg || !size)
		return 0;

	return obj_cgroup_charge(objcg, gfp, size);
#else
	return 0;
#endif
}

static void memcomp_objcg_uncharge(struct obj_cgroup *objcg, size_t size)
{
#if defined(CONFIG_MEMCG)
	if (!objcg || !size)
		return;
	obj_cgroup_uncharge(objcg, size);
#endif
}

#include "memcomp/format.h"
#include "memcomp/zero.h"
#include "memcomp/lifetime.h"
#include "memcomp/storage.h"

#ifdef CONFIG_SWAP
/* An entry owns one reference on its raw or packed backing slot. */

/*
 * Take and drop a slot's reference count outside swap core.  These exist
 * as named steps because the KUnit suite builds entries with synthetic
 * slots and must not touch swap_info[] - under UML there is no device, so
 * the real calls would log errors on every staged case.  The suite
 * compiles them to tallies instead, so a case can still assert that a
 * release or a restore handed its slot reference back; the production
 * build keeps the direct calls.
 */

static void memcomp_slot_put(swp_entry_t swap)
{
	swap_free(swap);
}

/*
 * Drop an entry's reference on a packed slot.  A sibling's fault may have
 * left the slot's page in the swap cache for the others - see
 * memcomp_read_swapped_page() - so the last entry to go takes that page
 * with it rather than leaving the slot pinned until reclaim finds it.  The
 * page is only trylocked, so this stays safe wherever a put is.
 */
static void memcomp_slot_put_packed(swp_entry_t swap)
{
	free_swap_and_cache(swap);
}

/* References on the slot, of entries and in-flight faults; racy by nature. */

/*
 * Detach a swap-backed entry from its slot.  Caller holds the token lock
 * and must memcomp_slot_put() the returned entry after unlocking.
 */

/* Release the slot of a dying swap-backed entry.  Atomic-context safe. */

#else /* !CONFIG_SWAP */

#endif /* CONFIG_SWAP */

#include "memcomp/access.h"
#include "memcomp/fifo.h"
#include "memcomp/refs.h"
#include "memcomp/owner.h"
#include "memcomp/span.h"
#include "memcomp/migrate.h"
#include "memcomp/query.h"
#include "memcomp/scan.h"
#include "memcomp/payload.h"

/* Caller holds the token lock. */

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
void memcomp_count_split_refused(struct folio *folio)
{
	atomic64_inc(&memcomp_split_refused);
	atomic64_add(folio_nr_pages(folio), &memcomp_split_refused_pages);
}
#endif

/* Drop the caller's temporary lookup plus one long-lived owner/PTE ref. */

/* Cache and PTE owners have independent zero-token accounting. */

/*
 * Clear @flag on @head, the first entry of the span reserved for the locked
 * @folio, and return whether it was set.
 */

/*
 * Take over the reference memcomp_reserve() took on the anon_vma of the
 * locked @folio, if it took one, for the span @head is the first entry of.
 */

/*
 * Drop the owner-folio reference of @entry if @folio still owns it.
 * Consumes the caller's lookup reference either way.
 */

/*
 * Take a stored folio off its entries, unless it no longer names any or
 * someone else holds it.  On success it is frozen at no references, ready to
 * be freed.
 */

/* Drop the PTE references of held @entries, which a fault replaced. */

/*
 * What the @nr_pages entries from @token are to @folio, finding each once:
 * MC_SPAN_PENDING if it owns them all as the span they were reserved as and
 * they are all still pending, MC_SPAN_STORED if it owns them all and none is,
 * MC_SPAN_NONE for anything else.  With @entries, a span found pending or
 * stored is left there with a reference on each entry for the caller to drop,
 * so that it need not look them up again.
 */

/* Pin while the caller's PTL or page-cache lookup still names this token. */

/* Consume the caller-owned guard before its final put can free storage. */

static u64 memcomp_pool_total_size(void)
{
	u64 total = 0;
	int i;

	if (!memcomp_is_enabled())
		return 0;

	for (i = 0; i < memcomp_nr_pools; i++)
		total += (zs_get_total_pages(memcomp_pools[i]) << PAGE_SHIFT);

	return total;
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static unsigned int memcomp_pool_huge_class_size(void)
{
	if (!memcomp_is_enabled() || !memcomp_nr_pools)
		return 0;

	return zs_huge_class_size(memcomp_pools[0]);
}

#endif

static u8 memcomp_select_pool_idx(void)
{
	return raw_smp_processor_id() % memcomp_nr_pools;
}

bool memcomp_available(void)
{
	return memcomp_is_enabled() &&
	       READ_ONCE(memcomp_nr_comp_ctxs);
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static void memcomp_placement_stat_inc(enum memcomp_placement_stat stat)
{
	this_cpu_inc(memcomp_placement_stats.count[stat]);
}

static u64 memcomp_placement_stat_read(enum memcomp_placement_stat stat)
{
	u64 total = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct memcomp_placement_stats *stats;

		stats = per_cpu_ptr(&memcomp_placement_stats, cpu);
		total += READ_ONCE(stats->count[stat]);
	}

	return total;
}

void memcomp_count_initial_compress(void)
{
	memcomp_placement_stat_inc(MEMCOMP_PLACEMENT_STAT_COMPRESS);
}

void memcomp_count_normal_swap_fallback(void)
{
	enum memcomp_placement_stat stat =
		MEMCOMP_PLACEMENT_STAT_NORMAL_SWAP_FALLBACK;

	memcomp_placement_stat_inc(stat);
}
#endif

static bool memcomp_can_retry_alloc(void)
{
	/* PF_MEMALLOC prevents recursive reclaim and permits net progress. */
	return current_is_kswapd() &&
	       (current->flags & (PF_MEMALLOC | PF_KTHREAD)) ==
		       (PF_MEMALLOC | PF_KTHREAD);
}

static bool
memcomp_comp_ctx_background(enum memcomp_payload_mode mode,
			    enum memcomp_reclaim_source source,
				bool can_retry)
{
	return mode != MEMCOMP_PAYLOAD_BEST_EFFORT &&
	       source != MEMCOMP_RECLAIM_DIRECT && can_retry;
}

/*
 * Drivers that compress entirely within the workspace allocated with their
 * transform, so a lease can be carried into a preempt-disabled staging
 * region, and whose output is the plain zstd frame the entropy shadow
 * predictor is calibrated against.
 */
static bool memcomp_driver_is_zstd(const char *driver)
{
	return !strcmp(driver, "zstd-generic");
}

static bool
memcomp_staging_policy(enum memcomp_payload_mode mode,
		       enum memcomp_reclaim_source source,
			   unsigned int nr_pages, const char *driver)
{
	return !IS_ENABLED(CONFIG_PREEMPT_RT) &&
	       mode == MEMCOMP_PAYLOAD_MUST_SUCCEED &&
	       source == MEMCOMP_RECLAIM_DIRECT && nr_pages == 1 &&
	       memcomp_driver_is_zstd(driver);
}

/* Reserve a staging buffer for direct-reclaim compression. */
static struct memcomp_direct_staging *
memcomp_direct_staging_try_get(void)
{
	struct memcomp_direct_staging *staging = NULL;
	unsigned int nr_staging;
	unsigned long flags;
	unsigned int idx;

	nr_staging = READ_ONCE(memcomp_nr_direct_staging);
	spin_lock_irqsave(&memcomp_direct_staging_lock, flags);
	idx = find_first_bit(&memcomp_direct_staging_free, nr_staging);
	if (idx < nr_staging) {
		__clear_bit(idx, &memcomp_direct_staging_free);
		memcomp_direct_staging_active++;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		memcomp_direct_staging_peak =
			max(memcomp_direct_staging_peak,
			    memcomp_direct_staging_active);
#endif
		staging = &memcomp_direct_staging[idx];
	}
	spin_unlock_irqrestore(&memcomp_direct_staging_lock, flags);
	if (!staging)
		memcomp_debug_inc(&memcomp_direct_staging_miss);
	return staging;
}

static void
memcomp_direct_staging_put(struct memcomp_direct_staging *staging)
{
	unsigned long flags;
	unsigned int idx;

	if (!staging)
		return;
	idx = staging - memcomp_direct_staging;
	if (WARN_ON_ONCE(idx >= memcomp_nr_direct_staging))
		return;
	staging->timing.timing_active = false;
	spin_lock_irqsave(&memcomp_direct_staging_lock, flags);
	if (WARN_ON_ONCE(test_bit(idx, &memcomp_direct_staging_free))) {
		spin_unlock_irqrestore(&memcomp_direct_staging_lock, flags);
		return;
	}
	if (!WARN_ON_ONCE(!memcomp_direct_staging_active))
		memcomp_direct_staging_active--;
	__set_bit(idx, &memcomp_direct_staging_free);
	spin_unlock_irqrestore(&memcomp_direct_staging_lock, flags);
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static bool memcomp_ctx_timing_should_sample(u32 *seq)
{
	(*seq)++;
	return !(*seq & MEMCOMP_CTX_TIMING_SAMPLE_MASK);
}

static u32 memcomp_ctx_timing_seed(unsigned int index)
{
	/* The odd multiplier permutes the power-of-two sampling interval. */
	return index * 0x9e3779b9U & MEMCOMP_CTX_TIMING_SAMPLE_MASK;
}

static u64 memcomp_ctx_timing_remainder(u64 total, u64 measured)
{
	if (measured >= total)
		return 0;
	return total - measured;
}

static enum memcomp_ctx_timing_kind
memcomp_ctx_timing_aggregate_kind(enum memcomp_reclaim_source source)
{
	return source == MEMCOMP_RECLAIM_DIRECT ?
		MEMCOMP_CTX_TIMING_DIRECT :
		MEMCOMP_CTX_TIMING_BACKGROUND;
}

static void memcomp_atomic64_update_max(atomic64_t *counter, u64 value)
{
	s64 old = atomic64_read(counter);
	s64 new = min_t(u64, value, S64_MAX);

	while (new > old &&
	       !atomic64_try_cmpxchg_relaxed(counter, &old, new))
		;
}

static void
memcomp_ctx_timing_wait_start(enum memcomp_reclaim_source source)
{
	atomic64_inc(&memcomp_comp_ctx_waits);
	atomic64_inc(&memcomp_ctx_source_wait[source].events);
}

static void
memcomp_ctx_timing_wait_end(u64 start_ns,
			    enum memcomp_reclaim_source source)
{
	struct memcomp_ctx_wait_stats *stats;
	u64 delta = ktime_get_ns() - start_ns;

	atomic64_add(delta, &memcomp_comp_ctx_wait_ns);
	memcomp_atomic64_update_max(&memcomp_comp_ctx_wait_max_ns, delta);
	stats = &memcomp_ctx_source_wait[source];
	atomic64_add(delta, &stats->ns);
	memcomp_atomic64_update_max(&stats->max_ns, delta);
}

static void memcomp_ctx_timing_start(struct memcomp_reclaim_ctx *ctx,
				     unsigned int nr_pages,
					 enum memcomp_reclaim_source source)
{
	struct memcomp_ctx_timing_record *timing = &ctx->timing;

	timing->timing_active = false;
	if (WARN_ON_ONCE((unsigned int)source >=
			 MEMCOMP_RECLAIM_SOURCE_NR))
		source = MEMCOMP_RECLAIM_DIRECT;
	if (!memcomp_ctx_timing_should_sample(&ctx->timing_seq[source]))
		return;

	timing->timing_store_start_ns = 0;
	timing->timing_store_end_ns = 0;
	timing->timing_lease_end_ns = 0;
	timing->timing_compress_ns = 0;
	timing->timing_compress_cpu_ns = 0;
	timing->timing_compress_call_cpu_max_ns = 0;
	memset(timing->timing_compress_outcome_ns, 0,
	       sizeof(timing->timing_compress_outcome_ns));
	memset(timing->timing_compress_outcome_cpu_ns, 0,
	       sizeof(timing->timing_compress_outcome_cpu_ns));
	memset(timing->timing_compress_outcome_calls, 0,
	       sizeof(timing->timing_compress_outcome_calls));
	timing->timing_zs_ns = 0;
	timing->timing_pages = nr_pages;
	timing->timing_source = source;
	timing->timing_store_success = false;
	timing->timing_start_ns = ktime_get_ns();
	timing->timing_active = true;
}

static __always_inline u64
memcomp_ctx_timing_phase_start(struct memcomp_ctx_timing_record *timing)
{
	if (likely(!timing->timing_active))
		return 0;
	return ktime_get_ns();
}

static __always_inline void
memcomp_ctx_timing_phase_end(struct memcomp_ctx_timing_record *timing,
			     u64 start_ns, u64 *total_ns)
{
	if (unlikely(timing->timing_active))
		*total_ns += ktime_get_ns() - start_ns;
}

static __always_inline struct memcomp_compress_timing_stamp
memcomp_compress_timing_start(struct memcomp_ctx_timing_record *timing)
{
	struct memcomp_compress_timing_stamp stamp = {};

	if (likely(!timing->timing_active))
		return stamp;
	stamp.wall_ns = ktime_get_ns();
	stamp.cpu_ns = task_sched_runtime(current);
	return stamp;
}

static __always_inline struct memcomp_compress_timing_stamp
memcomp_compress_timing_end(struct memcomp_ctx_timing_record *timing,
			    struct memcomp_compress_timing_stamp start)
{
	struct memcomp_compress_timing_stamp delta = {};
	u64 cpu_ns;

	if (likely(!timing->timing_active))
		return delta;
	cpu_ns = task_sched_runtime(current);
	delta.wall_ns = ktime_get_ns() - start.wall_ns;
	delta.cpu_ns = cpu_ns - start.cpu_ns;
	timing->timing_compress_ns += delta.wall_ns;
	timing->timing_compress_cpu_ns += delta.cpu_ns;
	return delta;
}

static __always_inline void
memcomp_compress_timing_account(struct memcomp_ctx_timing_record *timing,
				struct memcomp_compress_timing_stamp delta,
				    enum memcomp_compress_timing_outcome outcome)
{
	if (likely(!timing->timing_active))
		return;
	timing->timing_compress_outcome_calls[outcome]++;
	timing->timing_compress_outcome_ns[outcome] += delta.wall_ns;
	timing->timing_compress_outcome_cpu_ns[outcome] += delta.cpu_ns;
	timing->timing_compress_call_cpu_max_ns =
		max(timing->timing_compress_call_cpu_max_ns, delta.cpu_ns);
}

static void
memcomp_ctx_timing_store_start(struct memcomp_ctx_timing_record *timing)
{
	if (timing->timing_active)
		timing->timing_store_start_ns = ktime_get_ns();
}

static void
memcomp_ctx_timing_store_end(struct memcomp_ctx_timing_record *timing,
			     bool success)
{
	if (!timing->timing_active)
		return;
	timing->timing_store_end_ns = ktime_get_ns();
	timing->timing_store_success = success;
}

static void
memcomp_ctx_timing_account_bucket(struct memcomp_ctx_timing_record *timing,
				  struct memcomp_ctx_timing_stats *stats,
				      u64 lease_ns, u64 pre_store_ns,
				      u64 store_other_ns)
{
	unsigned int i;

	atomic64_inc(&stats->samples);
	atomic64_add(timing->timing_pages, &stats->pages);
	if (timing->timing_store_start_ns)
		atomic64_inc(&stats->store_samples);
	if (timing->timing_store_success)
		atomic64_inc(&stats->store_success);
	atomic64_add(lease_ns, &stats->lease_ns);
	memcomp_atomic64_update_max(&stats->lease_max_ns, lease_ns);
	atomic64_add(pre_store_ns, &stats->pre_store_ns);
	memcomp_atomic64_update_max(&stats->pre_store_max_ns, pre_store_ns);
	atomic64_add(timing->timing_compress_ns, &stats->compress_ns);
	memcomp_atomic64_update_max(&stats->compress_max_ns,
				    timing->timing_compress_ns);
	atomic64_add(timing->timing_compress_cpu_ns, &stats->compress_cpu_ns);
	memcomp_atomic64_update_max(&stats->compress_call_cpu_max_ns,
				    timing->timing_compress_call_cpu_max_ns);
	if (timing->timing_store_success) {
		atomic64_add(timing->timing_compress_ns,
			     &stats->compress_store_success_ns);
		atomic64_add(timing->timing_compress_cpu_ns,
			     &stats->compress_store_success_cpu_ns);
	} else {
		atomic64_add(timing->timing_compress_ns,
			     &stats->compress_store_fail_ns);
		atomic64_add(timing->timing_compress_cpu_ns,
			     &stats->compress_store_fail_cpu_ns);
	}
	for (i = 0; i < MEMCOMP_COMPRESS_TIMING_NR; i++) {
		struct memcomp_compress_timing_stats *outcome;

		if (!timing->timing_compress_outcome_calls[i])
			continue;
		outcome = &stats->compress_outcome[i];
		atomic64_add(timing->timing_compress_outcome_calls[i],
			     &outcome->calls);
		atomic64_add(timing->timing_compress_outcome_ns[i],
			     &outcome->wall_ns);
		atomic64_add(timing->timing_compress_outcome_cpu_ns[i],
			     &outcome->cpu_ns);
	}
	atomic64_add(timing->timing_zs_ns, &stats->zs_ns);
	memcomp_atomic64_update_max(&stats->zs_max_ns,
				    timing->timing_zs_ns);
	atomic64_add(store_other_ns, &stats->store_other_ns);
	memcomp_atomic64_update_max(&stats->store_other_max_ns, store_other_ns);
}

static void
memcomp_ctx_timing_account(struct memcomp_ctx_timing_record *timing)
{
	struct memcomp_ctx_timing_stats *stats;
	enum memcomp_ctx_timing_kind kind;
	u64 measured_store_ns;
	u64 store_other_ns = 0;
	u64 pre_store_ns;
	u64 lease_ns;
	u64 now_ns;

	if (!timing->timing_active)
		return;

	now_ns = timing->timing_lease_end_ns ?: ktime_get_ns();
	lease_ns = now_ns - timing->timing_start_ns;
	if (timing->timing_store_start_ns) {
		u64 store_end_ns = timing->timing_store_end_ns ?: now_ns;
		u64 store_ns = store_end_ns - timing->timing_store_start_ns;

		pre_store_ns = timing->timing_store_start_ns -
				timing->timing_start_ns;
		measured_store_ns = timing->timing_compress_ns +
				    timing->timing_zs_ns;
		store_other_ns =
			memcomp_ctx_timing_remainder(store_ns,
						     measured_store_ns);
	} else {
		/* The lease ended while unmapping or validating the folio. */
		pre_store_ns = lease_ns;
	}

	kind = memcomp_ctx_timing_aggregate_kind(timing->timing_source);
	stats = &memcomp_ctx_timing[kind];
	memcomp_ctx_timing_account_bucket(timing, stats, lease_ns,
					  pre_store_ns, store_other_ns);
	stats = &memcomp_ctx_source_timing[timing->timing_source];
	memcomp_ctx_timing_account_bucket(timing, stats, lease_ns,
					  pre_store_ns, store_other_ns);
	timing->timing_active = false;
}
#else
static void memcomp_ctx_timing_start(struct memcomp_reclaim_ctx *ctx,
				     unsigned int nr_pages,
					 enum memcomp_reclaim_source source)
{
}

static __always_inline u64
memcomp_ctx_timing_phase_start(struct memcomp_ctx_timing_record *timing)
{
	return 0;
}

static __always_inline void
memcomp_ctx_timing_phase_end(struct memcomp_ctx_timing_record *timing,
			     u64 start_ns, u64 *total_ns)
{
}

static __always_inline struct memcomp_compress_timing_stamp
memcomp_compress_timing_start(struct memcomp_ctx_timing_record *timing)
{
	return (struct memcomp_compress_timing_stamp){};
}

static __always_inline struct memcomp_compress_timing_stamp
memcomp_compress_timing_end(struct memcomp_ctx_timing_record *timing,
			    struct memcomp_compress_timing_stamp start)
{
	return (struct memcomp_compress_timing_stamp){};
}

static __always_inline void
memcomp_compress_timing_account(struct memcomp_ctx_timing_record *timing,
				struct memcomp_compress_timing_stamp delta,
				    enum memcomp_compress_timing_outcome outcome)
{
}

static void
memcomp_ctx_timing_store_start(struct memcomp_ctx_timing_record *timing)
{
}

static void
memcomp_ctx_timing_store_end(struct memcomp_ctx_timing_record *timing,
			     bool success)
{
}

static void
memcomp_ctx_timing_account(struct memcomp_ctx_timing_record *timing)
{
}
#endif

static gfp_t memcomp_zs_gfp(bool fast)
{
	gfp_t gfp = __GFP_KSWAPD_RECLAIM | __GFP_NOWARN | __GFP_HIGHMEM |
		    __GFP_MOVABLE;

	/*
	 * Match zram's two-stage allocation policy.  Keep direct reclaim
	 * explicitly disabled for the first attempt instead of depending on
	 * the caller already having PF_MEMALLOC set.  The second attempt is
	 * restricted to background reclaim by memcomp_can_retry_alloc().
	 */
	if (!fast)
		gfp |= __GFP_DIRECT_RECLAIM;

	return gfp;
}

static int memcomp_zs_malloc(struct zs_pool *pool, unsigned int size,
			     unsigned long *handle, bool allow_slow)
{
	unsigned long obj;
	int ret;

	obj = zs_malloc(pool, size, memcomp_zs_gfp(true));
	if (!IS_ERR_VALUE(obj)) {
		*handle = obj;
		return 0;
	}
	ret = PTR_ERR((void *)obj);
	if (ret != -ENOMEM)
		memcomp_debug_inc(&memcomp_zs_other_fail);
	memcomp_debug_inc(&memcomp_zs_fast_fail);

	if (allow_slow) {
		memcomp_debug_inc(&memcomp_zs_retries);
		obj = zs_malloc(pool, size, memcomp_zs_gfp(false));
		if (IS_ERR_VALUE(obj)) {
			ret = PTR_ERR((void *)obj);
			if (ret != -ENOMEM)
				memcomp_debug_inc(&memcomp_zs_other_fail);
			memcomp_debug_inc(&memcomp_zs_retry_fail);
		} else {
			*handle = obj;
			ret = 0;
			memcomp_debug_inc(&memcomp_zs_retry_success);
		}
	}
	if (ret)
		memcomp_debug_inc(&memcomp_zs_fail);

	return ret;
}

static int
memcomp_payload_zs_malloc(struct zs_pool *pool, unsigned int size,
			  unsigned long *handle, bool allow_slow,
			  struct memcomp_store_ctx *store)
{
	return memcomp_zs_malloc(pool, size, handle, allow_slow);
}

static int memcomp_decompress_payload(const void *src,
				      unsigned int compressed_len,
					  void *dst)
{
	struct memcomp_decomp_ctx *decomp_ctx;
	unsigned int decompressed_len;
	int ret;

	if (compressed_len > PAGE_SIZE)
		return -EIO;

	if (compressed_len == PAGE_SIZE) {
		memcpy(dst, src, PAGE_SIZE);
		return 0;
	}

	local_lock(&memcomp_decomp_ctxs.lock);
	decomp_ctx = this_cpu_ptr(&memcomp_decomp_ctxs);
	decompressed_len = PAGE_SIZE;
	ret = crypto_comp_decompress(decomp_ctx->tfm, src, compressed_len,
				     dst, &decompressed_len);
	local_unlock(&memcomp_decomp_ctxs.lock);

	if (ret)
		return ret;
	if (decompressed_len != PAGE_SIZE)
		return -EIO;
	return 0;
}

/*
 * Check whether a page is entirely filled with the same unsigned long
 * value (common for zero pages and uninitialised BSS).  Borrowed from
 * zram - same-filled pages are stored with no zsmalloc allocation.
 */
static bool page_same_filled(const void *addr, unsigned long *element)
{
	const unsigned long *page = addr;
	const unsigned int last = PAGE_SIZE / sizeof(*page) - 1;
	unsigned long val = page[0];
	unsigned int pos;

	/*
	 * Probe the neighbour first, then the two ends: a page whose leading
	 * words happen to repeat - a partially written buffer, a zeroed
	 * leading field - would otherwise be walked all the way to its first
	 * mismatch before being rejected.  The words probed here are then
	 * known equal, so the loop skips them and the result is unchanged.
	 */
	if (val != page[1] || val != page[last] || val != page[last / 2])
		return false;

	for (pos = 2; pos < last; pos++) {
		if (val != page[pos])
			return false;
	}
	*element = val;
	return true;
}

static unsigned int
memcomp_comp_budget_limit(unsigned int online_cpus,
			  unsigned int nr_contexts)
{
	unsigned int limit = online_cpus > 1 ? online_cpus - 1 : 1;

	return min(limit, nr_contexts);
}

static unsigned int memcomp_current_comp_budget_limit(void)
{
	return memcomp_comp_budget_limit(num_online_cpus(),
					     memcomp_nr_comp_ctxs);
}

static bool
memcomp_comp_budget_available(const struct memcomp_comp_budget *budget,
			      unsigned int limit)
{
	return budget->background_active < limit;
}

static bool
memcomp_comp_budget_try_acquire(struct memcomp_comp_budget *budget,
				unsigned int limit)
{
	if (!memcomp_comp_budget_available(budget, limit))
		return false;

	budget->active++;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	budget->peak = max(budget->peak, budget->active);
#endif
	budget->background_active++;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	budget->background_peak = max(budget->background_peak,
				      budget->background_active);
#endif
	return true;
}

static bool
memcomp_comp_budget_release(struct memcomp_comp_budget *budget)
{
	if (!budget->active)
		return false;
	if (!budget->background_active)
		return false;
	budget->background_active--;
	budget->active--;
	return true;
}

static bool memcomp_comp_budget_available_locked(void)
{
	lockdep_assert_held(&memcomp_comp_ctx_lock);
	return memcomp_comp_budget_available(&memcomp_comp_budget,
		memcomp_current_comp_budget_limit());
}

static bool memcomp_comp_budget_get_locked(void)
{
	lockdep_assert_held(&memcomp_comp_ctx_lock);
	return memcomp_comp_budget_try_acquire(&memcomp_comp_budget,
		memcomp_current_comp_budget_limit());
}

static bool memcomp_comp_budget_put_locked(void)
{
	lockdep_assert_held(&memcomp_comp_ctx_lock);
	return memcomp_comp_budget_release(&memcomp_comp_budget);
}

static int memcomp_comp_ctx_claim(unsigned long *free_mask,
				  unsigned long eligible_mask)
{
	unsigned long available = *free_mask & eligible_mask;
	unsigned int idx;

	if (!available)
		return -EBUSY;

	idx = __ffs(available);
	__clear_bit(idx, free_mask);
	return idx;
}

static bool memcomp_comp_ctx_release(unsigned long *free_mask,
				     unsigned int idx)
{
	if (test_bit(idx, free_mask))
		return false;

	__set_bit(idx, free_mask);
	return true;
}

static enum memcomp_comp_ctx_miss
memcomp_comp_ctx_classify(unsigned long free_mask,
			  unsigned long foreground_mask,
			      unsigned long all_mask)
{
	if (free_mask & foreground_mask)
		return MEMCOMP_COMP_CTX_AVAILABLE;
	if (free_mask & all_mask & ~foreground_mask)
		return MEMCOMP_COMP_CTX_RESERVED_FREE;
	return MEMCOMP_COMP_CTX_ALL_BUSY;
}

static enum memcomp_comp_ctx_miss
memcomp_comp_ctx_borrow_decision(unsigned int borrowed,
				 unsigned int limit,
				     unsigned int waiters)
{
	if (waiters)
		return MEMCOMP_COMP_CTX_BACKGROUND_WAITERS;
	if (borrowed >= limit)
		return MEMCOMP_COMP_CTX_BORROW_LIMIT;
	return MEMCOMP_COMP_CTX_AVAILABLE;
}

static enum memcomp_comp_ctx_miss
memcomp_comp_ctx_foreground_status(unsigned long free_mask,
				   unsigned long foreground_mask,
				       unsigned long all_mask,
				       unsigned int borrowed,
				       unsigned int borrow_limit,
				       unsigned int waiters)
{
	unsigned long borrow_mask = all_mask & ~foreground_mask;

	if (free_mask & foreground_mask)
		return MEMCOMP_COMP_CTX_AVAILABLE;
	if (free_mask & borrow_mask)
		return memcomp_comp_ctx_borrow_decision(borrowed,
							    borrow_limit,
							    waiters);
	return memcomp_comp_ctx_classify(free_mask, foreground_mask,
					     all_mask);
}

static enum memcomp_comp_ctx_miss
memcomp_comp_ctx_foreground_status_locked(void)
{
	unsigned long all = memcomp_comp_ctx_all;
	unsigned long foreground = memcomp_comp_ctx_foreground;
	unsigned long free = memcomp_comp_ctx_free;
	unsigned int borrowed = memcomp_comp_ctx_borrowed_active;
	unsigned int borrow_limit = memcomp_nr_borrowable_comp_ctxs;
	unsigned int waiters = memcomp_comp_ctx_waiters;

	lockdep_assert_held(&memcomp_comp_ctx_lock);
	return memcomp_comp_ctx_foreground_status(free, foreground, all,
						      borrowed, borrow_limit,
						      waiters);
}

static void
memcomp_account_reclaim_ctx_miss(enum memcomp_comp_ctx_miss miss,
				 unsigned int nr_pages)
{
	switch (miss) {
	case MEMCOMP_COMP_CTX_BUDGET:
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_budget_pages);
		break;
	case MEMCOMP_COMP_CTX_BORROW_LIMIT:
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_borrow_limit_pages);
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_busy_reserved_free_pages);
		break;
	case MEMCOMP_COMP_CTX_BACKGROUND_WAITERS:
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_borrow_waiter_pages);
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_busy_reserved_free_pages);
		break;
	case MEMCOMP_COMP_CTX_RESERVED_FREE:
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_busy_reserved_free_pages);
		break;
	case MEMCOMP_COMP_CTX_ALL_BUSY:
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_busy_all_busy_pages);
		break;
	case MEMCOMP_COMP_CTX_AVAILABLE:
		WARN_ON_ONCE(1);
		return;
	}
}

static bool
memcomp_reclaim_ctx_precheck_result(unsigned int nr_pages,
				    enum memcomp_comp_ctx_miss miss)
{
	if (miss == MEMCOMP_COMP_CTX_AVAILABLE)
		return true;

	memcomp_debug_inc(&memcomp_ctx_busy);
	memcomp_debug_inc(&memcomp_reclaim_ctx_early_reject);
	memcomp_debug_add(nr_pages,
			  &memcomp_reclaim_ctx_early_reject_pages);
	if (miss == MEMCOMP_COMP_CTX_BUDGET) {
		memcomp_debug_inc(&memcomp_reclaim_ctx_budget_reject);
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_budget_reject_pages);
	}
	memcomp_account_reclaim_ctx_miss(miss, nr_pages);
	memcomp_account_anon_store(false, nr_pages);
	return false;
}

bool memcomp_reclaim_ctx_precheck(unsigned int nr_pages,
				  enum memcomp_reclaim_source source)
{
	enum memcomp_comp_ctx_miss miss;
	unsigned long flags;

	if (WARN_ON_ONCE((unsigned int)source >=
			 MEMCOMP_RECLAIM_SOURCE_NR))
		source = MEMCOMP_RECLAIM_DIRECT;
	if (source != MEMCOMP_RECLAIM_DIRECT)
		return true;

	spin_lock_irqsave(&memcomp_comp_ctx_lock, flags);
	miss = memcomp_comp_ctx_foreground_status_locked();
	spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
	return memcomp_reclaim_ctx_precheck_result(nr_pages, miss);
}

static void memcomp_comp_ctx_waiter_done(void)
{
	unsigned long flags;

	spin_lock_irqsave(&memcomp_comp_ctx_lock, flags);
	if (WARN_ON_ONCE(!memcomp_comp_ctx_waiters)) {
		spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
		return;
	}
	memcomp_comp_ctx_waiters--;
	spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
}

static bool memcomp_comp_ctx_wait_ready(void)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&memcomp_comp_ctx_lock, flags);
	ready = memcomp_comp_ctx_free &&
		memcomp_comp_budget_available_locked();
	spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
	return ready;
}

static void
memcomp_comp_ctx_unclaim_locked(unsigned int idx, bool borrowed,
				bool budget_acquired)
{
	lockdep_assert_held(&memcomp_comp_ctx_lock);
	if (borrowed) {
		WARN_ON_ONCE(!test_bit(idx, &memcomp_comp_ctx_borrowed));
		__clear_bit(idx, &memcomp_comp_ctx_borrowed);
		if (!WARN_ON_ONCE(!memcomp_comp_ctx_borrowed_active))
			memcomp_comp_ctx_borrowed_active--;
	} else {
		WARN_ON_ONCE(test_bit(idx, &memcomp_comp_ctx_borrowed));
	}
	if (budget_acquired)
		WARN_ON_ONCE(!memcomp_comp_budget_put_locked());
	WARN_ON_ONCE(!memcomp_comp_ctx_release(&memcomp_comp_ctx_free,
					       idx));
}

static struct memcomp_reclaim_ctx *
memcomp_try_get_comp_ctx(bool background, bool allow_borrow,
			 enum memcomp_comp_ctx_miss *miss,
			     bool *registered_waiter)
{
	struct memcomp_reclaim_ctx *ctx;
	unsigned long *freep = &memcomp_comp_ctx_free;
	unsigned long all_mask;
	unsigned long borrow_mask;
	unsigned long eligible;
	unsigned long foreground_mask;
	unsigned long free_mask;
	unsigned long flags;
	unsigned int borrowed_active;
	unsigned int borrow_limit;
	unsigned int waiters;
	bool borrowed = false;
	bool budget_acquired;
	int idx;

	eligible = background ? memcomp_comp_ctx_all :
				memcomp_comp_ctx_foreground;
	spin_lock_irqsave(&memcomp_comp_ctx_lock, flags);
	if (background && !memcomp_comp_budget_available_locked()) {
		if (miss)
			*miss = MEMCOMP_COMP_CTX_BUDGET;
		idx = -EBUSY;
		goto unavailable;
	}

	idx = memcomp_comp_ctx_claim(freep, eligible);
	if (idx < 0 && allow_borrow) {
		borrow_mask = memcomp_comp_ctx_all &
			      ~memcomp_comp_ctx_foreground;
		if (memcomp_comp_ctx_free & borrow_mask) {
			borrowed_active = memcomp_comp_ctx_borrowed_active;
			borrow_limit = memcomp_nr_borrowable_comp_ctxs;
			waiters = memcomp_comp_ctx_waiters;
			*miss = memcomp_comp_ctx_borrow_decision(borrowed_active,
								 borrow_limit,
								     waiters);
			if (*miss == MEMCOMP_COMP_CTX_AVAILABLE) {
				idx = memcomp_comp_ctx_claim(freep, borrow_mask);
				if (idx >= 0)
					borrowed = true;
			}
		}
	}
	if (idx < 0 && miss) {
		if (background) {
			*miss = MEMCOMP_COMP_CTX_ALL_BUSY;
		} else if (*miss == MEMCOMP_COMP_CTX_AVAILABLE) {
			free_mask = memcomp_comp_ctx_free;
			foreground_mask = memcomp_comp_ctx_foreground;
			all_mask = memcomp_comp_ctx_all;
			*miss = memcomp_comp_ctx_classify(free_mask,
							  foreground_mask,
							      all_mask);
		}
	}
	budget_acquired = false;
	if (idx >= 0 && background)
		budget_acquired = memcomp_comp_budget_get_locked();
	if (idx >= 0 && background && WARN_ON_ONCE(!budget_acquired)) {
		memcomp_comp_ctx_unclaim_locked(idx, false, false);
		if (miss)
			*miss = MEMCOMP_COMP_CTX_BUDGET;
		idx = -EBUSY;
	}
	if (idx >= 0 && borrowed) {
		__set_bit(idx, &memcomp_comp_ctx_borrowed);
		memcomp_comp_ctx_borrowed_active++;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		memcomp_comp_ctx_borrowed_peak =
			max(memcomp_comp_ctx_borrowed_peak,
			    memcomp_comp_ctx_borrowed_active);
#endif
	}

unavailable:
	if (idx < 0 && background && !*registered_waiter) {
		memcomp_comp_ctx_waiters++;
		*registered_waiter = true;
	}
	if (idx >= 0 && *registered_waiter) {
		WARN_ON_ONCE(!memcomp_comp_ctx_waiters);
		if (memcomp_comp_ctx_waiters)
			memcomp_comp_ctx_waiters--;
		*registered_waiter = false;
	}
	spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
	if (idx < 0)
		return NULL;

	ctx = &memcomp_comp_ctxs[idx];
	/* The free bit is published only after the previous owner unlocks. */
	if (background) {
		mutex_lock(&ctx->lock);
	} else if (WARN_ON_ONCE(!mutex_trylock(&ctx->lock))) {
		spin_lock_irqsave(&memcomp_comp_ctx_lock, flags);
		memcomp_comp_ctx_unclaim_locked(idx, borrowed, false);
		spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
		if (miss)
			*miss = MEMCOMP_COMP_CTX_ALL_BUSY;
		wake_up(&memcomp_comp_ctx_wait);
		return NULL;
	}
	ctx->borrowed = borrowed;
	ctx->background = background;
	ctx->direct_staging = false;
	return ctx;
}

/*
 * Lease a compression context.  PF_MEMALLOC kswapd may use
 * every context and wait for both a context and an execution-budget slot.
 * Foreground reclaim may borrow a bounded number of reserved contexts when
 * no background waiter exists, but never waits.  BEST_EFFORT shmem stores
 * only try non-reserved contexts.  Admission remains held until ctx_put().
 * A direct single-page zstd store may transfer its output to a staging slot
 * and put the context immediately after compression; every other store keeps
 * the context until its caller finishes.  Either way, an unmapped folio keeps
 * a stable destination through its payload snapshot.
 */
static struct memcomp_reclaim_ctx *
memcomp_get_comp_ctx(enum memcomp_payload_mode mode,
		     enum memcomp_comp_ctx_miss *miss,
			 enum memcomp_reclaim_source source)
{
	struct memcomp_reclaim_ctx *ctx;
	bool allow_borrow;
	bool background;
	bool can_retry;
	bool registered_waiter = false;
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	u64 wait_start_ns;
#endif
	int ret;

	if (miss)
		*miss = MEMCOMP_COMP_CTX_AVAILABLE;

	can_retry = memcomp_can_retry_alloc();
	background = memcomp_comp_ctx_background(mode, source, can_retry);
	if (mode != MEMCOMP_PAYLOAD_BEST_EFFORT &&
	    source != MEMCOMP_RECLAIM_DIRECT)
		WARN_ON_ONCE(!can_retry);
	allow_borrow = mode == MEMCOMP_PAYLOAD_MUST_SUCCEED &&
		       !background && miss;
	for (;;) {
		ctx = memcomp_try_get_comp_ctx(background, allow_borrow, miss,
					       &registered_waiter);
		if (ctx)
			return ctx;
		if (!background)
			break;
		WARN_ON_ONCE(!registered_waiter);
		if (miss && *miss == MEMCOMP_COMP_CTX_BUDGET)
			memcomp_debug_inc(&memcomp_comp_budget_waits[source]);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		memcomp_ctx_timing_wait_start(source);
		wait_start_ns = ktime_get_ns();
#endif
		ret = wait_event_freezable_exclusive(memcomp_comp_ctx_wait,
						     memcomp_comp_ctx_wait_ready() ||
						     kthread_should_stop());
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		memcomp_ctx_timing_wait_end(wait_start_ns, source);
#endif
		if (ret || kthread_should_stop()) {
			memcomp_comp_ctx_waiter_done();
			return NULL;
		}
	}

	/* Only the foreground path breaks out; background either wins or waits. */
	memcomp_debug_inc(&memcomp_ctx_busy);
	return NULL;
}

static void memcomp_put_comp_ctx(struct memcomp_reclaim_ctx *ctx)
{
	unsigned long flags;
	unsigned int waiters;
	unsigned int idx;
	bool background;
	bool borrowed;

	idx = ctx - memcomp_comp_ctxs;
	if (WARN_ON_ONCE(idx >= memcomp_nr_comp_ctxs))
		return;
	/*
	 * We hold the lease, so our own bit is clear and stays clear until we
	 * publish it below; only the other bits of the word need the lock.
	 * The test itself must stay: releasing ctx->lock for a context we no
	 * longer own would unlock a mutex held by another task.
	 */
	if (WARN_ON_ONCE(test_bit(idx, &memcomp_comp_ctx_free)))
		return;

	borrowed = ctx->borrowed;
	background = ctx->background;
	ctx->borrowed = false;
	ctx->background = false;
	ctx->direct_staging = false;
	/*
	 * Unlock before publishing the free bit: a claimer trylocks ctx->lock
	 * as soon as it sees the bit, and expects the previous owner to be
	 * gone.
	 */
	mutex_unlock(&ctx->lock);
	spin_lock_irqsave(&memcomp_comp_ctx_lock, flags);
	memcomp_comp_ctx_unclaim_locked(idx, borrowed, background);
	/*
	 * A waiter registers itself in the same critical section in which its
	 * claim failed, and only then sleeps, so reading the count here - in
	 * the critical section that makes a context available again - cannot
	 * miss one.  With no waiter there is nobody to wake and the wait queue
	 * head's lock stays off this path entirely.
	 */
	waiters = memcomp_comp_ctx_waiters;
	spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
	if (waiters)
		wake_up(&memcomp_comp_ctx_wait);
}

struct memcomp_reclaim_ctx *
memcomp_reclaim_ctx_get(unsigned int nr_pages,
			enum memcomp_reclaim_source source)
{
	enum memcomp_payload_mode mode = MEMCOMP_PAYLOAD_MUST_SUCCEED;
	struct memcomp_reclaim_ctx *ctx;
	enum memcomp_comp_ctx_miss miss;
	const char *driver;

	if (WARN_ON_ONCE((unsigned int)source >=
			 MEMCOMP_RECLAIM_SOURCE_NR))
		source = MEMCOMP_RECLAIM_DIRECT;
	ctx = memcomp_get_comp_ctx(mode, &miss, source);
	if (ctx) {
		driver = crypto_tfm_alg_driver_name(crypto_comp_tfm(ctx->tfm));
		ctx->direct_staging =
			memcomp_staging_policy(mode, source, nr_pages,
					       driver);
		if (ctx->borrowed)
			memcomp_debug_add(nr_pages,
					  &memcomp_reclaim_ctx_borrow_pages);
		memcomp_ctx_timing_start(ctx, nr_pages, source);
		return ctx;
	}

	if (source == MEMCOMP_RECLAIM_DIRECT &&
	    miss == MEMCOMP_COMP_CTX_BUDGET) {
		memcomp_debug_inc(&memcomp_reclaim_ctx_budget_reject);
		memcomp_debug_add(nr_pages,
				  &memcomp_reclaim_ctx_budget_reject_pages);
	}
	memcomp_account_reclaim_ctx_miss(miss, nr_pages);
	memcomp_account_anon_store(false, nr_pages);
	return NULL;
}

void memcomp_reclaim_ctx_put(struct memcomp_reclaim_ctx *ctx)
{
	if (!ctx)
		return;
	lockdep_assert_held(&ctx->lock);
	memcomp_ctx_timing_account(&ctx->timing);
	memcomp_put_comp_ctx(ctx);
}

static int memcomp_alloc_raw_payload(u8 pool_idx, const void *src,
				     unsigned int src_len,
					 unsigned int *length,
					 unsigned long *handle,
					 bool allow_slow,
					  struct memcomp_store_ctx *store)
{
	struct zs_pool *pool = memcomp_pools[pool_idx];
	void *dst;
	int ret;

	ret = memcomp_payload_zs_malloc(pool, src_len, handle, allow_slow,
					store);
	if (ret)
		return ret;

	dst = zs_map_object(pool, *handle, ZS_MM_WO);
	if (!dst) {
		zs_free(pool, *handle);
		return -EIO;
	}

	memcpy(dst, src, src_len);
	zs_unmap_object(pool, *handle);

	*length = src_len;
	return 0;
}

enum memcomp_raw_reason {
	MEMCOMP_RAW_NONE,
	MEMCOMP_RAW_HUGE_CLASS,
	MEMCOMP_RAW_GE_PAGE,
};

static enum memcomp_raw_reason
memcomp_payload_raw_reason(unsigned int compressed_len,
			   unsigned int src_len,
			       unsigned int huge_class_size)
{
	if (compressed_len >= src_len)
		return MEMCOMP_RAW_GE_PAGE;
	if (huge_class_size && compressed_len >= huge_class_size)
		return MEMCOMP_RAW_HUGE_CLASS;
	return MEMCOMP_RAW_NONE;
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static unsigned int
memcomp_raw_payload_count(const unsigned int *lengths,
			  unsigned int nr_pages)
{
	unsigned int raw = 0;
	unsigned int i;

	for (i = 0; i < nr_pages; i++) {
		if (lengths[i] == PAGE_SIZE)
			raw++;
	}

	return raw;
}
#endif

/*
 * A raw object can still make progress as part of a larger folio when the
 * other payloads save enough space.  Require at least one page of aggregate
 * payload headroom before replacing the resident folio.  zsmalloc class
 * rounding is measured separately by the device tests; pool-wide size deltas
 * cannot be a correctness check here because stores run concurrently.
 */
static bool
memcomp_payloads_net_reclaimable(const unsigned int *lengths,
				 unsigned int nr_pages)
{
	u64 total = 0;
	unsigned int raw = 0;
	unsigned int i;

	for (i = 0; i < nr_pages; i++) {
		if (lengths[i] > PAGE_SIZE)
			return false;
		if (lengths[i] == PAGE_SIZE)
			raw++;
		total += lengths[i];
	}
	if (!raw)
		return true;
	if (nr_pages < 2)
		return false;

	return total <= (u64)(nr_pages - 1) * PAGE_SIZE;
}

/*
 * Compress @src and place the result in zsmalloc, or detect a same-fill
 * page.  On success: *length == 0 means same-fill with the pattern in
 * *handle; otherwise the handle and pool index name the payload.
 */
static void
memcomp_detach_reclaim_ctx(struct memcomp_reclaim_ctx **ctxp,
			   struct memcomp_direct_staging *staging)
{
	struct memcomp_reclaim_ctx *ctx = *ctxp;

	lockdep_assert_preemption_disabled();
	lockdep_assert_held(&ctx->lock);
	staging->timing = ctx->timing;
	ctx->timing.timing_active = false;
	*ctxp = NULL;
	memcomp_put_comp_ctx(ctx);
	if (staging->timing.timing_active)
		staging->timing.timing_lease_end_ns = ktime_get_ns();
	memcomp_debug_inc(&memcomp_direct_staging_detach);
}

/*
 * A compressor that cannot fit a page into less than the page - a hardware
 * engine stopping at its output limit, say - may say so with -E2BIG or
 * -ENOSPC instead of producing that output.  Take it as the payload a CPU
 * compressor would have produced for such a page: one at least a page long,
 * which is stored raw where the caller allows it and refused where it does
 * not.
 */
static int memcomp_compress_result(int ret, unsigned int *dst_len,
				   unsigned int src_len)
{
	if (ret != -E2BIG && ret != -ENOSPC)
		return ret;
	*dst_len = src_len;
	return 0;
}

static int
memcomp_prepare_payload_locked(struct memcomp_reclaim_ctx **ctxp,
			       const void *src, unsigned int src_len,
				   unsigned int *length,
				   unsigned long *handle, u8 *pool_idx,
				   bool allow_provisional_raw,
				   struct memcomp_store_ctx *store)
{
	struct memcomp_compress_timing_stamp compress_timing;
	struct memcomp_direct_staging *staging = NULL;
	struct memcomp_reclaim_ctx *ctx = *ctxp;
	struct memcomp_ctx_timing_record *timing = &ctx->timing;
	struct zs_pool *pool;
	enum memcomp_compress_timing_outcome timing_outcome;
	enum memcomp_raw_reason raw_reason = MEMCOMP_RAW_NONE;
	void *compressed;
	unsigned long element;
	unsigned int huge_class_size;
	unsigned int dst_len;
	u64 timing_start;
	u8 target;
	bool allow_slow;
	void *dst;
	int ret;

	lockdep_assert_held(&ctx->lock);

	if (page_same_filled(src, &element)) {
		*length = 0;
		*handle = element;
		return 0;
	}

	target = memcomp_select_pool_idx();
	pool = memcomp_pools[target];
	huge_class_size = zs_huge_class_size(pool);
	allow_slow = ctx->background;
	if (store && ctx->direct_staging) {
		WARN_ON_ONCE(store->staging);
		staging = memcomp_direct_staging_try_get();
		store->staging = staging;
	}
	compressed = staging ? staging->dst : ctx->dst;

	dst_len = MEMCOMP_MAX_COMPRESSED_SIZE;
	if (staging)
		preempt_disable();
	compress_timing = memcomp_compress_timing_start(timing);
		ret = crypto_comp_compress(ctx->tfm, src, src_len,
					   compressed, &dst_len);
	ret = memcomp_compress_result(ret, &dst_len, src_len);
	if (!ret)
		raw_reason = memcomp_payload_raw_reason(dst_len, src_len,
							huge_class_size);
	/* Nothing after this point is compression. */
	compress_timing =
		memcomp_compress_timing_end(timing, compress_timing);
	if (ret)
		timing_outcome = MEMCOMP_COMPRESS_TIMING_ERROR;
	else if (raw_reason == MEMCOMP_RAW_HUGE_CLASS)
		timing_outcome = MEMCOMP_COMPRESS_TIMING_RAW_HUGE;
	else if (raw_reason == MEMCOMP_RAW_GE_PAGE)
		timing_outcome = MEMCOMP_COMPRESS_TIMING_RAW_GE_PAGE;
	else
		timing_outcome = MEMCOMP_COMPRESS_TIMING_COMPRESSIBLE;
	memcomp_compress_timing_account(timing, compress_timing,
					timing_outcome);
	if (staging) {
		memcomp_detach_reclaim_ctx(ctxp, staging);
		timing = &staging->timing;
		preempt_enable();
	}
	if (ret) {
		memcomp_debug_inc(&memcomp_compress_fail);
		return ret;
	}
	if (raw_reason != MEMCOMP_RAW_NONE) {
		if (raw_reason == MEMCOMP_RAW_HUGE_CLASS)
			memcomp_debug_inc(&memcomp_raw_candidate_huge);
		else
			memcomp_debug_inc(&memcomp_raw_candidate_ge_page);
		if (!allow_provisional_raw) {
			if (raw_reason == MEMCOMP_RAW_HUGE_CLASS)
				memcomp_debug_inc(&memcomp_raw_reject_huge);
			else
				memcomp_debug_inc(&memcomp_raw_reject_ge_page);
			return -E2BIG;
		}
		/* zram stores incompressible pages in its PAGE_SIZE class. */
		timing_start = memcomp_ctx_timing_phase_start(timing);
		ret = memcomp_alloc_raw_payload(target, src, src_len,
						length, handle,
						    allow_slow, store);
		memcomp_ctx_timing_phase_end(timing, timing_start,
					     &timing->timing_zs_ns);
		if (!ret)
			*pool_idx = target;
		return ret;
	}

	timing_start = memcomp_ctx_timing_phase_start(timing);
	ret = memcomp_payload_zs_malloc(pool, dst_len, handle, allow_slow,
					store);
	if (ret) {
		if (!allow_provisional_raw) {
			memcomp_debug_inc(&memcomp_raw_reject_zs_fallback);
			memcomp_ctx_timing_phase_end(timing, timing_start,
						     &timing->timing_zs_ns);
			return ret;
		}
		/*
		 * A small-object class can need a multi-page zspage even when
		 * the one-page raw class can still grow.  Keep this fallback
		 * fast-only: background reclaim already paid for the slow
		 * compressed allocation, and foreground reclaim must not stall.
		 */
		ret = memcomp_alloc_raw_payload(target, src, src_len,
						length, handle, false,
						    store);
		memcomp_ctx_timing_phase_end(timing, timing_start,
					     &timing->timing_zs_ns);
		if (!ret)
			*pool_idx = target;
		return ret;
	}

	dst = zs_map_object(pool, *handle, ZS_MM_WO);
	if (!dst) {
		zs_free(pool, *handle);
		memcomp_ctx_timing_phase_end(timing, timing_start,
					     &timing->timing_zs_ns);
		return -EIO;
	}
	memcpy(dst, compressed, dst_len);
	zs_unmap_object(pool, *handle);
	memcomp_ctx_timing_phase_end(timing, timing_start,
				     &timing->timing_zs_ns);

	*length = dst_len;
	*pool_idx = target;
	return 0;
}

static __always_inline int
memcomp_arch_prepare_to_swap(struct folio *folio)
{
	return arch_prepare_to_swap(folio);
}

/* Allocate @nr_pages consecutive tokens, skipping fragmented free ranges. */

/*
 * Core store - compress @nr_pages pages and insert them into the global
 * entries xarray under consecutive tokens.  Returns 0 with *token set
 * to the first id.
 */

/*
 * Reserve tokens for @folio before it is unmapped.  The entries stay in
 * MC_PENDING with a reference on the resident folio, so faults arriving
 * between unmap and the payload store can map the original pages back.
 */

#include "memcomp/anon_store.h"

/*
 * Fill an existing MC_PENDING span after unmap while keeping tokens stable.
 * @held are the span's entries if the caller has them and holds a reference
 * on each, NULL to have them looked up here.
 */

/*
 * Core load - decompress one page from @entry into @dst.  Sleeps on the
 * token lock and possibly swap IO; never called under the PTE lock.
 */
#include "memcomp/load.h"
#include "memcomp/consumers.h"

/*
 * Claim a reclaimable entry for writeback.
 */

#ifdef CONFIG_SWAP
/*
 * Migrate up to @limit of @nid's coldest payloads out to swap as original
 * pages.  Returns the number of entries scanned.  Caller holds
 * memcomp_writeback_lock with memalloc_noreclaim_save() in force.
 *
 * Writes are asynchronous; the batch is waited on once per
 * MEMCOMP_WRITEBACK_BATCH stagings and once at the end, so a pass
 * costs about limit/batch blocking waits rather than one per page, with
 * the device kept at batch depth in between.
 */

#endif

unsigned int memcomp_pte_batch(pte_t *start_pte, unsigned int max_nr)
{
	pte_t first_pte;
	swp_entry_t first_entry;
	unsigned int i;

	if (!max_nr)
		return 0;

	first_pte = ptep_get(start_pte);
	if (!is_swap_pte(first_pte))
		return 0;

	first_entry = pte_to_swp_entry(first_pte);
	if (!is_memcomp_entry(first_entry))
		return 0;

	for (i = 0; i < max_nr; i++) {
		pte_t pte;
		swp_entry_t entry;

		pte = ptep_get(start_pte + i);
		if (!is_swap_pte(pte))
			break;

		entry = pte_to_swp_entry(pte);
		if (!is_memcomp_entry(entry))
			break;
		if (swp_offset(entry) != (swp_offset(first_entry) ?
					  swp_offset(first_entry) + i : 0))
			break;
		if (pte_swp_exclusive(pte) != pte_swp_exclusive(first_pte))
			break;
		if (pte_swp_soft_dirty(pte) != pte_swp_soft_dirty(first_pte))
			break;
		if (pte_swp_uffd_wp(pte) != pte_swp_uffd_wp(first_pte))
			break;
	}

	return i;
}

/*
 * Use as a branch condition: a successful map retains both its PTE spinlock
 * and the RCU read lock from __pte_offset_map(); NULL retains neither.
 * Keep both real conditional contexts visible here rather than returning
 * them through pte_offset_map_lock()'s inline wrapper.
 */
#define memcomp_pte_map_lock(mm, pmd, address, ptep, ptlp) \
	({ \
		spinlock_t **__ptlp = (ptlp); \
		__cond_lock(RCU, __cond_lock(*__ptlp, \
			*(ptep) = __pte_offset_map_lock((mm), (pmd), \
						     (address), __ptlp))); \
	})

#include "memcomp/zero_load.h"

static vm_fault_t memcomp_fault_sigbus_or_retry(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;

	if (!memcomp_pte_map_lock(vma->vm_mm, vmf->pmd, vmf->address,
				  &vmf->pte, &vmf->ptl))
		return 0;
	if (!pte_same(ptep_get(vmf->pte), vmf->orig_pte)) {
		pte_unmap_unlock(vmf->pte, vmf->ptl);
		vmf->pte = NULL;
		return 0;
	}

	pte_unmap_unlock(vmf->pte, vmf->ptl);
	vmf->pte = NULL;
	return VM_FAULT_SIGBUS;
}

/*
 * Map @page of the locked @folio at @address, where @ptep, locked, holds
 * @orig_pte, a resident token for it.  The caller has taken the folio
 * reference the mapping needs and drops the token's entry reference.
 * Returns whether the PTE was made dirty for a @write.
 */
static bool memcomp_map_resident(struct vm_area_struct *vma,
				 unsigned long address, pte_t *ptep,
				     pte_t orig_pte, struct folio *folio,
				     struct page *page, bool write)
{
	bool exclusive = pte_swp_exclusive(orig_pte);
	rmap_t rmap_flags = RMAP_NONE;
	bool dirty = false;
	pte_t pte;

	pte = mk_pte(page, vma->vm_page_prot);
	if (pte_swp_soft_dirty(orig_pte))
		pte = pte_mksoft_dirty(pte);
	if (pte_swp_uffd_wp(orig_pte))
		pte = pte_mkuffd_wp(pte);
	if (exclusive &&
	    (vma->vm_flags & VM_WRITE) && !userfaultfd_pte_wp(vma, pte) &&
	    !pte_needs_soft_dirty_wp(vma, pte)) {
		pte = pte_mkwrite(pte, vma);
		if (write) {
			pte = pte_mkdirty(pte);
			dirty = true;
		}
	}
	if (exclusive)
		rmap_flags = RMAP_EXCLUSIVE;

	add_mm_counter(vma->vm_mm, MM_ANONPAGES, 1);
	add_mm_counter(vma->vm_mm, MM_SWAPENTS, -1);
	flush_icache_pages(vma, page, 1);
	folio_add_anon_rmap_pte(folio, page, vma, address, rmap_flags);
	set_pte_at(vma->vm_mm, address, ptep, pte);
	arch_do_swap_page_nr(vma->vm_mm, vma, address, pte, pte, 1);
	update_mmu_cache(vma, address, ptep);
	return dirty;
}

/*
 * Fast path for the pending window: reclaim reserved the token but has
 * not stored the payload yet, so the original folio is still resident.
 * Map it back instead of decompressing.
 */
#include "memcomp/resident.h"
#include "memcomp/fault.h"
#include "memcomp/zero_convert.h"

/*
 * Every token but a resident pending one is position independent: loads
 * allocate a fresh folio wherever the PTE is found.  A resident token is
 * not, because the fault maps the original folio back and that folio's
 * anon rmap still names the address the token was unmapped from.  Moving
 * such a PTE elsewhere would map the folio where rmap cannot find it.
 *
 * Return the lookup reference to the caller.  Keeping it across a PTE
 * retry prevents the token ID from being reused for a different entry.
 * A token never re-enters MC_PENDING, so a false answer stays valid for as
 * long as the caller holds this reference and a PTE that names the token.
 */

/*
 * Map a resident token's folio back at the address in @vmf, without the
 * decompression fallback memcomp_load() would take once the window has
 * closed.  Returns -EFAULT if the PTE still names a token that is gone and 0
 * otherwise; the caller re-reads the PTE either way.  May sleep on the folio
 * lock.
 */

/*
 * Map the locked @folio back wherever try_to_unmap() left one of the resident
 * tokens of its span at @token, the way remove_migration_ptes() puts back a
 * folio that failed to migrate.  Each PTE restored drops its token's
 * reference.  The caller keeps the folio's anon_vma alive, as rmap walks of
 * a folio that is no longer mapped must.
 *
 * Tokens only get into PTEs by try_to_unmap(), which needs the folio lock,
 * and by fork() copying one that is already in a PTE.  The walk visits a
 * parent's VMAs before a child linked to the same anon_vma, as migration
 * relies on too, so a token fork copies meanwhile is found in the child.
 * Returns how many PTEs it mapped back.
 */

#ifdef CONFIG_MEMCOMP_DEBUG_STATS

#endif

/* Check if a page-cache xa_value is a memcomp entry. */

/*
 * Shmem/tmpfs variant: compress a folio and return the token.  This does not
 * leave a token in folio->swap; the caller (shmem_writepage) stores it in the
 * page-cache xarray via shmem_delete_from_page_cache().
 */

/*
 * Shmem/tmpfs swapin: pin the entry, decompress into a pre-allocated
 * folio, then commit (after the page-cache shadow was replaced) or
 * abort.  The caller handles locking, page-cache insertion and rmap.
 */
/* Explicit zero transport: no sentinel guard or owner consumption. */

/* Fill an existing token span after every mapping has been removed. */

#include "memcomp/anon_folio.h"

static bool memcomp_reserve_nr_pages_valid(unsigned int nr_pages)
{
	return nr_pages && nr_pages <= MEMCOMP_MAX_COMPRESS_PAGES;
}

bool memcomp_reserve_eligible(struct folio *folio)
{
	unsigned int nr_pages = folio_nr_pages(folio);

	if (!folio_test_locked(folio))
		return false;
	if (!folio_test_anon(folio) || !folio_test_swapbacked(folio))
		return false;
	if (folio_test_mlocked(folio) || folio_test_ksm(folio) ||
	    folio_mapping(folio))
		return false;
	if (folio_test_swapcache(folio))
		return false;
	if (!memcomp_reserve_nr_pages_valid(nr_pages))
		return false;
	return true;
}

#include "memcomp/folio.h"
#include "memcomp/api.h"

/*
 * A pending entry starts with one owner reference.  memcomp_get_entry()
 * adds the lookup reference observed below, so any further reference means
 * the token may have escaped the folio, normally into a PTE installed by
 * try_to_unmap().  A lookup can look the same, which costs no more than an
 * rmap walk that finds nothing.  Nothing escapes once this finds no further
 * reference: try_to_unmap() needs the folio lock, and fork() only copies a
 * token that is already in a PTE.
 */

/*
 * Map the locked @folio back wherever a token of its pending span went.  A
 * reference left after a walk is normally a fault holding a lookup while it
 * waits for the folio lock, with its PTE already mapped back.  But it may be
 * a PTE the walk missed, which would pin the folio until faulted on, so walk
 * again for as long as walks find PTEs, a few times at most.
 */
#define MEMCOMP_RESTORE_WALKS	3

static bool memcomp_folio_dirty_one(struct folio *folio,
				    struct vm_area_struct *vma,
				    unsigned long address, void *arg)
{
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, address, PVMW_SYNC);
	bool *dirty = arg;

	while (page_vma_mapped_walk(&pvmw)) {
		if (pvmw.pte)
			*dirty = pte_dirty(ptep_get(pvmw.pte));
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		else
			*dirty = pmd_dirty(*pvmw.pmd);
#endif
		if (*dirty) {
			page_vma_mapped_walk_done(&pvmw);
			return false;
		}
	}
	return true;
}

/* Check a locked anonymous folio without clearing or write-protecting PTEs. */
bool memcomp_folio_dirty(struct folio *folio)
{
	bool dirty = false;
	struct rmap_walk_control rwc = {
		.arg = &dirty,
		.rmap_one = memcomp_folio_dirty_one,
		.anon_lock = folio_lock_anon_vma_read,
	};

	if (folio_test_dirty(folio))
		return true;
	/* A writable swap-in PTE can be dirtied without another write fault. */
	rmap_walk(folio, &rwc);
	return dirty || folio_test_dirty(folio);
}

/*
 * Whether reclaim should leave the locked @folio uncompressed, because it
 * holds the contents it was marked incompressible with.  A folio written to
 * since loses the mark.
 */
bool memcomp_folio_incompressible(struct folio *folio)
{
	if (!folio_test_incompressible(folio))
		return false;
	if (memcomp_folio_hash(folio) == folio_incompressible_hash(folio))
		return true;

	folio_clear_incompressible(folio);
	memcomp_debug_add(folio_nr_pages(folio),
			  &memcomp_incompressible_changed_pages);
	return false;
}

/*
 * Drop the locked @folio's reservation.  Reclaim reserves resident tokens
 * before unmap so PTEs have a stable non-swap entry to install; if the store
 * then fails, or the folio cannot be unmapped completely, the folio is mapped
 * back wherever a token went first, so that nothing is left naming the span.
 * The folio goes back to the LRU as it was, holding no reservation, but
 * marked incompressible if that is why the store failed.
 */

static bool
__memcomp_store_after_unmap(struct folio *folio,
			    struct memcomp_reclaim_ctx **ctxp)
{
	struct memcomp_ctx_timing_record *timing;
	struct memcomp_store_ctx store = { };
	enum memcomp_anon_store_fail failure;
	struct memcomp_reclaim_ctx *ctx;
	unsigned int nr_pages;
	bool success;

	nr_pages = folio_nr_pages(folio);
	if (WARN_ON_ONCE(!ctxp || !*ctxp))
		return false;

	ctx = *ctxp;
	memcomp_ctx_timing_store_start(&ctx->timing);
	success = mc16_anon_store_internal(folio, ctxp, &store, &failure);
	if (!success)
		memcomp_account_anon_store_fail(failure, nr_pages);
	memcomp_account_anon_store(success, nr_pages);
	if (store.staging) {
		WARN_ON_ONCE(*ctxp);
		timing = &store.staging->timing;
	} else {
		ctx = *ctxp;
		if (WARN_ON_ONCE(!ctx))
			return false;
		timing = &ctx->timing;
	}
	memcomp_ctx_timing_store_end(timing, success);
	if (*ctxp) {
		ctx = *ctxp;
		*ctxp = NULL;
		memcomp_reclaim_ctx_put(ctx);
	} else {
		memcomp_ctx_timing_account(timing);
	}
	memcomp_direct_staging_put(store.staging);
	if (success && store.anon_vma)
		mc16_zero_convert_folio(folio, &store.zero);
	store.zero = (struct mc16_zero_carrier) { };
	if (store.anon_vma)
		put_anon_vma(store.anon_vma);
	return success;
}

bool memcomp_store_after_unmap(struct folio *folio,
			       struct memcomp_reclaim_ctx **ctxp)
{
	return __memcomp_store_after_unmap(folio, ctxp);
}

static void memcomp_destroy_comp_ctxs(void)
{
	unsigned int i;

	WARN_ON_ONCE(memcomp_direct_staging_active);
	for (i = 0; i < memcomp_nr_direct_staging; i++) {
		kfree(memcomp_direct_staging[i].dst);
		memcomp_direct_staging[i].dst = NULL;
	}
	memcomp_nr_direct_staging = 0;
	memcomp_direct_staging_free = 0;
	memcomp_direct_staging_active = 0;
	memcomp_direct_staging_peak = 0;

	for (i = 0; i < memcomp_nr_comp_ctxs; i++) {
		struct memcomp_reclaim_ctx *ctx = &memcomp_comp_ctxs[i];

		if (ctx->tfm) {
			crypto_free_comp(ctx->tfm);
			ctx->tfm = NULL;
		}
		kfree(ctx->dst);
		ctx->dst = NULL;
	}
	memcomp_nr_comp_ctxs = 0;
	memcomp_nr_reserved_comp_ctxs = 0;
	memcomp_nr_borrowable_comp_ctxs = 0;
	memcomp_comp_ctx_free = 0;
	memcomp_comp_ctx_all = 0;
	memcomp_comp_ctx_foreground = 0;
	memcomp_comp_ctx_borrowed = 0;
	memcomp_comp_ctx_borrowed_active = 0;
	memcomp_comp_ctx_borrowed_peak = 0;
	memcomp_comp_ctx_waiters = 0;
	memset(&memcomp_comp_budget, 0, sizeof(memcomp_comp_budget));
}

static void memcomp_destroy_decomp_ctxs(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct memcomp_decomp_ctx *ctx;

		ctx = &per_cpu(memcomp_decomp_ctxs, cpu);
		if (ctx->tfm) {
			crypto_free_comp(ctx->tfm);
			ctx->tfm = NULL;
		}
	}
}

/* Set everything up for memcomp_compressor and enable memcomp. */
static int memcomp_start(void)
{
	unsigned int target_comp_ctxs;
	int ret = -ENOMEM;
	int cpu;
	int i;

	for (i = 0; i < MAX_NUMNODES; i++) {
		spin_lock_init(&memcomp_lrus[i].lock);
		INIT_LIST_HEAD(&memcomp_lrus[i].entries);
		atomic_long_set(&memcomp_lrus[i].nr_entries, 0);
	}

	for (i = 0; i < MEMCOMP_NR_LOCKS; i++)
		mutex_init(&memcomp_locks[i]);

	target_comp_ctxs = min_t(unsigned int, MEMCOMP_MAX_COMP_CTXS,
				 4 * num_possible_cpus());
	for (i = 0; i < target_comp_ctxs; i++) {
		struct memcomp_reclaim_ctx *ctx = &memcomp_comp_ctxs[i];
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		unsigned int source;
#endif

		mutex_init(&ctx->lock);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
		for (source = 0; source < MEMCOMP_RECLAIM_SOURCE_NR;
		     source++) {
			unsigned int seed;

			seed = i * MEMCOMP_RECLAIM_SOURCE_NR + source;
			ctx->timing_seq[source] =
				memcomp_ctx_timing_seed(seed);
		}
#endif
		ctx->tfm = crypto_alloc_comp(memcomp_compressor, 0, 0);
		if (IS_ERR(ctx->tfm)) {
			ret = PTR_ERR(ctx->tfm);
			ctx->tfm = NULL;
			break;
		}
		ctx->dst = kmalloc(MEMCOMP_MAX_COMPRESSED_SIZE,
				   GFP_KERNEL);
		if (!ctx->dst) {
			crypto_free_comp(ctx->tfm);
			ctx->tfm = NULL;
			break;
		}
		memcomp_nr_comp_ctxs++;
		__set_bit(i, &memcomp_comp_ctx_all);
	}
	if (!memcomp_nr_comp_ctxs)
		goto err_comp;
	if (memcomp_nr_comp_ctxs < target_comp_ctxs)
		pr_warn("initialized %u of %u compression contexts\n",
			memcomp_nr_comp_ctxs, target_comp_ctxs);
	memcomp_nr_reserved_comp_ctxs =
		min_t(unsigned int, num_possible_cpus(),
		      memcomp_nr_comp_ctxs / 2);
	if (memcomp_nr_reserved_comp_ctxs)
		memcomp_nr_borrowable_comp_ctxs =
			min_t(unsigned int, MEMCOMP_MAX_BORROWED_COMP_CTXS,
			      memcomp_nr_reserved_comp_ctxs - 1);
	memcomp_comp_ctx_foreground = memcomp_comp_ctx_all;
	bitmap_clear(&memcomp_comp_ctx_foreground, 0,
		     memcomp_nr_reserved_comp_ctxs);
	memcomp_comp_ctx_free = memcomp_comp_ctx_all;
	memcomp_comp_ctx_borrowed = 0;
	memcomp_comp_ctx_borrowed_active = 0;
	memcomp_comp_ctx_borrowed_peak = 0;
	memcomp_comp_ctx_waiters = 0;
	memset(&memcomp_comp_budget, 0, sizeof(memcomp_comp_budget));
	for (i = 0; i < memcomp_nr_comp_ctxs; i++) {
		struct memcomp_direct_staging *staging;

		staging = &memcomp_direct_staging[i];
		staging->dst = kmalloc(MEMCOMP_MAX_COMPRESSED_SIZE,
				       GFP_KERNEL);
		if (!staging->dst)
			break;
		memcomp_nr_direct_staging++;
		__set_bit(i, &memcomp_direct_staging_free);
	}
	if (memcomp_nr_direct_staging < memcomp_nr_comp_ctxs)
		pr_warn("initialized %u of %u direct staging buffers\n",
			memcomp_nr_direct_staging, memcomp_nr_comp_ctxs);

	for_each_possible_cpu(cpu) {
		struct memcomp_decomp_ctx *ctx;

		ctx = &per_cpu(memcomp_decomp_ctxs, cpu);
		ctx->tfm = crypto_alloc_comp(memcomp_compressor, 0, 0);
		if (IS_ERR(ctx->tfm)) {
			ret = PTR_ERR(ctx->tfm);
			ctx->tfm = NULL;
			goto err_decomp;
		}
	}

	for (i = 0; i < MEMCOMP_NR_POOLS; i++) {
		char name[24];

		snprintf(name, sizeof(name), "memcomp%d", i);
		memcomp_pools[i] = zs_create_pool(name);
		if (!memcomp_pools[i])
			break;
		memcomp_nr_pools++;
	}

	if (!memcomp_nr_pools) {
		ret = -ENODEV;
		goto err_decomp;
	}

	memcomp_meta_shrinker = shrinker_alloc(0, "memcomp-meta");
	if (!memcomp_meta_shrinker)
		pr_warn("metadata shrinker allocation failed; pressure reclaim unavailable\n");
	if (memcomp_meta_shrinker) {
		memcomp_meta_shrinker->count_objects = mc16_meta_shrinker_count;
		memcomp_meta_shrinker->scan_objects = mc16_meta_shrinker_scan;
		shrinker_register(memcomp_meta_shrinker);
	}
	/* Make every initialized resource visible before enabling callers. */
	smp_store_release(&memcomp_enabled, true);
	pr_info("using %s\n", memcomp_compressor);

	return 0;

err_decomp:
	memcomp_destroy_decomp_ctxs();
err_comp:
	memcomp_destroy_comp_ctxs();
	pr_warn("%s: init failed: %d\n", memcomp_compressor,
		ret);
	return ret;
}

/*
 * The compressor memcomp is built to use may be one whose driver only
 * registers it after late_initcall - a hardware engine that probes once the
 * IOMMU in front of it does, from a module, say.  Wait that long for it, and
 * take MEMCOMP_FALLBACK_COMPRESSOR once it is plain it is not coming or
 * memcomp cannot be set up on it.  memcomp stays disabled until one
 * of the two is decided on: payloads of two compressors cannot share it.
 */
#define MEMCOMP_FALLBACK_COMPRESSOR	"zstd"
/* As long as the driver core waits for the modules a device's probe needs. */
#define MEMCOMP_COMPRESSOR_WAIT	(10 * HZ)

/* The startup decision is protected by memcomp_select_lock. */
struct memcomp_compressor_select {
	struct delayed_work work;
	struct notifier_block nb;
	char *compressor;
	unsigned long deadline;
	bool decided;
};

static void memcomp_select_worker(struct work_struct *work);
static int memcomp_crypto_notify(struct notifier_block *nb,
				 unsigned long msg, void *data);

static struct memcomp_compressor_select memcomp_select = {
	.work = __DELAYED_WORK_INITIALIZER(memcomp_select.work,
					   memcomp_select_worker, 0),
	.nb = {
		.notifier_call = memcomp_crypto_notify,
	},
	.compressor = memcomp_compressor,
};

static int memcomp_crypto_notify(struct notifier_block *nb,
				 unsigned long msg, void *data)
{
	struct memcomp_compressor_select *select;

	if (msg != CRYPTO_MSG_ALG_LOADED)
		return NOTIFY_DONE;
	select = container_of(nb, struct memcomp_compressor_select, nb);
	/*
	 * crypto_alg_sem is held.  Neither allocate a tfm nor take the
	 * selection lock here: selection takes that semaphore in turn.
	 * The worker looks up only its selected name under the mutex, so
	 * neither a fallback name change nor the event's lifetime can race.
	 */
	mod_delayed_work(system_unbound_wq, &select->work, 0);
	return NOTIFY_OK;
}

static void memcomp_select_worker(struct work_struct *work)
{
	struct memcomp_compressor_select *select;
	unsigned long now = jiffies;

	select = container_of(to_delayed_work(work),
			      struct memcomp_compressor_select, work);
	mutex_lock(&memcomp_select_lock);
	if (select->decided)
		goto out;

	if (crypto_has_comp(select->compressor, 0, 0) != 1) {
		if (time_before(now, select->deadline)) {
			/* Not mod_: a registration may have asked for sooner. */
			queue_delayed_work(system_unbound_wq, &select->work,
					   select->deadline - now);
			goto out;
		}
		pr_warn("compressor %s unavailable\n", select->compressor);
	}
	/* The synchronous compressor choice is final. */
	select->decided = true;
	if (crypto_has_comp(select->compressor, 0, 0) == 1 &&
	    !memcomp_start()) {
		goto out;
	}

	if (strcmp(select->compressor, MEMCOMP_FALLBACK_COMPRESSOR)) {
		pr_warn("falling back to %s\n", MEMCOMP_FALLBACK_COMPRESSOR);
		strscpy(select->compressor, MEMCOMP_FALLBACK_COMPRESSOR,
			sizeof(memcomp_compressor));
		if (crypto_has_comp(select->compressor, 0, 0) == 1 &&
		    !memcomp_start()) {
			goto out;
		}
	}
	pr_warn("disabled, no usable compressor\n");
out:
	mutex_unlock(&memcomp_select_lock);
}

static int __init memcomp_init(void)
{
	int ret;

	BUILD_BUG_ON(MAX_NUMNODES > U16_MAX);
	BUILD_BUG_ON(MEMCOMP_MAX_COMP_CTXS > BITS_PER_LONG);

	/* Listen first, so a registration racing the check is not missed. */
	memcomp_select.deadline = jiffies + MEMCOMP_COMPRESSOR_WAIT;
	ret = crypto_register_notifier(&memcomp_select.nb);
	if (ret)
		return ret;
	/* A compressor that is already there is set up before this returns. */
	memcomp_select_worker(&memcomp_select.work.work);
	return 0;
}
late_initcall(memcomp_init);

#ifdef CONFIG_SYSFS
static ssize_t stat_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu %llu %llu\n",
			  mc16_zero_stored_bytes(atomic64_read(&memcomp_stored_bytes)),
			  (u64)atomic64_read(&memcomp_compressed_bytes),
			  memcomp_pool_total_size());
}

static ssize_t compressor_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	ssize_t len;

	/* The name can still fall back while a late compressor is awaited. */
	mutex_lock(&memcomp_select_lock);
	len = sysfs_emit(buf, "%s\n", memcomp_compressor);
	mutex_unlock(&memcomp_select_lock);
	return len;
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
/* The span upkeep part of execution_events, appended at @len. */
static ssize_t memcomp_span_events_show(char *buf, ssize_t len)
{
	len += sysfs_emit_at(buf, len,
			     "restore_spans %llu\n"
			     "restore_ptes %llu\n"
			     "restore_missed_pages %llu\n"
			     "restore_rewalks %llu\n"
			     "restore_ns %llu\n"
			     "restore_max_ns %llu\n"
			     "split_refused %llu\n"
			     "split_refused_pages %llu\n",
			     (u64)atomic64_read(&memcomp_restore_spans),
			     (u64)atomic64_read(&memcomp_restore_ptes),
			     (u64)atomic64_read(&memcomp_restore_missed_pages),
			     (u64)atomic64_read(&memcomp_restore_rewalks),
			     (u64)atomic64_read(&memcomp_restore_ns),
			     (u64)atomic64_read(&memcomp_restore_max_ns),
			     (u64)atomic64_read(&memcomp_split_refused),
			     (u64)atomic64_read(&memcomp_split_refused_pages));

	return len;
}

static ssize_t __events_show(char *buf)
{
	struct memcomp_state_stats state;
	u64 raw_candidate_ge_page;
	u64 raw_candidate_huge;
	u64 raw_reject_ge_page;
	u64 raw_reject_huge;
	u64 raw_reject_net;
	u64 raw_reject_zs_fallback;
	u64 incompressible;
	u64 raw_reject;
	u64 refresh_preflight_fail_pages;
	u64 refresh_folio_busy_fail_pages;
	u64 shmem_eligibility_fail_pages;
	u64 reclaim_ctx_borrow_limit_pages;
	u64 reclaim_ctx_borrow_waiter_pages;
	u64 reclaim_ctx_borrow_pages;
	u64 reclaim_ctx_busy_reserved_free_pages;
	u64 reclaim_ctx_busy_all_busy_pages;
	unsigned long comp_ctx_flags;
	unsigned int comp_ctx_borrowed_active;
	unsigned int comp_ctx_borrowed_peak;
	unsigned int comp_ctx_borrow_limit;
	unsigned int comp_ctx_free;
	unsigned int comp_ctx_waiters;

	memcomp_state_read(&state);
	{
		struct mc16_zero_owners zero;

		mc16_zero_owners_read(&zero);
		state.samefill_pages += max_t(s64, zero.pte_pages, 0) +
			max_t(s64, zero.cache_pages, 0);
		state.shmem_pages += max_t(s64, zero.cache_pages, 0);
	}
	raw_candidate_huge = atomic64_read(&memcomp_raw_candidate_huge);
	raw_candidate_ge_page =
		atomic64_read(&memcomp_raw_candidate_ge_page);
	raw_reject_huge = atomic64_read(&memcomp_raw_reject_huge);
	raw_reject_ge_page = atomic64_read(&memcomp_raw_reject_ge_page);
	raw_reject_zs_fallback =
		atomic64_read(&memcomp_raw_reject_zs_fallback);
	raw_reject_net = atomic64_read(&memcomp_raw_reject_net);
	incompressible = raw_candidate_huge + raw_candidate_ge_page;
	raw_reject = raw_reject_huge + raw_reject_ge_page +
		     raw_reject_zs_fallback + raw_reject_net;
	refresh_preflight_fail_pages =
		atomic64_read(&memcomp_refresh_preflight_fail_pages);
	refresh_folio_busy_fail_pages =
		atomic64_read(&memcomp_refresh_folio_busy_fail_pages);
	shmem_eligibility_fail_pages =
		atomic64_read(&memcomp_shmem_eligibility_fail_pages);
	reclaim_ctx_busy_reserved_free_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_reserved_free_pages);
	reclaim_ctx_busy_all_busy_pages =
		atomic64_read(&memcomp_reclaim_ctx_busy_all_busy_pages);
	reclaim_ctx_borrow_pages =
		atomic64_read(&memcomp_reclaim_ctx_borrow_pages);
	reclaim_ctx_borrow_limit_pages =
		atomic64_read(&memcomp_reclaim_ctx_borrow_limit_pages);
	reclaim_ctx_borrow_waiter_pages =
		atomic64_read(&memcomp_reclaim_ctx_borrow_waiter_pages);
	spin_lock_irqsave(&memcomp_comp_ctx_lock, comp_ctx_flags);
	comp_ctx_free = hweight_long(memcomp_comp_ctx_free);
	comp_ctx_borrowed_active = memcomp_comp_ctx_borrowed_active;
	comp_ctx_borrowed_peak = memcomp_comp_ctx_borrowed_peak;
	comp_ctx_borrow_limit = memcomp_nr_borrowable_comp_ctxs;
	comp_ctx_waiters = memcomp_comp_ctx_waiters;
	spin_unlock_irqrestore(&memcomp_comp_ctx_lock, comp_ctx_flags);
	return sysfs_emit(buf,
			  "enabled %u\n"
			  "comp_ctx_count %u\n"
			  "comp_ctx_reserved %u\n"
			  "comp_ctx_borrow_limit %u\n"
			  "comp_ctx_free %u\n"
			  "comp_ctx_wait %llu\n"
			  "comp_ctx_waiters %u\n"
			  "comp_ctx_borrowed_active %u\n"
			  "comp_ctx_borrowed_peak %u\n"
			  "ctx_busy %llu\n"
			  "reclaim_ctx_busy_pages %llu\n"
			  "reclaim_ctx_busy_reserved_free_pages %llu\n"
			  "reclaim_ctx_busy_all_busy_pages %llu\n"
			  "reclaim_ctx_borrow_pages %llu\n"
			  "reclaim_ctx_borrow_limit_pages %llu\n"
			  "reclaim_ctx_borrow_waiter_pages %llu\n"
			  "compress_fail %llu\n"
			  "incompressible %llu\n"
			  "zs_fast_fail %llu\n"
			  "zs_retry %llu\n"
			  "zs_retry_success %llu\n"
			  "zs_retry_fail %llu\n"
			  "zs_fail %llu\n"
			  "zs_alloc_other_fail %llu\n"
			  "zs_huge_class_size %u\n"
			  "raw_store %llu\n"
			  "raw_candidate_huge_class %llu\n"
			  "raw_candidate_ge_page %llu\n"
			  "raw_reject %llu\n"
			  "raw_reject_huge_class %llu\n"
			  "raw_reject_ge_page %llu\n"
			  "raw_reject_zs_fallback %llu\n"
			  "raw_reject_net %llu\n"
			  "reserve_entry_alloc_fail %llu\n"
			  "refresh_state_fail %llu\n"
			  "refresh_payload_fail %llu\n"
			  "anon_unmap_fail_pages %llu\n"
			  "refresh_preflight_fail_pages %llu\n"
			  "refresh_folio_busy_fail_pages %llu\n"
			  "refresh_arch_fail_pages %llu\n"
			  "refresh_state_fail_pages %llu\n"
			  "refresh_payload_fail_pages %llu\n"
			  "anon_finish_fail_pages %llu\n"
			  "incompressible_marked_pages %llu\n"
			  "incompressible_changed_pages %llu\n"
			  "incompressible_folio_skip_pages %llu\n"
			  "shmem_store_ok_pages %llu\n"
			  "shmem_store_fail_pages %llu\n"
			  "shmem_eligibility_fail_pages %llu\n"
			  "shmem_payload_fail_pages %llu\n"
			  "shmem_arch_fail_pages %llu\n"
			  "folio_owner_pages %lld\n"
			  "samefill_pages %lld\n"
			  "raw_pages %lld\n"
			  "shmem_pages %lld\n"
			  "pending_pages %llu\n"
			  "bg_store_ok %llu\n"
			  "bg_store_fail %llu\n"
			  "direct_store_ok %llu\n"
			  "direct_store_fail %llu\n"
			  "pageout_store_ok %llu\n"
			  "pageout_store_fail %llu\n"
			  "swapped_read_io %llu\n"
			  "swapped_read_error %llu\n"
			  "swapped_read_cached %llu\n"
			  "swapped_entries %lu\n"
			  "swapoff_entries %llu\n"
			  "swapoff_read_io %llu\n"
			  "swapoff_fail %llu\n",
			  memcomp_is_enabled(),
			  READ_ONCE(memcomp_nr_comp_ctxs),
			  READ_ONCE(memcomp_nr_reserved_comp_ctxs),
			  comp_ctx_borrow_limit,
			  comp_ctx_free,
			  (u64)atomic64_read(&memcomp_comp_ctx_waits),
			  comp_ctx_waiters,
			  comp_ctx_borrowed_active,
			  comp_ctx_borrowed_peak,
			  (u64)atomic64_read(&memcomp_ctx_busy),
			  reclaim_ctx_busy_reserved_free_pages +
				reclaim_ctx_busy_all_busy_pages,
			  reclaim_ctx_busy_reserved_free_pages,
			  reclaim_ctx_busy_all_busy_pages,
			  reclaim_ctx_borrow_pages,
			  reclaim_ctx_borrow_limit_pages,
			  reclaim_ctx_borrow_waiter_pages,
			  (u64)atomic64_read(&memcomp_compress_fail),
			  incompressible,
			  (u64)atomic64_read(&memcomp_zs_fast_fail),
			  (u64)atomic64_read(&memcomp_zs_retries),
			  (u64)atomic64_read(&memcomp_zs_retry_success),
			  (u64)atomic64_read(&memcomp_zs_retry_fail),
			  (u64)atomic64_read(&memcomp_zs_fail),
			  (u64)atomic64_read(&memcomp_zs_other_fail),
			  memcomp_pool_huge_class_size(),
			  (u64)atomic64_read(&memcomp_raw_store),
			  raw_candidate_huge,
			  raw_candidate_ge_page,
			  raw_reject,
			  raw_reject_huge,
			  raw_reject_ge_page,
			  raw_reject_zs_fallback,
			  raw_reject_net,
			  (u64)atomic64_read(&memcomp_reserve_entry_alloc_fail),
			  (u64)atomic64_read(&memcomp_refresh_state_fail),
			  (u64)atomic64_read(&memcomp_refresh_payload_fail),
			  (u64)atomic64_read(&memcomp_anon_unmap_fail_pages),
			  refresh_preflight_fail_pages,
			  refresh_folio_busy_fail_pages,
			  (u64)atomic64_read(&memcomp_refresh_arch_fail_pages),
			  (u64)atomic64_read(&memcomp_refresh_state_fail_pages),
			  (u64)atomic64_read(&memcomp_refresh_payload_fail_pages),
			  (u64)atomic64_read(&memcomp_anon_finish_fail_pages),
			  (u64)atomic64_read(&memcomp_incompressible_marked_pages),
			  (u64)atomic64_read(&memcomp_incompressible_changed_pages),
			  (u64)atomic64_read(&memcomp_incompressible_folio_skip_pages),
			  (u64)atomic64_read(&memcomp_shmem_store_ok_pages),
			  (u64)atomic64_read(&memcomp_shmem_store_fail_pages),
			  shmem_eligibility_fail_pages,
			  (u64)atomic64_read(&memcomp_shmem_payload_fail_pages),
			  (u64)atomic64_read(&memcomp_shmem_arch_fail_pages),
			  state.folio_owner_pages,
			  state.samefill_pages,
			  state.raw_pages,
			  state.shmem_pages,
			  (u64)atomic64_read(&memcomp_pending_pages),
			  (u64)atomic64_read(&memcomp_bg_store_ok),
			  (u64)atomic64_read(&memcomp_bg_store_fail),
			  (u64)atomic64_read(&memcomp_direct_store_ok),
			  (u64)atomic64_read(&memcomp_direct_store_fail),
			  (u64)atomic64_read(&memcomp_pageout_store_ok),
			  (u64)atomic64_read(&memcomp_pageout_store_fail),
			  (u64)atomic64_read(&memcomp_swapped_read_io),
			  (u64)atomic64_read(&memcomp_swapped_read_error),
			  (u64)atomic64_read(&memcomp_swapped_read_cached),
			  atomic_long_read(&memcomp_swapped_entries),
			  (u64)atomic64_read(&memcomp_swapoff_entries),
			  (u64)atomic64_read(&memcomp_swapoff_read_io),
			  (u64)atomic64_read(&memcomp_swapoff_fail));
}

static ssize_t events_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	return __events_show(buf);
}

#endif /* CONFIG_MEMCOMP_DEBUG_STATS */

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static ssize_t placement_events_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	u64 stats[MEMCOMP_PLACEMENT_STAT_NR];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(stats); i++)
		stats[i] = memcomp_placement_stat_read(i);

	return sysfs_emit(buf,
			  "compress %llu\n"
			  "normal_swap_fallback %llu\n",
			  stats[MEMCOMP_PLACEMENT_STAT_COMPRESS],
			  stats[MEMCOMP_PLACEMENT_STAT_NORMAL_SWAP_FALLBACK]);
}

/*
 * The swap-backed store has no descriptor state left to report - a slot
 * maps to exactly one entry and frees with it - so what remains here is
 * the writeback control loop and its outcomes.
 */
#endif

#ifdef CONFIG_MEMCOMP_DEBUG_STATS

static ssize_t
memcomp_context_bucket_emit(char *buf, ssize_t at, const char *prefix,
			    const struct memcomp_ctx_timing_stats *stats)
{
	return at + sysfs_emit_at(buf, at,
			     "%s_samples %llu\n"
			     "%s_pages %llu\n"
			     "%s_store_samples %llu\n"
			     "%s_store_success %llu\n"
			     "%s_lease_ns %llu\n"
			     "%s_lease_max_ns %llu\n"
			     "%s_pre_store_ns %llu\n"
			     "%s_pre_store_max_ns %llu\n"
			     "%s_compress_ns %llu\n"
			     "%s_compress_max_ns %llu\n"
			     "%s_compress_cpu_ns %llu\n"
			     "%s_compress_call_cpu_max_ns %llu\n"
			     "%s_compress_store_success_ns %llu\n"
			     "%s_compress_store_success_cpu_ns %llu\n"
			     "%s_compress_store_fail_ns %llu\n"
			     "%s_compress_store_fail_cpu_ns %llu\n"
			     "%s_zs_ns %llu\n"
			     "%s_zs_max_ns %llu\n"
			     "%s_store_other_ns %llu\n"
			     "%s_store_other_max_ns %llu\n",
			     prefix, (u64)atomic64_read(&stats->samples),
			     prefix, (u64)atomic64_read(&stats->pages),
			     prefix, (u64)atomic64_read(&stats->store_samples),
			     prefix, (u64)atomic64_read(&stats->store_success),
			     prefix, (u64)atomic64_read(&stats->lease_ns),
			     prefix, (u64)atomic64_read(&stats->lease_max_ns),
			     prefix, (u64)atomic64_read(&stats->pre_store_ns),
			     prefix, (u64)atomic64_read(&stats->pre_store_max_ns),
			     prefix, (u64)atomic64_read(&stats->compress_ns),
			     prefix, (u64)atomic64_read(&stats->compress_max_ns),
			     prefix, (u64)atomic64_read(&stats->compress_cpu_ns),
			     prefix,
			     (u64)atomic64_read(&stats->compress_call_cpu_max_ns),
			     prefix,
			     (u64)atomic64_read(&stats->compress_store_success_ns),
			     prefix,
			     (u64)atomic64_read(&stats->compress_store_success_cpu_ns),
			     prefix,
			     (u64)atomic64_read(&stats->compress_store_fail_ns),
			     prefix,
			     (u64)atomic64_read(&stats->compress_store_fail_cpu_ns),
			     prefix, (u64)atomic64_read(&stats->zs_ns),
			     prefix, (u64)atomic64_read(&stats->zs_max_ns),
			     prefix, (u64)atomic64_read(&stats->store_other_ns),
			     prefix, (u64)atomic64_read(&stats->store_other_max_ns));
}

static ssize_t memcomp_outcome_emit(char *buf, ssize_t at,
				    const char *prefix, const char *name,
					const struct memcomp_compress_timing_stats *stats)
{
	return at + sysfs_emit_at(buf, at,
			     "%s_compress_%s_calls %llu\n"
			     "%s_compress_%s_ns %llu\n"
			     "%s_compress_%s_cpu_ns %llu\n",
			     prefix, name, (u64)atomic64_read(&stats->calls),
			     prefix, name, (u64)atomic64_read(&stats->wall_ns),
			     prefix, name, (u64)atomic64_read(&stats->cpu_ns));
}

static ssize_t
memcomp_outcomes_emit(char *buf, ssize_t at, const char *prefix,
		      const struct memcomp_ctx_timing_stats *stats)
{
	const struct memcomp_compress_timing_stats *outcome;
	unsigned int index;

	index = MEMCOMP_COMPRESS_TIMING_COMPRESSIBLE;
	outcome = &stats->compress_outcome[index];
	at = memcomp_outcome_emit(buf, at, prefix, "compressible", outcome);
	outcome = &stats->compress_outcome[MEMCOMP_COMPRESS_TIMING_RAW_HUGE];
	at = memcomp_outcome_emit(buf, at, prefix, "raw_huge", outcome);
	index = MEMCOMP_COMPRESS_TIMING_RAW_GE_PAGE;
	outcome = &stats->compress_outcome[index];
	at = memcomp_outcome_emit(buf, at, prefix, "raw_ge_page", outcome);
	outcome = &stats->compress_outcome[MEMCOMP_COMPRESS_TIMING_ERROR];
	at = memcomp_outcome_emit(buf, at, prefix, "error", outcome);
	return at;
}

static ssize_t context_events_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	const struct memcomp_ctx_timing_stats *stats;
	ssize_t at;

	at = sysfs_emit(buf,
			"sample_interval %u\n"
			"wait_events %llu\n"
			"wait_ns %llu\n"
			"wait_max_ns %llu\n",
			MEMCOMP_CTX_TIMING_SAMPLE_INTERVAL,
			(u64)atomic64_read(&memcomp_comp_ctx_waits),
			(u64)atomic64_read(&memcomp_comp_ctx_wait_ns),
			(u64)atomic64_read(&memcomp_comp_ctx_wait_max_ns));
	stats = &memcomp_ctx_timing[MEMCOMP_CTX_TIMING_DIRECT];
	at = memcomp_context_bucket_emit(buf, at, "direct", stats);
	at = memcomp_outcomes_emit(buf, at, "direct", stats);
	stats = &memcomp_ctx_timing[MEMCOMP_CTX_TIMING_BACKGROUND];
	at = memcomp_context_bucket_emit(buf, at, "bg", stats);
	at = memcomp_outcomes_emit(buf, at, "bg", stats);
	return at;
}

static ssize_t
memcomp_context_source_emit(char *buf, ssize_t at, const char *prefix,
			    const struct memcomp_ctx_timing_stats *stats,
				const struct memcomp_ctx_wait_stats *wait)
{
	return at + sysfs_emit_at(buf, at,
			     "%s_samples %llu\n"
			     "%s_pages %llu\n"
			     "%s_store_samples %llu\n"
			     "%s_store_success %llu\n"
			     "%s_wait_events %llu\n"
			     "%s_wait_ns %llu\n"
			     "%s_wait_max_ns %llu\n"
			     "%s_lease_ns %llu\n"
			     "%s_lease_max_ns %llu\n"
			     "%s_pre_store_ns %llu\n"
			     "%s_pre_store_max_ns %llu\n"
			     "%s_compress_ns %llu\n"
			     "%s_compress_max_ns %llu\n"
			     "%s_compress_cpu_ns %llu\n"
			     "%s_compress_call_cpu_max_ns %llu\n"
			     "%s_zs_ns %llu\n"
			     "%s_zs_max_ns %llu\n"
			     "%s_store_other_ns %llu\n"
			     "%s_store_other_max_ns %llu\n",
			     prefix, (u64)atomic64_read(&stats->samples),
			     prefix, (u64)atomic64_read(&stats->pages),
			     prefix, (u64)atomic64_read(&stats->store_samples),
			     prefix, (u64)atomic64_read(&stats->store_success),
			     prefix, (u64)atomic64_read(&wait->events),
			     prefix, (u64)atomic64_read(&wait->ns),
			     prefix, (u64)atomic64_read(&wait->max_ns),
			     prefix, (u64)atomic64_read(&stats->lease_ns),
			     prefix, (u64)atomic64_read(&stats->lease_max_ns),
			     prefix, (u64)atomic64_read(&stats->pre_store_ns),
			     prefix, (u64)atomic64_read(&stats->pre_store_max_ns),
			     prefix, (u64)atomic64_read(&stats->compress_ns),
			     prefix, (u64)atomic64_read(&stats->compress_max_ns),
			     prefix, (u64)atomic64_read(&stats->compress_cpu_ns),
			     prefix,
			     (u64)atomic64_read(&stats->compress_call_cpu_max_ns),
			     prefix, (u64)atomic64_read(&stats->zs_ns),
			     prefix, (u64)atomic64_read(&stats->zs_max_ns),
			     prefix, (u64)atomic64_read(&stats->store_other_ns),
			     prefix, (u64)atomic64_read(&stats->store_other_max_ns));
}

static ssize_t context_sources_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	const struct memcomp_ctx_timing_stats *stats;
	const struct memcomp_ctx_wait_stats *wait;
	ssize_t at;

	at = sysfs_emit(buf, "sample_interval %u\n",
			MEMCOMP_CTX_TIMING_SAMPLE_INTERVAL);
	stats = &memcomp_ctx_source_timing[MEMCOMP_RECLAIM_DIRECT];
	wait = &memcomp_ctx_source_wait[MEMCOMP_RECLAIM_DIRECT];
	at = memcomp_context_source_emit(buf, at, "direct", stats, wait);
	stats = &memcomp_ctx_source_timing[MEMCOMP_RECLAIM_KSWAPD];
	wait = &memcomp_ctx_source_wait[MEMCOMP_RECLAIM_KSWAPD];
	at = memcomp_context_source_emit(buf, at, "kswapd", stats, wait);
	return at;
}

static ssize_t __execution_events_show(char *buf)
{
	atomic64_t *budget_waits = memcomp_comp_budget_waits;
	struct memcomp_comp_budget budget;
	u64 refresh_cross_node_pages = 0;
	u64 budget_pages;
	u64 budget_reject;
	u64 budget_reject_pages;
	u64 kswapd_wait;
	unsigned int staging_active;
	unsigned int staging_count;
	unsigned int staging_free;
	unsigned int staging_peak;
	unsigned int limit;
	unsigned int waiters;
	int cpu;
	unsigned long flags;

	spin_lock_irqsave(&memcomp_comp_ctx_lock, flags);
	budget = memcomp_comp_budget;
	limit = memcomp_current_comp_budget_limit();
	waiters = memcomp_comp_ctx_waiters;
	spin_unlock_irqrestore(&memcomp_comp_ctx_lock, flags);
	spin_lock_irqsave(&memcomp_direct_staging_lock, flags);
	staging_count = memcomp_nr_direct_staging;
	staging_free = bitmap_weight(&memcomp_direct_staging_free,
				     staging_count);
	staging_active = memcomp_direct_staging_active;
	staging_peak = memcomp_direct_staging_peak;
	spin_unlock_irqrestore(&memcomp_direct_staging_lock, flags);
	budget_pages = atomic64_read(&memcomp_reclaim_ctx_budget_pages);
	budget_reject = atomic64_read(&memcomp_reclaim_ctx_budget_reject);
	budget_reject_pages =
		atomic64_read(&memcomp_reclaim_ctx_budget_reject_pages);
	kswapd_wait = atomic64_read(&budget_waits[MEMCOMP_RECLAIM_KSWAPD]);
	for_each_possible_cpu(cpu)
		refresh_cross_node_pages +=
			READ_ONCE(per_cpu(mc16_refresh_cross_node_pages, cpu));

	return sysfs_emit(buf,
			  "comp_budget_limit %u\n"
			  "comp_budget_background_limit %u\n"
			  "comp_budget_direct_limit %u\n"
			  "comp_budget_active %u\n"
			  "comp_budget_background_active %u\n"
			  "comp_budget_direct_active %u\n"
			  "comp_budget_peak %u\n"
			  "comp_budget_background_peak %u\n"
			  "comp_budget_direct_peak %u\n"
			  "comp_budget_waiters %u\n"
			  "reclaim_ctx_budget_pages %llu\n"
			  "direct_budget_reject %llu\n"
			  "direct_budget_reject_pages %llu\n"
			  "kswapd_budget_wait %llu\n"
			  "direct_staging_count %u\n"
			  "direct_staging_free %u\n"
			  "direct_staging_active %u\n"
			  "direct_staging_peak %u\n"
			  "direct_staging_miss %llu\n"
			  "direct_staging_detach %llu\n"
			  "refresh_cross_node_pages %llu\n",
			  limit, limit,
			  MEMCOMP_DIRECT_COMP_BUDGET,
			  budget.active, budget.background_active,
			  0U, budget.peak,
			  budget.background_peak, 0U, waiters,
			  budget_pages, budget_reject, budget_reject_pages,
			  kswapd_wait,
			  staging_count, staging_free, staging_active,
			  staging_peak,
			  (u64)atomic64_read(&memcomp_direct_staging_miss),
			  (u64)atomic64_read(&memcomp_direct_staging_detach),
			  refresh_cross_node_pages);
}

static ssize_t mc16_metadata_events_show(char *buf, ssize_t at)
{
	unsigned long retained = READ_ONCE(mc16_retained_empty_headers);
	u64 count_calls = 0, scan_calls = 0, detached = 0;
	u64 aging_calls = 0, aging_detached = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct mc16_storage_debug *debug;

		debug = per_cpu_ptr(&mc16_storage_debug, cpu);
		count_calls += READ_ONCE(debug->meta_count_calls);
		scan_calls += READ_ONCE(debug->meta_scan_calls);
		detached += READ_ONCE(debug->meta_detached);
		aging_calls += READ_ONCE(debug->aging_work_calls);
		aging_detached += READ_ONCE(debug->aging_detached);
	}
	return at + sysfs_emit_at(buf, at,
		"meta_shrinker_registered %u\n"
		"meta_shrinker_count_calls %llu\n"
		"meta_shrinker_scan_calls %llu\n"
		"meta_shrinker_detached_headers %llu\n"
		"retained_empty_headers %lu\n"
		"retained_empty_header_bytes %lu\n"
		"aging_work_calls %llu\n"
		"aging_detached_headers %llu\n",
		!!memcomp_meta_shrinker, count_calls, scan_calls, detached,
		retained, retained * kmalloc_size_roundup(sizeof(struct mc16_cluster)),
		aging_calls, aging_detached);
}

static ssize_t execution_events_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return mc16_metadata_events_show(buf,
		memcomp_span_events_show(buf, __execution_events_show(buf)));
}

static ssize_t admission_events_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	u64 early_reject = atomic64_read(&memcomp_reclaim_ctx_early_reject);
	u64 early_reject_pages =
		atomic64_read(&memcomp_reclaim_ctx_early_reject_pages);

	return sysfs_emit(buf,
			  "reclaim_ctx_early_reject %llu\n"
			  "reclaim_ctx_early_reject_pages %llu\n",
			  early_reject, early_reject_pages);
}
#endif

static struct kobj_attribute stat_attr = __ATTR_RO(stat);
static struct kobj_attribute compressor_attr = __ATTR_RO(compressor);
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static struct kobj_attribute placement_events_attr =
	__ATTR_RO(placement_events);
static struct kobj_attribute events_attr = __ATTR_RO(events);
static struct kobj_attribute admission_events_attr =
	__ATTR_RO(admission_events);
static struct kobj_attribute context_events_attr = __ATTR_RO(context_events);
static struct kobj_attribute context_sources_attr = __ATTR_RO(context_sources);
static struct kobj_attribute execution_events_attr =
	__ATTR_RO(execution_events);
#endif

static struct attribute *memcomp_attrs[] = {
	&stat_attr.attr,
	&compressor_attr.attr,
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	&placement_events_attr.attr,
	&events_attr.attr,
	&admission_events_attr.attr,
	&context_events_attr.attr,
	&context_sources_attr.attr,
	&execution_events_attr.attr,
#endif
	NULL,
};

static const struct attribute_group memcomp_attr_group = {
	.name = "memcomp",
	.attrs = memcomp_attrs,
};

static int __init memcomp_sysfs_init(void)
{
	if (!mm_kobj)
		return -EINVAL;

	return sysfs_create_group(mm_kobj, &memcomp_attr_group);
}
subsys_initcall(memcomp_sysfs_init);
#endif
