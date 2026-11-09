/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MEMCOMP_TESTS_ASYNC_TEST_H
#define _MM_MEMCOMP_TESTS_ASYNC_TEST_H

/*
 * Include after shared_test.h. Definitions only.
 * Run only with the public positive-token dispatch using clustered slots.
 * The registered acomp frontend and codec work are real. Only objcg identity
 * and charging use the shared explicit model; no swap device is involved.
 */
#ifdef CONFIG_MEMCG
struct mc16_act_snapshot {
	u64 word;
	u32 refs;
	u32 bytes;
	u32 charge;
	u16 flags;
	bool attached;
};

struct mc16_act {
	struct kunit *test;
	struct memcomp_kunit_objcg model;
	struct memcomp_kunit_async_probe probe;
	struct crypto_acomp *tfm;
	struct memcomp_async *async;
	struct folio *folio[2];
	struct memcomp_ref guards[2][MEMCOMP_MAX_COMPRESS_PAGES];
	struct folio *dst;
	unsigned long token[2];
	unsigned int initialized;
	unsigned int inflight[3];
	bool ready_inflight[3];
	bool stored[2];
	bool finalized[2];
	bool registered;
	bool modeled;
	bool observing;
	bool ctx_cleared;
	bool fail_consumed;
	int acquisitions_left;
	s64 stored_before;
	s64 compressed_before;
	u8 *packed;
};

/* Value/copy adapter only: a guard spans T, never a borrowed record pointer. */
static bool mc16_act_snapshot(unsigned long token, struct mc16_act_snapshot *s,
			      void *packed)
{
	struct memcomp_ref ref = { };
	struct mc16_view view;
	void *src;
	bool valid = false;

	*s = (struct mc16_act_snapshot) { };
	if (!memcomp_entry_get_ref(token, &ref))
		return false;
	mutex_lock(memcomp_lock_of(ref.token));
	if (!mc16_view_locked(&ref, &view) || !mc16_flags_valid(*view.flags))
		goto out;
	s->word = *view.word;
	s->flags = *view.flags;
	s->refs = refcount_read(&ref.slot->refs);
	s->attached = !!view.record;
	s->charge = view.member ? view.member->payload_charge : 0;
	if (mc16_flags_state(s->flags) == MC16_PENDING)
		valid = !*view.length;
	else
		valid = mc16_payload_length_decode(mc16_flags_state(s->flags),
						   *view.length, &s->bytes);
	if (!valid || !packed || mc16_flags_state(s->flags) != MC16_STORED)
		goto out;
	if (!s->word || !s->bytes || s->bytes > PAGE_SIZE) {
		valid = false;
		goto out;
	}
	src = zs_map_object(memcomp_pools[0], s->word, ZS_MM_RO);
	if (!src) {
		valid = false;
		goto out;
	}
	memcpy(packed, src, s->bytes);
	zs_unmap_object(memcomp_pools[0], s->word);
out:
	mutex_unlock(memcomp_lock_of(ref.token));
	memcomp_entry_put_ref(&ref);
	return valid;
}

