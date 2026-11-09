/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_AGING_TEST_H
#define _MM_MEMCOMP_TESTS_AGING_TEST_H

static atomic_t mc16_test_aging_schedules = ATOMIC_INIT(0);
static unsigned long mc16_test_aging_clock;
static unsigned long mc16_test_aging_delay;
static void (*mc16_test_aging_queue_action)(void);
static unsigned long mc16_test_aging_queue_next;
static unsigned long mc16_test_aging_queue_deadline;
static int mc16_test_aging_queue_duplicates;

/* The case action runs after all descendant storage/objcg cleanups. */
static void mc16_test_aging_reset(void)
{
	unsigned long flags;

	cancel_delayed_work_sync(&mc16_retained_aging);
	xa_lock_irqsave(&mc16_clusters, flags);
	memset(&mc16_aging, 0, sizeof(mc16_aging));
	mc16_aging.next_id = 1;
	xa_unlock_irqrestore(&mc16_clusters, flags);
	mc16_test_aging_clock = 0;
	mc16_test_aging_delay = 0;
	mc16_test_aging_queue_action = NULL;
	mc16_test_aging_queue_next = 0;
	mc16_test_aging_queue_deadline = 0;
	mc16_test_aging_queue_duplicates = 0;
}

static void mc16_test_schedule_aging(void)
{
	atomic_inc(&mc16_test_aging_schedules);
}

#ifdef CONFIG_MEMCG
static unsigned long mc16_test_aging_now(void)
{
	return mc16_test_aging_clock;
}

static void mc16_test_queue_aging(unsigned long delay)
{
	void (*action)(void) = mc16_test_aging_queue_action;

	mc16_test_aging_delay = delay;
	atomic_inc(&mc16_test_aging_schedules);
	/* A nested producer can queue too; consume this boundary only once. */
	mc16_test_aging_queue_action = NULL;
	if (action)
		action();
}

/* All traversal and transitions below execute the production step/helper. */
#define MC16_TEST_AGING_IDS (4 * MC16_AGING_BATCH + 16)
struct mc16_test_aging_batch {
	struct kunit *test;
	struct mc16_test_storage *storage;
	unsigned long ids[MC16_TEST_AGING_IDS];
	unsigned int nr;
};

struct mc16_test_aging_step {
	struct mc16_cluster *detached[MC16_AGING_BATCH + 1];
	unsigned int publications[MC16_AGING_BATCH + 1];
	unsigned int publish_attempts;
	unsigned int published;
	unsigned int nr;
	unsigned int checked;
	unsigned long next_id;
	bool active;
	bool owned;
	long delay;
};

/* Count every adapter publication attempt before forwarding to real RCU. */
static void mc16_test_aging_publish(struct kunit *test,
				    struct mc16_test_aging_step *r,
				    unsigned int index)
{
	unsigned int first;

	/* Compare addresses only: a previous callback may already have run. */
	for (first = 0; first < index; first++)
		if (r->detached[first] == r->detached[index])
			break;
	r->publish_attempts++;
	r->publications[first]++;
	KUNIT_EXPECT_EQ_MSG(test, r->publications[first], 1U,
			    "same header published more than once");
	if (r->publications[first] != 1)
		return;
	r->published++;
	mc16_queue_cluster(r->detached[index]);
}

