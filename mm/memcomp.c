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

#include <crypto/acompress.h>
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
#include <linux/scatterlist.h>
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
#define MEMCOMP_WRITEBACK_BATCH	SWAP_CLUSTER_MAX
#define MEMCOMP_WRITEBACK_FEEDBACK_MIN_BYTES \
	(MEMCOMP_WRITEBACK_BATCH * PAGE_SIZE)
/*
 * A severe scope may migrate a bounded share of the node's in-pool cold
 * payloads, freeing about the same share of the zsmalloc pool footprint.
 * Draining a share of what remains is self-limiting: the pool can never
 * be emptied by writeback alone.
 *
 * The share is not a threshold but the state of a negative feedback loop,
 * closed through the zsmalloc pool footprint - the one quantity writing a payload
 * back actually reduces.  Its error signal is the kernel's own: a scope
 * only opens when the node is below its low watermark, and the loop asks
 * whether the pool shrank while the previous share was in force.  Nothing
 * here names a pool size, a device size or a byte threshold; the share is
 * whatever sustained pressure has driven it to, and it decays back to
 * nothing once the pool is holding.
 *
 * Only the two gains are constants: how fast an unhelped scope raises the
 * share, and the ceiling of half the cold payloads that it cannot pass.
 */
#define MEMCOMP_WRITEBACK_FRAC_SHIFT	10
#define MEMCOMP_WRITEBACK_FRAC_ONE \
	((unsigned int)BIT(MEMCOMP_WRITEBACK_FRAC_SHIFT))
#define MEMCOMP_WRITEBACK_DRAIN_STEP	(MEMCOMP_WRITEBACK_FRAC_ONE / 64)
/*
 * Evidence the loop must accumulate before it grants anything.  A boot or a
 * short allocation burst opens a scope or two and then the node recovers;
 * that is not the pool outgrowing its welcome, and draining for it writes
 * back pages the pool was holding perfectly well.  The share has to clear
 * the deadband first, which takes a few scopes of unrelieved pressure.
 *
 * Set against how long a recovered node keeps: together they decide when a
 * growing pool starts being drained.  Too much of either and a workload has
 * to build gigabytes before anything moves, because the intermittent
 * recoveries of the ramp keep knocking the share back down.
 */
#define MEMCOMP_WRITEBACK_DRAIN_DEADBAND	MEMCOMP_WRITEBACK_DRAIN_STEP
/* Ceiling sits a deadband above half, so the top of the ramp still grants half. */
#define MEMCOMP_WRITEBACK_DRAIN_MAX		(MEMCOMP_WRITEBACK_FRAC_ONE / 2 + \
						 MEMCOMP_WRITEBACK_DRAIN_DEADBAND)
/*
 * What a recovered node keeps, so transient pressure cannot accumulate into
 * writeback while still letting the ramp of a real workload build.
 */
#define MEMCOMP_WRITEBACK_DRAIN_RECOVER_SHIFT	1
/*
 * How much physical memory the compressed pool may occupy, in permille of
 * total RAM, before any of it is written out.  Below this the pool keeps
 * everything and compression carries the load on its own; above it the excess
 * is what the level asks to drain.  This is the setpoint of the level term
 * rather than a pool or device size - see memcomp_writeback_level_floor().
 *
 * At 15% of RAM the pool still holds the larger part of what it compressed
 * while a workload that fills it moves roughly half the payload out.  The
 * loop settles within a few percent of this, so it is the number that decides
 * the split, and it is the only one to change if the pool should keep more or
 * less.
 */
#define MEMCOMP_WRITEBACK_LEVEL_PERMILLE		150
/*
 * The gate in front of all of that: how much uncompressed memory the pool
 * holds, in permille of total RAM, before writeback runs at all.
 *
 * The loop and the level floor decide how fast to drain once draining is
 * wanted; neither decides whether it is.  The loop steps up on any scope
 * whose pool did not shrink, and between the back-to-back shrink_node()
 * calls of one kswapd wakeup the pool always grows, because reclaim is
 * what fills it.  So every wakeup of a few calls raises the share, and a
 * device that goes through a few such wakeups an hour, with a pool that
 * never gets near the level, writes back a slice of it each time.  Over a
 * day that swaps out most of what the pool holds.  The level floor cannot
 * stop that either: it is max()ed into the share, so it can raise the
 * share but never lower it.
 *
 * So the gate asks the question separately, on the one number the user
 * sees as "how much is compressed": the entries in the pool.  Writeback
 * moves those one for one, unlike the stored total, so the gate sees its
 * own effect.  It opens at two thirds of RAM and closes at three fifths,
 * and while closed nothing is granted, nothing is carried over, and
 * the loop is reset so that the next opening starts from reset state.
 * While open, every grant, quota and writeback worker pass is
 * capped at the entries above the close point, so writeback can never
 * take the pool below it however long the loop has been winding up.
 *
 * The footprint term is a backstop for incompressible data: at worse than
 * about 2.7:1 the pool's footprint reaches a quarter of RAM before the
 * entries reach two thirds, and the gate opens on that instead.
 */
#define MEMCOMP_WRITEBACK_GATE_START_PERMILLE	667
#define MEMCOMP_WRITEBACK_GATE_STOP_PERMILLE	600
#define MEMCOMP_WRITEBACK_GATE_FP_START_PERMILLE	250
#define MEMCOMP_WRITEBACK_GATE_FP_STOP_PERMILLE	200
/*
 * Pace the off-reclaim drain so it interleaves with whatever the device is
 * doing instead of monopolising the storage right after a workload exits.
 */
#define MEMCOMP_WRITEBACK_DRAIN_DELAY	msecs_to_jiffies(200)
/* Slices the off-reclaim drain spends its quota in, to bound one pass. */
#define MEMCOMP_WRITEBACK_DRAIN_SLICES	16UL
/*
 * And an absolute bound on one pass, whatever the quota.  Each entry is one
 * page-sized swap write; a pass submits them in MEMCOMP_WRITEBACK_BATCH
 * asynchronous groups and waits once per group, so it costs about
 * limit/MEMCOMP_WRITEBACK_BATCH blocking waits with the device kept at
 * batch depth in between.  At this cap that is a handful of waits - a few
 * milliseconds against the delay between passes, leaving most of the device
 * for whoever is using it.
 */
#define MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX	512UL
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

/*
 * A compressor with an asynchronous form compresses a foreground run
 * of folios together: reclaim hands all of them over, waits once, and
 * stores each with the payloads already made.  One request per page of the
 * run, a folio's pages in a row; see memcomp_async_submit().
 */
struct memcomp_async_req {
	struct acomp_req *req;
	struct memcomp_async *async;
	struct scatterlist src;
	struct scatterlist dst;
	void *buf;
	/* The folio whose page this is, at its offset from its first. */
	struct folio *folio;
	unsigned int len;
	int ret;
	/* Set only by memcomp's full software scan, never by a codec error. */
	bool same_filled;
	/* The value the software scan found. */
	unsigned long element;
};