static void mc16_act_cleanup(void *data)
{
	struct mc16_act *f = data;
	unsigned int i, page;

	/* Open every gate first: the shared frontend uses one serial workqueue. */
	for (i = 0; i < f->initialized; i++) {
		struct memcomp_test_acomp_req *r;

		r = acomp_request_ctx(f->async->reqs[i].req);
		complete_all(&r->release);
	}
	for (i = 0; i < f->initialized; i++) {
		struct memcomp_test_acomp_req *r;

		r = acomp_request_ctx(f->async->reqs[i].req);
		flush_work(&r->work);
	}
	/* Fatal-assert actions can run in a different KUnit cleanup task. */
	if (f->observing)
		WRITE_ONCE(f->probe.task, current);
	for (i = 0; i < ARRAY_SIZE(f->folio); i++) {
		struct folio *folio = f->folio[i];
		int refs;

		if (!folio)
			continue;
		folio_lock(folio);
		if (!f->finalized[i]) {
			if (f->stored[i])
				memcomp_invalidate_folio(folio);
			else
				memcomp_rollback_folio(folio);
		}
		/* OWNER is gone; guards are the final payload lifetime holders. */
		for (page = 0; page < ARRAY_SIZE(f->guards[i]); page++) {
			if (f->guards[i][page].slot)
				memcomp_entry_put_ref(&f->guards[i][page]);
		}
		refs = folio_ref_count(folio);
		folio_unlock(folio);
		KUNIT_EXPECT_EQ(f->test, refs, 1);
		folio_put(folio);
		f->folio[i] = NULL;
	}
	if (f->observing) {
		KUNIT_EXPECT_EQ(f->test, f->probe.frees, f->probe.allocs);
		KUNIT_EXPECT_FALSE(f->test, f->probe.live);
		KUNIT_EXPECT_FALSE(f->test, f->probe.bad_lifetime);
		KUNIT_EXPECT_PTR_EQ(f->test,
				    rcu_access_pointer(memcomp_kunit_async_probe),
				    &f->probe);
		rcu_assign_pointer(memcomp_kunit_async_probe, NULL);
		synchronize_rcu();
	}
	if (f->async)
		memcomp_async_free_batch(f->async);
	if (f->tfm)
		crypto_free_acomp(f->tfm);
	if (f->registered)
		crypto_unregister_acomp(&memcomp_test_acomp_alg);
	if (f->dst)
		folio_put(f->dst);
	mc16_test_reclaim_retained();
	if (f->modeled) {
		KUNIT_EXPECT_TRUE(f->test, xa_empty(&mc16_clusters));
		KUNIT_EXPECT_EQ(f->test, atomic_long_read(&f->model.charged), 0L);
		KUNIT_EXPECT_EQ(f->test, atomic_read(&f->model.get_calls),
				atomic_read(&f->model.put_calls));
		rcu_assign_pointer(memcomp_kunit_objcg, NULL);
		synchronize_rcu();
	}
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&memcomp_stored_bytes),
			f->stored_before);
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&memcomp_compressed_bytes),
			f->compressed_before);
}

/* Return unlocked, including every allocation/registration failure. */
static struct mc16_act *mc16_act_begin(struct kunit *test, bool large,
				       bool prefix, bool samefill)
{
	struct mc16_act *f;
	unsigned int i, page, nr = (large ? 2 : 1) + prefix;
	int ret;

	if (!memcomp_is_enabled()) {
		kunit_skip(test, "memcomp initialization failed");
		return NULL;
	}
	if (nr > MEMCOMP_ASYNC_PAGES || (large && MEMCOMP_MAX_COMPRESS_PAGES < 2)) {
		kunit_skip(test, "two-page compression batch unavailable");
		return NULL;
	}
	rcu_barrier();
	if (rcu_access_pointer(memcomp_kunit_objcg) ||
	    rcu_access_pointer(memcomp_kunit_async_probe) || !xa_empty(&mc16_clusters)) {
		KUNIT_FAIL(test, "async fixture requires idle storage/objcg/probe");
		return NULL;
	}
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		goto fail;
	f->test = test;
	f->stored_before = atomic64_read(&memcomp_stored_bytes);
	f->compressed_before = atomic64_read(&memcomp_compressed_bytes);
	if (kunit_add_action_or_reset(test, mc16_act_cleanup, f))
		goto fail;
	ret = crypto_register_acomp(&memcomp_test_acomp_alg);
	if (ret) {
		KUNIT_FAIL(test, "registered acomp frontend: %d", ret);
		goto fail;
	}
	f->registered = true;
	f->tfm = crypto_alloc_acomp("memcomp-kunit-acomp", CRYPTO_ALG_ASYNC,
				    CRYPTO_ALG_ASYNC | CRYPTO_NOLOAD);
	if (IS_ERR(f->tfm)) {
		ret = PTR_ERR(f->tfm);
		f->tfm = NULL;
		KUNIT_FAIL(test, "registered acomp tfm: %d", ret);
		goto fail;
	}
	f->async = kzalloc(sizeof(*f->async), GFP_KERNEL);
	if (!f->async)
		goto fail;
	init_completion(&f->async->done);
	atomic_set(&f->async->pending, 1);
	for (i = 0; i < nr; i++) {
		struct memcomp_async_req *req = &f->async->reqs[i];
		struct memcomp_test_acomp_req *r;

		req->async = f->async;
		req->req = acomp_request_alloc(f->tfm);
		if (!req->req)
			goto fail;
		req->buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!req->buf)
			goto fail;
		r = acomp_request_ctx(req->req);
		/* The crypto request's private bytes are not guaranteed zeroed. */
		memset(r, 0, sizeof(*r));
		INIT_WORK(&r->work, memcomp_test_acomp_work);
		init_completion(&r->started);
		init_completion(&r->release);
		init_completion(&r->finished);
		atomic_set(&r->submitted, 0);
		atomic_set(&r->completed, 0);
		f->initialized++;
	}
	f->dst = folio_alloc(GFP_KERNEL, 0);
	f->packed = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!f->dst || !f->packed)
		goto fail;
	f->folio[0] = folio_alloc(GFP_KERNEL | __GFP_ZERO, large ? 1 : 0);
	if (prefix)
		f->folio[1] = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!f->folio[0] || (prefix && !f->folio[1]))
		goto fail;
	for (i = 0; i < ARRAY_SIZE(f->folio); i++) {
		struct folio *folio = f->folio[i];

		if (!folio)
			continue;
		folio->mapping = (struct address_space *)PAGE_MAPPING_ANON;
		__folio_set_swapbacked(folio);
		/* Finalize requires real source-folio memcg identity. */
		if (mem_cgroup_charge(folio, NULL, GFP_KERNEL))
			goto fail;
		for (page = 0; page < folio_nr_pages(folio); page++) {
			u8 *src = kmap_local_folio(folio, page * PAGE_SIZE);

			memset(src, 0x31 + 0x17 * page + 0x23 * i, PAGE_SIZE);
			if (!samefill)
				src[17] ^= 0xff;
			kunmap_local(src);
		}
	}
	f->model.task = current;
	f->model.error = -ENOMEM;
	rcu_assign_pointer(memcomp_kunit_objcg, &f->model);
	f->modeled = true;
	return f;
