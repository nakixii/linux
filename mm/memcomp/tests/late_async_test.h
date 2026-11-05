/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MEMCOMP_TESTS_LATE_ASYNC_TEST_H
#define _MM_MEMCOMP_TESTS_LATE_ASYNC_TEST_H

/* Included after the shared KUnit async codec definitions. */
/* A second real LOADED event must not replace an initialized pool. */
static struct acomp_alg memcomp_test_acomp_again_alg = {
	.compress = memcomp_test_acomp_compress,
	.decompress = memcomp_test_acomp_decompress,
	.init = memcomp_test_acomp_init,
	.exit = memcomp_test_acomp_exit,
	.reqsize = sizeof(struct memcomp_test_acomp_req),
	.base = {
		.cra_name = "memcomp-kunit-acomp",
		.cra_driver_name = "memcomp-kunit-acomp-again",
		.cra_flags = CRYPTO_ALG_ASYNC,
		.cra_blocksize = 1,
		.cra_ctxsize = sizeof(struct memcomp_test_acomp_tfm),
		.cra_module = THIS_MODULE,
	},
};

/* An unrelated algorithm must never become this selector's transform. */
static struct acomp_alg memcomp_test_acomp_other_alg = {
	.compress = memcomp_test_acomp_compress,
	.decompress = memcomp_test_acomp_decompress,
	.init = memcomp_test_acomp_init,
	.exit = memcomp_test_acomp_exit,
	.reqsize = sizeof(struct memcomp_test_acomp_req),
	.base = {
		.cra_name = "memcomp-kunit-other",
		.cra_driver_name = "memcomp-kunit-other-async",
		.cra_flags = CRYPTO_ALG_ASYNC,
		.cra_blocksize = 1,
		.cra_ctxsize = sizeof(struct memcomp_test_acomp_tfm),
		.cra_module = THIS_MODULE,
	},
};

/* The synchronous form uses the boot-selected codec without replacing it. */
struct memcomp_test_comp_tfm {
	struct crypto_comp *sync;
};

static int memcomp_test_comp_init(struct crypto_tfm *tfm)
{
	struct memcomp_test_comp_tfm *ctx = crypto_tfm_ctx(tfm);

	ctx->sync = crypto_alloc_comp(memcomp_compressor, 0, 0);
	return PTR_ERR_OR_ZERO(ctx->sync);
}

static void memcomp_test_comp_exit(struct crypto_tfm *tfm)
{
	struct memcomp_test_comp_tfm *ctx = crypto_tfm_ctx(tfm);

	crypto_free_comp(ctx->sync);
}

static int memcomp_test_comp_compress(struct crypto_tfm *tfm, const u8 *src,
				      unsigned int slen, u8 *dst,
				      unsigned int *dlen)
{
	struct memcomp_test_comp_tfm *ctx = crypto_tfm_ctx(tfm);

	return crypto_comp_compress(ctx->sync, src, slen, dst, dlen);
}

static int memcomp_test_comp_decompress(struct crypto_tfm *tfm, const u8 *src,
					unsigned int slen, u8 *dst,
					unsigned int *dlen)
{
	struct memcomp_test_comp_tfm *ctx = crypto_tfm_ctx(tfm);

	return crypto_comp_decompress(ctx->sync, src, slen, dst, dlen);
}

static struct crypto_alg memcomp_test_comp_alg = {
	.cra_name = "memcomp-kunit-acomp",
	.cra_driver_name = "memcomp-kunit-acomp-sync",
	.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,
	.cra_blocksize = 1,
	.cra_ctxsize = sizeof(struct memcomp_test_comp_tfm),
	.cra_init = memcomp_test_comp_init,
	.cra_exit = memcomp_test_comp_exit,
	.cra_module = THIS_MODULE,
	.cra_u = {
		.compress = {
			.coa_compress = memcomp_test_comp_compress,
			.coa_decompress = memcomp_test_comp_decompress,
		},
	},
};

struct memcomp_test_late_async_fixture {
	struct memcomp_compressor_select select;
	struct memcomp_async_pool pool;
	struct notifier_block loaded_nb;
	struct completion loaded;
	struct crypto_comp *sync;
	struct memcomp_async *taken;
	struct crypto_acomp *initial_tfm;
	struct memcomp_async *initial_batches[BITS_PER_LONG];
	unsigned int initial_nr;
	struct kunit *test;
	char compressor[MEMCOMP_COMPRESSOR_NAME_LEN];
	int inits_before;
	int exits_before;
	bool sync_registered;
	bool async_registered;
	bool again_registered;
	bool other_registered;
	bool select_registered;
	bool observer_registered;
};

static int memcomp_test_late_loaded(struct notifier_block *nb,
				    unsigned long msg, void *data)
{
	struct memcomp_test_late_async_fixture *f;
	struct crypto_alg *alg = data;

	if (msg != CRYPTO_MSG_ALG_LOADED ||
	    (alg != &memcomp_test_acomp_alg.base &&
	     alg != &memcomp_test_acomp_again_alg.base &&
	     alg != &memcomp_test_acomp_other_alg.base))
		return NOTIFY_DONE;
	f = container_of(nb, struct memcomp_test_late_async_fixture, loaded_nb);
	/* Lower priority than the production callback: its queueing is done. */
	complete(&f->loaded);
	return NOTIFY_OK;
}