struct memcomp_async {
	/* Requests not yet done, plus one until the batch is waited for. */
	atomic_t pending;
	struct completion done;
	unsigned int id;
	/* Requests taken, from the first. */
	unsigned int nr;
	struct memcomp_async_req reqs[MEMCOMP_ASYNC_PAGES];
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
	/*
	 * The payloads, made before the store from the same contents: the
	 * request for the folio's first page, followed by one for each other.
	 */
	const struct memcomp_async_req *precomp;
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

static struct shrinker *memcomp_shrinker;
static struct shrinker *memcomp_meta_shrinker;

#ifdef CONFIG_SWAP
static struct delayed_work memcomp_writeback_drain_work;

/*
 * The writepage plug handed to __swap_writepage().  On a swapfile
 * (SWP_FS_OPS) it coalesces the batch's writes into direct-I/O chains
 * instead of submitting one kiocb per page; on a block device the writes
 * are already separate bios and the plug is unused.  It lives as long as
 * the batch does and is unplugged by the flush - or by swapoff, which
 * takes the same lock - before any of the folios are waited on.
 */
static struct swap_iocb *memcomp_writeback_plugged;

static void memcomp_writeback_drain_schedule(int nid);
static void memcomp_writeback_drain_worker(struct work_struct *work);
static bool memcomp_writeback_defrag_due(void);
static bool memcomp_writeback_defrag_pass(void);
/* Set by a gate-closed period end with swap short; see the defrag below. */
static bool memcomp_writeback_defrag_armed;
#endif

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
/* Serializes compressor selection and publication of an async pool. */
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
static atomic_t memcomp_writeback_scopes[MAX_NUMNODES];
static atomic_t memcomp_writeback_stopped[MAX_NUMNODES];
static spinlock_t memcomp_writeback_scope_locks[MAX_NUMNODES];
static int memcomp_writeback_reclaim_idx[MAX_NUMNODES];
static u64 memcomp_writeback_stored_bytes[MAX_NUMNODES];
static u64 memcomp_writeback_pool_bytes[MAX_NUMNODES];
static bool memcomp_writeback_feedback_valid[MAX_NUMNODES];
/* Loop state: share of cold payloads a scope may drain, in 1/1024ths. */
static unsigned int memcomp_writeback_drain[MAX_NUMNODES];
/* Entries the off-reclaim drain still owes, settled once per period end. */
static unsigned long memcomp_writeback_carry[MAX_NUMNODES];
static unsigned long memcomp_writeback_budget[MAX_NUMNODES];
/* Level gate latch, see MEMCOMP_WRITEBACK_GATE_START_PERMILLE. */
static atomic_t memcomp_writeback_gate_open = ATOMIC_INIT(0);

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
static atomic64_t memcomp_writeback_pressure_events = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_pressure_scans = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_payloads = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_zs_reclaimed = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_zs_no_progress = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_compression_throttled = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_limited_batches = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_budget_granted = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_gate_opens = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_packed_slots = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_packed_payloads = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_defrag_moved = ATOMIC64_INIT(0);

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
static atomic64_t memcomp_async_payloads = ATOMIC64_INIT(0);
static atomic64_t memcomp_async_fail = ATOMIC64_INIT(0);
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
static atomic64_t memcomp_writeback_decompress_error = ATOMIC64_INIT(0);
static atomic64_t memcomp_writeback_write_error = ATOMIC64_INIT(0);
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

static int memcomp_slot_dup(swp_entry_t swap)
{
	return swap_duplicate(swap);
}

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
static int memcomp_slot_count(swp_entry_t swap)
{
	return __swap_count(swap);
}

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
#include "memcomp/writeback.h"

static struct memcomp_lru *memcomp_lru_nid(int nid)
{
	if (nid < 0 || nid >= MAX_NUMNODES)
		return NULL;
	return &memcomp_lrus[nid];
}

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

static unsigned long memcomp_pool_compact(void)
{
	unsigned long freed = 0;
	int i;

	if (!memcomp_is_enabled())
		return 0;

	for (i = 0; i < memcomp_nr_pools; i++)
		freed += zs_compact(memcomp_pools[i]);

	return freed;
}

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

enum memcomp_writeback_feedback {
	MEMCOMP_WRITEBACK_PENDING,
	MEMCOMP_WRITEBACK_EFFECTIVE,
	MEMCOMP_WRITEBACK_INEFFICIENT,
};

/*
 * Compare new logical payload against the real zsmalloc pool footprint accumulated
 * since this pressure scope began.  This turns current compression efficacy
 * into the feedback signal: zsmalloc pool that is not growing, or grows by at most
 * half of the reclaimed source bytes, gets another compression-first pass.
 */
static enum memcomp_writeback_feedback
memcomp_writeback_feedback_values(u64 stored_start, u64 pool_start,
				  u64 stored_bytes, u64 pool_bytes)
{
	u64 stored_growth;
	u64 pool_growth;

	if (stored_bytes <= stored_start)
		return MEMCOMP_WRITEBACK_PENDING;

	stored_growth = stored_bytes - stored_start;
	/* Do not mistake one zsmalloc page allocation for poor compression. */
	if (stored_growth < MEMCOMP_WRITEBACK_FEEDBACK_MIN_BYTES)
		return MEMCOMP_WRITEBACK_PENDING;
	if (pool_bytes <= pool_start)
		return MEMCOMP_WRITEBACK_EFFECTIVE;
	pool_growth = pool_bytes - pool_start;
	if (pool_growth <= stored_growth / 2)
		return MEMCOMP_WRITEBACK_EFFECTIVE;

	return MEMCOMP_WRITEBACK_INEFFICIENT;
}

/*
 * Next drain share for a node, in 1/1024ths of its cold payloads.
 *
 * This is the loop's integrator, and the loop closes through the zsmalloc pool
 * footprint: draining moves payloads out of the pool, the pool shrinks,
 * and the next severe scope backs off.  @pool_shrank is that measurement,
 * taken across the interval the previous share was in force, so it already
 * nets writeback against whatever new compression arrived meanwhile.
 *
 * Additive increase, multiplicative decrease.  A scope whose predecessor
 * failed to shrink the pool - too small a share to matter, or compression
 * outrunning writeback - raises the share by one step.
 *
 * The pool yielding is the only thing that lowers the share, and it gives
 * back a quarter: overshooting here costs a few extra swap writes rather
 * than anything structural, and handing back half the ramp after a single
 * good scope leaves the loop hovering at twice the step, too low to drain
 * a pool that keeps refilling.
 *
 * The compression sample only slows the rise.  It reports net-positive
 * whenever the pool grows by at most half of what was stored, i.e. at any
 * ratio better than 2:1 - which is the normal case for a compressed pool,
 * not an exceptional one.  Letting it force a decay pinned the share at
 * zero for exactly the workloads that grow the pool: every scope of a
 * healthy 2.5:1 workload halved a share that had never been allowed to
 * rise.  A pool earning its keep is a reason to lean on it a little
 * longer, not evidence that its footprint is harmless.
 *
 * Note a pool that merely holds steady raises the share rather than
 * lowering it: the node is below its watermark and the pool is not
 * yielding, which is what the share exists to correct.  One scope of
 * draining is enough to shrink it and start the decay.
 *
 * Rising by a step and falling by a fraction settles near the smallest
 * share that holds the pool rather than overshooting past it, and cannot
 * latch at zero: while pressure persists and the pool is not yielding,
 * every scope raises the share again.
 */
static unsigned int
memcomp_writeback_drain_next(unsigned int drain,
			     enum memcomp_writeback_feedback feedback,
				 bool pool_shrank)
{
	unsigned int step = MEMCOMP_WRITEBACK_DRAIN_STEP;

	if (pool_shrank) {
		/* Always give something back, so the decay reaches zero. */
		unsigned int give_back = max(drain / 4, 1U);

		return drain > give_back ? drain - give_back : 0;
	}

	if (feedback == MEMCOMP_WRITEBACK_EFFECTIVE)
		step /= 2;

	return min(drain + step, (unsigned int)MEMCOMP_WRITEBACK_DRAIN_MAX);
}

/*
 * Payload-entry budget for one severe scope: the part of @drain above the
 * deadband, applied to the node's in-pool cold payloads.  Once past the
 * deadband a node still holding a whole batch of them keeps a budget of at
 * least one entry, so a small share cannot round down to no writeback at
 * all while the loop is asking for some.
 */
static unsigned long
memcomp_writeback_budget_values(unsigned long cold_entries,
				unsigned int drain)
{
	unsigned long budget;

	if (drain <= MEMCOMP_WRITEBACK_DRAIN_DEADBAND)
		return 0;
	drain -= MEMCOMP_WRITEBACK_DRAIN_DEADBAND;

	budget = (unsigned long)(((u64)cold_entries * drain) >>
				 MEMCOMP_WRITEBACK_FRAC_SHIFT);
	if (!budget && cold_entries >= MEMCOMP_WRITEBACK_BATCH)
		budget = 1;

	return budget;
}

/*
 * How many entries one pass of the off-reclaim drain may move, given the
 * quota it still owes.
 *
 * The quota is spent in slices so a pass is bounded and the device stays
 * responsive while it drains.  The absolute cap is the load-bearing part: a
 * slice that is only a fraction of the quota scales with it, and the quota is
 * at its largest exactly when a memory-hungry workload has just exited - the
 * moment someone picks the device up.  Left uncapped the first pass is tens of
 * thousands of entries, which is seconds of back-to-back synchronous writes
 * with the batch lock held, repeated every period until the quota runs out.
 *
 * Never more than the cap, never more than is owed, and never zero while
 * anything is owed - a slice that rounded down to nothing would stall the
 * quota rather than spend it.
 */
static unsigned long memcomp_writeback_drain_slice(unsigned long quota)
{
	unsigned long limit;

	limit = max(quota / MEMCOMP_WRITEBACK_DRAIN_SLICES,
		    (unsigned long)MEMCOMP_WRITEBACK_BATCH);

	return min3(limit, quota,
		    (unsigned long)MEMCOMP_WRITEBACK_DRAIN_SLICE_MAX);
}

/*
 * What a node's drain share decays to when the node climbs back on its own.
 *
 * The pressure that raised the share is gone, so it should not be kept in
 * full - but a node that recovers is not the same as one that never needed
 * the share, so it is not dropped either.  Keeping half means a boot or a
 * short allocation burst, which opens a handful of scopes and then recovers,
 * settles at a bounded multiple of the burst length instead of accumulating
 * across a long uptime until an idle device starts writing back.  Real
 * pressure never reaches here and keeps everything it earned.
 */
static unsigned int memcomp_writeback_drain_recover(unsigned int drain)
{
	return drain >> MEMCOMP_WRITEBACK_DRAIN_RECOVER_SHIFT;
}

/*
 * Floor on the share, in 1/1024ths, from how far the pool's physical
 * footprint sits above the level it is allowed to occupy.
 *
 * The loop above is a rate controller: its only inputs are the sign of the
 * footprint's change and the compression ratio, and neither is a function of
 * how large the pool actually is.  Its equilibrium is therefore "writeback
 * removes as fast as compression adds", reached at whatever size the pool
 * happened to have when the rates met - a pool twice as large settles at the
 * same share as a pool half the size, because both have stopped growing.
 * That is the right shape for trimming and no shape at all for deciding how
 * much of a large pool belongs in swap.
 *
 * So state the level separately, as the share of the pool that is over the
 * line: below the target nothing is asked for, and above it the excess
 * fraction is, capped so no single scope drains more than half.  Draining
 * that share removes about that fraction of the footprint, which is why the
 * error closes - the next scope measures a smaller pool and asks for less,
 * until the pool is at the target and the level asks for nothing at all.
 *
 * The variable has to be the footprint and nothing else.  Writeback moves a
 * payload from zsmalloc pool to swap; it does not change the stored total, which only
 * entry release moves.  A level built on stored bytes therefore never sees
 * its own effect: it stays pinned at the cap and every scope asks for half of
 * what is left, which drains the pool to nothing.  The target is a fraction
 * of total RAM, so the policy still names no device or pool size.
 */
static unsigned int memcomp_writeback_level_floor_values(u64 pool_bytes,
							 u64 total_bytes)
{
	u64 target;
	u64 excess;
	u64 frac;

	if (!total_bytes || !pool_bytes)
		return 0;

	target = div64_u64(total_bytes * MEMCOMP_WRITEBACK_LEVEL_PERMILLE,
			   1000);
	if (pool_bytes <= target)
		return 0;

	/* Share of the footprint that is above the line. */
	excess = pool_bytes - target;
	frac = div64_u64(excess * MEMCOMP_WRITEBACK_FRAC_ONE, pool_bytes);
	frac = min_t(u64, frac, MEMCOMP_WRITEBACK_FRAC_ONE / 2);

	/* Stated as a share, so the deadband subtraction gives it back. */
	return (unsigned int)frac + MEMCOMP_WRITEBACK_DRAIN_DEADBAND;
}

static unsigned int memcomp_writeback_level_floor(u64 pool_bytes)
{
	return memcomp_writeback_level_floor_values(pool_bytes,
			(u64)totalram_pages() << PAGE_SHIFT);
}

/*
 * Advance the level gate's latch and return how many entries writeback may
 * still move, i.e. how far the pool sits above the close point.  Zero means
 * the gate is closed.  @in_pool is the number of entries in the pool,
 * @pool_bytes its footprint, @total_pages the RAM both are measured against.
 * Pure, so the suite can drive it with any numbers.
 *
 * Both terms state the excess in entries.  The footprint term converts its
 * byte excess at the pool's average entry size, so draining that many entries
 * removes about that many bytes; the larger of the two is what must go.
 */
static unsigned long
memcomp_writeback_gate_values(bool *open, unsigned long in_pool,
			      u64 pool_bytes, unsigned long total_pages)
{
	u64 total_bytes = (u64)total_pages << PAGE_SHIFT;
	unsigned long start, stop;
	u64 fp_start, fp_stop;
	unsigned long excess = 0;

	start = (unsigned long)div64_u64((u64)total_pages *
			MEMCOMP_WRITEBACK_GATE_START_PERMILLE, 1000);
	stop = (unsigned long)div64_u64((u64)total_pages *
			MEMCOMP_WRITEBACK_GATE_STOP_PERMILLE, 1000);
	fp_start = div64_u64(total_bytes *
			MEMCOMP_WRITEBACK_GATE_FP_START_PERMILLE, 1000);
	fp_stop = div64_u64(total_bytes *
			MEMCOMP_WRITEBACK_GATE_FP_STOP_PERMILLE, 1000);

	if (!*open && total_pages && in_pool &&
	    (in_pool >= start || pool_bytes >= fp_start))
		*open = true;
	if (!*open)
		return 0;

	if (in_pool > stop)
		excess = in_pool - stop;
	if (pool_bytes > fp_stop) {
		u64 fp_excess = div64_u64((u64)in_pool * (pool_bytes - fp_stop),
					  pool_bytes);

		excess = max_t(unsigned long, excess, (unsigned long)fp_excess);
	}
	if (!excess)
		*open = false;

	return excess;
}

static unsigned long memcomp_writeback_in_pool(void)
{
	unsigned long in_pool = 0;
	int nid;

	for_each_node_state(nid, N_MEMORY) {
		struct memcomp_lru *lru = memcomp_lru_nid(nid);

		if (lru)
			in_pool += atomic_long_read(&lru->nr_entries);
	}

	return in_pool;
}

/*
 * Re-read the level and advance the gate.  Returns @nid's part of the
 * excess - its share of the pool's entries, so that the nodes together can
 * never move more than the excess - or zero when the gate is closed.
 *
 * Only the caller that sees the gate open or close accounts for it, and the
 * per-node loop state is reset by each node itself: a grant or quota on a
 * closed gate clears its own node, and the worker clears every carry.  That
 * keeps this free of the per-node scope locks, which grants already hold.
 */
static unsigned long memcomp_writeback_gate_excess(int nid, u64 pool_bytes)
{
	struct memcomp_lru *lru = memcomp_lru_nid(nid);
	unsigned long in_pool = memcomp_writeback_in_pool();
	unsigned long node, excess;
	int was = atomic_read(&memcomp_writeback_gate_open);
	bool open = was;

	excess = memcomp_writeback_gate_values(&open, in_pool, pool_bytes,
					       totalram_pages());
	if (open != was &&
	    atomic_cmpxchg(&memcomp_writeback_gate_open, was, open) == was &&
	    open)
		memcomp_debug_inc(&memcomp_writeback_gate_opens);
	if (!excess || !lru)
		return 0;

	node = atomic_long_read(&lru->nr_entries);
	if (node >= in_pool)
		return excess;

	return (unsigned long)div64_u64((u64)excess * node, in_pool);
}

static bool memcomp_writeback_node_below_low(int nid, int reclaim_idx);

/* Caller holds the per-node scope lock. */
static void
memcomp_writeback_feedback_start_locked(int nid, u64 stored_bytes,
					u64 pool_bytes)
{
	WRITE_ONCE(memcomp_writeback_stored_bytes[nid], stored_bytes);
	WRITE_ONCE(memcomp_writeback_pool_bytes[nid], pool_bytes);
	WRITE_ONCE(memcomp_writeback_feedback_valid[nid], true);
}

/*
 * Advance the loop and grant this severe scope's payload-entry budget.
 * The measurement that closes the loop is whether the zsmalloc pool footprint
 * shrank while the previous share was in force; the compression sample
 * decides whether the pool growing was worth it.  Caller holds the
 * per-node scope lock.
 */
static void
memcomp_writeback_budget_grant_locked(int nid, u64 stored_bytes,
				      u64 pool_bytes, unsigned long excess)
{
	enum memcomp_writeback_feedback feedback =
		MEMCOMP_WRITEBACK_PENDING;
	struct memcomp_lru *lru = memcomp_lru_nid(nid);
	unsigned long cold = lru ? atomic_long_read(&lru->nr_entries) : 0;
	bool pool_shrank = false;
	unsigned long budget;
	unsigned int drain;

	/*
	 * A closed gate grants nothing and does not let the loop move: a
	 * pool below the gate is not a pool the loop has anything to say
	 * about, however the last few scopes went.  Whatever share an
	 * earlier opening earned goes with it, so the next opening starts
	 * from zero.  The sample is still taken, so that opening has one.
	 */
	if (!excess) {
		WRITE_ONCE(memcomp_writeback_drain[nid], 0);
		WRITE_ONCE(memcomp_writeback_budget[nid], 0);
		memcomp_writeback_feedback_start_locked(nid, stored_bytes,
							pool_bytes);
		return;
	}

	drain = READ_ONCE(memcomp_writeback_drain[nid]);
	if (READ_ONCE(memcomp_writeback_feedback_valid[nid])) {
		u64 stored_start;
		u64 pool_start;

		stored_start = READ_ONCE(memcomp_writeback_stored_bytes[nid]);
		pool_start = READ_ONCE(memcomp_writeback_pool_bytes[nid]);
		feedback = memcomp_writeback_feedback_values(stored_start,
							     pool_start,
								 stored_bytes,
								 pool_bytes);
		pool_shrank = pool_bytes < pool_start;
		drain = memcomp_writeback_drain_next(drain, feedback,
						     pool_shrank);
	}
	/*
	 * With no sample there is nothing to judge the last share by: the
	 * node climbed back above its low watermark since the previous scope,
	 * which discarded the sample.  Carry the loop's share over; the level
	 * below is re-derived from the stored memory as it stands, so a share
	 * left over from a workload that has since exited cannot outlive what
	 * that workload was holding.
	 */
	/*
	 * The level says how much of this pool belongs in swap; the loop only
	 * trims above it.  Below the start point the floor is zero and the
	 * share is exactly what the loop asked for, so boot and short spikes
	 * behave as before.
	 */
	drain = max(drain, memcomp_writeback_level_floor(pool_bytes));
	drain = min_t(unsigned int, drain, MEMCOMP_WRITEBACK_DRAIN_MAX);
	WRITE_ONCE(memcomp_writeback_drain[nid], drain);
	/*
	 * Above the gate's band the cap does not bind and the loop runs as
	 * it always has; near the close point it stops the scope there.
	 */
	budget = min(memcomp_writeback_budget_values(cold, drain), excess);
	WRITE_ONCE(memcomp_writeback_budget[nid], budget);
	memcomp_debug_add(budget, &memcomp_writeback_budget_granted);
	if (feedback == MEMCOMP_WRITEBACK_EFFECTIVE)
		memcomp_debug_inc(&memcomp_writeback_compression_throttled);
	memcomp_writeback_feedback_start_locked(nid, stored_bytes,
						pool_bytes);
}

void memcomp_reclaim_writeback_begin(int nid, int reclaim_idx)
{
	unsigned long flags;
	unsigned long excess;
	u64 stored_bytes;
	u64 pool_bytes;
	bool first;

	if (!memcomp_is_enabled() || nid < 0 || nid >= MAX_NUMNODES)
		return;

	reclaim_idx = clamp(reclaim_idx, 0, MAX_NR_ZONES - 1);
	stored_bytes = atomic64_read(&memcomp_stored_bytes);
	pool_bytes = memcomp_pool_total_size();
	excess = memcomp_writeback_gate_excess(nid, pool_bytes);
	spin_lock_irqsave(&memcomp_writeback_scope_locks[nid], flags);
	first = !atomic_read(&memcomp_writeback_scopes[nid]);
	if (first) {
		WRITE_ONCE(memcomp_writeback_reclaim_idx[nid], reclaim_idx);
		memcomp_writeback_budget_grant_locked(nid, stored_bytes,
						      pool_bytes, excess);
		atomic_set(&memcomp_writeback_stopped[nid], 0);
	}
	atomic_inc(&memcomp_writeback_scopes[nid]);
	spin_unlock_irqrestore(&memcomp_writeback_scope_locks[nid], flags);

	if (first)
		memcomp_debug_inc(&memcomp_writeback_pressure_events);
}

void memcomp_reclaim_writeback_end(int nid)
{
	unsigned long flags;
	int reclaim_idx = 0;
	int scopes;
	bool last = false;

	if (!memcomp_is_enabled() || nid < 0 || nid >= MAX_NUMNODES)
		return;

	spin_lock_irqsave(&memcomp_writeback_scope_locks[nid], flags);
	scopes = atomic_read(&memcomp_writeback_scopes[nid]);
	if (WARN_ON_ONCE(scopes <= 0)) {
		atomic_set(&memcomp_writeback_scopes[nid], 0);
	} else {
		last = scopes == 1;
		reclaim_idx = READ_ONCE(memcomp_writeback_reclaim_idx[nid]);
		atomic_dec(&memcomp_writeback_scopes[nid]);
	}
	spin_unlock_irqrestore(&memcomp_writeback_scope_locks[nid], flags);

	/*
	 * Settle the off-reclaim drain's quota at every period end, not only
	 * at one that saw the node recover.  A workload exiting frees its
	 * memory directly, so the watermarks are satisfied without reclaim
	 * running again: kswapd's last scope closed while the node was still
	 * below low, and then kswapd simply slept.  Waiting for a scope to
	 * end on a recovered node would miss exactly that case - the one
	 * where the pool is left at its largest and entirely cold.
	 *
	 * This is idempotent, and a pass that finds a scope open leaves that
	 * node to the reclaim path.
	 */
	if (last)
		memcomp_writeback_drain_schedule(nid);

	if (last && !memcomp_writeback_node_below_low(nid, reclaim_idx)) {
		spin_lock_irqsave(&memcomp_writeback_scope_locks[nid], flags);
		if (!atomic_read(&memcomp_writeback_scopes[nid])) {
			unsigned int drain;

			WRITE_ONCE(memcomp_writeback_feedback_valid[nid], false);
			/*
			 * The node climbed back on its own, so decay the share
			 * the pressure earned - see
			 * memcomp_writeback_drain_recover().  The level
			 * floor is unaffected either way: it is re-derived
			 * from stored memory.
			 */
			drain = READ_ONCE(memcomp_writeback_drain[nid]);
			WRITE_ONCE(memcomp_writeback_drain[nid],
				   memcomp_writeback_drain_recover(drain));
		}
		spin_unlock_irqrestore(&memcomp_writeback_scope_locks[nid], flags);
	}
}

static bool memcomp_writeback_node_below_low(int nid, int reclaim_idx)
{
	struct pglist_data *pgdat;
	struct zone *zone;
	bool found = false;
	int z;

	if (nid < 0 || nid >= MAX_NUMNODES || reclaim_idx < 0 ||
	    !node_state(nid, N_MEMORY))
		return false;

	pgdat = NODE_DATA(nid);
	if (!pgdat)
		return false;

	reclaim_idx = min_t(int, reclaim_idx, MAX_NR_ZONES - 1);
	for (z = 0; z <= reclaim_idx; z++) {
		zone = pgdat->node_zones + z;
		if (!managed_zone(zone))
			continue;
		found = true;
		if (zone_watermark_ok_safe(zone, 0, low_wmark_pages(zone),
					   reclaim_idx))
			return false;
	}

	return found;
}

static bool memcomp_writeback_scope_active(int nid)
{
	return nid >= 0 && nid < MAX_NUMNODES &&
		atomic_read(&memcomp_writeback_scopes[nid]) > 0;
}

static void memcomp_reclaim_writeback_stop(int nid)
{
	if (nid >= 0 && nid < MAX_NUMNODES)
		atomic_set(&memcomp_writeback_stopped[nid], 1);
}

static bool memcomp_writeback_pressure_active(int nid)
{
	/* A scope lives for one shrink_node call; budget bounds its I/O. */
	return memcomp_writeback_scope_active(nid) &&
		!atomic_read(&memcomp_writeback_stopped[nid]) &&
		READ_ONCE(memcomp_writeback_budget[nid]) > 0;
}

/*
 * How much of a node's writeback budget one pass may spend.
 *
 * Both writeback paths take this from the same place.  Every entry a pass
 * moves can flush a batch, and a flush is a __swap_writepage() immediately
 * waited on, so a pass length is a count of blocking device round trips taken
 * with the global writeback mutex held.  Slicing costs nothing: consume below
 * subtracts only what was scanned, so the remainder stays in the budget and
 * memcomp_writeback_pressure_active() keeps reporting pressure until it is
 * spent.
 */
static unsigned long memcomp_writeback_pass_limit(int nid)
{
	if (nid < 0 || nid >= MAX_NUMNODES)
		return 0;

	return memcomp_writeback_drain_slice(READ_ONCE(memcomp_writeback_budget[nid]));
}

/*
 * Consume payload-entry budget for the active scope.  Returns the remaining
 * budget; reaching zero stops new writeback until the next severe scope.
 */
static unsigned long memcomp_writeback_budget_consume(int nid,
						      unsigned long entries)
{
	unsigned long budget;

	if (nid < 0 || nid >= MAX_NUMNODES)
		return 0;
	budget = READ_ONCE(memcomp_writeback_budget[nid]);
	if (entries < budget)
		budget -= entries;
	else
		budget = 0;
	WRITE_ONCE(memcomp_writeback_budget[nid], budget);
	return budget;
}

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static unsigned long memcomp_writeback_pressure_scope_count(void)
{
	unsigned long active = 0;
	int nid;

	for_each_node_state(nid, N_MEMORY)
		active += atomic_read(&memcomp_writeback_scopes[nid]);

	return active;
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

/* Recheck the software same-filled result before storing the payload. */
static bool memcomp_same_filled_plausible(const void *src,
					  unsigned int src_len,
					  unsigned long element)
{
	const unsigned long *words = src;
	unsigned int last = src_len / sizeof(*words) - 1;

	return src_len == PAGE_SIZE && words[0] == element &&
	       words[last / 2] == element && words[last] == element;
}

static int
memcomp_prepare_payload_locked(struct memcomp_reclaim_ctx **ctxp,
			       const void *src, unsigned int src_len,
				   unsigned int *length,
				   unsigned long *handle, u8 *pool_idx,
				   bool allow_provisional_raw,
				   struct memcomp_store_ctx *store,
				   const struct memcomp_async_req *precomp)
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

	if (precomp && precomp->same_filled &&
	    !memcomp_same_filled_plausible(src, src_len, precomp->element))
		precomp = NULL;
	/*
	 * A software same-filled result follows the same full scan as a
	 * synchronous store.
	 */
	if (precomp ? precomp->same_filled :
		      page_same_filled(src, &element)) {
		*length = 0;
		*handle = precomp ? precomp->element : element;
		return 0;
	}

	/* The page has been compressed; all that is left is to store it. */
	if (precomp) {
		target = memcomp_select_pool_idx();
		pool = memcomp_pools[target];
		huge_class_size = zs_huge_class_size(pool);
		allow_slow = ctx->background;
		compressed = precomp->buf;
		dst_len = precomp->len;
		ret = memcomp_compress_result(precomp->ret, &dst_len,
					      src_len);
		if (!ret)
			raw_reason = memcomp_payload_raw_reason(dst_len,
								src_len,
								    huge_class_size);
		goto have_payload;
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
have_payload:
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

static int memcomp_prepare_payload(const void *src, unsigned int src_len,
				   unsigned int *length,
				       unsigned long *handle,
				       u8 *pool_idx,
				       enum memcomp_payload_mode mode,
				       bool allow_provisional_raw)
{
	struct memcomp_reclaim_ctx *ctx;
	unsigned long element;
	int ret;

	/* Same-filled shmem pages do not need a compression workspace. */
	if (page_same_filled(src, &element)) {
		*length = 0;
		*handle = element;
		return 0;
	}

	ctx = memcomp_get_comp_ctx(mode, NULL, MEMCOMP_RECLAIM_DIRECT);
	if (!ctx)
		return -EBUSY;
	ret = memcomp_prepare_payload_locked(&ctx, src, src_len, length,
					     handle, pool_idx,
						 allow_provisional_raw, NULL,
						 NULL);
	WARN_ON_ONCE(!ctx);
	memcomp_put_comp_ctx(ctx);
	return ret;
}

static __always_inline int
memcomp_arch_prepare_to_swap(struct folio *folio)
{
	return arch_prepare_to_swap(folio);
}

/* Allocate @nr_pages consecutive tokens, skipping fragmented free ranges. */

#include "memcomp/workingset_store.h"
#include "memcomp/cache_store.h"
#include "memcomp/cache_folio.h"

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

/*
 * The payload made beforehand for page @i of the folio @store is for, when
 * one was and it says what compressing the page would: a size, that the
 * page does not shrink, or that it is same-filled.
 */
static const struct memcomp_async_req *
memcomp_store_precomp(const struct memcomp_store_ctx *store,
		      unsigned int i)
{
	const struct memcomp_async_req *r;

	if (!store || !store->precomp)
		return NULL;
	r = &store->precomp[i];
	return !r->ret || r->ret == -E2BIG || r->ret == -ENOSPC ? r : NULL;
}

#include "memcomp/anon_store.h"

/*
 * Fill an existing MC_PENDING span after unmap while keeping tokens stable.
 * @held are the span's entries if the caller has them and holds a reference
 * on each, NULL to have them looked up here.
 */

#ifdef CONFIG_SWAP
/*
 * Writeback: cold payloads leave the pool for real swap slots, and each
 * slot is written in whichever form costs the device less.
 *
 * Consecutive cold payloads are gathered into one open page.  When the
 * next one does not fit, the page is closed: if it gathered two or more,
 * their compressed bytes go out back to back in one slot; if it gathered
 * only one, that one is decompressed and written as its original page,
 * exactly as a raw writeback would.  So a slot is only ever packed when
 * packing saves a whole slot, and a packed slot is never worse than the
 * raw pages it replaces: the write is the same one page, and a fault on
 * it is the same one-page read plus a decompression.
 *
 * The slot's swap count is its reference count - each entry living in it
 * owns one - so nothing describes a packed slot beyond the entries
 * themselves.  The last entry to go frees it; there is no descriptor, no
 * read/modify/write of a live slot and no separate lifetime to manage.
 * A slot whose other payloads have all gone keeps its one survivor where
 * it is; the device holds a little dead space rather than paying to move
 * it.
 *
 * Writes are submitted asynchronously and waited on once per batch of
 * MEMCOMP_WRITEBACK_BATCH slots, so the device sees batch-deep I/O
 * instead of a submit-and-wait cycle per page under the global lock.
 * memcomp_writeback_lock serializes passes, which is what lets the
 * staging batch live here rather than on kswapd's stack.
 */
/* Payloads per packed slot; far below the swap count's continuation. */
#define MEMCOMP_WRITEBACK_PACK_MAX	16

/* One payload on its way to the device. */

/* Attempt identity exists even for a denied or unaccounted reservation. */

/* One slot's write: @nr members from @first, packed or one raw page. */

static DEFINE_MUTEX(memcomp_writeback_lock);
/*
 * Members of the written slots, in slot order, followed by those of the
 * page still open - the ones from memcomp_writeback_open_first on,
 * with memcomp_writeback_open_bytes of payload between them.
 */
/*
 * Swap slots prefetched a batch at a time.  get_swap_pages() takes a device
 *-wide lock and scans the free-space bitmap per call, and scan_swap_map_slots()
 * hands out consecutive slots out of one cluster when asked for several at
 * once, so one call per staging both pays the lock 32 times where once would
 * do and scatters the writes this batch is about to submit together across
 * the device.  The pool holds slots taken but not yet attached to a folio;
 * a flush returns whatever a pass did not use, and swapoff drains it whole.
 */
static swp_entry_t
	memcomp_writeback_slot_pool[MEMCOMP_WRITEBACK_BATCH];
static unsigned int memcomp_writeback_slots_free;

static void memcomp_drop_swapcache_folio(struct folio *folio, bool dirty)
{
	if (dirty)
		folio_mark_dirty(folio);
	if (folio_test_swapcache(folio)) {
		folio_wait_writeback(folio);
		delete_from_swap_cache(folio);
	}
	folio_unlock(folio);
	folio_put(folio);
}

static void memcomp_writeback_slots_release(swp_entry_t *slots,
					    unsigned int n)
{
	swapcache_free_entries(slots, n);
}

/* Return every prefetched slot this pass did not spend. */
static void memcomp_writeback_slots_return(void)
{
	if (!memcomp_writeback_slots_free)
		return;

	/*
	 * Prefetched slots were never installed into the swap cache, so
	 * they are plain uncommitted reservations: hand them back to the
	 * device's free list as a batch through the same entry point that
	 * releases unused allocations.
	 */
	memcomp_writeback_slots_release(memcomp_writeback_slot_pool,
					memcomp_writeback_slots_free);
	memcomp_writeback_slots_free = 0;
}

/*
 * Take one prefetched slot, refilling the pool a full batch at a time when
 * it runs dry.  Returns 0 with an uncommitted reservation in *@swapp, or
 * -ENOSPC/-ENOMEM when no slots or no device remain.
 */
static int memcomp_writeback_slot_take(swp_entry_t *swapp)
{
	unsigned int want, i;

	if (!memcomp_writeback_slots_free) {
		want = min_t(unsigned int, MEMCOMP_WRITEBACK_BATCH,
			     SWAP_BATCH);

		if (get_nr_swap_pages() <= 0)
			return -ENOSPC;
		memcomp_writeback_slots_free =
			get_swap_pages(want, memcomp_writeback_slot_pool, 0);
		if (!memcomp_writeback_slots_free)
			return get_nr_swap_pages() <= 0 ? -ENOSPC : -ENOMEM;
		/* Mark the whole reservation, including slots later returned unused. */
		for (i = 0; i < memcomp_writeback_slots_free; i++) {
			swp_entry_t entry = memcomp_writeback_slot_pool[i];

			swap_memcomp_transport_set(swp_swap_info(entry),
						   swp_offset(entry));
		}
	}

	*swapp = memcomp_writeback_slot_pool[--memcomp_writeback_slots_free];
	return 0;
}

/*
 * One swap slot and a swapcache folio to carry it to the device.  On
 * success the folio is locked and in the cache, and the slot holds one
 * extra reference - the one the converted entry will own.
 */
static int memcomp_swap_folio_alloc(struct folio **foliop,
				    swp_entry_t *swapp)
{
	struct folio *folio;
	swp_entry_t swap;
	int ret;

	ret = memcomp_writeback_slot_take(&swap);
	if (ret)
		return ret;

	folio = folio_alloc(GFP_HIGHUSER_MOVABLE | __GFP_NOWARN |
			    __GFP_NOMEMALLOC, 0);
	if (!folio) {
		swapcache_free_entries(&swap, 1);
		return -ENOMEM;
	}
	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);

	ret = add_to_swap_cache(folio, swap,
				__GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN, NULL);
	if (ret) {
		folio_unlock(folio);
		folio_put(folio);
		swapcache_free_entries(&swap, 1);
		return ret;
	}

	ret = memcomp_slot_dup(swap);
	if (ret) {
		delete_from_swap_cache(folio);
		folio_unlock(folio);
		folio_put(folio);
		return ret;
	}

	*foliop = folio;
	*swapp = swap;
	return 0;
}

/*
 * Convert a staged entry now that its slot is on the device: the zsmalloc
 * copy is freed and @swap - with one slot reference the caller hands over -
 * becomes the payload, at @m->offset when the slot is packed.
 * @entry->length is left alone: it is the compressed length, which is also
 * what a packed slot holds, and the stored and compressed byte gauges track
 * live entries wherever the payload lives.  When this returns false the
 * caller keeps the reference.
 */

/* Put a claimed entry back where the claim found it. */

/*
 * Resolve one written slot's members.  Each member converted takes one of
 * the slot's references: the first the one staging took, every other one
 * a duplicate taken here - while the swap cache still holds the slot, so a
 * duplicate can never find it freed.  A member that raced - refreshed,
 * freed or claimed back - keeps its pool copy and gives its reference back.
 */

/*
 * Write one slot: members @first..@first + @nr - 1, their compressed bytes
 * back to back when there are several, or the lone one decompressed into
 * the original page.  The write is submitted, not waited on.
 */

/*
 * Write the open page out.  On failure every payload it gathered goes back
 * to the pool, and the batch's reference on each is dropped.
 */

/*
 * Wait for everything the batch submitted and resolve each slot.  Returns
 * the first device error; every staged entry ends converted or reverted
 * either way, including those of the page still open, which is written
 * first so that no entry stays claimed across the lock.
 *
 * The waits run as one sweep ahead of the resolution sweep: by the time the
 * first folio's writeback clears, the rest of the batch's IO has usually
 * landed with it, and the second sweep then takes each folio lock without
 * sleeping instead of waking and re-sleeping between resolutions.
 */

/*
 * Whether a payload of @length joins the open page, which holds @nr
 * payloads and @bytes of them.  A page-sized payload never does: it is a
 * page the compressor could not shrink, and shares its slot with nothing.
 */
static bool memcomp_writeback_pack_fits(unsigned int nr,
					unsigned int bytes,
					    unsigned int length)
{
	return length < PAGE_SIZE && nr < MEMCOMP_WRITEBACK_PACK_MAX &&
	       bytes + length <= PAGE_SIZE;
}

/*
 * Whether the batch must be flushed before the next staging: that one may
 * close the open page and write itself as well, so it needs two slots.
 */

/*
 * Stage one claimed entry into the open page, writing that page first if
 * the payload does not fit in it.  The batch flush waits and converts.
 */

/*
 * Recover an entry's page from the slot image @slot: the whole of it for a
 * raw slot, or @length compressed bytes at @off decompressed for a packed
 * one.  A packed payload that does not lie inside the slot is corrupt
 * state, never a short read.
 */
#include "memcomp/writeback_batch.h"
#include "memcomp/drain.h"

static int memcomp_decode_slot(const void *slot, bool packed,
			       unsigned int off, unsigned int length,
				   void *dst)
{
	if (!packed) {
		memcpy(dst, slot, PAGE_SIZE);
		return 0;
	}
	if (!length || length >= PAGE_SIZE || off > PAGE_SIZE - length)
		return -EIO;
	return memcomp_decompress_payload(slot + off, length, dst);
}

/*
 * Whether a fault should leave a packed slot's page in the swap cache.
 * @count is the slot's reference count as the fault sees it, which includes
 * this entry's and the fault's own; anything beyond those two is another
 * entry still living in the slot, or another fault on its way to it.  A raw
 * slot has no one else to keep it for.
 */
static bool memcomp_slot_keep_cached(bool packed, int count)
{
	return packed && count > 2;
}

/*
 * Read the slot backing @swap and recover this entry's page from it into
 * @dst - see memcomp_decode_slot() for @packed, @off and @length.  @gfp
 * is for the swap cache page, if the slot is not already cached.
 */
static int memcomp_read_swapped_page(swp_entry_t swap, bool packed,
				     unsigned int off, unsigned int length,
				     void *dst, gfp_t gfp)
{
	struct folio *folio;
	bool allocated;
	void *src;
	int ret;

	if (!swap.val || !dst)
		return -EIO;

	/*
	 * Through the swap cache, so concurrent faults on the same slot
	 * share one device read instead of each issuing their own.  There
	 * is no vma here - the fault path and swapoff both only have the
	 * swap entry - so the folio follows the task's policy, as zswap's
	 * writeback does; read_swap_cache_async() would hand a NULL vma to
	 * get_vma_policy(), which dereferences it on NUMA.
	 */
	folio = __read_swap_cache_async(swap, gfp, get_task_policy(current),
					NO_INTERLEAVE_INDEX, &allocated, false);
	if (!folio)
		return -ENOMEM;
	if (allocated)
		swap_read_folio_memcomp(folio, NULL);

	folio_lock(folio);
	if (!folio_test_uptodate(folio)) {
		memcomp_drop_swapcache_folio(folio, false);
		memcomp_debug_inc(&memcomp_swapped_read_error);
		return -EIO;
	}

	if (!allocated)
		memcomp_debug_inc(&memcomp_swapped_read_cached);

	src = kmap_local_folio(folio, 0);
	ret = memcomp_decode_slot(src, packed, off, length, dst);
	kunmap_local(src);
	if (ret)
		memcomp_debug_inc(&memcomp_swapped_read_error);

	/*
	 * A packed slot's neighbours were written together because they went
	 * cold together, and they tend to come back together.  While any of
	 * them still lives in the slot, leave its page in the swap cache: the
	 * next of them to fault finds it there and decompresses its payload
	 * with no device read at all.  Nothing else is kept - the page is
	 * clean, unmapped and on the LRU, so reclaim drops it like any
	 * readahead page, and the last entry to leave the slot takes it with
	 * it (memcomp_slot_put_packed()).
	 */
	if (!ret && memcomp_slot_keep_cached(packed,
					     memcomp_slot_count(swap))) {
		folio_unlock(folio);
		folio_put(folio);
		return 0;
	}
	memcomp_drop_swapcache_folio(folio, false);
	return ret;
}

/*
 * Continue draining after a pressure period ends.
 *
 * Writeback otherwise only runs inside a severe scope, and a severe scope
 * only exists while kswapd is looking at a node below its low watermark.
 * The moment a memory-hungry workload exits, the watermarks are satisfied,
 * kswapd stops, and every scope closes - leaving a pool that is now
 * entirely cold, at its largest, and draining nothing until the next
 * workload pushes the node back down.  That is both the worst time to
 * still be holding it and the cheapest time to write it out, with the CPU
 * and the storage otherwise idle.
 *
 * So a period that ends with the loop still asking for a share hands that
 * share to this worker rather than dropping it.  The loop is unchanged -
 * each pass drains its share, measures the pool, and decays - so the
 * worker simply runs the same controller off the reclaim path until the
 * share reaches zero and the pool is holding on its own.
 */
static void memcomp_writeback_drain_schedule(int nid)
{
	struct memcomp_lru *lru;
	unsigned long excess;
	unsigned long quota;
	u64 pool_bytes;

	if (!memcomp_is_enabled() || !total_swap_pages)
		return;
	if (nid < 0 || nid >= MAX_NUMNODES)
		return;

	lru = memcomp_lru_nid(nid);
	if (!lru)
		return;

	/*
	 * Settle the quota once, here, from the share the period earned and
	 * the payloads that were cold when it ended.  The worker then spends
	 * it and stops.  Recomputing a share every pass would instead take
	 * that fraction of whatever remains, over and over: half of the pool,
	 * then half of the rest, which drains almost all of it rather than
	 * the half the loop actually asked for.  Past this much the pool has
	 * given up what it should and the rest is compression's job again.
	 */
	quota = memcomp_writeback_budget_values(atomic_long_read(&lru->nr_entries),
						READ_ONCE(memcomp_writeback_drain[nid]));
	/*
	 * And never more than the pool holds above the gate's close point.
	 * A closed gate owes nothing, including what an earlier period left.
	 */
	pool_bytes = memcomp_pool_total_size();
	excess = memcomp_writeback_gate_excess(nid, pool_bytes);
	if (!excess) {
		WRITE_ONCE(memcomp_writeback_carry[nid], 0);
		/*
		 * Nothing to drain, which is the one time the device may be
		 * tidied - if swap is running short.
		 */
		if (!READ_ONCE(memcomp_writeback_defrag_armed) &&
		    memcomp_writeback_defrag_due()) {
			WRITE_ONCE(memcomp_writeback_defrag_armed, true);
			queue_delayed_work(system_power_efficient_wq,
					   &memcomp_writeback_drain_work,
					   MEMCOMP_WRITEBACK_DRAIN_DELAY);
		}
		return;
	}
	quota = min(quota, excess);
	if (!quota)
		return;

	if (quota > READ_ONCE(memcomp_writeback_carry[nid]))
		WRITE_ONCE(memcomp_writeback_carry[nid], quota);

	queue_delayed_work(system_power_efficient_wq,
			   &memcomp_writeback_drain_work,
			   MEMCOMP_WRITEBACK_DRAIN_DELAY);
}

static void memcomp_writeback_drain_worker(struct work_struct *work)
{
	unsigned int noreclaim_flag;
	bool more = false;
	int nid;

	(void)work;

	if (!memcomp_is_enabled() || !total_swap_pages)
		return;
	/* A full device is no drain, but exactly when a defrag pass helps. */
	if (get_nr_swap_pages() <= 0)
		goto defrag;

	for_each_node_state(nid, N_MEMORY) {
		struct memcomp_lru *lru = memcomp_lru_nid(nid);
		unsigned long freed = 0;
		unsigned long scanned;
		unsigned long excess;
		unsigned long quota;
		unsigned long limit;
		u64 pool_bytes;
		bool attempted = false;
		int write_ret = 0;

		quota = READ_ONCE(memcomp_writeback_carry[nid]);
		if (!quota || !lru)
			continue;
		/*
		 * The quota was settled against the pool as it was when the
		 * period ended; the level is re-read every pass, so the drain
		 * stops at the gate's close point rather than at the quota.
		 */
		pool_bytes = memcomp_pool_total_size();
		excess = memcomp_writeback_gate_excess(nid, pool_bytes);
		if (!excess) {
			WRITE_ONCE(memcomp_writeback_carry[nid], 0);
			continue;
		}
		/*
		 * A reopened scope owns the node again.  Leave the quota
		 * alone and come back for it: the scope will settle a fresh
		 * one when its period ends, but if it does not - a period can
		 * end without the node recovering - this is the only thing
		 * that would retry, so it has to re-arm itself rather than
		 * wait to be armed.
		 */
		if (memcomp_writeback_scope_active(nid)) {
			more = true;
			continue;
		}

		limit = min(memcomp_writeback_drain_slice(quota), excess);

		noreclaim_flag = memalloc_noreclaim_save();
		mutex_lock(&memcomp_writeback_lock);
		scanned = mc16_writeback_drain_queue(nid, lru, limit,
						     &freed, &write_ret,
						     &attempted);
		mutex_unlock(&memcomp_writeback_lock);
		memalloc_noreclaim_restore(noreclaim_flag);

		if (freed)
			memcomp_pool_compact();

		/*
		 * Only an empty queue or a device error ends the drain early.
		 * A pass can walk its whole slice without writing anything,
		 * and that is progress through the queue, not a reason to
		 * abandon the quota.
		 */
		if (!scanned || write_ret)
			quota = 0;
		else
			quota -= min(scanned, quota);
		(void)attempted;
		WRITE_ONCE(memcomp_writeback_carry[nid], quota);
		if (quota)
			more = true;
	}

defrag:
	if (memcomp_writeback_defrag_pass())
		more = true;
	if (more)
		queue_delayed_work(system_power_efficient_wq,
				   &memcomp_writeback_drain_work,
				   MEMCOMP_WRITEBACK_DRAIN_DELAY);
}

/*
 * Swapoff: every swap-backed entry is read back from the dying device and
 * its payload rebuilt in the pool.  The slot held the original page, so
 * coming back it pays compression again rather than occupying a page of
 * pool for ever; anything the compressor rejects is kept raw instead of
 * failing the swapoff.  The ops indirection exists so the KUnit suite can
 * drive the restore logic without a swap device.
 */
struct memcomp_swapoff_ops {
	int (*read_page)(swp_entry_t swap, void *dst);
	int (*alloc_payload)(const void *src, unsigned int *length,
			     unsigned long *handle, u8 *pool_idx, void *ctx);
	void (*free_payload)(u8 pool_idx, unsigned long handle, void *ctx);
	bool (*interrupted)(void *ctx);
	void *ctx;
};

#include "memcomp/swapoff.h"
#include "memcomp/swapoff_walk.h"

/*
 * Precharge outside the token mutex, then publish only if the slot still
 * belongs to this entry.  Return 1 on commit, 0 on a race, or a charge error.
 * The caller frees the new payload unless this returns 1.
 */

/* Restore one entry.  1 restored, 0 nothing to do, negative on error. */

/* The slot image as it is on the device; the caller decodes it. */
static int memcomp_swapoff_read_page(swp_entry_t swap, void *dst)
{
	return memcomp_read_swapped_page(swap, false, 0, 0, dst,
					     GFP_HIGHUSER_MOVABLE | __GFP_NOWARN);
}

static int memcomp_swapoff_alloc_payload(const void *src,
					 unsigned int *length,
					     unsigned long *handle,
					     u8 *pool_idx, void *ctx)
{
	int ret;

	*pool_idx = 0;
	ret = memcomp_prepare_payload(src, PAGE_SIZE, length, handle,
				      pool_idx,
					  MEMCOMP_PAYLOAD_MUST_SUCCEED,
					  true);
	if (!ret)
		return 0;

	/* No context or no fit: keep the page raw rather than fail. */
	*pool_idx = memcomp_select_pool_idx();
	return memcomp_alloc_raw_payload(*pool_idx, src, PAGE_SIZE, length,
					     handle, true, NULL);
}

static void memcomp_swapoff_free_payload(u8 pool_idx, unsigned long handle,
					 void *ctx)
{
	memcomp_zs_free(pool_idx, handle);
}

static bool memcomp_swapoff_interrupted(void *ctx)
{
	return signal_pending(current);
}

static const struct memcomp_swapoff_ops memcomp_swapoff_ops = {
	.read_page = memcomp_swapoff_read_page,
	.alloc_payload = memcomp_swapoff_alloc_payload,
	.free_payload = memcomp_swapoff_free_payload,
	.interrupted = memcomp_swapoff_interrupted,
};

static int mc16_swapoff_flush(void *ctx)
{
	return mc16_writeback_batch_flush(NULL);
}

static int mc16_swapoff_restore(struct memcomp_ref *ref, unsigned int type,
				void *ctx)
{
	return mc16_swapoff_entry(ref, type, &memcomp_swapoff_ops);
}

int memcomp_swapoff(unsigned int type)
{
	const struct mc16_swapoff_walk_ops ops = {
		.lock = &memcomp_writeback_lock,
		.flush = mc16_swapoff_flush,
		.restore = mc16_swapoff_restore,
	};

	if (!memcomp_is_enabled())
		return 0;
	cancel_delayed_work_sync(&memcomp_writeback_drain_work);
	return mc16_swapoff_walk(type, &ops);
}

/*
 * Conservative defragmentation of packed slots.
 *
 * A packed slot frees when its last entry goes, so one whose other payloads
 * have all been faulted back or freed keeps a whole slot for one small
 * survivor.  That is never worse than raw writeback, which would have spent
 * the slot on it anyway, so nothing here is needed for correctness or to
 * bound the device's use; it only matters once swap is running short.
 *
 * So it does as little as it can, as rarely as it can: only when the level
 * gate is closed, no node has a reclaim scope open, and less than an eighth
 * of swap is free.  It moves only a packed entry that is the sole reference
 * on its slot - nothing else living there and no fault on its way to it -
 * back into the pool by copying its compressed bytes, one slot read and no
 * write, no recompression.  The slot frees, and writeback packs the payload
 * again with the next cold ones if the pool ever needs it gone.  A slot that
 * still holds several payloads is never read, moved or rewritten.
 *
 * A pass looks at a bounded number of entries and moves a bounded number of
 * them, and passes run from the drain worker's delay, so a pass is a few
 * reads at most, spaced out, and never on a fault or reclaim path.  The walk
 * resumes where the last pass stopped and ends at the end of the table;
 * the next gate-closed period end with swap still short starts another.
 */
#define MEMCOMP_WRITEBACK_DEFRAG_SCAN	1024U
#define MEMCOMP_WRITEBACK_DEFRAG_MOVE	16U
#define MEMCOMP_WRITEBACK_DEFRAG_FREE_SHIFT	3

/* The device side of a move, so the suite can drive it without swap. */
struct memcomp_defrag_ops {
	int (*slot_count)(swp_entry_t swap);
	int (*read_page)(swp_entry_t swap, void *dst);
};

/* Whether defragmentation may run now; see above.  Pure. */
static bool memcomp_writeback_defrag_wanted(bool gate_open,
					    bool scope_active,
						long free_pages,
						long total_pages)
{
	if (gate_open || scope_active || total_pages <= 0)
		return false;

	return free_pages <
	       (total_pages >> MEMCOMP_WRITEBACK_DEFRAG_FREE_SHIFT);
}

static bool memcomp_writeback_any_scope_active(void)
{
	int nid;

	for_each_node_state(nid, N_MEMORY)
		if (memcomp_writeback_scope_active(nid))
			return true;
	return false;
}

static bool memcomp_writeback_defrag_due(void)
{
	bool open = atomic_read(&memcomp_writeback_gate_open);
	bool scope = memcomp_writeback_any_scope_active();

	return memcomp_writeback_defrag_wanted(open, scope,
						   get_nr_swap_pages(),
						   total_swap_pages);
}

/*
 * Move @entry back into the pool if it is a packed slot's sole survivor.
 * 1 moved, 0 not a candidate or lost a race, negative on failure.
 */

/*
 * One bounded pass from *@index.  Returns true while the walk has more of
 * the table to cover; *@index is where the next pass resumes.
 */

static int memcomp_defrag_slot_count(swp_entry_t swap)
{
	return memcomp_slot_count(swap);
}

/* No direct reclaim for a page that exists only to tidy the device. */
static int memcomp_defrag_read_page(swp_entry_t swap, void *dst)
{
	return memcomp_read_swapped_page(swap, false, 0, 0, dst,
					     GFP_NOWAIT | __GFP_NOWARN);
}

#include "memcomp/defrag.h"

static const struct memcomp_defrag_ops memcomp_defrag_ops = {
	.slot_count = memcomp_defrag_slot_count,
	.read_page = memcomp_defrag_read_page,
};

static u64 memcomp_writeback_defrag_index;

/* One pass of the walk; true when the worker should come back for more. */
static bool memcomp_writeback_defrag_pass(void)
{
	if (!READ_ONCE(memcomp_writeback_defrag_armed))
		return false;
	if (!memcomp_writeback_defrag_due() ||
	    !mc16_defrag_scan(&memcomp_writeback_defrag_index,
					       &memcomp_defrag_ops)) {
		WRITE_ONCE(memcomp_writeback_defrag_armed, false);
		return false;
	}
	return true;
}
#endif /* CONFIG_SWAP */

/*
 * Core load - decompress one page from @entry into @dst.  Sleeps on the
 * token lock and possibly swap IO; never called under the PTE lock.
 */
#include "memcomp/workingset_load.h"
#include "memcomp/load.h"
#include "memcomp/consumers.h"

/*
 * Claim a reclaimable entry for writeback.
 */

static unsigned long memcomp_shrinker_count(struct shrinker *shrinker,
					    struct shrink_control *sc)
{
#ifdef CONFIG_SWAP
	unsigned long nr_freeable;
	struct memcomp_lru *lru;

	if (!memcomp_is_enabled() || !total_swap_pages)
		return 0;
	if (get_nr_swap_pages() <= 0)
		return 0;
	/* Backing swap can live in a swapfile filesystem. */
	if (!gfp_has_io_fs(sc->gfp_mask))
		return 0;
	/* A direct-reclaim caller must never issue backing I/O. */
	if (!current_is_kswapd())
		return 0;

	/*
	 * Outside a severe VM reclaim scope nothing is offered: MC_STORED
	 * payloads stay in zsmalloc pool as soon as pressure falls, rather than
	 * turning allocator or zsmalloc pool activity into I/O.
	 */
	if (!memcomp_writeback_pressure_active(sc->nid))
		return 0;

	lru = memcomp_lru_nid(sc->nid);
	nr_freeable = lru ? atomic_long_read(&lru->nr_entries) : 0;
	return nr_freeable;
#else
	return 0;
#endif
}

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

static unsigned long memcomp_shrinker_scan(struct shrinker *shrinker,
					   struct shrink_control *sc)
{
#ifdef CONFIG_SWAP
	unsigned long scanned = 0;
	unsigned long freed = 0;
	unsigned long writeback_limit;
	unsigned long pool_pages;
	unsigned long pool_pages_after;
	unsigned long compacted;
	unsigned int noreclaim_flag;
	struct memcomp_lru *lru;
	int write_ret = 0;
	bool writeback_attempted = false;

	if (!memcomp_is_enabled() || !total_swap_pages)
		return SHRINK_STOP;
	if (get_nr_swap_pages() <= 0)
		return SHRINK_STOP;
	if (!gfp_has_io_fs(sc->gfp_mask))
		return SHRINK_STOP;
	if (!current_is_kswapd())
		return SHRINK_STOP;

	lru = memcomp_lru_nid(sc->nid);
	if (!lru || !memcomp_writeback_pressure_active(sc->nid))
		return SHRINK_STOP;
	memcomp_debug_inc(&memcomp_writeback_pressure_scans);

	/* Compact first: avoid backing I/O if zsmalloc can release pages. */
	pool_pages = memcomp_pool_total_size() >> PAGE_SHIFT;
	compacted = memcomp_pool_compact();
	if (compacted) {
		pool_pages_after = memcomp_pool_total_size() >> PAGE_SHIFT;
		if (pool_pages_after < pool_pages) {
			memcomp_wb_reclaimed(pool_pages - pool_pages_after);
			return pool_pages - pool_pages_after;
		}
		/* Baseline covers payload frees and final compaction. */
		pool_pages = memcomp_pool_total_size() >> PAGE_SHIFT;
	}

	/*
	 * The scope budget, not the shrinker scan share, paces writeback -
	 * but spend it in the same bounded slices the off-reclaim drain
	 * uses, not all at once.  A pass waits once per batch, but a
	 * saturated controller still grants half
	 * the cold payloads - six figures on a full pool - and unsliced
	 * that parks kswapd in one shrinker callback and blocks swapoff and
	 * the drain worker behind the global writeback mutex.
	 *
	 * Nothing is given up by slicing.  memcomp_writeback_budget_consume()
	 * below subtracts only what was scanned, so the remainder stays in
	 * memcomp_writeback_budget[nid] and memcomp_writeback_pressure_active()
	 * keeps reporting pressure until it is spent - the next shrinker call
	 * in the same scope picks up where this one stopped.
	 */
	writeback_limit = memcomp_writeback_pass_limit(sc->nid);
	noreclaim_flag = memalloc_noreclaim_save();
	mutex_lock(&memcomp_writeback_lock);
	scanned = mc16_writeback_drain_queue(sc->nid, lru,
					     writeback_limit, &freed,
					     &write_ret,
					     &writeback_attempted);
	mutex_unlock(&memcomp_writeback_lock);
	memalloc_noreclaim_restore(noreclaim_flag);

	if (freed) {
		memcomp_pool_compact();
		pool_pages_after = memcomp_pool_total_size() >> PAGE_SHIFT;
		if (pool_pages_after < pool_pages)
			memcomp_wb_reclaimed(pool_pages - pool_pages_after);
		else
			memcomp_debug_inc(&memcomp_writeback_zs_no_progress);
	}
	if (scanned &&
	    !memcomp_writeback_budget_consume(sc->nid, scanned)) {
		/* Budget spent: only observe until the next severe scope. */
		memcomp_debug_inc(&memcomp_writeback_limited_batches);
		memcomp_reclaim_writeback_stop(sc->nid);
	}

	sc->nr_scanned = scanned;
	if (write_ret)
		return freed ? freed : SHRINK_STOP;
	return (scanned || freed) ? freed : SHRINK_STOP;
#else
	return SHRINK_STOP;
#endif
}

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
bool is_memcomp_shadow(void *shadow)
{
	return xa_is_value(shadow) &&
	       is_memcomp_entry(radix_to_swp_entry(shadow));
}

unsigned long memcomp_shadow_token(void *shadow)
{
	return swp_offset(radix_to_swp_entry(shadow));
}

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
int memcomp_load_folio_zero(struct folio *folio)
{
	return mc16_cache_load_zero(folio);
}

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
			    struct memcomp_reclaim_ctx **ctxp,
				const struct memcomp_async_req *precomp)
{
	struct memcomp_ctx_timing_record *timing;
	struct memcomp_store_ctx store = { .precomp = precomp };
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
	return __memcomp_store_after_unmap(folio, ctxp, NULL);
}

/*
 * Asynchronous compression.  A batch lets a reclaimer compress its run of
 * folios at once on a compressor that works on its own - a hardware engine,
 * which otherwise has a CPU spin through every page - and sleep through all
 * of them at once.  A shared pool of batches serves foreground reclaim.
 * A run that finds none free is compressed synchronously in the store,
 * as it would be without them.  A foreground reclaimer may have two, one with
 * the compressor while it fills the other, but takes its second only while
 * that leaves another free; see memcomp_async_get_spare().
 */
struct memcomp_async_pool {
	struct crypto_acomp *tfm; /* Release-published after all batches. */
	spinlock_t lock; /* Protects the free-batch bitmap. */
	unsigned long free;
	unsigned int nr;
	struct memcomp_async *batches[BITS_PER_LONG];
};

static struct memcomp_async_pool memcomp_async_pool = {
	.lock = __SPIN_LOCK_UNLOCKED(memcomp_async_pool.lock),
};

/* Take a free foreground batch, leaving @keep for another reclaimer. */
static struct memcomp_async *
memcomp_async_take(struct memcomp_async_pool *pool, unsigned int keep)
{
	struct memcomp_async *async = NULL;
	unsigned int i;

	spin_lock(&pool->lock);
	i = find_first_bit(&pool->free, pool->nr);
	if (i < pool->nr && hweight_long(pool->free) > keep) {
		__clear_bit(i, &pool->free);
		async = pool->batches[i];
	}
	spin_unlock(&pool->lock);
	if (!async)
		return NULL;

	atomic_set(&async->pending, 1);
	reinit_completion(&async->done);
	async->nr = 0;
	return async;
}

static void memcomp_async_give(struct memcomp_async_pool *pool,
			       struct memcomp_async *async)
{
	/* Nothing may still be writing into a batch handed back. */
	if (atomic_read(&async->pending))
		memcomp_async_wait(async);
	spin_lock(&pool->lock);
	__set_bit(async->id, &pool->free);
	spin_unlock(&pool->lock);
}

struct memcomp_async *memcomp_async_get(void)
{
	if (!memcomp_is_enabled())
		return NULL;
	/* Pairs with async_init's release publication of a late pool. */
	if (!smp_load_acquire(&memcomp_async_pool.tfm))
		return NULL;
	return memcomp_async_take(&memcomp_async_pool, 0);
}

/*
 * memcomp_async_get(), but only while another foreground batch would
 * still be free after: for a reclaimer that has one out already, and would
 * otherwise leave the next to come none.
 */
struct memcomp_async *memcomp_async_get_spare(void)
{
	if (!memcomp_is_enabled())
		return NULL;
	/* Pairs with async_init's release publication of a late pool. */
	if (!smp_load_acquire(&memcomp_async_pool.tfm))
		return NULL;
	return memcomp_async_take(&memcomp_async_pool, 1);
}

void memcomp_async_put(struct memcomp_async *async)
{
	if (async)
		memcomp_async_give(&memcomp_async_pool, async);
}

static void memcomp_async_done(void *data, int err)
{
	struct memcomp_async_req *r = data;
	struct memcomp_async *async = r->async;

	/* A backlogged request has been started; it completes later. */
	if (err == -EINPROGRESS)
		return;

	r->len = r->req->dlen;
	r->ret = err;
	if (atomic_dec_and_test(&async->pending))
		complete(&async->done);
}

/*
 * Start compressing the pages of @folio in @async, each a request of its
 * own.  The pages may be read any time until the batch is waited for, so
 * their contents must already be what the store will keep: the folio is
 * locked and unmapped, with the TLB flushed, and holds no more references
 * than the store accepts.  Whatever then changes that also fails the
 * store's own checks, which run again before the payloads are used.
 *
 * Returns whether the folio went in, which takes a request for every page,
 * or room for them.  One that did not is compressed by the store, as usual.
 * A same-filled page takes its request too, to pass the software scan
 * result to the store without invoking the compressor.
 */
bool memcomp_async_submit(struct memcomp_async *async,
			  struct folio *folio)
{
	unsigned int nr_pages = folio_nr_pages(folio);
	u32 flags;
	unsigned int i;