fail:
	KUNIT_FAIL(test, "async fixture setup failed");
	return NULL;
}

static bool mc16_act_reserve(struct mc16_act *f, unsigned int index)
{
	struct folio *folio = f->folio[index];
	bool reserved;

	f->model.folio = folio;
	folio_lock(folio);
	reserved = memcomp_reserve(folio);
	if (reserved)
		f->token[index] = swp_offset(folio->swap);
	folio_unlock(folio);
	return reserved;
}

/* Preserve the locked-source contract until each real callback has returned. */
static int mc16_act_submit(struct mc16_act *f, unsigned int index)
{
	struct folio *folio = f->folio[index];
	unsigned int first = f->async->nr, i;
	unsigned long waited;
	int ret = -EINVAL;

	folio_lock(folio);
	if (!memcomp_async_submit(f->async, folio))
		goto out;
	ret = 0;
	for (i = first; i < f->async->nr; i++) {
		struct memcomp_test_acomp_req *r;

		r = acomp_request_ctx(f->async->reqs[i].req);
		if (!atomic_read(&r->submitted))
			continue;
		waited = wait_for_completion_timeout(&r->started, HZ);
		if (!waited) {
			ret = -ETIMEDOUT;
			goto out;
		}
		/* Progress (-EINPROGRESS) must not consume the terminal hold. */
		f->inflight[i] = atomic_read(&f->async->pending);
		f->ready_inflight[i] = memcomp_async_complete(f->async);
		if (atomic_read(&r->completed)) {
			ret = -EIO;
			goto out;
		}
		complete_all(&r->release);
		waited = wait_for_completion_timeout(&r->finished, HZ);
		if (!waited) {
			ret = -ETIMEDOUT;
			goto out;
		}
		flush_work(&r->work);
	}
out:
	/* Also open gates on failure before releasing the private source folio. */
	for (i = first; i < f->async->nr; i++) {
		struct memcomp_test_acomp_req *r;

		r = acomp_request_ctx(f->async->reqs[i].req);
		complete_all(&r->release);
	}
	for (i = first; i < f->async->nr; i++) {
		struct memcomp_test_acomp_req *r;

		r = acomp_request_ctx(f->async->reqs[i].req);
		flush_work(&r->work);
	}
	folio_unlock(folio);
	return ret;
}

