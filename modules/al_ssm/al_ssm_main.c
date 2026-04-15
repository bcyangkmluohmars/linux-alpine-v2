// SPDX-License-Identifier: GPL-2.0
/*
 * Annapurna Labs Alpine V2 SSM (Security Services Module) Crypto Driver
 *
 * Asynchronous PCI driver for the Alpine V2 SoC hardware crypto engine.
 * Registers AES-XTS and AES-CBC with the Linux crypto framework
 * so dm-crypt/LUKS can use hardware-accelerated encryption transparently.
 *
 * PCI device: vendor 0x1c36, device 0x0022, class 0x100000
 * BAR 0: UDMA registers (128KB)
 * BAR 4: Application/crypto registers (64KB)
 *
 * Architecture:
 *   dm-crypt  ->  Linux crypto API (async skcipher)
 *             ->  this driver: enqueue, submit to HW, return -EINPROGRESS
 *             ->  completion workqueue polls UDMA, calls skcipher_request_complete()
 *             ->  AL HAL SSM crypto  ->  UDMA DMA  ->  HW crypto engine
 *
 * IMPORTANT: The UDMA requires SEPARATE DMA buffers for submission
 * descriptor rings and completion descriptor rings. Sharing the same
 * buffer causes corruption when multiple operations are in-flight
 * because the hardware overwrites submission descriptors with completion data.
 *
 * Copyright (C) 2015 Annapurna Labs Ltd. (HAL code)
 * Copyright (C) 2024-2026 SecFirstNAS contributors (driver, async rewrite)
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <crypto/internal/skcipher.h>
#include <crypto/aes.h>
#include <crypto/xts.h>
#include <crypto/scatterwalk.h>
#include <crypto/algapi.h>

#include "al_hal_ssm.h"
#include "al_hal_ssm_crypto.h"
#include "al_hal_unit_adapter.h"

#define DRV_NAME	"al_ssm"
#define DRV_VERSION	"2.0.4"

#define AL_SSM_VENDOR_ID	0x1c36
#define AL_SSM_DEVICE_ID	0x0022
#define AL_SSM_CLASS_CRYPTO	0x100000

#define AL_SSM_RING_SIZE	256
#define AL_SSM_MAX_BACKLOG	4096
#define AL_SSM_SUBMIT_RETRIES	10
#define AL_SSM_POLL_INTERVAL_US	100
#define AL_SSM_MAX_DATA_SIZE	(256 * 1024)
#define RING_BYTES		(AL_SSM_RING_SIZE * sizeof(union al_udma_desc))

/*
 * Each crypto operation uses 3 TX descriptors and 1 RX descriptor.
 * Ring size is 256 entries. With HAL reserving AL_CRYPT_DESC_RES (16)
 * descriptors for padding, max concurrent ops = (256 - 16) / 3 = 80.
 * Use 64 for safety margin.
 */
#define AL_SSM_MAX_IN_FLIGHT	64

struct al_ssm_dev;

struct al_ssm_reqctx {
	struct list_head list;
	struct skcipher_request *req;
	struct al_ssm_dev *dev;
	enum al_crypto_dir dir;
	void *src_virt;
	void *dst_virt;
	u8 *iv_buf;
	struct al_crypto_hw_sa *hw_sa;
	dma_addr_t src_dma;
	dma_addr_t dst_dma;
	dma_addr_t sa_dma;
	dma_addr_t iv_dma;
	unsigned int nbytes;
};

struct al_ssm_chan {
	spinlock_t lock;
	void *tx_ring;
	dma_addr_t tx_ring_dma;
	void *tx_cdesc;
	dma_addr_t tx_cdesc_dma;
	void *rx_ring;
	dma_addr_t rx_ring_dma;
	void *rx_cdesc;
	dma_addr_t rx_cdesc_dma;
	struct list_head pending;
	int pending_count;
};

struct al_ssm_ctx {
	struct al_ssm_dev *dev;
	enum al_crypto_sa_enc_type enc_type;
	enum al_crypto_sa_aes_ksize aes_ksize;
	u8 key[AES_MAX_KEY_SIZE];
	u8 xts_key[AES_MAX_KEY_SIZE];
	unsigned int keylen;
};