	if (!async || !folio_test_locked(folio) ||
	    nr_pages > MEMCOMP_ASYNC_PAGES - async->nr)
		return false;
	if (folio_mapped(folio) || folio_maybe_dma_pinned(folio) ||
	    folio_ref_count(folio) != 1 + nr_pages)
		return false;

	/* The batch bounds the number of outstanding requests. */
	flags = CRYPTO_TFM_REQ_MAY_BACKLOG;
	for (i = 0; i < nr_pages; i++) {
		struct memcomp_async_req *r = &async->reqs[async->nr + i];
		unsigned long element;
		bool same;
		void *src;
		int ret;

		r->folio = folio;
		r->same_filled = false;
		/* The store keeps these as they are, without a payload. */
		src = kmap_local_folio(folio, i * PAGE_SIZE);
		same = page_same_filled(src, &element);
		kunmap_local(src);
		if (same) {
			r->ret = 0;
			r->len = 0;
			r->element = element;
			r->same_filled = true;
			continue;
		}

		sg_init_table(&r->src, 1);
		sg_set_page(&r->src, folio_page(folio, i), PAGE_SIZE, 0);
		sg_init_one(&r->dst, r->buf, PAGE_SIZE);
		acomp_request_set_params(r->req, &r->src, &r->dst, PAGE_SIZE,
					 PAGE_SIZE);
		acomp_request_set_callback(r->req, flags,
					   memcomp_async_done, r);
		atomic_inc(&async->pending);
		ret = crypto_acomp_compress(r->req);
		/* Anything else, and the request is done without a callback. */
		if (ret != -EINPROGRESS && ret != -EBUSY)
			memcomp_async_done(r, ret);
	}
	async->nr += nr_pages;
	return true;
}

/*
 * Wait for every request of @async; call once, before using any of them.
 * The compressor is doing the work meanwhile, so the wait is no load.
 */
void memcomp_async_wait(struct memcomp_async *async)
{
	if (async && !atomic_dec_and_test(&async->pending))
		wait_for_completion_state(&async->done, TASK_IDLE);
}

/*
 * Whether every request submitted in @async has completed, so that waiting
 * for it would not sleep; ask before memcomp_async_wait() only.
 */
bool memcomp_async_complete(struct memcomp_async *async)
{
	return !async || atomic_read(&async->pending) == 1;
}

/*
 * memcomp_store_after_unmap(), with the payloads @async made for @folio,
 * if it made them.  A page whose request failed for any reason but the page
 * not shrinking is compressed by the store itself.
 */
bool memcomp_store_after_unmap_async(struct folio *folio,
				     struct memcomp_reclaim_ctx **ctxp,
					 struct memcomp_async *async)
{
	const struct memcomp_async_req *precomp = NULL;
	unsigned int nr_pages = folio_nr_pages(folio);
	unsigned int i;