static bool mc16_act_wait(struct mc16_act *f)
{
	/* Refuse an unbounded production wait if callback accounting is broken. */
	if (!memcomp_async_complete(f->async) ||
	    atomic_read(&f->async->pending) != 1)
		return false;
	memcomp_async_wait(f->async);
	return atomic_read(&f->async->pending) == 0;
}

/* No context/folio lock or modified task flags escape into an assertion. */
static int mc16_act_store(struct mc16_act *f, unsigned int index,
			  bool finish_fail, bool lease_check, bool poison)
{
	struct folio *folio = f->folio[index];
	struct memcomp_reclaim_ctx *ctx;
	unsigned int nr = folio_nr_pages(folio), reclaim_flags = 0;
	bool stored;

	if (atomic_read(&memcomp_kunit_ctx_get_fail) ||
	    READ_ONCE(memcomp_kunit_fail.task))
		return -EBUSY;
	if (nr > 1)
		reclaim_flags = memcomp_test_reclaimer_enter();
	folio_lock(folio);
	ctx = memcomp_reclaim_ctx_get(nr, nr > 1 ? MEMCOMP_RECLAIM_KSWAPD :
						 MEMCOMP_RECLAIM_DIRECT);
	if (!ctx) {
		folio_unlock(folio);
		if (nr > 1)
			memcomp_test_reclaimer_exit(reclaim_flags);
		return -EBUSY;
	}
	/* An error injected in this precise leased object identifies its user. */
	if (poison)
		ctx->kunit_compress_error = -EBADMSG;
	if (lease_check)
		atomic_set(&memcomp_kunit_ctx_get_fail, 2);
	if (finish_fail)
		memcomp_kunit_fail_next(MEMCOMP_KUNIT_FAIL_REFRESH_COMMIT,
					1, -EAGAIN);
	stored = memcomp_store_after_unmap_async(folio, &ctx, f->async);
	f->stored[index] = stored;
	f->ctx_cleared = !ctx;
	f->fail_consumed = !READ_ONCE(memcomp_kunit_fail.task) &&
		memcomp_kunit_fail.stage == MEMCOMP_KUNIT_FAIL_NONE &&
		!memcomp_kunit_fail.remaining;
	if (finish_fail)
		memcomp_kunit_fail_clear();
	if (lease_check) {
		f->acquisitions_left = atomic_read(&memcomp_kunit_ctx_get_fail);
		atomic_set(&memcomp_kunit_ctx_get_fail, 0);
	}
	if (ctx)
		memcomp_reclaim_ctx_put(ctx);
	folio_unlock(folio);
	if (nr > 1)
		memcomp_test_reclaimer_exit(reclaim_flags);
	return stored;
}

/* Snapshot assertions run first: persistent guards would change refs. */
static bool mc16_act_finalize(struct mc16_act *f, unsigned int index)
{
	struct folio *folio = f->folio[index];
	unsigned int page, nr = folio_nr_pages(folio);
	bool released;

	if (!f->stored[index] || f->finalized[index])
		return false;
	for (page = 0; page < nr; page++) {
		if (!f->guards[index][page].slot &&
		    !memcomp_entry_get_ref(f->token[index] + page,
					   &f->guards[index][page]))
			return false;
	}
	/* These allocator sources were never on LRU; no isolation ref exists. */
	folio_lock(folio);
	released = memcomp_release_folio(folio);
	if (released) {
		folio_ref_unfreeze(folio, 1);
		f->finalized[index] = true;
	}
	folio_unlock(folio);
	return released;
}

static int mc16_act_load(struct mc16_act *f, unsigned int index,
			 unsigned int page, bool *same)
{
	struct memcomp_ref ref = { };
	void *src, *dst;
	int ret;

	*same = false;
	if (!memcomp_entry_get_ref(f->token[index] + page, &ref))
		return -ENOENT;
	folio_lock(f->dst);
	ret = memcomp_load_folio(&ref, f->dst);
	if (!ret) {
		src = kmap_local_folio(f->folio[index], page * PAGE_SIZE);
		dst = kmap_local_folio(f->dst, 0);
		*same = !memcmp(src, dst, PAGE_SIZE);
		kunmap_local(dst);
		kunmap_local(src);
	}
	folio_unlock(f->dst);
	memcomp_entry_put_ref(&ref);
	return ret;
}