struct al_ssm_dev {
	struct pci_dev *pdev;
	struct device *dev;
	void __iomem *bar0;
	void __iomem *bar4;
	void *bars[6];
	struct al_ssm_dma ssm_dma;
	struct al_unit_adapter unit_adapter;
	struct al_ssm_unit_regs_info unit_info;
	struct al_ssm_chan channel;
	bool crypto_registered;
	spinlock_t backlog_lock;
	struct list_head backlog;
	atomic_t backlog_count;
	struct workqueue_struct *wq;
	struct delayed_work poll_work;
	atomic_t total_pending;
};

static struct al_ssm_dev *g_ssm_dev;

static void al_ssm_poll_completions(struct work_struct *work);
static void al_ssm_complete_one(struct al_ssm_reqctx *rctx, int err);

/* ── DMA resource management ──────────────────────────────────────────── */

static int al_ssm_alloc_resources(struct al_ssm_reqctx *rctx,
				  struct al_ssm_ctx *ctx,
				  struct scatterlist *src,
				  unsigned int nbytes, u8 *iv,
				  enum al_crypto_dir dir)
{
	struct al_ssm_dev *dev = ctx->dev;
	struct al_crypto_sa sa;

	rctx->dev = dev;
	rctx->dir = dir;
	rctx->nbytes = nbytes;

	if (nbytes == 0 || nbytes > AL_SSM_MAX_DATA_SIZE)
		return -EINVAL;

	rctx->src_virt = dma_alloc_coherent(dev->dev, nbytes, &rctx->src_dma, GFP_KERNEL);
	if (!rctx->src_virt)
		return -ENOMEM;
	rctx->dst_virt = dma_alloc_coherent(dev->dev, nbytes, &rctx->dst_dma, GFP_KERNEL);
	if (!rctx->dst_virt)
		goto err_free_src;
	rctx->iv_buf = dma_alloc_coherent(dev->dev, AES_BLOCK_SIZE, &rctx->iv_dma, GFP_KERNEL);
	if (!rctx->iv_buf)
		goto err_free_dst;
	rctx->hw_sa = dma_alloc_coherent(dev->dev, sizeof(*rctx->hw_sa), &rctx->sa_dma, GFP_KERNEL);
	if (!rctx->hw_sa)
		goto err_free_iv;

	scatterwalk_map_and_copy(rctx->src_virt, src, 0, nbytes, 0);
	memcpy(rctx->iv_buf, iv, AES_BLOCK_SIZE);

	memset(&sa, 0, sizeof(sa));
	sa.sa_op = AL_CRYPT_ENC;
	sa.enc_type = ctx->enc_type;
	sa.aes_ksize = ctx->aes_ksize;
	if (ctx->enc_type == AL_CRYPT_AES_XTS || ctx->enc_type == AL_CRYPT_AES_CTR)
		sa.cntr_size = AL_CRYPT_CNTR_128_BIT;
	memcpy(sa.enc_key, ctx->key, min_t(unsigned int, ctx->keylen, sizeof(sa.enc_key)));
	if (ctx->enc_type == AL_CRYPT_AES_XTS)
		memcpy(sa.enc_xts_tweak_key, ctx->xts_key,
		       min_t(unsigned int, ctx->keylen, sizeof(sa.enc_xts_tweak_key)));
	memcpy(sa.enc_iv, rctx->iv_buf, AES_BLOCK_SIZE);

	memset(rctx->hw_sa, 0, sizeof(*rctx->hw_sa));
	al_crypto_hw_sa_init(&sa, rctx->hw_sa);
	return 0;

err_free_iv:
	dma_free_coherent(dev->dev, AES_BLOCK_SIZE, rctx->iv_buf, rctx->iv_dma);
err_free_dst:
	dma_free_coherent(dev->dev, nbytes, rctx->dst_virt, rctx->dst_dma);
err_free_src:
	dma_free_coherent(dev->dev, nbytes, rctx->src_virt, rctx->src_dma);
	return -ENOMEM;
}

static void al_ssm_free_resources(struct al_ssm_reqctx *rctx)
{
	struct al_ssm_dev *dev = rctx->dev;

	if (rctx->hw_sa)
		dma_free_coherent(dev->dev, sizeof(*rctx->hw_sa), rctx->hw_sa, rctx->sa_dma);
	if (rctx->iv_buf)
		dma_free_coherent(dev->dev, AES_BLOCK_SIZE, rctx->iv_buf, rctx->iv_dma);
	if (rctx->dst_virt)
		dma_free_coherent(dev->dev, rctx->nbytes, rctx->dst_virt, rctx->dst_dma);
	if (rctx->src_virt)
		dma_free_coherent(dev->dev, rctx->nbytes, rctx->src_virt, rctx->src_dma);
}