static void mc16_test_aging_batch_cleanup(void *data)
{
	struct mc16_test_aging_batch *b = data;
	struct mc16_cluster *cluster;
	unsigned long flags, id, retained = 0;
	unsigned int i;

	cancel_delayed_work_sync(&mc16_retained_aging);
	for (i = 0; i < b->nr; i++) {
		xa_lock_irqsave(&mc16_clusters, flags);
		cluster = __xa_erase(&mc16_clusters, b->ids[i]);
		if (cluster && !xa_is_value(cluster) &&
		    !xa_is_internal(cluster)) {
			if (cluster->key->current_cluster == cluster)
				cluster->key->current_cluster = NULL;
			INIT_LIST_HEAD(&cluster->fifo);
		} else {
			cluster = NULL;
		}
		xa_unlock_irqrestore(&mc16_clusters, flags);
		if (cluster)
			mc16_queue_cluster(cluster);
	}
	xa_lock_irqsave(&mc16_clusters, flags);
	xa_for_each(&mc16_clusters, id, cluster) {
		if (xa_is_value(cluster) || xa_is_internal(cluster))
			continue;
		if (cluster->key->current_cluster == cluster &&
		    !cluster->live_slots && cluster->next &&
		    cluster->next < MC16_CLUSTER_SLOTS)
			retained++;
	}
	WRITE_ONCE(mc16_retained_empty_headers, retained);
	xa_unlock_irqrestore(&mc16_clusters, flags);
	rcu_barrier();
}

static struct mc16_test_aging_batch *
mc16_test_aging_batch_begin(struct kunit *test)
{
	struct mc16_test_aging_batch *b;

	b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	if (!b)
		return NULL;
	b->test = test;
	b->storage = mc16_test_storage_begin(test);
	if (!b->storage)
		return NULL;
	if (kunit_add_action_or_reset(test, mc16_test_aging_batch_cleanup, b))
		return NULL;
	kunit_activate_static_stub(test, mc16_aging_now, mc16_test_aging_now);
	kunit_activate_static_stub(test, mc16_queue_aging, mc16_test_queue_aging);
	return b;
}

/* Real header/key allocators and RCU destructor; no simulated aging engine. */
static int mc16_test_aging_add(struct mc16_test_aging_batch *b,
			       unsigned long id, unsigned long parent)
{
	struct mc16_cluster *cluster, *previous;
	struct mc16_key *key;
	unsigned long flags;
	int ret;

	if (b->nr == ARRAY_SIZE(b->ids))
		return -ENOSPC;
	if (parent) {
		previous = xa_load(&mc16_clusters, parent);
		if (!previous)
			return -ENOENT;
		key = previous->key;
	} else {
		key = mc16_key_alloc(&b->storage->owner->model.objcg,
				     0, MC16_KEY_ANON, false);
		if (IS_ERR(key))
			return PTR_ERR(key);
	}
	cluster = mc16_alloc_zero(MC16_META_CLUSTER, sizeof(*cluster),
				  &b->storage->owner->model.objcg);
	if (!cluster) {
		if (!parent)
			mc16_key_free(key);
		return -ENOMEM;
	}
	cluster->key = key;
	cluster->id = id;
	cluster->next = 1;
	cluster->metadata_bytes = ksize(cluster);
	INIT_LIST_HEAD(&cluster->fifo);
	ret = xa_insert_irq(&mc16_clusters, id, cluster, GFP_KERNEL);
	if (ret) {
		mc16_meta_free(MC16_META_CLUSTER, cluster);
		if (!parent)
			mc16_key_free(key);
		return ret;
	}
	xa_lock_irqsave(&mc16_clusters, flags);
	if (!parent)
		hash_add(mc16_keys, &key->node,
			 mc16_key_tag(key->objcg, 0, MC16_KEY_ANON, false));
	previous = key->current_cluster;
	if (previous) {
		previous->next = MC16_CLUSTER_SLOTS;
		mc16_retained_empty_dec_locked();
	}
	key->current_cluster = cluster;
	key->clusters++;
	mc16_retained_empty_inc_locked();
	xa_unlock_irqrestore(&mc16_clusters, flags);
	b->ids[b->nr++] = id;
	return 0;
}

static int mc16_test_aging_marker(struct mc16_test_aging_batch *b,
				  unsigned long id, bool value)
{
	int ret;

	if (b->nr == ARRAY_SIZE(b->ids))
		return -ENOSPC;
	ret = value ? xa_insert_irq(&mc16_clusters, id, xa_mk_value(7),
				   GFP_KERNEL) :
		xa_reserve_irq(&mc16_clusters, id, GFP_KERNEL);
	if (!ret)
		b->ids[b->nr++] = id;
	return ret;
}