static void mc16_act_frontend(struct kunit *test, bool backlog, int error,
			      bool finish_fail, bool samefill)
{
	struct mc16_act *f = mc16_act_begin(test, false, false, samefill);
	struct mc16_act_snapshot s;
	struct memcomp_test_acomp_req *r;
	struct memcomp_async_req *req;
	unsigned long token;
	long charged;
	bool same;
	int ret;

	if (!f)
		return;
	req = &f->async->reqs[0];
	r = acomp_request_ctx(req->req);
	r->backlog = backlog;
	r->error = error;
	KUNIT_ASSERT_TRUE(test, mc16_act_reserve(f, 0));
	token = f->token[0];
	KUNIT_ASSERT_NE(test, token, 0UL);
	charged = atomic_long_read(&f->model.charged);
	KUNIT_ASSERT_EQ(test, mc16_act_submit(f, 0), 0);
	KUNIT_EXPECT_EQ(test, f->async->nr, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->submitted), samefill ? 0 : 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->completed), samefill ? 0 : 1);
	if (!samefill) {
		KUNIT_EXPECT_EQ(test, f->inflight[0], 2U);
		KUNIT_EXPECT_FALSE(test, f->ready_inflight[0]);
	}
	KUNIT_ASSERT_TRUE(test, mc16_act_wait(f));
	KUNIT_EXPECT_EQ(test, req->ret, samefill ? 0 : error);
	KUNIT_EXPECT_EQ(test, req->same_filled, samefill);
	f->probe.task = current;
	f->probe.folio = f->folio[0];
	f->probe.request = req;
	rcu_assign_pointer(memcomp_kunit_async_probe, &f->probe);
	f->observing = true;
	ret = mc16_act_store(f, 0, finish_fail, false, false);
	KUNIT_EXPECT_EQ(test, ret, finish_fail ? 0 : 1);
	KUNIT_EXPECT_TRUE(test, f->ctx_cleared);
	KUNIT_EXPECT_TRUE(test, f->fail_consumed);
	KUNIT_EXPECT_EQ(test, f->probe.commit_fail_hits, finish_fail ? 1U : 0U);
	if (finish_fail)
		KUNIT_EXPECT_EQ(test, f->probe.commit_error, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->probe.precomp_hits, error ? 0U : 1U);
	KUNIT_EXPECT_EQ(test, f->probe.allocs, samefill ? 0U : 1U);
	KUNIT_EXPECT_EQ(test, f->probe.frees, finish_fail ? 1U : 0U);
	KUNIT_EXPECT_EQ(test, f->probe.live, !samefill && !finish_fail);
	KUNIT_EXPECT_FALSE(test, f->probe.bad_lifetime);
	KUNIT_ASSERT_TRUE(test, mc16_act_snapshot(token, &s, f->packed));
	KUNIT_EXPECT_TRUE(test, s.attached);
	KUNIT_EXPECT_EQ(test, s.refs, 2U); /* owner + snapshot's lookup */
	KUNIT_EXPECT_TRUE(test, s.flags & MC16_F_OWNER);
	KUNIT_EXPECT_EQ(test, !!(s.flags & MC16_F_RESIDENT), finish_fail);
	if (finish_fail) {
		KUNIT_EXPECT_EQ(test, mc16_flags_state(s.flags), MC16_PENDING);
		KUNIT_EXPECT_EQ(test, s.charge, 0U);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged), charged);
	} else {
		KUNIT_EXPECT_EQ(test, mc16_flags_state(s.flags),
				samefill ? MC16_SAMEFILL : MC16_STORED);
		KUNIT_EXPECT_EQ(test, s.charge, s.bytes);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged),
				charged + s.bytes);
		if (samefill)
			KUNIT_EXPECT_EQ(test, s.word, (u64)req->element);
		if (!samefill) {
			KUNIT_EXPECT_PTR_EQ(test, f->probe.pool, memcomp_pools[0]);
			KUNIT_EXPECT_EQ(test, s.word, (u64)f->probe.handle);
			KUNIT_ASSERT_GT(test, s.bytes, 0U);
			KUNIT_ASSERT_LT(test, s.bytes, (u32)PAGE_SIZE);
			if (!error) {
				KUNIT_ASSERT_EQ(test, s.bytes, req->len);
				KUNIT_EXPECT_MEMEQ(test, f->packed, req->buf, s.bytes);
			}
		}
		KUNIT_ASSERT_TRUE(test, mc16_act_finalize(f, 0));
		ret = mc16_act_load(f, 0, 0, &same);
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_TRUE(test, same);
	}
	kunit_release_action(test, mc16_act_cleanup, f);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token));
}