/* ── Hardware submit + inline completion ───────────────────────────────
 *
 * The SSM crypto engine completes most 4KB operations in < 10us.
 * We submit and immediately spin-poll for completion. If it completes
 * inline, we return 0 (synchronous success) — no workqueue overhead.
 *
 * If the hardware queue is full (ENOSPC), the request goes to the
 * software backlog. The poll workqueue drains completions and
 * re-submits backlogged requests.
 *
 * This hybrid approach:
 *   - Fast path: submit → spin-poll → return 0  (majority of requests)
 *   - Slow path: backlog → workqueue drain → complete async
 * ──────────────────────────────────────────────────────────────────── */

#define AL_SSM_INLINE_POLL_US	500	/* Max spin-poll time per request */

/**
 * al_ssm_submit_and_poll - Submit one request and try to complete inline
 *
 * Returns:
 *   0       - completed synchronously, result already in dst_virt
 *  -ENOSPC  - hardware queue full, caller should backlog
 *  -EIO     - hardware completion error
 *  -EINPROGRESS - submitted but not yet complete, added to pending list
 */
static int al_ssm_submit_and_poll(struct al_ssm_reqctx *rctx)
{
	struct al_ssm_dev *dev = rctx->dev;
	struct al_ssm_chan *chan = &dev->channel;
	struct al_crypto_transaction xaction;
	struct al_buf src_buf, dst_buf;
	struct al_block src_block, dst_block;
	unsigned long flags;
	uint32_t comp_status;
	ktime_t deadline;
	int rc;

	src_buf.addr = rctx->src_dma;
	src_buf.len = rctx->nbytes;
	src_block.bufs = &src_buf;
	src_block.num = 1;
	dst_buf.addr = rctx->dst_dma;
	dst_buf.len = rctx->nbytes;
	dst_block.bufs = &dst_buf;
	dst_block.num = 1;

	memset(&xaction, 0, sizeof(xaction));
	xaction.dir = rctx->dir;
	xaction.flags = AL_SSM_INTERRUPT;
	xaction.src = src_block;
	xaction.src_size = rctx->nbytes;
	xaction.dst = dst_block;
	xaction.sa_indx = 0;
	xaction.sa_in.addr = rctx->sa_dma;
	xaction.sa_in.len = sizeof(*rctx->hw_sa);
	xaction.enc_iv_in.addr = rctx->iv_dma;
	xaction.enc_iv_in.len = AES_BLOCK_SIZE;

	spin_lock_irqsave(&chan->lock, flags);

	if (chan->pending_count >= AL_SSM_MAX_IN_FLIGHT) {
		spin_unlock_irqrestore(&chan->lock, flags);
		return -ENOSPC;
	}

	rc = al_crypto_dma_prepare(&dev->ssm_dma, 0, &xaction);
	if (rc) {
		spin_unlock_irqrestore(&chan->lock, flags);
		return -ENOSPC;
	}

	rc = al_crypto_dma_action(&dev->ssm_dma, 0, xaction.tx_descs_count);
	if (rc) {
		spin_unlock_irqrestore(&chan->lock, flags);
		return rc;
	}

	/*
	 * Add to pending list BEFORE polling. Completions are FIFO —
	 * we must not consume a completion meant for an earlier request.
	 */
	list_add_tail(&rctx->list, &chan->pending);
	chan->pending_count++;
	atomic_inc(&dev->total_pending);

	/*
	 * Spin-poll for completion. Drain the pending list in order.
	 * If OUR request completes, return 0 (sync fast path).
	 * If timeout, leave it in pending for the workqueue.
	 */
	deadline = ktime_add_us(ktime_get(), AL_SSM_INLINE_POLL_US);

	while (ktime_before(ktime_get(), deadline)) {
		struct al_ssm_reqctx *done;

		rc = al_crypto_dma_completion(&dev->ssm_dma, 0, &comp_status);
		if (rc <= 0) {
			cpu_relax();
			continue;
		}

		/* FIFO: first pending entry is the one that completed */
		done = list_first_entry(&chan->pending,
					struct al_ssm_reqctx, list);
		list_del(&done->list);
		chan->pending_count--;
		atomic_dec(&dev->total_pending);

		if (done == rctx) {
			/* OUR request completed — sync fast path */
			spin_unlock_irqrestore(&chan->lock, flags);
			return comp_status ? -EIO : 0;
		}

		/* Earlier request completed — finish it */
		spin_unlock_irqrestore(&chan->lock, flags);
		al_ssm_complete_one(done, comp_status ? -EIO : 0);
		spin_lock_irqsave(&chan->lock, flags);
	}

	/* Timeout — our request stays in pending for the poll workqueue */
	spin_unlock_irqrestore(&chan->lock, flags);
	return -EINPROGRESS;
}