static struct mc16_test_aging_step
mc16_test_aging_step(struct kunit *test, bool publish)
{
	struct mc16_test_aging_step r = { .owned = true };
	struct mc16_cluster *alias;
	unsigned long flags, id;
	unsigned int i, j;

	xa_lock_irqsave(&mc16_clusters, flags);
	r.delay = mc16_aging_step_locked(mc16_test_aging_clock, r.detached,
					 &r.nr, &r.checked);
	r.next_id = mc16_aging.next_id;
	r.active = mc16_aging.active;
	for (i = 0; i < min_t(unsigned int, r.nr, MC16_AGING_BATCH); i++) {
		struct mc16_cluster *c = r.detached[i];

		r.owned &= c->key->current_cluster != c &&
			!xa_load(&mc16_clusters, c->id) && c->key->clusters;
		for (j = 0; j < i; j++)
			r.owned &= c != r.detached[j];
		/* Preserve failure evidence before repairing a variant for cleanup. */
		if (c->key->current_cluster == c)
			c->key->current_cluster = NULL;
		/* An ID-guard mutation can erase the wrong slot, leaving an alias. */
		xa_for_each(&mc16_clusters, id, alias) {
			if (alias != c)
				continue;
			r.owned = false;
			__xa_erase(&mc16_clusters, id);
			/* Restore the fixture ID before its one permitted publication. */
			c->id = id;
		}
	}
	xa_unlock_irqrestore(&mc16_clusters, flags);
	KUNIT_EXPECT_LE(test, r.checked, (unsigned int)MC16_AGING_BATCH);
	KUNIT_EXPECT_LE(test, r.nr, (unsigned int)MC16_AGING_BATCH);
	KUNIT_EXPECT_PTR_EQ(test, r.detached[MC16_AGING_BATCH], NULL);
	KUNIT_EXPECT_TRUE(test, r.owned);
	if (publish) {
		for (i = 0; i < min_t(unsigned int, r.nr, MC16_AGING_BATCH); i++)
			mc16_test_aging_publish(test, &r, i);
		KUNIT_EXPECT_EQ(test, r.publish_attempts, r.nr);
		KUNIT_EXPECT_EQ(test, r.published, r.publish_attempts);
	}
	return r;
}

static void mc16_test_aging_observe(struct kunit *test)
{
	struct mc16_test_aging_step r;
	unsigned int i;

	for (i = 0; i < MC16_TEST_AGING_IDS + 1; i++) {
		r = mc16_test_aging_step(test, true);
		if (r.delay)
			return;
	}
	KUNIT_FAIL(test, "numeric cursor did not finish a bounded population");
}