static void memcomp_test_async_registered_submit(struct kunit *test)
{
	mc16_act_frontend(test, false, 0, false, false);
}

static void memcomp_test_async_registered_backlog(struct kunit *test)
{
	mc16_act_frontend(test, true, 0, false, false);
}

static void memcomp_test_async_registered_error(struct kunit *test)
{
	mc16_act_frontend(test, false, -ENODATA, false, false);
}

static void memcomp_test_async_registered_finish_fail(struct kunit *test)
{
	mc16_act_frontend(test, false, 0, true, false);
}

static void memcomp_test_async_software_samefill(struct kunit *test)
{
	mc16_act_frontend(test, false, 0, false, true);
}

/* Both requests really fail in the registered codec; store must reuse ctx. */
static void memcomp_test_async_lease_identity(struct kunit *test)
{
	struct mc16_act *f = mc16_act_begin(test, true, false, false);
	struct mc16_act_snapshot s;
	unsigned int i;
	long charged;
	bool same;
	int ret;

	if (!f)
		return;
	for (i = 0; i < 2; i++) {
		struct memcomp_test_acomp_req *r;

		r = acomp_request_ctx(f->async->reqs[i].req);
		r->error = -ENODATA;
	}
	KUNIT_ASSERT_TRUE(test, mc16_act_reserve(f, 0));
	charged = atomic_long_read(&f->model.charged);
	KUNIT_ASSERT_EQ(test, mc16_act_submit(f, 0), 0);
	KUNIT_ASSERT_TRUE(test, mc16_act_wait(f));
	for (i = 0; i < 2; i++)
		KUNIT_ASSERT_EQ(test, f->async->reqs[i].ret, -ENODATA);
	/* Only the exact caller-held ctx contains this one-shot error. */
	ret = mc16_act_store(f, 0, false, true, true);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, f->ctx_cleared);
	KUNIT_EXPECT_EQ(test, f->acquisitions_left, 2);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f->model.charged), charged);
	for (i = 0; i < 2; i++) {
		KUNIT_ASSERT_TRUE(test, mc16_act_snapshot(f->token[0] + i, &s, NULL));
		KUNIT_EXPECT_EQ(test, mc16_flags_state(s.flags), MC16_PENDING);
		KUNIT_EXPECT_EQ(test, s.charge, 0U);
	}
	/* Same live reservation and real outputs: unpoisoned lease must succeed. */
	ret = mc16_act_store(f, 0, false, true, false);
	KUNIT_ASSERT_EQ(test, ret, 1);
	KUNIT_EXPECT_TRUE(test, f->ctx_cleared);
	KUNIT_EXPECT_EQ(test, f->acquisitions_left, 2);
	KUNIT_ASSERT_TRUE(test, mc16_act_finalize(f, 0));
	for (i = 0; i < 2; i++) {
		ret = mc16_act_load(f, 0, i, &same);
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_TRUE(test, same);
	}
	kunit_release_action(test, mc16_act_cleanup, f);
}