static void memcomp_test_late_async_cleanup(void *data)
{
	struct memcomp_test_late_async_fixture *f = data;
	unsigned int i, j;

	/* Never hold select_lock while waiting for either callback or work. */
	if (f->select_registered)
		crypto_unregister_notifier(&f->select.nb);
	if (f->observer_registered)
		crypto_unregister_notifier(&f->loaded_nb);
	cancel_delayed_work_sync(&f->select.work);
	if (f->taken)
		memcomp_async_give(&f->pool, f->taken);
	for (i = 0; i < f->pool.nr; i++)
		memcomp_async_free_batch(f->pool.batches[i]);
	/* Preserve ownership even when a broken retry replaced the first pool. */
	for (i = 0; i < f->initial_nr; i++) {
		for (j = 0; j < f->pool.nr; j++) {
			if (f->initial_batches[i] == f->pool.batches[j])
				break;
		}
		if (j == f->pool.nr)
			memcomp_async_free_batch(f->initial_batches[i]);
	}
	if (f->pool.tfm)
		crypto_free_acomp(f->pool.tfm);
	if (f->initial_tfm && f->initial_tfm != f->pool.tfm)
		crypto_free_acomp(f->initial_tfm);
	if (f->other_registered)
		crypto_unregister_acomp(&memcomp_test_acomp_other_alg);
	if (f->again_registered)
		crypto_unregister_acomp(&memcomp_test_acomp_again_alg);
	if (f->async_registered)
		crypto_unregister_acomp(&memcomp_test_acomp_alg);
	if (f->sync)
		crypto_free_comp(f->sync);
	if (f->sync_registered)
		crypto_unregister_alg(&memcomp_test_comp_alg);
	KUNIT_EXPECT_EQ(f->test,
			atomic_read(&memcomp_test_acomp_inits) - f->inits_before,
			atomic_read(&memcomp_test_acomp_exits) - f->exits_before);
}