/* Group 1: raw budget, private reservations, sparse/max IDs and key shapes. */
static void memcomp_test_aging_batch_bounds(struct kunit *test)
{
	struct mc16_test_aging_batch *b = mc16_test_aging_batch_begin(test);
	struct mc16_test_aging_step r;
	unsigned int i, n;
	unsigned long before;
	struct mc16_cluster *c;

	KUNIT_ASSERT_NOT_NULL(test, b);
	for (i = 1; i <= MC16_AGING_BATCH + 1; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_aging_marker(b, i, false), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, MC16_AGING_BATCH + 2, 0), 0);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, (unsigned int)MC16_AGING_BATCH);
	KUNIT_EXPECT_EQ(test, r.nr, 0U);
	KUNIT_EXPECT_EQ(test, r.next_id, MC16_AGING_BATCH + 1UL);
	c = xa_load(&mc16_clusters, MC16_AGING_BATCH + 2);
	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_EXPECT_EQ(test, c->flags & MC16_CLUSTER_AGED, 0);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, 2U);
	KUNIT_EXPECT_EQ(test, c->flags & MC16_CLUSTER_AGED, MC16_CLUSTER_AGED);
	mc16_test_aging_batch_cleanup(b);
	b->nr = 0;
	mc16_test_aging_reset();
	/* All-reserved arrays still consume B raw returns; holes are not units. */
	for (i = 1; i <= MC16_AGING_BATCH + 1; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_aging_marker(b, 2 * i, false), 0);
	WRITE_ONCE(mc16_retained_empty_headers, 1);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, (unsigned int)MC16_AGING_BATCH);
	KUNIT_EXPECT_EQ(test, r.next_id, 2UL * MC16_AGING_BATCH + 1);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, 1U);
	mc16_test_aging_batch_cleanup(b);
	b->nr = 0;
	for (n = MC16_AGING_BATCH - 1; n <= MC16_AGING_BATCH + 1; n++) {
		mc16_test_aging_reset();
		for (i = 1; i <= n; i++)
			KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, i, 0), 0);
		r = mc16_test_aging_step(test, true);
		KUNIT_EXPECT_EQ(test, r.checked, min_t(unsigned int, n, MC16_AGING_BATCH));
		KUNIT_EXPECT_EQ(test, r.active, n >= MC16_AGING_BATCH);
		before = r.next_id;
		if (r.active) {
			r = mc16_test_aging_step(test, true);
			KUNIT_EXPECT_EQ(test, r.checked, n - MC16_AGING_BATCH);
			KUNIT_EXPECT_FALSE(test, r.active);
			KUNIT_EXPECT_GT(test, before, 1UL);
		}
		mc16_test_aging_batch_cleanup(b);
		b->nr = 0;
	}
	mc16_test_aging_reset();
	for (i = 1; i <= 2 * MC16_AGING_BATCH + 1; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, i, i == 1 ? 0 : i - 1), 0);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.next_id, MC16_AGING_BATCH + 1UL);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.next_id, 2UL * MC16_AGING_BATCH + 1);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, 1U);
	mc16_test_aging_batch_cleanup(b);
	b->nr = 0;
	mc16_test_aging_reset();
	KUNIT_ASSERT_EQ(test, mc16_test_aging_marker(b, 5, true), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, MC16_MAX_CLUSTER_ID, 0), 0);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, 2U);
	KUNIT_EXPECT_FALSE(test, r.active);
	KUNIT_EXPECT_EQ(test, r.next_id, 1UL);
	mc16_test_aging_clock += HZ;
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.nr, 1U);
}

/* Group 2: real producer requests cannot shorten wrap-safe pass deadlines. */
static void memcomp_test_aging_deadlines(struct kunit *test)
{
	struct mc16_test_aging_batch *b = mc16_test_aging_batch_begin(test);
	struct mc16_test_aging_step r;
	unsigned long deadline;
	int queued;

	KUNIT_ASSERT_NOT_NULL(test, b);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 1, 0), 0);
	kunit_deactivate_static_stub(test, mc16_schedule_aging);
	mc16_test_aging_clock = ULONG_MAX - HZ / 2;
	mc16_schedule_aging();
	deadline = mc16_aging.not_before;
	queued = atomic_read(&mc16_test_aging_schedules);
	mc16_schedule_aging();
	KUNIT_EXPECT_EQ(test, mc16_aging.not_before, deadline);
	KUNIT_EXPECT_EQ(test, atomic_read(&mc16_test_aging_schedules), queued);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, 0U);
	KUNIT_EXPECT_EQ(test, r.delay, (long)HZ);
	mc16_test_aging_clock = deadline;
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.nr, 0U);
	KUNIT_EXPECT_EQ(test, r.checked, 1U);
	deadline = mc16_aging.not_before;
	mc16_schedule_aging();
	mc16_test_aging_clock += HZ - 1;
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, 0U);
	KUNIT_EXPECT_EQ(test, r.delay, 1L);
	KUNIT_EXPECT_EQ(test, mc16_aging.not_before, deadline);
	mc16_test_aging_clock++;
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.nr, 1U);
	KUNIT_EXPECT_EQ(test, r.delay, -1L);
}