/* ── Completion polling ───────────────────────────────────────────────── */

static void al_ssm_complete_one(struct al_ssm_reqctx *rctx, int err)
{
	struct skcipher_request *req = rctx->req;

	if (!err)
		scatterwalk_map_and_copy(rctx->dst_virt, req->dst, 0, rctx->nbytes, 1);
	al_ssm_free_resources(rctx);
	skcipher_request_complete(req, err);
}

static int al_ssm_poll_channel(struct al_ssm_dev *dev)
{
	struct al_ssm_chan *chan = &dev->channel;
	struct al_ssm_reqctx *rctx;
	unsigned long flags;
	uint32_t comp_status;
	int completed = 0, rc;

	spin_lock_irqsave(&chan->lock, flags);
	while (!list_empty(&chan->pending)) {
		rc = al_crypto_dma_completion(&dev->ssm_dma, 0, &comp_status);
		if (rc <= 0) {
			if (!list_empty(&chan->pending) && completed == 0) {
				rctx = list_first_entry(&chan->pending,
							struct al_ssm_reqctx, list);
				dev_dbg(dev->dev, "poll: no completion, pending=%d first: dir=%d nbytes=%u\n",
					chan->pending_count, rctx->dir, rctx->nbytes);
			}
			break;
		}
		rctx = list_first_entry(&chan->pending, struct al_ssm_reqctx, list);
		list_del(&rctx->list);
		chan->pending_count--;
		atomic_dec(&dev->total_pending);
		spin_unlock_irqrestore(&chan->lock, flags);

		dev_dbg(dev->dev, "complete: dir=%d nbytes=%u comp_status=0x%x remaining=%d\n",
			rctx->dir, rctx->nbytes, comp_status,
			atomic_read(&dev->total_pending));

		al_ssm_complete_one(rctx, comp_status ? -EIO : 0);
		completed++;

		spin_lock_irqsave(&chan->lock, flags);
	}
	spin_unlock_irqrestore(&chan->lock, flags);
	return completed;
}

static void al_ssm_drain_backlog(struct al_ssm_dev *dev)
{
	struct al_ssm_reqctx *rctx;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&dev->backlog_lock, flags);
	while (!list_empty(&dev->backlog)) {
		rctx = list_first_entry(&dev->backlog, struct al_ssm_reqctx, list);
		list_del(&rctx->list);
		atomic_dec(&dev->backlog_count);
		spin_unlock_irqrestore(&dev->backlog_lock, flags);

		rc = al_ssm_submit_and_poll(rctx);
		if (rc == -ENOSPC) {
			/* Still full — put it back and stop */
			spin_lock_irqsave(&dev->backlog_lock, flags);
			list_add(&rctx->list, &dev->backlog);
			atomic_inc(&dev->backlog_count);
			break;
		}
		if (rc == 0) {
			/* Completed synchronously */
			scatterwalk_map_and_copy(rctx->dst_virt, rctx->req->dst,
						 0, rctx->nbytes, 1);
			al_ssm_free_resources(rctx);
			skcipher_request_complete(rctx->req, 0);
		} else if (rc == -EINPROGRESS) {
			/* In pending list, poll will complete it */
		} else {
			/* Error */
			al_ssm_free_resources(rctx);
			skcipher_request_complete(rctx->req, rc);
		}

		spin_lock_irqsave(&dev->backlog_lock, flags);
	}
	spin_unlock_irqrestore(&dev->backlog_lock, flags);
}