/* A distinct folio precedes a two-page target; page 1 may require fallback. */
static void mc16_act_precomp_order(struct kunit *test, bool mixed)
{
	struct mc16_act *f = mc16_act_begin(test, true, true, false);
	struct mc16_act_snapshot s;
	unsigned long token, prefix;
	unsigned int i;
	bool same;
	int ret;

	if (!f)
		return;
	if (mixed) {
		struct memcomp_test_acomp_req *r;

		r = acomp_request_ctx(f->async->reqs[2].req);
		r->error = -EIO;
	}
	KUNIT_ASSERT_TRUE(test, mc16_act_reserve(f, 1));
	KUNIT_ASSERT_TRUE(test, mc16_act_reserve(f, 0));
	token = f->token[0];
	prefix = f->token[1];
	KUNIT_ASSERT_EQ(test, mc16_act_submit(f, 1), 0);
	KUNIT_ASSERT_EQ(test, mc16_act_submit(f, 0), 0);
	KUNIT_ASSERT_EQ(test, f->async->nr, 3U);
	KUNIT_ASSERT_TRUE(test, mc16_act_wait(f));
	KUNIT_EXPECT_PTR_EQ(test, f->async->reqs[0].folio, f->folio[1]);
	KUNIT_EXPECT_PTR_EQ(test, f->async->reqs[1].folio, f->folio[0]);
	KUNIT_EXPECT_PTR_EQ(test, f->async->reqs[2].folio, f->folio[0]);
	/* Store out of submission order, exercising selection by folio identity. */
	ret = mc16_act_store(f, 0, false, true, false);
	KUNIT_ASSERT_EQ(test, ret, 1);
	KUNIT_EXPECT_TRUE(test, f->ctx_cleared);
	KUNIT_EXPECT_EQ(test, f->acquisitions_left, 2);
	for (i = 0; i < 2; i++) {
		struct memcomp_async_req *req = &f->async->reqs[i + 1];
		struct memcomp_test_acomp_req *r = acomp_request_ctx(req->req);

		KUNIT_EXPECT_EQ(test, atomic_read(&r->submitted), 1);
		KUNIT_EXPECT_EQ(test, atomic_read(&r->completed), 1);
		KUNIT_EXPECT_EQ(test, req->ret, mixed && i ? -EIO : 0);
		KUNIT_ASSERT_TRUE(test, mc16_act_snapshot(token + i, &s, f->packed));
		KUNIT_EXPECT_EQ(test, mc16_flags_state(s.flags), MC16_STORED);
		KUNIT_EXPECT_TRUE(test, s.flags & MC16_F_OWNER);
		KUNIT_EXPECT_FALSE(test, s.flags & MC16_F_RESIDENT);
		KUNIT_EXPECT_EQ(test, s.charge, s.bytes);
		if (!mixed || !i) {
			KUNIT_ASSERT_EQ(test, s.bytes, req->len);
			KUNIT_EXPECT_MEMEQ(test, f->packed, req->buf, s.bytes);
		}
	}
	/* Finish every original OWNER/payload snapshot before detaching the span. */
	KUNIT_ASSERT_TRUE(test, mc16_act_finalize(f, 0));
	for (i = 0; i < 2; i++) {
		ret = mc16_act_load(f, 0, i, &same);
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_TRUE(test, same);
	}
	/* Target publication must not consume the earlier folio's reservation. */
	KUNIT_ASSERT_TRUE(test, mc16_act_snapshot(prefix, &s, NULL));
	KUNIT_EXPECT_EQ(test, mc16_flags_state(s.flags), MC16_PENDING);
	KUNIT_EXPECT_TRUE(test, s.flags & MC16_F_RESIDENT);
	ret = mc16_act_store(f, 1, false, false, false);
	KUNIT_ASSERT_EQ(test, ret, 1);
	KUNIT_ASSERT_TRUE(test, mc16_act_finalize(f, 1));
	ret = mc16_act_load(f, 1, 0, &same);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, same);
	kunit_release_action(test, mc16_act_cleanup, f);
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token));
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(token + 1));
	KUNIT_EXPECT_FALSE(test, memcomp_entry_present(prefix));
}

static void memcomp_test_async_precomp_offset(struct kunit *test)
{
	mc16_act_precomp_order(test, false);
}

static void memcomp_test_async_precomp_mixed(struct kunit *test)
{
	mc16_act_precomp_order(test, true);
}
#endif /* CONFIG_MEMCG */
#endif /* _MM_MEMCOMP_TESTS_ASYNC_TEST_H */
