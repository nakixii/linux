/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_WORKINGSET_LOAD_TEST_H
#define _MM_MEMCOMP_TESTS_WORKINGSET_LOAD_TEST_H

/*
 * Author: nakixii
 *
 * Include after fault_test.h. Real MM, payload, charge and stock workingset.
 * Required observation seam: at the start of mc16_load_page(), after local
 * declarations, under CONFIG_MEMCOMP_KUNIT_TEST:
 *	KUNIT_STATIC_STUB_REDIRECT(mc16_load_page, ref, dst);
 * The observer calls the original function, not a second implementation.
 * Its hit count is asserted; omitting this seam cannot silently pass.
 */
#ifdef MC16_FAULT_MM_TEST_ENABLED
#define MC16_WORKINGSET_LOAD_TEST_ENABLED

#include <kunit/static_stub.h>
#include <linux/cgroup.h>
#include <linux/mm_inline.h>
#include <linux/swap_slots.h>
#include <linux/vmstat.h>
#include "../../memcontrol-v1.h"

#if defined(CONFIG_PSI) && defined(CONFIG_TASK_DELAY_ACCT)
#define MC16_WORKINGSET_SCOPE_TEST_ENABLED
#endif

struct mc16_wl {
	struct kunit *test;
	struct mc16_ft *fault;
	struct memcomp_ref refs[MC16_FT_PAGES];
	struct folio *dst[2];
	unsigned int nr;
	unsigned int reads;
	int read_error;
	bool observe_scope;
	unsigned int scope_reads;
};

/* Test-task-local static stub; KUnit never redirects another task's reads. */
static struct mc16_wl *mc16_wl_current;

static void mc16_wl_cleanup(void *data)
{
	struct mc16_wl *f = data;
	unsigned int i;

	mc16_wl_current = NULL;
	for (i = 0; i < ARRAY_SIZE(f->dst); i++) {
		if (!f->dst[i])
			continue;
		if (folio_test_locked(f->dst[i]))
			folio_unlock(f->dst[i]);
		folio_clear_active(f->dst[i]);
		folio_put(f->dst[i]);
	}
	for (i = 0; i < f->nr; i++)
		mc16_ref_put(&f->refs[i]);
}

static struct mc16_wl *mc16_wl_begin(struct kunit *test, unsigned int nr,
				     bool attached)
{
	struct mc16_wl *f;
	unsigned int i;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->test = test;
	f->fault = mc16_ft_begin(test, nr, false, !attached);
	if (!f->fault)
		return NULL;
	if (kunit_add_action_or_reset(test, mc16_wl_cleanup, f))
		return NULL;
	for (i = 0; i < nr; i++) {
		if (!mc16_get(f->fault->tokens[i], &f->refs[i]))
			return NULL;
		f->nr++;
	}
	return f;
}

static struct folio *mc16_wl_destination(struct mc16_wl *f, unsigned int index,
					 unsigned int order)
{
	struct folio *folio;
	int nid = folio_nid(f->fault->refs->storage->owner->folio);

	folio = __folio_alloc_node(GFP_KERNEL, order, nid);
	if (!folio)
		return NULL;
	f->dst[index] = folio;
	folio_lock(folio);
	if (mc16_user_charge(&f->refs[0], folio, f->fault->mm, GFP_KERNEL))
		return NULL;
	__folio_set_swapbacked(folio);
	return folio;
}

/* Produce a native shadow: never encode generation, memcg or distance bits. */
static unsigned long mc16_wl_shadow(struct mc16_wl *f, bool hot)
{
	struct folio *source;
	struct mem_cgroup *memcg;
	unsigned long shadow = 0;
	int nid = folio_nid(f->fault->refs->storage->owner->folio);

	source = __folio_alloc_node(GFP_KERNEL, 0, nid);
	if (!source)
		return 0;
	folio_lock(source);
	if (mc16_user_charge(&f->refs[0], source, f->fault->mm, GFP_KERNEL))
		goto out;
	__folio_set_swapbacked(source);
	if (hot) {
		folio_set_workingset(source);
#ifdef CONFIG_LRU_GEN
		/* Model a frequently accessed source using stock's flag format. */
		source->flags |= LRU_REFS_MASK;
#endif
	}
	memcg = folio_memcg(source);
	if (!memcg && !mem_cgroup_disabled())
		memcg = root_mem_cgroup;
	shadow = (unsigned long)workingset_memcomp_eviction(source, memcg);
out:
	folio_clear_workingset(source);
#ifdef CONFIG_LRU_GEN
	source->flags &= ~LRU_REFS_MASK;
#endif
	folio_unlock(source);
	folio_put(source);
	return shadow;
}

static void mc16_wl_set_shadow(struct memcomp_ref *ref, unsigned long shadow)
{
	mutex_lock(memcomp_lock_of(ref->token));
	*mc16_shadow_ptr(ref) = shadow;
	mutex_unlock(memcomp_lock_of(ref->token));
}

static u64 mc16_wl_get_shadow(struct memcomp_ref *ref)
{
	u64 shadow;

	mutex_lock(memcomp_lock_of(ref->token));
	shadow = *mc16_shadow_ptr(ref);
	mutex_unlock(memcomp_lock_of(ref->token));
	return shadow;
}

/*
 * Include pending per-CPU node deltas, not just the periodically folded
 * counter. These cases run serially in the isolated KUnit guest.
 */