static void al_ssm_poll_completions(struct work_struct *work)
{
	struct delayed_work *dw = to_delayed_work(work);
	struct al_ssm_dev *dev = container_of(dw, struct al_ssm_dev, poll_work);
	int completed;
	int pending;

	completed = al_ssm_poll_channel(dev);
	if (completed > 0)
		al_ssm_drain_backlog(dev);

	pending = atomic_read(&dev->total_pending) + atomic_read(&dev->backlog_count);

	dev_dbg(dev->dev, "poll_work: completed=%d pending=%d\n", completed, pending);

	if (pending > 0) {
		mod_delayed_work(dev->wq, &dev->poll_work,
				 completed > 0 ? 0 : usecs_to_jiffies(AL_SSM_POLL_INTERVAL_US));
	}
	/*
	 * If pending == 0, we stop polling. New requests will restart
	 * via al_ssm_schedule_poll() in al_ssm_enqueue().
	 * mod_delayed_work() guarantees the work starts even if it was
	 * already queued, preventing the race where queue_delayed_work
	 * silently drops a reschedule.
	 */
}

static void al_ssm_schedule_poll(struct al_ssm_dev *dev)
{
	mod_delayed_work(dev->wq, &dev->poll_work, 0);
}

/* ── Async crypto entry ───────────────────────────────────────────────── */

static int al_ssm_enqueue(struct al_ssm_ctx *ctx, struct skcipher_request *req,
			   enum al_crypto_dir dir)
{
	struct al_ssm_dev *dev = ctx->dev;
	struct al_ssm_reqctx *rctx = skcipher_request_ctx(req);
	u8 iv[AES_BLOCK_SIZE];
	unsigned long flags;
	int rc;

	if (!dev || !dev->bar0)
		return -ENODEV;

	memcpy(iv, req->iv, AES_BLOCK_SIZE);
	INIT_LIST_HEAD(&rctx->list);
	rctx->req = req;

	rc = al_ssm_alloc_resources(rctx, ctx, req->src, req->cryptlen, iv, dir);
	if (rc)
		return rc;

	rc = al_ssm_submit_and_poll(rctx);

	if (rc == 0) {
		/* Completed synchronously — copy result and return success */
		scatterwalk_map_and_copy(rctx->dst_virt, req->dst,
					0, rctx->nbytes, 1);
		al_ssm_free_resources(rctx);
		return 0;
	}

	if (rc == -EINPROGRESS) {
		/* Submitted but not yet complete — workqueue will finish it */
		al_ssm_schedule_poll(dev);
		return -EINPROGRESS;
	}

	if (rc == -ENOSPC) {
		/* HW queue full — software backlog */
		spin_lock_irqsave(&dev->backlog_lock, flags);
		if (atomic_read(&dev->backlog_count) >= AL_SSM_MAX_BACKLOG) {
			spin_unlock_irqrestore(&dev->backlog_lock, flags);
			al_ssm_free_resources(rctx);
			return -EBUSY;
		}
		list_add_tail(&rctx->list, &dev->backlog);
		atomic_inc(&dev->backlog_count);
		spin_unlock_irqrestore(&dev->backlog_lock, flags);
		al_ssm_schedule_poll(dev);
		return -EINPROGRESS;
	}

	/* Real error */
	al_ssm_free_resources(rctx);
	return rc;
}

/* ── Crypto algorithm callbacks ───────────────────────────────────────── */

static int al_ssm_init_tfm(struct crypto_skcipher *tfm)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	if (!g_ssm_dev)
		return -ENODEV;
	ctx->dev = g_ssm_dev;
	crypto_skcipher_set_reqsize(tfm, sizeof(struct al_ssm_reqctx));
	return 0;
}

static void al_ssm_exit_tfm(struct crypto_skcipher *tfm)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	memzero_explicit(ctx->key, sizeof(ctx->key));
	memzero_explicit(ctx->xts_key, sizeof(ctx->xts_key));
}

static int al_ssm_xts_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	unsigned int half = keylen / 2;
	int ret = xts_verify_key(tfm, key, keylen);
	if (ret)
		return ret;
	ctx->enc_type = AL_CRYPT_AES_XTS;
	switch (half) {
	case AES_KEYSIZE_128: ctx->aes_ksize = AL_CRYPT_AES_128; break;
	case AES_KEYSIZE_256: ctx->aes_ksize = AL_CRYPT_AES_256; break;
	default: return -EINVAL;
	}
	ctx->keylen = half;
	memcpy(ctx->key, key, half);
	memcpy(ctx->xts_key, key + half, half);
	return 0;
}

static int al_ssm_xts_encrypt(struct skcipher_request *req)
{
	return al_ssm_enqueue(crypto_skcipher_ctx(crypto_skcipher_reqtfm(req)), req, AL_CRYPT_ENCRYPT);
}