/* Group 3: numeric continuation survives deletion, insertion and ID reuse. */
static void memcomp_test_aging_mutation(struct kunit *test)
{
	struct mc16_test_aging_batch *b = mc16_test_aging_batch_begin(test);
	struct mc16_test_aging_step r;
	struct shrink_control sc = { .nr_to_scan = 1 };
	struct mc16_cluster *c;
	unsigned long flags;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, b);
	for (i = 1; i <= MC16_AGING_BATCH; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_aging_marker(b, 2 * i, false), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 200, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 201, 0), 0);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.next_id, 2UL * MC16_AGING_BATCH + 1);
	mc16_meta_shrinker_cursor = 200;
	KUNIT_ASSERT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 1UL);
	rcu_barrier();
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 200, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 1, 0), 0);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 300, 0), 0);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.checked, 3U);
	c = xa_load(&mc16_clusters, 1);
	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_EXPECT_EQ(test, c->flags & MC16_CLUSTER_AGED, 0);
	c = xa_load(&mc16_clusters, 200);
	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_EXPECT_NE(test, c->flags & MC16_CLUSTER_AGED, 0);
	/* A completed final-key callback did not leave a persistent pointer. */
	mc16_test_aging_clock += HZ;
	mc16_test_aging_observe(test);
	KUNIT_EXPECT_NOT_NULL(test, xa_load(&mc16_clusters, 1));
	KUNIT_EXPECT_PTR_EQ(test, xa_load(&mc16_clusters, 200), NULL);
	mc16_meta_shrinker_cursor = 1;
	KUNIT_EXPECT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 1UL);
	xa_lock_irqsave(&mc16_clusters, flags);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	xa_unlock_irqrestore(&mc16_clusters, flags);
	rcu_barrier();
}

/* Group 4: independently exercise every production eligibility predicate. */
static void memcomp_test_aging_eligibility(struct kunit *test)
{
	struct mc16_test_aging_batch *b = mc16_test_aging_batch_begin(test);
	struct mc16_test_aging_step r;
	struct mc16_cluster *c;
	struct mc16_key *key;
	LIST_HEAD(fifo);
	unsigned long flags;
	unsigned int guard;

	KUNIT_ASSERT_NOT_NULL(test, b);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 10, 0), 0);
	for (guard = 0; guard < 7; guard++) {
		xa_lock_irqsave(&mc16_clusters, flags);
		c = xa_load(&mc16_clusters, 10);
		if (!c) {
			xa_unlock_irqrestore(&mc16_clusters, flags);
			KUNIT_FAIL(test, "eligibility guard detached the header");
			return;
		}
		key = c->key;
		c->flags = MC16_CLUSTER_AGED;
		if (guard == 0)
			c->id = 11;
		if (guard == 1)
			key->current_cluster = NULL;
		if (guard == 2)
			c->live_slots = 1;
		if (guard == 3)
			c->next = 0;
		if (guard == 4)
			c->next = MC16_CLUSTER_SLOTS;
		if (guard == 5)
			c->eligible_slots = 1;
		if (guard == 6)
			list_add(&c->fifo, &fifo);
		xa_unlock_irqrestore(&mc16_clusters, flags);
		r = mc16_test_aging_step(test, true);
		KUNIT_EXPECT_EQ(test, r.nr, 0U);
		xa_lock_irqsave(&mc16_clusters, flags);
		c = xa_load(&mc16_clusters, 10);
		if (!c) {
			xa_unlock_irqrestore(&mc16_clusters, flags);
			KUNIT_FAIL(test, "eligibility mutation escaped its guard");
			return;
		}
		c->id = 10;
		key->current_cluster = c;
		c->live_slots = 0;
		c->next = 1;
		c->eligible_slots = 0;
		list_del_init(&c->fifo);
		xa_unlock_irqrestore(&mc16_clusters, flags);
		mc16_test_aging_clock += HZ;
	}
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.nr, 1U);
	/* Refill uses the real reserve/publish path, not a flags simulation. */
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(b->storage, 1), 0);
	mc16_test_cancel_pending(&b->storage->refs[0]);
	mc16_test_aging_clock += HZ;
	mc16_test_aging_observe(test);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(b->storage, 1), 0);
	KUNIT_EXPECT_EQ(test, b->storage->refs[1].cluster->flags &
			 MC16_CLUSTER_AGED, 0);
	mc16_test_cancel_pending(&b->storage->refs[1]);
	mc16_test_aging_clock += HZ;
	mc16_test_aging_observe(test);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
}