static long mc16_wl_refaults(void)
{
	long count = atomic_long_read(&vm_node_stat[WORKINGSET_REFAULT_ANON]);
#ifdef CONFIG_SMP
	int nid, cpu;

	for_each_online_node(nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);

		for_each_online_cpu(cpu)
			count += READ_ONCE(per_cpu_ptr(pgdat->per_cpu_nodestats,
					cpu)->vm_node_stat_diff[WORKINGSET_REFAULT_ANON]);
	}
#endif
	return count;
}

/* Match handle_mm_fault()'s context without replacing stock's policy. */
static vm_fault_t mc16_wl_fault(struct mc16_ft *f, unsigned int index)
{
	vm_fault_t ret;
#ifdef CONFIG_LRU_GEN
	bool saved = current->in_lru_fault;

	current->in_lru_fault = true;
#endif
	ret = mc16_ft_fault(f->test, f->mm, f->addr + index * PAGE_SIZE,
			    mc16_ft_orig(f, index), FAULT_FLAG_WRITE);
#ifdef CONFIG_LRU_GEN
	current->in_lru_fault = saved;
#endif
	return ret;
}

#ifdef CONFIG_VM_EVENT_COUNTERS
struct mc16_wl_major {
	unsigned long global;
	unsigned long memcg;
	bool has_memcg;
};

static struct mc16_wl_major mc16_wl_major_read(struct mm_struct *mm)
{
	struct mc16_wl_major count = { };
	struct mem_cgroup *memcg;
	int cpu;

	for_each_possible_cpu(cpu)
		count.global += READ_ONCE(per_cpu(vm_event_states, cpu).event[PGMAJFAULT]);
	if (mem_cgroup_disabled())
		return count;
	rcu_read_lock();
	memcg = mem_cgroup_from_task(rcu_dereference(mm->owner));
	if (memcg)
		css_get(&memcg->css);
	rcu_read_unlock();
	if (memcg) {
		/* The public rate-limited flush need not flush a single event. */
		cgroup_rstat_flush(memcg->css.cgroup);
		count.memcg = memcg_events(memcg, PGMAJFAULT);
		count.has_memcg = true;
		css_put(&memcg->css);
	}
	return count;
}

static void mc16_wl_major_expect(struct kunit *test, struct mm_struct *mm,
				 struct mc16_wl_major before, unsigned int nr)
{
	struct mc16_wl_major after = mc16_wl_major_read(mm);

	KUNIT_EXPECT_EQ(test, after.global - before.global, (unsigned long)nr);
	KUNIT_EXPECT_EQ(test, after.has_memcg, before.has_memcg);
	if (before.has_memcg)
		KUNIT_EXPECT_EQ(test, after.memcg - before.memcg, (unsigned long)nr);
}
#endif

static int mc16_wl_read_page(struct memcomp_ref *ref, void *dst)
{
	struct mc16_wl *f = mc16_wl_current;
	int ret;

	f->reads++;
#ifdef MC16_WORKINGSET_SCOPE_TEST_ENABLED
	if (f->observe_scope) {
		struct folio *folio = page_folio(kmap_to_page(dst));
		bool workingset = folio_test_workingset(folio);

		/* Match the native refault result, not the source's hot hint. */
		KUNIT_EXPECT_EQ(f->test, (bool)current->in_memstall, workingset);
		KUNIT_EXPECT_EQ(f->test, (bool)current->in_thrashing, workingset);
		KUNIT_EXPECT_GT(f->test, current->delays->swapin_start, 0ULL);
		if (workingset) {
			KUNIT_EXPECT_GT(f->test, current->delays->thrashing_start, 0ULL);
			f->scope_reads++;
		}
		if (lru_gen_enabled())
			KUNIT_EXPECT_TRUE(f->test, workingset);
	}
#endif
	kunit_deactivate_static_stub(f->test, mc16_load_page);
	ret = mc16_load_page(ref, dst);
	kunit_activate_static_stub(f->test, mc16_load_page, mc16_wl_read_page);
	/* Make the real delayacct_end() positive even for a same-clock read. */
	udelay(1);
	return ret ? ret : f->read_error;
}

static void mc16_wl_observe(struct mc16_wl *f, int error, bool scope)
{
	f->read_error = error;
	f->observe_scope = scope;
	mc16_wl_current = f;
	kunit_activate_static_stub(f->test, mc16_load_page, mc16_wl_read_page);
}

static void memcomp_test_workingset_load_charge_failure(struct kunit *test)
{
	struct mc16_wl *f = mc16_wl_begin(test, 1, false);
	unsigned long shadow;
	long before;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	before = mc16_wl_refaults();
	mc16_ft_probe_start(f->fault);
	mc16_fault_probe.fail_small_charge = true;
	ret = mc16_wl_fault(f->fault, 0);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_OOM);
	KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[0]), (u64)shadow);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 0L);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.small_charges, 1U);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_ft_read(f->fault, 0),
					 mc16_ft_orig(f->fault, 0)));
}