static int al_ssm_xts_decrypt(struct skcipher_request *req)
{
	return al_ssm_enqueue(crypto_skcipher_ctx(crypto_skcipher_reqtfm(req)), req, AL_CRYPT_DECRYPT);
}

static int al_ssm_cbc_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	ctx->enc_type = AL_CRYPT_AES_CBC;
	switch (keylen) {
	case AES_KEYSIZE_128: ctx->aes_ksize = AL_CRYPT_AES_128; break;
	case AES_KEYSIZE_192: ctx->aes_ksize = AL_CRYPT_AES_192; break;
	case AES_KEYSIZE_256: ctx->aes_ksize = AL_CRYPT_AES_256; break;
	default: return -EINVAL;
	}
	ctx->keylen = keylen;
	memcpy(ctx->key, key, keylen);
	return 0;
}

static int al_ssm_cbc_encrypt(struct skcipher_request *req)
{
	return al_ssm_enqueue(crypto_skcipher_ctx(crypto_skcipher_reqtfm(req)), req, AL_CRYPT_ENCRYPT);
}

static int al_ssm_cbc_decrypt(struct skcipher_request *req)
{
	return al_ssm_enqueue(crypto_skcipher_ctx(crypto_skcipher_reqtfm(req)), req, AL_CRYPT_DECRYPT);
}

/* ── Algorithm registration ───────────────────────────────────────────── */

static struct skcipher_alg al_ssm_algs[] = {
	{
		.base.cra_name		= "xts(aes)",
		.base.cra_driver_name	= "xts-aes-al-ssm",
		.base.cra_priority	= 400,
		.base.cra_flags		= CRYPTO_ALG_ASYNC | CRYPTO_ALG_KERN_DRIVER_ONLY,
		.base.cra_blocksize	= AES_BLOCK_SIZE,
		.base.cra_ctxsize	= sizeof(struct al_ssm_ctx),
		.base.cra_module	= THIS_MODULE,
		.min_keysize		= 2 * AES_MIN_KEY_SIZE,
		.max_keysize		= 2 * AES_MAX_KEY_SIZE,
		.ivsize			= AES_BLOCK_SIZE,
		.setkey			= al_ssm_xts_setkey,
		.encrypt		= al_ssm_xts_encrypt,
		.decrypt		= al_ssm_xts_decrypt,
		.init			= al_ssm_init_tfm,
		.exit			= al_ssm_exit_tfm,
	},
	{
		.base.cra_name		= "cbc(aes)",
		.base.cra_driver_name	= "cbc-aes-al-ssm",
		.base.cra_priority	= 400,
		.base.cra_flags		= CRYPTO_ALG_ASYNC | CRYPTO_ALG_KERN_DRIVER_ONLY,
		.base.cra_blocksize	= AES_BLOCK_SIZE,
		.base.cra_ctxsize	= sizeof(struct al_ssm_ctx),
		.base.cra_module	= THIS_MODULE,
		.min_keysize		= AES_MIN_KEY_SIZE,
		.max_keysize		= AES_MAX_KEY_SIZE,
		.ivsize			= AES_BLOCK_SIZE,
		.setkey			= al_ssm_cbc_setkey,
		.encrypt		= al_ssm_cbc_encrypt,
		.decrypt		= al_ssm_cbc_decrypt,
		.init			= al_ssm_init_tfm,
		.exit			= al_ssm_exit_tfm,
	},
};

/* ── Channel init with SEPARATE completion rings ──────────────────────── */