	for (i = 0; async && i < async->nr; i++) {
		if (async->reqs[i].folio != folio)
			continue;
		/* Taken in a row for the folio, which cannot have split. */
		if (!WARN_ON_ONCE(nr_pages > async->nr - i))
			precomp = &async->reqs[i];
		break;
	}
	for (i = 0; precomp && i < nr_pages; i++) {
		int ret = precomp[i].ret;

		/* A software same-filled page has no compressed payload. */
		if (precomp[i].same_filled)
			continue;
		if (!ret || ret == -E2BIG || ret == -ENOSPC)
			memcomp_debug_inc(&memcomp_async_payloads);
		else
			memcomp_debug_inc(&memcomp_async_fail);
	}
	return __memcomp_store_after_unmap(folio, ctxp, precomp);
}

static void memcomp_async_free_batch(struct memcomp_async *async)
{
	unsigned int i;

	for (i = 0; i < MEMCOMP_ASYNC_PAGES; i++) {
		if (async->reqs[i].req)
			acomp_request_free(async->reqs[i].req);
		kfree(async->reqs[i].buf);
	}
	kfree(async);
}

/*
 * Set up the batches when the compressor has an asynchronous form; the
 * synchronous one stays what every other path uses.  Both are the same
 * algorithm by name, so they make the same payloads.
 */
static void memcomp_async_init(struct memcomp_async_pool *pool,
			       const char *compressor)
{
	unsigned int nr = min_t(unsigned int, num_possible_cpus(),
				    BITS_PER_LONG);
	struct crypto_acomp *tfm;
	unsigned int i, j;

	lockdep_assert_held(&memcomp_select_lock);
	if (pool->tfm)
		return;

	tfm = crypto_alloc_acomp(compressor, CRYPTO_ALG_ASYNC,
				 CRYPTO_ALG_ASYNC | CRYPTO_NOLOAD);
	if (IS_ERR(tfm))
		return;

	for (i = 0; i < nr; i++) {
		struct memcomp_async *async;

		async = kzalloc(sizeof(*async), GFP_KERNEL);
		if (!async)
			break;
		init_completion(&async->done);
		async->id = i;
		for (j = 0; j < MEMCOMP_ASYNC_PAGES; j++) {
			struct memcomp_async_req *r = &async->reqs[j];

			r->async = async;
			r->req = acomp_request_alloc(tfm);
			r->buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
			if (!r->req || !r->buf)
				break;
		}
		if (j < MEMCOMP_ASYNC_PAGES) {
			memcomp_async_free_batch(async);
			break;
		}
		pool->batches[i] = async;
		__set_bit(i, &pool->free);
	}
	if (!i) {
		crypto_free_acomp(tfm);
		return;
	}
	pool->nr = i;
	/* Late readers must see every request, buffer and free bit first. */
	smp_store_release(&pool->tfm, tfm);
	pr_info("async compression through %s, %u foreground batches\n",
		crypto_tfm_alg_driver_name(crypto_acomp_tfm(tfm)), i);
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

static void memcomp_destroy_pools(void)
{
	int i;

	for (i = 0; i < memcomp_nr_pools; i++) {
		if (!memcomp_pools[i])
			continue;
		zs_destroy_pool(memcomp_pools[i]);
		memcomp_pools[i] = NULL;
	}
	memcomp_nr_pools = 0;
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
		spin_lock_init(&memcomp_writeback_scope_locks[i]);
		atomic_set(&memcomp_writeback_scopes[i], 0);
		atomic_set(&memcomp_writeback_stopped[i], 0);
		memcomp_writeback_reclaim_idx[i] = 0;
		memcomp_writeback_stored_bytes[i] = 0;
		memcomp_writeback_pool_bytes[i] = 0;
		memcomp_writeback_feedback_valid[i] = false;
		memcomp_writeback_drain[i] = 0;
		memcomp_writeback_carry[i] = 0;
		memcomp_writeback_budget[i] = 0;
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

#ifdef CONFIG_SWAP
	INIT_DELAYED_WORK(&memcomp_writeback_drain_work,
			  memcomp_writeback_drain_worker);
#endif
	memcomp_meta_shrinker = shrinker_alloc(0, "memcomp-meta");
	if (!memcomp_meta_shrinker)
		pr_warn("metadata shrinker allocation failed; pressure reclaim unavailable\n");
	memcomp_shrinker = shrinker_alloc(SHRINKER_NONSLAB |
					SHRINKER_NUMA_AWARE, "memcomp");
	if (!memcomp_shrinker) {
		ret = -ENOMEM;
		goto err_pools;
	}
	memcomp_shrinker->count_objects = memcomp_shrinker_count;
	memcomp_shrinker->scan_objects = memcomp_shrinker_scan;
	shrinker_register(memcomp_shrinker);
	if (memcomp_meta_shrinker) {
		memcomp_meta_shrinker->count_objects = mc16_meta_shrinker_count;
		memcomp_meta_shrinker->scan_objects = mc16_meta_shrinker_scan;
		shrinker_register(memcomp_meta_shrinker);
	}
	memcomp_async_init(&memcomp_async_pool, memcomp_compressor);
	/* Make every initialized resource visible before enabling callers. */
	smp_store_release(&memcomp_enabled, true);
	pr_info("using %s\n", memcomp_compressor);

	return 0;

err_pools:
	shrinker_free(memcomp_meta_shrinker);
	memcomp_meta_shrinker = NULL;
	memcomp_destroy_pools();
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

/* The startup decision and late async work share memcomp_select_lock. */
struct memcomp_compressor_select {
	struct delayed_work work;
	struct notifier_block nb;
	struct memcomp_async_pool *pool;
	char *compressor;
	unsigned long deadline;
	bool decided;
	bool started;
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
	.pool = &memcomp_async_pool,
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
	if (select->decided) {
		if (select->started)
			memcomp_async_init(select->pool, select->compressor);
		goto out;
	}

	if (crypto_has_comp(select->compressor, 0, 0) != 1) {
		if (time_before(now, select->deadline)) {
			/* Not mod_: a registration may have asked for sooner. */
			queue_delayed_work(system_unbound_wq, &select->work,
					   select->deadline - now);
			goto out;
		}
		pr_warn("compressor %s unavailable\n", select->compressor);
	}
	/* The synchronous choice is final; keep listening for its async form. */
	select->decided = true;
	if (crypto_has_comp(select->compressor, 0, 0) == 1 &&
	    !memcomp_start()) {
		select->started = true;
		goto out;
	}

	if (strcmp(select->compressor, MEMCOMP_FALLBACK_COMPRESSOR)) {
		pr_warn("falling back to %s\n", MEMCOMP_FALLBACK_COMPRESSOR);
		strscpy(select->compressor, MEMCOMP_FALLBACK_COMPRESSOR,
			sizeof(memcomp_compressor));
		if (crypto_has_comp(select->compressor, 0, 0) == 1 &&
		    !memcomp_start()) {
			select->started = true;
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
			  "async_payloads %llu\n"
			  "async_fail %llu\n"
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
			  (u64)atomic64_read(&memcomp_async_payloads),
			  (u64)atomic64_read(&memcomp_async_fail),
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
static ssize_t packed_swap_events_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	unsigned long pool_pages = memcomp_pool_total_size() >> PAGE_SHIFT;

	return sysfs_emit(buf,
			  "writeback_pool_pages %lu\n"
			  "writeback_pressure_scopes %lu\n"
			  "writeback_pressure_events %llu\n"
			  "writeback_pressure_scans %llu\n"
			  "writeback_compression_throttled %llu\n"
			  "writeback_limited_batches %llu\n"
			  "writeback_budget_granted %llu\n"
			  "writeback_payloads %llu\n"
			  "writeback_zs_reclaimed %llu\n"
			  "writeback_zs_no_progress %llu\n"
			  "writeback_decompress_error %llu\n"
			  "writeback_write_error %llu\n"
			  "writeback_gate_open %d\n"
			  "writeback_gate_opens %llu\n"
			  "writeback_packed_slots %llu\n"
			  "writeback_packed_payloads %llu\n"
			  "writeback_defrag_moved %llu\n",
			  pool_pages,
			  memcomp_writeback_pressure_scope_count(),
			  (u64)atomic64_read(&memcomp_writeback_pressure_events),
			  (u64)atomic64_read(&memcomp_writeback_pressure_scans),
			  (u64)atomic64_read(&memcomp_writeback_compression_throttled),
			  (u64)atomic64_read(&memcomp_writeback_limited_batches),
			  (u64)atomic64_read(&memcomp_writeback_budget_granted),
			  (u64)atomic64_read(&memcomp_writeback_payloads),
			  (u64)atomic64_read(&memcomp_writeback_zs_reclaimed),
			  (u64)atomic64_read(&memcomp_writeback_zs_no_progress),
			  (u64)atomic64_read(&memcomp_writeback_decompress_error),
			  (u64)atomic64_read(&memcomp_writeback_write_error),
			  atomic_read(&memcomp_writeback_gate_open),
			  (u64)atomic64_read(&memcomp_writeback_gate_opens),
			  (u64)atomic64_read(&memcomp_writeback_packed_slots),
			  (u64)atomic64_read(&memcomp_writeback_packed_payloads),
			  (u64)atomic64_read(&memcomp_writeback_defrag_moved));
}
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
static struct kobj_attribute packed_swap_events_attr =
	__ATTR_RO(packed_swap_events);
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
	&packed_swap_events_attr.attr,
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