static void memcomp_test_workingset_load_take_once(struct kunit *test)
{
	struct mc16_wl *f = mc16_wl_begin(test, 1, false);
	struct folio *first, *second;
	unsigned long shadow;
	long before;
	int a, b;

	KUNIT_ASSERT_NOT_NULL(test, f);
	first = mc16_wl_destination(f, 0, 0);
	second = mc16_wl_destination(f, 1, 0);
	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	before = mc16_wl_refaults();
	a = mc16_user_refault(f->refs, 1, first);
	KUNIT_EXPECT_EQ(test, a, 0);
	KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[0]), 0ULL);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 1L);
	b = mc16_user_refault(f->refs, 1, second);
	KUNIT_EXPECT_EQ(test, b, 0);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 1L);
	KUNIT_EXPECT_FALSE(test, folio_test_workingset(second));
	KUNIT_EXPECT_FALSE(test, folio_test_active(second));
	/* The counter above makes removing the real stock call observable. */
	if (!mem_cgroup_disabled()) {
		KUNIT_EXPECT_NE(test, f->refs[0].cluster->key->owner_id,
				(unsigned short)0);
		KUNIT_EXPECT_EQ(test, f->refs[0].cluster->key->owner_id,
				mem_cgroup_id(root_mem_cgroup));
		KUNIT_EXPECT_PTR_EQ(test, folio_memcg(first), root_mem_cgroup);
	}
}

static void memcomp_test_workingset_load_ineligible(struct kunit *test)
{
	struct mc16_wl *f = mc16_wl_begin(test, MC16_FT_PAGES, true);
	struct folio *dst;
	struct mc16_view view;
	unsigned long shadow;
	unsigned int kind, i;
	struct memcomp_ref *last;
	u16 saved;
	u16 tested;
	long before;

	KUNIT_ASSERT_NOT_NULL(test, f);
	dst = mc16_wl_destination(f, 0, 2);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	last = &f->refs[f->nr - 1];
	for (kind = 0; kind < 4; kind++) {
		int ret;
		u64 expected = kind == 2 ? MC16_SHADOW_PUBLISHING : shadow;

		for (i = 0; i < f->nr; i++)
			mc16_wl_set_shadow(&f->refs[i], shadow);
		mutex_lock(memcomp_lock_of(last->token));
		mc16_view_locked(last, &view);
		saved = *view.flags;
		if (!kind)
			*view.flags = (saved & ~MC16_STATE_MASK) | MC16_PENDING |
				      MC16_F_RESIDENT;
		else if (kind == 1)
			*view.flags |= MC16_F_RESIDENT;
		else if (kind == 3)
			*view.flags = (saved & ~(MC16_STATE_MASK |
						 MC16_F_RESIDENT)) | MC16_PENDING;
		tested = *view.flags;
		*mc16_shadow_ptr(last) = expected;
		mutex_unlock(memcomp_lock_of(last->token));
		before = mc16_wl_refaults();
		ret = mc16_user_refault(f->refs, f->nr, dst);
		mutex_lock(memcomp_lock_of(last->token));
		mc16_view_locked(last, &view);
		*view.flags = saved;
		mutex_unlock(memcomp_lock_of(last->token));
		if (kind == 3) {
			kunit_info(test, "ineligible: PENDING without RESIDENT");
			KUNIT_EXPECT_TRUE(test, mc16_flags_valid(tested));
			KUNIT_EXPECT_EQ(test, mc16_flags_state(tested),
					(unsigned int)MC16_PENDING);
			KUNIT_EXPECT_FALSE(test, tested & MC16_F_RESIDENT);
			KUNIT_EXPECT_NE(test, expected,
					(u64)MC16_SHADOW_PUBLISHING);
		}
		KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
		KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(last), expected);
		for (i = 0; i + 1 < f->nr; i++)
			KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[i]),
					(u64)shadow);
		KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 0L);
	}
	for (i = 0; i < f->nr; i++)
		mc16_wl_set_shadow(&f->refs[i], 0);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define MC16_WORKINGSET_MTHP_TEST_ENABLED
static const unsigned int mc16_wl_batch_cases[] = { 0, 1, 2, 3 };

static void mc16_wl_batch_desc(const unsigned int *kind, char *desc)
{
	static const char * const names[] = {
		"same", "empty-first", "different", "part-consumed",
	};

	strscpy(desc, names[*kind], KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(mc16_wl_batch, mc16_wl_batch_cases, mc16_wl_batch_desc);

static void memcomp_test_workingset_load_mthp(struct kunit *test)
{
	unsigned int kind = *(const unsigned int *)test->param_value;
	struct mc16_wl *f = mc16_wl_begin(test, MC16_FT_PAGES, true);
	unsigned long hot, cold;
	unsigned int i;
	long before;
	vm_fault_t ret;
#ifdef CONFIG_VM_EVENT_COUNTERS
	struct mc16_wl_major major;
#endif

	KUNIT_ASSERT_NOT_NULL(test, f);
	/*
	 * Unlike the old convenience helper, this case must not silently skip
	 * a configured order-2 guest. Keep restoration in the existing fixture.
	 */
	f->fault->orders = READ_ONCE(huge_anon_orders_always);
	f->fault->saved_orders = true;
	WRITE_ONCE(huge_anon_orders_always, f->fault->orders | BIT(2));
	mc16_ft_policy(f->fault, VM_HUGEPAGE, VM_NOHUGEPAGE);
	KUNIT_ASSERT_NE(test, THP_ORDERS_ALL_ANON & BIT(2), 0UL);
	hot = mc16_wl_shadow(f, true);
	cold = mc16_wl_shadow(f, false);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)hot));
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)cold));
	KUNIT_ASSERT_NE(test, hot, cold);
	for (i = 0; i < f->nr; i++) {
		unsigned long shadow = hot;

		if (kind == 1 && !i)
			shadow = 0;
		if (kind == 2 && i)
			shadow = cold;
		if (kind == 3 && (i & 1))
			shadow = 0;
		mc16_wl_set_shadow(&f->refs[i], shadow);
	}
	before = mc16_wl_refaults();