static int al_ssm_init_channel(struct al_ssm_dev *dev)
{
	struct al_ssm_chan *chan = &dev->channel;
	struct al_udma_q_params tx_params, rx_params;
	int rc;

	spin_lock_init(&chan->lock);
	INIT_LIST_HEAD(&chan->pending);
	chan->pending_count = 0;

	chan->tx_ring = dma_alloc_coherent(dev->dev, RING_BYTES, &chan->tx_ring_dma, GFP_KERNEL);
	if (!chan->tx_ring)
		return -ENOMEM;
	chan->tx_cdesc = dma_alloc_coherent(dev->dev, RING_BYTES, &chan->tx_cdesc_dma, GFP_KERNEL);
	if (!chan->tx_cdesc)
		goto err_free_tx;
	chan->rx_ring = dma_alloc_coherent(dev->dev, RING_BYTES, &chan->rx_ring_dma, GFP_KERNEL);
	if (!chan->rx_ring)
		goto err_free_tx_cdesc;
	chan->rx_cdesc = dma_alloc_coherent(dev->dev, RING_BYTES, &chan->rx_cdesc_dma, GFP_KERNEL);
	if (!chan->rx_cdesc)
		goto err_free_rx;

	memset(&tx_params, 0, sizeof(tx_params));
	tx_params.size = AL_SSM_RING_SIZE;
	tx_params.desc_base = chan->tx_ring;
	tx_params.desc_phy_base = chan->tx_ring_dma;
	tx_params.cdesc_base = chan->tx_cdesc;
	tx_params.cdesc_phy_base = chan->tx_cdesc_dma;
	tx_params.cdesc_size = sizeof(union al_udma_desc);

	memset(&rx_params, 0, sizeof(rx_params));
	rx_params.size = AL_SSM_RING_SIZE;
	rx_params.desc_base = chan->rx_ring;
	rx_params.desc_phy_base = chan->rx_ring_dma;
	rx_params.cdesc_base = chan->rx_cdesc;
	rx_params.cdesc_phy_base = chan->rx_cdesc_dma;
	rx_params.cdesc_size = sizeof(union al_udma_desc);

	rc = al_ssm_dma_q_init(&dev->ssm_dma, 0, &tx_params, &rx_params, AL_CRYPT_AUTH_Q);
	if (rc) {
		dev_err(dev->dev, "failed to init queue 0: %d\n", rc);
		goto err_free_rx_cdesc;
	}
	return 0;

err_free_rx_cdesc:
	dma_free_coherent(dev->dev, RING_BYTES, chan->rx_cdesc, chan->rx_cdesc_dma);
err_free_rx:
	dma_free_coherent(dev->dev, RING_BYTES, chan->rx_ring, chan->rx_ring_dma);
err_free_tx_cdesc:
	dma_free_coherent(dev->dev, RING_BYTES, chan->tx_cdesc, chan->tx_cdesc_dma);
err_free_tx:
	dma_free_coherent(dev->dev, RING_BYTES, chan->tx_ring, chan->tx_ring_dma);
	return rc ? rc : -ENOMEM;
}

static void al_ssm_free_channel(struct al_ssm_dev *dev)
{
	struct al_ssm_chan *chan = &dev->channel;
	if (chan->rx_cdesc)
		dma_free_coherent(dev->dev, RING_BYTES, chan->rx_cdesc, chan->rx_cdesc_dma);
	if (chan->rx_ring)
		dma_free_coherent(dev->dev, RING_BYTES, chan->rx_ring, chan->rx_ring_dma);
	if (chan->tx_cdesc)
		dma_free_coherent(dev->dev, RING_BYTES, chan->tx_cdesc, chan->tx_cdesc_dma);
	if (chan->tx_ring)
		dma_free_coherent(dev->dev, RING_BYTES, chan->tx_ring, chan->tx_ring_dma);
}

/* ── PCI driver ───────────────────────────────────────────────────────── */

static const struct pci_device_id al_ssm_pci_tbl[] = {
	{ PCI_VDEVICE(AMAZON_ANNAPURNA_LABS, AL_SSM_DEVICE_ID),
	  .class = AL_SSM_CLASS_CRYPTO, .class_mask = 0xffffff },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, al_ssm_pci_tbl);