static void memcomp_test_async_late_registration(struct kunit *test)
{
	struct memcomp_test_late_async_fixture *f;
	struct crypto_acomp *tfm, *boot_tfm;
	struct memcomp_async *first;
	const char *driver;
	unsigned long waited;
	unsigned int nr, i, j;
	int ret;

	KUNIT_ASSERT_TRUE(test, memcomp_is_enabled());
	KUNIT_ASSERT_NE(test, strcmp(memcomp_compressor,
				     "memcomp-kunit-acomp"), 0);
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->test = test;
	f->inits_before = atomic_read(&memcomp_test_acomp_inits);
	f->exits_before = atomic_read(&memcomp_test_acomp_exits);
	spin_lock_init(&f->pool.lock);
	INIT_DELAYED_WORK(&f->select.work, memcomp_select_worker);
	init_completion(&f->loaded);
	strscpy(f->compressor, "memcomp-kunit-acomp", sizeof(f->compressor));
	f->select.compressor = f->compressor;
	f->select.pool = &f->pool;
	f->select.nb.notifier_call = memcomp_crypto_notify;
	f->loaded_nb.notifier_call = memcomp_test_late_loaded;
	f->loaded_nb.priority = -1;
	ret = kunit_add_action_or_reset(test, memcomp_test_late_async_cleanup, f);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = crypto_register_alg(&memcomp_test_comp_alg);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->sync_registered = true;
	f->sync = crypto_alloc_comp(f->compressor, 0, 0);
	if (IS_ERR(f->sync)) {
		ret = PTR_ERR(f->sync);
		f->sync = NULL;
		KUNIT_FAIL(test, "late async synchronous selection: %d", ret);
		return;
	}
	KUNIT_EXPECT_STREQ(test, crypto_comp_name(f->sync), f->compressor);
	ret = crypto_has_acomp(f->compressor, CRYPTO_ALG_ASYNC,
			       CRYPTO_ALG_ASYNC | CRYPTO_NOLOAD);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* Only boot does global startup; this context owns a selected sync form. */
	mutex_lock(&memcomp_select_lock);
	f->select.decided = true;
	f->select.started = true;
	mutex_unlock(&memcomp_select_lock);
	/* Before publication there must not be a visible async transform. */
	KUNIT_ASSERT_PTR_EQ(test, smp_load_acquire(&f->pool.tfm), NULL);
	KUNIT_ASSERT_EQ(test, f->pool.nr, 0U);
	/* Observe the production pool without replacing its release-published tfm. */
	boot_tfm = smp_load_acquire(&memcomp_async_pool.tfm);

	ret = crypto_register_notifier(&f->loaded_nb);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->observer_registered = true;
	ret = crypto_register_notifier(&f->select.nb);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->select_registered = true;
	/* Run a real event while only the selected synchronous form exists. */
	ret = crypto_register_acomp(&memcomp_test_acomp_other_alg);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->other_registered = true;
	waited = wait_for_completion_timeout(&f->loaded, 10 * HZ);
	KUNIT_ASSERT_NE(test, waited, 0UL);
	flush_delayed_work(&f->select.work);
	/* No release publication is allowed for the unrelated async name. */
	KUNIT_EXPECT_PTR_EQ(test, smp_load_acquire(&f->pool.tfm), NULL);
	KUNIT_EXPECT_EQ(test, f->pool.nr, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_test_acomp_inits),
			f->inits_before);
	reinit_completion(&f->loaded);
	/* No direct async_init/worker call, queueing or global pool replacement. */
	ret = crypto_register_acomp(&memcomp_test_acomp_alg);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->async_registered = true;
	waited = wait_for_completion_timeout(&f->loaded, 10 * HZ);
	KUNIT_ASSERT_NE(test, waited, 0UL);
	flush_delayed_work(&f->select.work);
	/* Consume exactly the publication performed by the production worker. */
	tfm = smp_load_acquire(&f->pool.tfm);
	KUNIT_ASSERT_NOT_NULL(test, tfm);
	driver = crypto_tfm_alg_driver_name(crypto_acomp_tfm(tfm));
	KUNIT_EXPECT_STREQ(test, driver, "memcomp-kunit-acomp-async");
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_test_acomp_inits),
			f->inits_before + 1);
	nr = f->pool.nr;
	KUNIT_ASSERT_GT(test, nr, 0U);
	KUNIT_EXPECT_EQ(test, (unsigned int)hweight_long(f->pool.free), nr);
	for (i = 0; i < nr; i++) {
		struct memcomp_async *batch = f->pool.batches[i];

		KUNIT_ASSERT_NOT_NULL(test, batch);
		KUNIT_EXPECT_EQ(test, batch->id, i);
		for (j = 0; j < MEMCOMP_ASYNC_PAGES; j++) {
			KUNIT_EXPECT_NOT_NULL(test, batch->reqs[j].req);
			KUNIT_EXPECT_NOT_NULL(test, batch->reqs[j].buf);
			KUNIT_EXPECT_PTR_EQ(test, batch->reqs[j].async, batch);
		}
	}
	f->initial_tfm = tfm;
	f->initial_nr = nr;
	memcpy(f->initial_batches, f->pool.batches,
	       nr * sizeof(f->initial_batches[0]));
	first = f->pool.batches[0];
	KUNIT_ASSERT_NOT_NULL(test, first);
	f->taken = memcomp_async_take(&f->pool, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->taken);
	memcomp_async_give(&f->pool, f->taken);
	f->taken = NULL;

	/* A second real registration must not allocate or replace anything. */
	reinit_completion(&f->loaded);
	ret = crypto_register_acomp(&memcomp_test_acomp_again_alg);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->again_registered = true;
	waited = wait_for_completion_timeout(&f->loaded, 10 * HZ);
	KUNIT_ASSERT_NE(test, waited, 0UL);
	flush_delayed_work(&f->select.work);
	/* Repeated events must leave the release-published transform unchanged. */
	KUNIT_ASSERT_PTR_EQ(test, smp_load_acquire(&f->pool.tfm), tfm);
	KUNIT_EXPECT_PTR_EQ(test, f->pool.batches[0], first);
	KUNIT_ASSERT_EQ(test, f->pool.nr, nr);
	for (i = 0; i < nr; i++)
		KUNIT_EXPECT_PTR_EQ(test, f->pool.batches[i], f->initial_batches[i]);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_test_acomp_inits),
			f->inits_before + 1);

	/* Unregister first, drain second, then prove a new event cannot requeue. */
	ret = crypto_unregister_notifier(&f->select.nb);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->select_registered = false;
	cancel_delayed_work_sync(&f->select.work);
	crypto_unregister_acomp(&memcomp_test_acomp_again_alg);
	f->again_registered = false;
	reinit_completion(&f->loaded);
	ret = crypto_register_acomp(&memcomp_test_acomp_again_alg);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f->again_registered = true;
	waited = wait_for_completion_timeout(&f->loaded, 10 * HZ);
	KUNIT_ASSERT_NE(test, waited, 0UL);
	KUNIT_EXPECT_FALSE(test, flush_delayed_work(&f->select.work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&f->select.work));
	/* Repeated events must leave the release-published transform unchanged. */
	KUNIT_EXPECT_PTR_EQ(test, smp_load_acquire(&f->pool.tfm), tfm);
	KUNIT_EXPECT_EQ(test, atomic_read(&memcomp_test_acomp_inits),
			f->inits_before + 1);
	/* The boot pool's independently published transform was never replaced. */
	KUNIT_EXPECT_PTR_EQ(test, smp_load_acquire(&memcomp_async_pool.tfm),
			    boot_tfm);
}

#endif /* _MM_MEMCOMP_TESTS_LATE_ASYNC_TEST_H */