#ifdef CONFIG_VM_EVENT_COUNTERS
	major = mc16_wl_major_read(f->fault->mm);
#endif
	mc16_ft_probe_start(f->fault);
	/* Middle PTE: only the span's first shadow is authoritative. */
	ret = mc16_wl_fault(f->fault, 2);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.large_allocs, 1U);
	KUNIT_EXPECT_EQ(test, mc16_fault_probe.small_allocs, 0U);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before,
			kind == 1 ? 0L : (long)MC16_FT_PAGES);
	for (i = 0; i < f->nr; i++) {
		KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[i]), 0ULL);
		mc16_ft_present(f->fault, i, 2, MC16_FT_PAGES);
	}
	mc16_ft_counts(f->fault, 0, MC16_FT_PAGES);
#ifdef CONFIG_VM_EVENT_COUNTERS
	mc16_wl_major_expect(test, f->fault->mm, major, 1);
#endif
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static void memcomp_test_workingset_load_read_error(struct kunit *test)
{
	struct mc16_wl *f = mc16_wl_begin(test, 1, false);
	unsigned long shadow;
	long before;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	mc16_wl_observe(f, -EIO, false);
	before = mc16_wl_refaults();
	ret = mc16_wl_fault(f->fault, 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_SIGBUS);
	KUNIT_EXPECT_EQ(test, f->reads, 1U);
	KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[0]), 0ULL);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 1L);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_ft_read(f->fault, 0),
					 mc16_ft_orig(f->fault, 0)));
	/* A retry succeeds but must not recreate or consume another shadow. */
	f->read_error = 0;
	ret = mc16_wl_fault(f->fault, 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	KUNIT_EXPECT_EQ(test, f->reads, 2U);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 1L);
}

static void memcomp_test_workingset_load_pte_race(struct kunit *test)
{
	struct mc16_wl *f = mc16_wl_begin(test, 1, false);
	unsigned long shadow;
	long before;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	before = mc16_wl_refaults();
	mc16_ft_probe_start(f->fault);
	mc16_fault_probe.before_install = mc16_ft_race;
	ret = mc16_wl_fault(f->fault, 0);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_TRUE(test, f->fault->actor_ran);
	KUNIT_EXPECT_EQ(test, f->fault->actor_error, 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_MAJOR);
	KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[0]), 0ULL);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 1L);
	KUNIT_EXPECT_TRUE(test, pte_same(mc16_ft_read(f->fault, 0),
					 pte_swp_mkexclusive(mc16_ft_orig(f->fault, 0))));
}

#ifdef CONFIG_VM_EVENT_COUNTERS
#define MC16_WORKINGSET_MAJOR_TEST_ENABLED
static void memcomp_test_workingset_load_major_shared(struct kunit *test)
{
	struct mc16_wl *f = mc16_wl_begin(test, 1, false);
	struct mc16_wl_major major;
	unsigned long shadow;
	long before;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_ft_install(f->fault, 1,
					      mc16_ft_orig(f->fault, 0)), 0);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	major = mc16_wl_major_read(f->fault->mm);
	before = mc16_wl_refaults();
	ret = mc16_wl_fault(f->fault, 0);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	mc16_wl_major_expect(test, f->fault->mm, major, 1);
	/*
	 * Index 1 owns the same token, not tokens[1]. The second actual
	 * materialization is another major, but not another stock refault.
	 */
	ret = mc16_ft_fault(test, f->fault->mm, f->fault->addr + PAGE_SIZE,
			    mc16_ft_orig(f->fault, 0), FAULT_FLAG_WRITE);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	mc16_wl_major_expect(test, f->fault->mm, major, 2);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - before, 1L);
	KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[0]), 0ULL);
	mc16_ft_counts(f->fault, 0, 2);
}

static void memcomp_test_workingset_load_zero_not_major(struct kunit *test)
{
	struct mc16_zl_mm *f = mc16_zl_begin(test);
	struct mc16_wl_major major;
	pte_t orig = mc16_zl_zero_pte();
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, mc16_zl_install(f, orig), 0);
	major = mc16_wl_major_read(f->mm);
	ret = mc16_ft_fault(test, f->mm, f->addr, orig, FAULT_FLAG_WRITE);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	mc16_wl_major_expect(test, f->mm, major, 0);
	mc16_zl_present(f, mc16_zl_read(f));
}

#ifdef MC16_RESIDENT_TEST_ENABLED
#define MC16_WORKINGSET_RESIDENT_MAJOR_TEST_ENABLED
static void memcomp_test_workingset_load_resident_not_major(struct kunit *test)
{
	struct mc16_rl *f = mc16_rl_begin(test, 1, 0);
	struct mc16_wl_major major;
	vm_fault_t ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	major = mc16_wl_major_read(f->mm);
	/* Use the complete dispatcher, not the resident helper in isolation. */
	ret = mc16_ft_fault(test, f->mm, f->addr, f->orig, FAULT_FLAG_WRITE);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)VM_FAULT_NOPAGE);
	mc16_wl_major_expect(test, f->mm, major, 0);
	KUNIT_EXPECT_TRUE(test, pte_present(mc16_rl_read(f)));
}
#endif
#endif /* CONFIG_VM_EVENT_COUNTERS */