/* Runs from the actual wrapper's queue call, after its locked step. */
static void mc16_test_aging_producer_at_queue(void)
{
	int queued;

	mc16_test_aging_queue_next = mc16_aging.next_id;
	mc16_test_aging_queue_deadline = mc16_aging.not_before;
	mc16_schedule_aging();
	queued = atomic_read(&mc16_test_aging_schedules);
	mc16_schedule_aging();
	mc16_test_aging_queue_duplicates =
		atomic_read(&mc16_test_aging_schedules) - queued;
}

/* Group 5: queue boundaries consume requests, duplicates preserve progress. */
static void memcomp_test_aging_requests(struct kunit *test)
{
	struct mc16_test_aging_batch *b = mc16_test_aging_batch_begin(test);
	struct mc16_test_aging_step r;
	struct shrink_control sc = { .nr_to_scan = 1 };
	unsigned long next, deadline;
	unsigned int i;
	int queued;

	KUNIT_ASSERT_NOT_NULL(test, b);
	kunit_deactivate_static_stub(test, mc16_schedule_aging);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.delay, -1L);
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 1000, 0), 0);
	mc16_schedule_aging();
	KUNIT_EXPECT_TRUE(test, mc16_aging.requested);
	queued = atomic_read(&mc16_test_aging_schedules);
	mc16_schedule_aging();
	KUNIT_EXPECT_EQ(test, atomic_read(&mc16_test_aging_schedules), queued);
	mc16_test_aging_clock += HZ;
	for (i = 1; i <= MC16_AGING_BATCH + 1; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_aging_marker(b, i, false), 0);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.delay, 0L);
	KUNIT_EXPECT_FALSE(test, mc16_aging.requested);
	next = mc16_aging.next_id;
	deadline = mc16_aging.not_before;
	/* Producer obtains X after step unlock but before wrapper queueing. */
	mc16_schedule_aging();
	queued = atomic_read(&mc16_test_aging_schedules);
	mc16_schedule_aging();
	KUNIT_EXPECT_EQ(test, mc16_aging.next_id, next);
	KUNIT_EXPECT_EQ(test, mc16_aging.not_before, deadline);
	KUNIT_EXPECT_EQ(test, atomic_read(&mc16_test_aging_schedules), queued);
	mc16_meta_shrinker_cursor = 1000;
	KUNIT_ASSERT_EQ(test, mc16_meta_shrinker_scan(NULL, &sc), 1UL);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.delay, -1L);
	KUNIT_EXPECT_FALSE(test, r.active);
	/* First retained header appears after the old callback chose idle. */
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 2000, 0), 0);
	mc16_schedule_aging();
	KUNIT_EXPECT_TRUE(test, mc16_aging.requested);
	KUNIT_EXPECT_GE(test, atomic_read(&mc16_test_aging_schedules), queued + 1);
	mc16_test_aging_clock += HZ;
	mc16_test_aging_observe(test);
	mc16_test_aging_clock += HZ;
	mc16_test_aging_observe(test);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	queued = atomic_read(&mc16_test_aging_schedules);
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, atomic_read(&mc16_test_aging_schedules), queued);
	/* Exercise the real wrapper at a continuation queue boundary. */
	KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, 3000, 0), 0);
	mc16_schedule_aging();
	mc16_test_aging_clock += HZ;
	mc16_test_aging_queue_duplicates = -1;
	mc16_test_aging_queue_action = mc16_test_aging_producer_at_queue;
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_TRUE(test, !mc16_test_aging_queue_action);
	KUNIT_EXPECT_EQ(test, mc16_test_aging_queue_duplicates, 0);
	KUNIT_EXPECT_EQ(test, mc16_aging.next_id, mc16_test_aging_queue_next);
	KUNIT_EXPECT_EQ(test, mc16_aging.not_before,
			mc16_test_aging_queue_deadline);
	KUNIT_EXPECT_TRUE(test, mc16_aging.requested);
	KUNIT_EXPECT_EQ(test, mc16_test_aging_delay, 0UL);
	/* Finish this sweep; the queued request cannot bypass the next pass. */
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	KUNIT_EXPECT_EQ(test, mc16_test_aging_delay, (unsigned long)HZ);
	mc16_test_aging_clock += HZ;
	mc16_retained_age_work(&mc16_retained_aging.work);
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
}