static int al_ssm_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct al_ssm_dev *dev;
	struct al_ssm_dma_params dma_params;
	int rc;
	u8 rev_id;

	pci_read_config_byte(pdev, PCI_REVISION_ID, &rev_id);
	dev_info(&pdev->dev, "Alpine V2 SSM crypto engine found (vendor=%04x dev=%04x class=%06x rev=%d)\n",
		 pdev->vendor, pdev->device, pdev->class, rev_id);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	dev->dev = &pdev->dev;
	spin_lock_init(&dev->backlog_lock);
	INIT_LIST_HEAD(&dev->backlog);
	atomic_set(&dev->backlog_count, 0);
	atomic_set(&dev->total_pending, 0);

	rc = pci_enable_device(pdev);
	if (rc)
		goto err_free;
	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc)
		goto err_disable;
	pci_set_master(pdev);

	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (rc)
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (rc)
		goto err_regions;

	dev->bar0 = pci_iomap(pdev, 0, 0);
	if (!dev->bar0) { rc = -ENOMEM; goto err_regions; }
	dev->bar4 = pci_iomap(pdev, 4, 0);
	if (!dev->bar4) { rc = -ENOMEM; goto err_unmap_bar0; }

	dev_info(&pdev->dev, "BAR0=%pR mapped at %p, BAR4=%pR mapped at %p\n",
		 &pdev->resource[0], dev->bar0, &pdev->resource[4], dev->bar4);

	memset(dev->bars, 0, sizeof(dev->bars));
	dev->bars[0] = dev->bar0;
	dev->bars[4] = dev->bar4;

	al_ssm_unit_regs_info_get(dev->bars, AL_CRYPTO_ALPINE_V2_DEV_ID,
				  AL_SSM_REV_ID_REV2, &dev->unit_info);

	memset(&dma_params, 0, sizeof(dma_params));
	dma_params.rev_id = AL_SSM_REV_ID_REV2;
	dma_params.udma_regs_base = dev->bar0;
	dma_params.name = DRV_NAME;
	dma_params.num_of_queues = 1;

	rc = al_ssm_dma_init(&dev->ssm_dma, &dma_params);
	if (rc)
		goto err_unmap_bar4;
	rc = al_ssm_dma_state_set(&dev->ssm_dma, UDMA_NORMAL);
	if (rc)
		goto err_unmap_bar4;
	rc = al_ssm_init_channel(dev);
	if (rc)
		goto err_dma_disable;

	dev->wq = alloc_workqueue("al_ssm_wq", WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (!dev->wq) { rc = -ENOMEM; goto err_free_channel; }
	INIT_DELAYED_WORK(&dev->poll_work, al_ssm_poll_completions);

	pci_set_drvdata(pdev, dev);
	g_ssm_dev = dev;

	rc = crypto_register_skciphers(al_ssm_algs, ARRAY_SIZE(al_ssm_algs));
	if (rc)
		goto err_destroy_wq;
	dev->crypto_registered = true;

	dev_info(&pdev->dev,
		 "Alpine V2 SSM crypto engine initialized: AES-XTS, AES-CBC "
		 "(async, ring %d, max %d in-flight, separate completion rings)\n",
		 AL_SSM_RING_SIZE, AL_SSM_MAX_IN_FLIGHT);
	return 0;

err_destroy_wq:
	destroy_workqueue(dev->wq);
err_free_channel:
	al_ssm_free_channel(dev);
err_dma_disable:
	al_ssm_dma_state_set(&dev->ssm_dma, UDMA_DISABLE);
err_unmap_bar4:
	pci_iounmap(pdev, dev->bar4);
err_unmap_bar0:
	pci_iounmap(pdev, dev->bar0);
err_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	kfree(dev);
	return rc;
}

static void al_ssm_pci_remove(struct pci_dev *pdev)
{
	struct al_ssm_dev *dev = pci_get_drvdata(pdev);
	struct al_ssm_reqctx *rctx, *tmp;

	if (!dev)
		return;
	if (dev->crypto_registered) {
		crypto_unregister_skciphers(al_ssm_algs, ARRAY_SIZE(al_ssm_algs));
		dev->crypto_registered = false;
	}
	cancel_delayed_work_sync(&dev->poll_work);
	destroy_workqueue(dev->wq);

	list_for_each_entry_safe(rctx, tmp, &dev->backlog, list) {
		list_del(&rctx->list);
		al_ssm_complete_one(rctx, -ESHUTDOWN);
	}
	list_for_each_entry_safe(rctx, tmp, &dev->channel.pending, list) {
		list_del(&rctx->list);
		al_ssm_complete_one(rctx, -ESHUTDOWN);
	}

	al_ssm_free_channel(dev);
	al_ssm_dma_state_set(&dev->ssm_dma, UDMA_DISABLE);
	if (dev->bar4) pci_iounmap(pdev, dev->bar4);
	if (dev->bar0) pci_iounmap(pdev, dev->bar0);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	g_ssm_dev = NULL;
	kfree(dev);
	dev_info(&pdev->dev, "Alpine V2 SSM crypto engine removed\n");
}

static struct pci_driver al_ssm_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= al_ssm_pci_tbl,
	.probe		= al_ssm_pci_probe,
	.remove		= al_ssm_pci_remove,
};

module_pci_driver(al_ssm_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Annapurna Labs (HAL), SecFirstNAS contributors (driver)");
MODULE_DESCRIPTION("Annapurna Labs Alpine V2 SSM hardware crypto engine driver");
MODULE_VERSION(DRV_VERSION);