#ifdef MC16_WORKINGSET_SCOPE_TEST_ENABLED
struct mc16_wl_delays {
	u32 swapin;
	u32 thrashing;
};

static struct mc16_wl_delays mc16_wl_delays_read(void)
{
	return (struct mc16_wl_delays) {
		.swapin = current->delays->swapin_count,
		.thrashing = current->delays->thrashing_count,
	};
}

static bool mc16_wl_scope_ready(struct kunit *test)
{
	bool ready = !static_branch_unlikely(&psi_disabled) &&
		     static_branch_unlikely(&delayacct_key) &&
		     current->delays && !current->in_memstall &&
		     !current->in_thrashing;

	/* Missing boot options are a failed precondition, not a skipped case. */
	KUNIT_EXPECT_TRUE_MSG(test, ready, "requires PSI and boot delayacct");
	return ready;
}

static void mc16_wl_scope_expect(struct kunit *test,
				 struct mc16_wl_delays before, unsigned int nr,
				 unsigned int thrashing)
{
	unsigned long pflags = 0;
	bool in_thrashing = false;

	KUNIT_EXPECT_FALSE(test, current->in_memstall);
	KUNIT_EXPECT_FALSE(test, current->in_thrashing);
	KUNIT_EXPECT_EQ(test, current->delays->swapin_count - before.swapin, nr);
	KUNIT_EXPECT_EQ(test, current->delays->thrashing_count - before.thrashing,
			thrashing);
	/* Do not let a failed end-scope mutation affect later cases. */
	if (current->in_memstall)
		psi_memstall_leave(&pflags);
	if (current->in_thrashing)
		delayacct_thrashing_end(&in_thrashing);
}

static const unsigned int mc16_wl_scope_cases[] = { 0, 1, 2 };

static void mc16_wl_scope_desc(const unsigned int *kind, char *desc)
{
	static const char * const names[] = { "success", "read-error", "pte-race" };

	strscpy(desc, names[*kind], KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(mc16_wl_scope, mc16_wl_scope_cases, mc16_wl_scope_desc);

static void memcomp_test_workingset_load_scope(struct kunit *test)
{
	unsigned int kind = *(const unsigned int *)test->param_value;
	struct mc16_wl *f;
	struct mc16_wl_delays before;
	unsigned long shadow;
	vm_fault_t ret;
#ifdef CONFIG_VM_EVENT_COUNTERS
	struct mc16_wl_major major;
#endif

	if (!mc16_wl_scope_ready(test))
		return;
	f = mc16_wl_begin(test, 1, false);
	KUNIT_ASSERT_NOT_NULL(test, f);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	mc16_wl_observe(f, kind == 1 ? -EIO : 0, true);
	if (kind == 2) {
		mc16_ft_probe_start(f->fault);
		mc16_fault_probe.before_install = mc16_ft_race;
	}
	before = mc16_wl_delays_read();
#ifdef CONFIG_VM_EVENT_COUNTERS
	major = mc16_wl_major_read(f->fault->mm);
#endif
	ret = mc16_wl_fault(f->fault, 0);
	WRITE_ONCE(mc16_fault_probe.task, NULL);
	KUNIT_EXPECT_EQ(test, (__force unsigned int)ret,
			(__force unsigned int)(kind == 1 ? VM_FAULT_SIGBUS :
				kind == 2 ? VM_FAULT_MAJOR :
				VM_FAULT_NOPAGE | VM_FAULT_MAJOR));
	KUNIT_EXPECT_EQ(test, f->reads, 1U);
	mc16_wl_scope_expect(test, before, 1, f->scope_reads);
	KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[0]), 0ULL);
#ifdef CONFIG_VM_EVENT_COUNTERS
	mc16_wl_major_expect(test, f->fault->mm, major, kind == 1 ? 0 : 1);
#endif
}

static const unsigned int mc16_wl_cache_cases[] = { 0, 1 };

KUNIT_ARRAY_PARAM(mc16_wl_cache, mc16_wl_cache_cases, mc16_wl_scope_desc);

static void memcomp_test_workingset_load_cache_scope(struct kunit *test)
{
	unsigned int error = *(const unsigned int *)test->param_value;
	struct memcomp_ref guard = { };
	struct mc16_wl *f;
	struct mc16_wl_delays before;
	struct folio *folio;
	unsigned long shadow;
	long refaults;
	int ret;
#ifdef CONFIG_LRU_GEN
	bool saved_in_fault;
#endif
#ifdef CONFIG_VM_EVENT_COUNTERS
	struct mc16_wl_major major;
#endif

	if (!mc16_wl_scope_ready(test))
		return;
	f = mc16_wl_begin(test, 1, false);
	KUNIT_ASSERT_NOT_NULL(test, f);
	folio = mc16_wl_destination(f, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	mc16_wl_observe(f, error ? -EIO : 0, true);
	KUNIT_ASSERT_TRUE(test, mc16_get(f->refs[0].token, &guard));
	before = mc16_wl_delays_read();
	refaults = mc16_wl_refaults();
#ifdef CONFIG_VM_EVENT_COUNTERS
	major = mc16_wl_major_read(f->fault->mm);
#endif
#ifdef CONFIG_LRU_GEN
	saved_in_fault = current->in_lru_fault;
	current->in_lru_fault = true;
#endif
	ret = mc16_cache_load(&guard, folio);
#ifdef CONFIG_LRU_GEN
	current->in_lru_fault = saved_in_fault;
#endif
	/* Model losing the cache replacement using the real guard abort. */
	mc16_cache_abort(&guard);
	KUNIT_EXPECT_EQ(test, ret, error ? -EIO : 0);
	KUNIT_EXPECT_EQ(test, f->reads, 1U);
	KUNIT_EXPECT_EQ(test, folio_test_uptodate(folio), !error);
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - refaults, 1L);
	KUNIT_EXPECT_EQ(test, mc16_wl_get_shadow(&f->refs[0]), 0ULL);
	mc16_wl_scope_expect(test, before, 1, f->scope_reads);
	KUNIT_EXPECT_PTR_EQ(test, guard.slot, NULL);
#ifdef CONFIG_VM_EVENT_COUNTERS
	mc16_wl_major_expect(test, f->fault->mm, major, 0);
#endif
}

