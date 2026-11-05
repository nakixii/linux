/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_ZERO_H
#define _MM_MEMCOMP_ZERO_H

#include <linux/compiler.h>
#include <linux/limits.h>
#include <asm/page.h>
#include <linux/minmax.h>
#include <linux/percpu.h>
#include <linux/string.h>

/*
 * Pure-value ownership only: no slot, record, folio reference or objcg.
 * The winning PTL/xa transaction accounts the actual committed owner count.
 * Initial anonymous PTEs use pte_init; fork uses pte_get; removal uses pte_put.
 * A failed installation accounts nothing, MOVE is neutral, and neither a
 * transport folio nor a present shmem mapping owns an additional zero count.
 * Cache ownership has its own get/put pair, independent of anonymous PTEs.
 * These helpers neither publish PTEs nor select the producer's zero branch.
 *
 * The always-on production ledger is perCPU, not a new global hot-path RMW.
 * Get and put may run on different CPUs, so individual CPU values can be
 * negative. Sum ALL possible CPUs before interpreting either signed domain.
 * Snapshots are observational, like the existing memcomp logical statistics;
 * an in-flight cross-CPU transfer need not yield a transactional snapshot.
 * There is no runtime reset and no per-CPU underflow test.
 */
struct mc16_zero_owners {
	s64 pte_pages;
	s64 cache_pages;
};

static DEFINE_PER_CPU(struct mc16_zero_owners, mc16_zero_owners);

enum mc16_zero_event {
	MC16_ZERO_PTE_INIT,
	MC16_ZERO_PTE_GET,
	MC16_ZERO_PTE_PUT,
	MC16_ZERO_CACHE_GET,
	MC16_ZERO_CACHE_PUT,
	MC16_ZERO_EVENTS,
};

/* Separate from slot get/put, members, stored/compressed RMW and eligibility. */
struct mc16_zero_diagnostics {
	u64 calls[MC16_ZERO_EVENTS];
	u64 pages[MC16_ZERO_EVENTS];
};

#ifdef CONFIG_MEMCOMP_DEBUG_STATS
static DEFINE_PER_CPU(struct mc16_zero_diagnostics, mc16_zero_diagnostics);
#endif

static inline void mc16_zero_event(enum mc16_zero_event event,
				   unsigned long nr)
{
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	this_cpu_inc(mc16_zero_diagnostics.calls[event]);
	this_cpu_add(mc16_zero_diagnostics.pages[event], nr);
#endif
}

/* Reject a wide unsigned input before signed conversion/negation. */
static inline bool mc16_zero_nr_valid(unsigned long nr)
{
	return nr <= LONG_MAX;
}

static inline bool mc16_zero_pte_add(unsigned long nr,
				     enum mc16_zero_event event)
{
	if (!mc16_zero_nr_valid(nr))
		return false;
	if (nr) {
		this_cpu_add(mc16_zero_owners.pte_pages, (s64)nr);
		mc16_zero_event(event, nr);
	}
	return true;
}

/* Initial committed PTEs, not fork gets and not a counter reset. */
static inline bool mc16_zero_pte_init(unsigned long nr)
{
	return mc16_zero_pte_add(nr, MC16_ZERO_PTE_INIT);
}

static inline bool mc16_zero_pte_get(unsigned long nr)
{
	return mc16_zero_pte_add(nr, MC16_ZERO_PTE_GET);
}

/* Caller owns nr logical PTEs; this is not a speculative refcount put. */
static inline bool mc16_zero_pte_put(unsigned long nr)
{
	if (!mc16_zero_nr_valid(nr))
		return false;
	if (nr) {
		this_cpu_add(mc16_zero_owners.pte_pages, -(s64)nr);
		mc16_zero_event(MC16_ZERO_PTE_PUT, nr);
	}
	return true;
}

static inline bool mc16_zero_cache_get(unsigned long nr)
{
	if (!mc16_zero_nr_valid(nr))
		return false;
	if (nr) {
		this_cpu_add(mc16_zero_owners.cache_pages, (s64)nr);
		mc16_zero_event(MC16_ZERO_CACHE_GET, nr);
	}
	return true;
}

static inline bool mc16_zero_cache_put(unsigned long nr)
{
	if (!mc16_zero_nr_valid(nr))
		return false;
	if (nr) {
		this_cpu_add(mc16_zero_owners.cache_pages, -(s64)nr);
		mc16_zero_event(MC16_ZERO_CACHE_PUT, nr);
	}
	return true;
}

/* Keep raw signed totals available to quiescent diagnostic/balance checks. */
static inline void mc16_zero_owners_read(struct mc16_zero_owners *snapshot)
{
	u64 pte = 0, cache = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		const struct mc16_zero_owners *owners;

		owners = per_cpu_ptr(&mc16_zero_owners, cpu);
		/* Unsigned accumulation also handles negative perCPU contributions. */
		pte += (u64)READ_ONCE(owners->pte_pages);
		cache += (u64)READ_ONCE(owners->cache_pages);
	}
	snapshot->pte_pages = (s64)pte;
	snapshot->cache_pages = (s64)cache;
}

static inline void
mc16_zero_diagnostics_read(struct mc16_zero_diagnostics *snapshot)
{
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	int cpu, event;
#endif

	memset(snapshot, 0, sizeof(*snapshot));
#ifdef CONFIG_MEMCOMP_DEBUG_STATS
	for_each_possible_cpu(cpu) {
		const struct mc16_zero_diagnostics *debug;

		debug = per_cpu_ptr(&mc16_zero_diagnostics, cpu);
		for (event = 0; event < MC16_ZERO_EVENTS; event++) {
			snapshot->calls[event] += READ_ONCE(debug->calls[event]);
			snapshot->pages[event] += READ_ONCE(debug->pages[event]);
		}
	}
#endif
}

static inline u64 mc16_zero_pages(void)
{
	struct mc16_zero_owners owners;

	mc16_zero_owners_read(&owners);
	/* Clamp only after summing each entire domain, never per CPU. */
	return (u64)max_t(s64, 0, owners.pte_pages) +
	       (u64)max_t(s64, 0, owners.cache_pages);
}

/*
 * stat stored = slot stored + logical zero bytes, compressed = slot compressed.
 * Do not also call the slot stored/compressed add/sub wrappers for zero owners,
 * even with argument zero. No slot metadata, payload charge or eligible page.
 * Debug samefill totals can add zero_pages once, as in the old zero backend.
 */
static inline u64 mc16_zero_stored_bytes(u64 slot_stored_bytes)
{
	return slot_stored_bytes + (mc16_zero_pages() << PAGE_SHIFT);
}

/* Already decoded MEMCOMP token only. Nonzero leaves optional outputs alone. */
static inline bool mc16_zero_entry_stat(unsigned long token,
					unsigned long *compressed, int *mapcount)
{
	if (token)
		return false;
	if (compressed)
		*compressed = 0;
	if (mapcount)
		*mapcount = 1;
	return true;
}

/* Exactly one readable base page; supports unaligned input, no codec/alloc. */
static inline bool mc16_zero_is_page(const void *src, size_t bytes)
{
	return src && bytes == PAGE_SIZE && !memchr_inv(src, 0, PAGE_SIZE);
}

#endif /* _MM_MEMCOMP_ZERO_H */