/* Group 6: detached headers retain keys/charges until the real RCU path. */
static void memcomp_test_aging_rcu_ownership(struct kunit *test)
{
	struct mc16_test_aging_batch *b = mc16_test_aging_batch_begin(test);
	struct mc16_test_aging_step r;
	struct memcomp_kunit_objcg *model;
	unsigned int i;
	long charged, published_charge;
	int published_puts;
	unsigned long flags;
	bool keys_owned = true;

	KUNIT_ASSERT_NOT_NULL(test, b);
	model = &b->storage->owner->model;
	for (i = 1; i <= MC16_AGING_BATCH + 1; i++)
		KUNIT_ASSERT_EQ(test, mc16_test_aging_add(b, i, 0), 0);
	mc16_test_aging_observe(test);
	charged = atomic_long_read(&model->charged);
	mc16_test_aging_clock += HZ;
	/* Deliberately defer publication, as permitted for detached headers. */
	r = mc16_test_aging_step(test, false);
	KUNIT_ASSERT_EQ(test, r.nr, (unsigned int)MC16_AGING_BATCH);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&model->charged), charged);
	KUNIT_EXPECT_EQ(test, atomic_read(&model->put_calls), 0);
	KUNIT_EXPECT_FALSE(test, hash_empty(mc16_keys));
	/* Publish through call_rcu while a pre-existing real reader holds GP. */
	rcu_read_lock();
	for (i = 0; i < r.nr; i++)
		mc16_test_aging_publish(test, &r, i);
	published_charge = atomic_long_read(&model->charged);
	published_puts = atomic_read(&model->put_calls);
	xa_lock_irqsave(&mc16_clusters, flags);
	for (i = 0; i < r.nr; i++)
		keys_owned &= r.detached[i]->key->clusters == 1 &&
			!hlist_unhashed(&r.detached[i]->key->node) &&
			!r.detached[i]->key->current_cluster;
	xa_unlock_irqrestore(&mc16_clusters, flags);
	rcu_read_unlock();
	KUNIT_EXPECT_EQ(test, r.publish_attempts, r.nr);
	KUNIT_EXPECT_EQ(test, r.published, r.nr);
	KUNIT_EXPECT_EQ(test, published_charge, charged);
	KUNIT_EXPECT_EQ(test, published_puts, 0);
	KUNIT_EXPECT_TRUE(test, keys_owned);
	rcu_barrier();
	KUNIT_EXPECT_EQ(test, atomic_read(&model->put_calls), MC16_AGING_BATCH);
	r = mc16_test_aging_step(test, true);
	KUNIT_EXPECT_EQ(test, r.nr, 1U);
	rcu_barrier();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&model->charged), 0L);
	KUNIT_EXPECT_EQ(test, atomic_read(&model->get_calls),
			atomic_read(&model->put_calls));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
}