static void memcomp_test_workingset_load_scope_cold(struct kunit *test)
{
	struct mc16_wl *f;
	struct folio *folio;
	struct mc16_user_read scope;
	struct mc16_wl_delays before;

	if (!mc16_wl_scope_ready(test))
		return;
	f = mc16_wl_begin(test, 1, false);
	KUNIT_ASSERT_NOT_NULL(test, f);
	folio = mc16_wl_destination(f, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_ASSERT_FALSE(test, folio_test_workingset(folio));
	before = mc16_wl_delays_read();
	mc16_user_read_start(&scope, folio);
	KUNIT_EXPECT_FALSE(test, scope.workingset);
	KUNIT_EXPECT_FALSE(test, current->in_memstall);
	KUNIT_EXPECT_FALSE(test, current->in_thrashing);
	udelay(1);
	mc16_user_read_end(&scope);
	KUNIT_EXPECT_FALSE(test, current->in_memstall);
	KUNIT_EXPECT_FALSE(test, current->in_thrashing);
	KUNIT_EXPECT_EQ(test, current->delays->swapin_count - before.swapin, 1U);
	KUNIT_EXPECT_EQ(test, current->delays->thrashing_count - before.thrashing, 0U);
}

/* Deterministic scope-on control, independent of native recency policy. */
static void memcomp_test_workingset_load_scope_hot(struct kunit *test)
{
	struct mc16_wl *f;
	struct folio *folio;
	struct mc16_user_read scope;
	struct mc16_wl_delays before;
	u64 previous_swapin_start, this_swapin_start;

	if (!mc16_wl_scope_ready(test))
		return;
	f = mc16_wl_begin(test, 1, false);
	KUNIT_ASSERT_NOT_NULL(test, f);
	folio = mc16_wl_destination(f, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_set_workingset(folio);
	/*
	 * Leave a real, nonzero old timestamp on this CPU. The tested start
	 * must replace it; a later end can increment count even without start.
	 * Keep the warmup outside the existing count snapshot.
	 */
	preempt_disable();
	delayacct_swapin_start();
	previous_swapin_start = current->delays->swapin_start;
	udelay(1);
	delayacct_swapin_end();
	before = mc16_wl_delays_read();
	mc16_user_read_start(&scope, folio);
	this_swapin_start = current->delays->swapin_start;
	preempt_enable();
	KUNIT_EXPECT_GT(test, previous_swapin_start, 0ULL);
	KUNIT_EXPECT_GT(test, this_swapin_start, previous_swapin_start);
	KUNIT_EXPECT_TRUE(test, scope.workingset);
	KUNIT_EXPECT_TRUE(test, current->in_memstall);
	KUNIT_EXPECT_TRUE(test, current->in_thrashing);
	KUNIT_EXPECT_GT(test, current->delays->swapin_start, 0ULL);
	KUNIT_EXPECT_GT(test, current->delays->thrashing_start, 0ULL);
	/*
	 * Repair a missing PSI entry only after the start expectations.
	 * Keep the real end path paired and its exit expectations intact.
	 */
	if (scope.workingset && !current->in_memstall)
		psi_memstall_enter(&scope.pflags);
	udelay(1);
	mc16_user_read_end(&scope);
	mc16_wl_scope_expect(test, before, 1, 1);
}

#ifdef CONFIG_SWAP
#define MC16_WORKINGSET_TRANSPORT_TEST_ENABLED
struct mc16_wl_transport {
	struct folio *folio;
	swp_entry_t swap;
	bool real;
};

static void mc16_wl_transport_cleanup(void *data)
{
	struct mc16_wl_transport *t = data;

	if (t->folio) {
		folio_lock(t->folio);
		memcomp_drop_swapcache_folio(t->folio, false);
	}
	if (t->swap.val)
		memcomp_slot_put(t->swap);
	if (t->real)
		WRITE_ONCE(mc16_kunit_real_swap, false);
}

#include "transport_reclaim_test.h"

static void memcomp_test_workingset_load_transport(struct kunit *test)
{
	struct mc16_wl_transport *t;
	struct mc16_wl *f;
	struct folio *user;
	struct mc16_user_read scope;
	struct mc16_wl_delays before;
	struct writeback_control wbc = { .sync_mode = WB_SYNC_ALL };
	struct {
		swp_entry_t entry;
		bool marked;
		struct swap_info_struct *si;
		unsigned char reserved_map, returned_map;
		bool checked, returned_marked;
	} prefetch[MEMCOMP_WRITEBACK_BATCH];
	unsigned int prefetch_nr, i;
	unsigned long shadow;
	long refaults;
	void *src, *dst;
	bool no_swap;
	int ret;
#ifdef CONFIG_VM_EVENT_COUNTERS
	struct mc16_wl_major major;
#endif

	if (!mc16_wl_scope_ready(test))
		return;
	f = mc16_wl_begin(test, 1, false);
	KUNIT_ASSERT_NOT_NULL(test, f);
	user = mc16_wl_destination(f, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, user);
	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_FALSE(test, READ_ONCE(mc16_kunit_real_swap));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							mc16_wl_transport_cleanup, t), 0);
	mutex_lock(&memcomp_writeback_lock);
	if (memcomp_writeback_slots_free) {
		mutex_unlock(&memcomp_writeback_lock);
		KUNIT_FAIL(test, "transport test requires an idle slot pool");
		return;
	}
	t->real = true;
	WRITE_ONCE(mc16_kunit_real_swap, true);
	no_swap = get_nr_swap_pages() <= 0;
	ret = memcomp_swap_folio_alloc(&t->folio, &t->swap);
	/* W keeps every unused reservation live until the native return. */
	prefetch_nr = memcomp_writeback_slots_free;
	for (i = 0; i < prefetch_nr && i < ARRAY_SIZE(prefetch); i++) {
		swp_entry_t entry = memcomp_writeback_slot_pool[i];

		prefetch[i].entry = entry;
		prefetch[i].marked =
			swap_memcomp_transport_test(swp_swap_info(entry), swp_offset(entry));
		/* Keep the bitmap/map alive across this reservation's return. */
		prefetch[i].si = get_swap_device(entry);
		prefetch[i].checked = prefetch[i].si;
		prefetch[i].reserved_map = prefetch[i].si ?
			READ_ONCE(prefetch[i].si->swap_map[swp_offset(entry)]) : 0;
		prefetch[i].returned_map = SWAP_HAS_CACHE;
		prefetch[i].returned_marked = true;
	}
	memcomp_writeback_slots_return();
	/* W excludes another memcomp take until these snapshots finish. */
	for (i = 0; i < prefetch_nr && i < ARRAY_SIZE(prefetch); i++) {
		struct swap_info_struct *si = prefetch[i].si;
		unsigned long offset = swp_offset(prefetch[i].entry);

		if (!si)
			continue;
		spin_lock(&si->lock);
		prefetch[i].returned_map = READ_ONCE(si->swap_map[offset]);
		prefetch[i].returned_marked =
			swap_memcomp_transport_test(si, offset);
		spin_unlock(&si->lock);
		put_swap_device(si);
		prefetch[i].si = NULL;
	}
	mutex_unlock(&memcomp_writeback_lock);
	/* alloc returns locked; cleanup always takes its own lock. */
	if (t->folio)
		folio_unlock(t->folio);
	if (no_swap) {
		KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
		KUNIT_EXPECT_PTR_EQ(test, t->folio, NULL);
		KUNIT_EXPECT_EQ(test, t->swap.val, 0UL);
		kunit_info(test, "ENOSPC checked; rerun with swap for native read");
		return;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, t->folio);
	/* Only copied entries/bits are read after W and the unused slots go. */
	kunit_info(test, "transport prefetch: taken=%#lx batch=%u unused=%u",
		   t->swap.val, prefetch_nr + 1, prefetch_nr);
	KUNIT_EXPECT_GT(test, prefetch_nr, 0U);
	KUNIT_EXPECT_LT(test, prefetch_nr,
			(unsigned int)ARRAY_SIZE(prefetch));
	for (i = 0; i < prefetch_nr && i < ARRAY_SIZE(prefetch); i++) {
		swp_entry_t entry = prefetch[i].entry;

		kunit_info(test,
			   "transport prefetch: index=%u entry=%#lx type=%u offset=%lu marked=%d",
			   i, entry.val, swp_type(entry), swp_offset(entry),
			   prefetch[i].marked);
		KUNIT_EXPECT_TRUE(test, prefetch[i].marked);
		kunit_info(test,
			   "transport unused return: index=%u entry=%#lx pinned=%d map=%u->%u marked=%d->%d",
			   i, entry.val, prefetch[i].checked,
			   prefetch[i].reserved_map, prefetch[i].returned_map,
			   prefetch[i].marked, prefetch[i].returned_marked);
		KUNIT_EXPECT_TRUE(test, prefetch[i].checked);
		KUNIT_EXPECT_EQ(test, prefetch[i].reserved_map,
				(unsigned char)SWAP_HAS_CACHE);
		KUNIT_EXPECT_EQ(test, prefetch[i].returned_map, (unsigned char)0);
		KUNIT_EXPECT_FALSE(test, prefetch[i].returned_marked);
	}
	folio_lock(t->folio);
	{
		bool marked = swap_memcomp_transport_test(swp_swap_info(t->swap),
							 swp_offset(t->swap));

		kunit_info(test, "transport bitmap: allocated entry=%#lx marked=%d",
			   t->swap.val, marked);
		KUNIT_EXPECT_TRUE(test, marked);
	}
	src = kmap_local_folio(t->folio, 0);
	memcpy(src, f->fault->src, PAGE_SIZE);
	kunmap_local(src);
	__folio_mark_uptodate(t->folio);
	/* Normal stock write also handles zeromap and zswap consistently. */
	ret = swap_writepage(&t->folio->page, &wbc);
	if (ret && folio_test_locked(t->folio))
		folio_unlock(t->folio);
	KUNIT_ASSERT_EQ(test, ret, 0);
	folio_lock(t->folio);
	folio_wait_writeback(t->folio);
	KUNIT_EXPECT_FALSE(test, folio_test_dirty(t->folio));
	memcomp_drop_swapcache_folio(t->folio, false);
	t->folio = NULL;
	shadow = mc16_wl_shadow(f, true);
	KUNIT_ASSERT_TRUE(test, xa_is_value((void *)shadow));
	mc16_wl_set_shadow(&f->refs[0], shadow);
	KUNIT_ASSERT_EQ(test, mc16_user_refault(f->refs, 1, user), 0);
	KUNIT_ASSERT_TRUE(test, folio_test_workingset(user));
	before = mc16_wl_delays_read();
	refaults = mc16_wl_refaults();
#ifdef CONFIG_VM_EVENT_COUNTERS
	major = mc16_wl_major_read(f->fault->mm);
#endif
	mc16_user_read_start(&scope, user);
	dst = kmap_local_folio(user, 0);
	ret = memcomp_read_swapped_page(t->swap, false, 0, 0, dst, GFP_KERNEL);
	if (!ret)
		KUNIT_EXPECT_EQ(test, memcmp(dst, f->fault->src, PAGE_SIZE), 0);
	kunmap_local(dst);
	udelay(1);
	mc16_user_read_end(&scope);
	KUNIT_EXPECT_EQ(test, ret, 0);
	mc16_wl_scope_expect(test, before, 1, 1);
	/* Native transport adds neither user refault nor major nor delay scope. */
	KUNIT_EXPECT_EQ(test, mc16_wl_refaults() - refaults, 0L);
#ifdef CONFIG_VM_EVENT_COUNTERS
	mc16_wl_major_expect(test, f->fault->mm, major, 0);
#endif
	{
		struct swap_info_struct *si;
		struct folio *cached;
		swp_entry_t released = t->swap, ordinary = { };
		unsigned long offset = swp_offset(released);
		unsigned char before_map = 0, after_map = SWAP_HAS_CACHE;
		unsigned char ordinary_map = 0;
		bool after_marked = true, ordinary_marked = false;
		bool same_device = false;
		int nr = 0;

		/*
		 * A single-slot put can leave SWAP_HAS_CACHE in a return cache.
		 * Use the native drain/disable pair so the final put reaches
		 * swap_range_free(). Never clear the transport bit here.
		 */
		disable_swap_slots_cache_lock();
		si = get_swap_device(released);
		if (si) {
			cached = swap_cache_get_folio(released, NULL, 0);
			if (cached) {
				folio_lock(cached);
				memcomp_drop_swapcache_folio(cached, false);
			}
			before_map = READ_ONCE(si->swap_map[offset]);
			memcomp_slot_put(released);
			t->swap.val = 0;
			after_map = READ_ONCE(si->swap_map[offset]);
			after_marked = swap_memcomp_transport_test(si, offset);

			/* Ordinary reservation, with no memcomp tagging or cursor edits. */
			nr = get_swap_pages(1, &ordinary, 0);
			if (nr) {
				same_device = swp_type(ordinary) == si->type;
				if (same_device) {
					ordinary_map =
						READ_ONCE(si->swap_map[swp_offset(ordinary)]);
					ordinary_marked =
					    swap_memcomp_transport_test(si, swp_offset(ordinary));
				}
				swapcache_free_entries(&ordinary, 1);
			}
			put_swap_device(si);
		}
		reenable_swap_slots_cache_unlock();
		/* No assertion or early return while holding the cache/device pins. */
		KUNIT_ASSERT_NOT_NULL(test, si);
		kunit_info(test,
			   "transport bitmap: freed=%#lx map=%u->%u marked=%d",
			   released.val, before_map, after_map, after_marked);
		KUNIT_EXPECT_EQ(test, before_map, (unsigned char)1);
		KUNIT_EXPECT_EQ(test, after_map, (unsigned char)0);
		KUNIT_EXPECT_FALSE(test, after_marked);
		kunit_info(test,
			   "transport bitmap: ordinary=%#lx same_slot=%d map=%u marked=%d",
			   ordinary.val, nr == 1 && ordinary.val == released.val,
			   ordinary_map, ordinary_marked);
		KUNIT_EXPECT_EQ(test, nr, 1);
		KUNIT_EXPECT_TRUE(test, same_device);
		KUNIT_EXPECT_EQ(test, ordinary_map, (unsigned char)SWAP_HAS_CACHE);
		KUNIT_EXPECT_FALSE(test, ordinary_marked);
	}
	mc16_tr_one(test, f->fault->src, true);
	mc16_tr_one(test, f->fault->src, false);
}
#endif /* CONFIG_SWAP */
#endif /* MC16_WORKINGSET_SCOPE_TEST_ENABLED */
#endif /* MC16_FAULT_MM_TEST_ENABLED */
#endif /* _MM_MEMCOMP_TESTS_WORKINGSET_LOAD_TEST_H */
