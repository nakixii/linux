/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_STORAGE_FIXTURE_H
#define _MM_MEMCOMP_TESTS_STORAGE_FIXTURE_H

/* Fault/ordering injection only. No operation or XA budget accounting. */
struct mc16_storage_fixture {
	struct task_struct *task;
	unsigned int allocations;
	unsigned int fail_at;
	bool fail_descriptor;
	bool hold_segments;
	struct rcu_head *delayed_segments;
	void (*allocation_window)(void *data);
	void *window_data;
};

static struct mc16_storage_fixture __rcu *mc16_storage_fixture;

static void mc16_kunit_queue_rcu(struct rcu_head *head, rcu_callback_t func)
{
	struct mc16_storage_fixture *probe;
	bool held = false;

	rcu_read_lock();
	probe = rcu_dereference(mc16_storage_fixture);
	if (probe && probe->task == current && probe->hold_segments &&
	    func == mc16_segment_rcu_free) {
		head->next = probe->delayed_segments;
		probe->delayed_segments = head;
		held = true;
	}
	rcu_read_unlock();
	if (!held)
		call_rcu(head, func);
}

static void mc16_test_drain_segments(struct mc16_storage_fixture *probe)
{
	struct rcu_head *head;

	probe->hold_segments = false;
	while ((head = probe->delayed_segments)) {
		probe->delayed_segments = head->next;
		call_rcu(head, mc16_segment_rcu_free);
	}
}

static void *mc16_kunit_storage_alloc(size_t bytes, gfp_t gfp,
				      struct obj_cgroup *objcg)
{
	struct mc16_storage_fixture *probe;
	void (*window)(void *data) = NULL;
	void *data = NULL, *ptr;
	bool modeled = false, fail = false;
#ifdef CONFIG_MEMCG
	struct memcomp_kunit_objcg *model;
#endif

	rcu_read_lock();
#ifdef CONFIG_MEMCG
	model = rcu_dereference(memcomp_kunit_objcg);
	modeled = model && objcg == &model->objcg;
#endif
	probe = rcu_dereference(mc16_storage_fixture);
	if (probe && probe->task == current) {
		probe->allocations++;
		fail = probe->allocations == probe->fail_at ||
			(probe->fail_descriptor &&
			 bytes == sizeof(struct mc16_segment_retire));
		window = probe->allocation_window;
		data = probe->window_data;
		probe->allocation_window = NULL;
	}
	rcu_read_unlock();
	if (fail)
		return NULL;
	/* Synthetic objcg has no css: explicitly model only the allocator. */
	ptr = modeled ? kmalloc(bytes, gfp & ~__GFP_ACCOUNT) :
		mc16_kmalloc_owner(bytes, gfp, objcg);
	if (window)
		window(data);
	return ptr;
}
#endif /* _MM_MEMCOMP_TESTS_STORAGE_FIXTURE_H */