static struct mc16_test_storage *mc16_test_aging_begin(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_storage_begin(test);

	if (!s)
		return NULL;
	kunit_activate_static_stub(test, mc16_aging_now, mc16_test_aging_now);
	kunit_activate_static_stub(test, mc16_queue_aging, mc16_test_queue_aging);
	if (mc16_test_reserve(s, 1))
		return NULL;
	mc16_test_cancel_pending(&s->refs[0]);
	return s;
}

static void memcomp_test_aging_teardown(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_aging_begin(test);

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	mc16_test_aging_clock += HZ;
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
}

static void memcomp_test_aging_reuse(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_aging_begin(test);
	u32 id;

	KUNIT_ASSERT_NOT_NULL(test, s);
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_ASSERT_EQ(test, mc16_test_reserve(s, 1), 0);
	id = s->refs[1].cluster->id;
	mc16_test_cancel_pending(&s->refs[1]);
	mc16_test_aging_clock += HZ;
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 1UL);
	KUNIT_EXPECT_NOT_NULL(test, xa_load(&mc16_clusters, id));
}

static void memcomp_test_aging_stops(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_aging_begin(test);
	int schedules;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_EXPECT_EQ(test, atomic_read(&mc16_test_aging_schedules), 1);
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, atomic_read(&mc16_test_aging_schedules), 2);
	mc16_test_aging_clock += HZ;
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_ASSERT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	schedules = atomic_read(&mc16_test_aging_schedules);
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, atomic_read(&mc16_test_aging_schedules), schedules);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&mc16_retained_aging));
}

static void memcomp_test_aging_shrinker(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_aging_begin(test);
	struct shrink_control sc = { .nr_to_scan = 64 };

	KUNIT_ASSERT_NOT_NULL(test, s);
	mc16_retained_age_work(&mc16_retained_aging.work);
	mc16_meta_shrinker_scan(NULL, &sc);
	mc16_test_aging_clock += HZ;
	mc16_retained_age_work(&mc16_retained_aging.work);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	rcu_barrier();
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
	KUNIT_EXPECT_EQ(test, atomic_read(&s->owner->model.get_calls),
			atomic_read(&s->owner->model.put_calls));
}

static void memcomp_test_aging_real_work(struct kunit *test)
{
	struct mc16_test_storage *s = mc16_test_aging_begin(test);
	unsigned long flags;

	KUNIT_ASSERT_NOT_NULL(test, s);
	kunit_deactivate_static_stub(test, mc16_aging_now);
	kunit_deactivate_static_stub(test, mc16_schedule_aging);
	kunit_deactivate_static_stub(test, mc16_queue_aging);
	mc16_schedule_aging();
	/* The deadline contract is checked separately with a controlled clock. */
	xa_lock_irqsave(&mc16_clusters, flags);
	mc16_aging.not_before = jiffies;
	xa_unlock_irqrestore(&mc16_clusters, flags);
	flush_delayed_work(&mc16_retained_aging);
	xa_lock_irqsave(&mc16_clusters, flags);
	mc16_aging.not_before = jiffies;
	xa_unlock_irqrestore(&mc16_clusters, flags);
	flush_delayed_work(&mc16_retained_aging);
	cancel_delayed_work_sync(&mc16_retained_aging);
	KUNIT_EXPECT_EQ(test, READ_ONCE(mc16_retained_empty_headers), 0UL);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&mc16_retained_aging));
	rcu_barrier();
	KUNIT_EXPECT_TRUE(test, xa_empty(&mc16_clusters));
	KUNIT_EXPECT_TRUE(test, hash_empty(mc16_keys));
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_AGING_TEST_H */
