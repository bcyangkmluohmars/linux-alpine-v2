// SPDX-License-Identifier: GPL-2.0
/*
 * Annapurna Labs Alpine V2 SSM (Security Services Module) Crypto Driver
 *
 * PCI driver for the Alpine V2 SoC hardware crypto engine.
 * Registers AES-XTS and AES-CBC with the Linux crypto framework
 * so dm-crypt/LUKS can use hardware-accelerated encryption transparently.
 *
 * PCI device: vendor 0x1c36, device 0x0022, class 0x100000
 * BAR 0: UDMA registers (128KB)
 * BAR 4: Application/crypto registers (64KB)
 *
 * The SSM uses the same UDMA (Unified DMA) infrastructure as the Ethernet
 * and DMA/RAID engines. It provides an M2S (Memory-to-Stream) + S2M
 * (Stream-to-Memory) DMA pair connected to a crypto engine.
 *
 * Architecture:
 *   Linux crypto API  ->  this driver  ->  AL HAL SSM crypto  ->  UDMA DMA  ->  HW crypto engine
 *
 * Copyright (C) 2015 Annapurna Labs Ltd. (HAL code)
 * Copyright (C) 2024 SecFirstNAS contributors (Linux driver glue, kernel 6.12 port)
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#include <crypto/internal/skcipher.h>
#include <crypto/internal/aead.h>
#include <crypto/aes.h>
#include <crypto/xts.h>
#include <crypto/gcm.h>
#include <crypto/scatterwalk.h>
#include <crypto/algapi.h>

/* HAL includes */
#include "al_hal_ssm.h"
#include "al_hal_ssm_crypto.h"
#include "al_hal_unit_adapter.h"

#define DRV_NAME	"al_ssm"
#define DRV_VERSION	"1.0.0"

/* PCI device identification */
#define AL_SSM_VENDOR_ID	0x1c36
#define AL_SSM_DEVICE_ID	0x0022
#define AL_SSM_CLASS_CRYPTO	0x100000  /* Network & encryption controller */

/* DMA ring sizes - must be power of 2 */
#define AL_SSM_RING_SIZE	256
#define AL_SSM_MAX_CHANNELS	4
#define AL_SSM_SA_CACHE_SIZE	16

/* Timeout for synchronous crypto operations */
#define AL_SSM_TIMEOUT_MS	1000

/* Maximum data size per single operation (256KB) */
#define AL_SSM_MAX_DATA_SIZE	(256 * 1024)

/**
 * struct al_ssm_sa_entry - Cached Security Association
 * @sa:       HAL SA parameters
 * @hw_sa:    Hardware SA (written to SA cache)
 * @hw_sa_dma: DMA address of hw_sa
 * @in_use:   Whether this cache slot is occupied
 */
struct al_ssm_sa_entry {
	struct al_crypto_sa sa;
	struct al_crypto_hw_sa *hw_sa;
	dma_addr_t hw_sa_dma;
	bool in_use;
};

/**
 * struct al_ssm_chan - Per-channel (queue) state
 * @dma:         SSM DMA handle (shared, but each queue is independent)
 * @qid:         Queue ID for this channel
 * @lock:        Spinlock protecting this channel's ring
 * @tx_ring:     TX descriptor ring (DMA coherent)
 * @rx_ring:     RX descriptor ring (DMA coherent)
 * @tx_ring_dma: DMA address of TX ring
 * @rx_ring_dma: DMA address of RX ring
 * @comp:        Completion for synchronous operations
 */
struct al_ssm_chan {
	struct al_ssm_dma *dma;
	uint32_t qid;
	spinlock_t lock;
	void *tx_ring;
	void *rx_ring;
	dma_addr_t tx_ring_dma;
	dma_addr_t rx_ring_dma;
	struct completion comp;
};

/**
 * struct al_ssm_dev - Per-device state
 * @pdev:          PCI device
 * @dev:           Device pointer for DMA API
 * @bar0:          UDMA register base (BAR 0)
 * @bar4:          Application/crypto register base (BAR 4)
 * @bars:          Array of BAR pointers (for HAL)
 * @ssm_dma:       SSM DMA handle
 * @unit_adapter:  Unit adapter handle
 * @unit_info:     SSM unit register info
 * @channels:      Per-queue channel state
 * @num_channels:  Number of active channels
 * @sa_cache:      SA cache entries
 * @sa_lock:       Lock for SA cache management
 * @crypto_registered: Whether crypto algorithms are registered
 */
struct al_ssm_dev {
	struct pci_dev *pdev;
	struct device *dev;
	void __iomem *bar0;
	void __iomem *bar4;
	void *bars[6];
	struct al_ssm_dma ssm_dma;
	struct al_unit_adapter unit_adapter;
	struct al_ssm_unit_regs_info unit_info;
	struct al_ssm_chan channels[AL_SSM_MAX_CHANNELS];
	int num_channels;
	struct al_ssm_sa_entry sa_cache[AL_SSM_SA_CACHE_SIZE];
	spinlock_t sa_lock;
	bool crypto_registered;
};

/* Global device pointer (single SSM instance per SoC) */
static struct al_ssm_dev *g_ssm_dev;

/*
 * Forward declarations for crypto algorithm templates
 */
static int al_ssm_xts_setkey(struct crypto_skcipher *tfm, const u8 *key,
			     unsigned int keylen);
static int al_ssm_xts_encrypt(struct skcipher_request *req);
static int al_ssm_xts_decrypt(struct skcipher_request *req);
static int al_ssm_cbc_setkey(struct crypto_skcipher *tfm, const u8 *key,
			     unsigned int keylen);
static int al_ssm_cbc_encrypt(struct skcipher_request *req);
static int al_ssm_cbc_decrypt(struct skcipher_request *req);
static int al_ssm_init_tfm(struct crypto_skcipher *tfm);
static void al_ssm_exit_tfm(struct crypto_skcipher *tfm);

/* ========================================================================
 * Crypto transform context
 * ======================================================================== */

/**
 * struct al_ssm_ctx - Per-transform context
 * @dev:       SSM device
 * @enc_type:  HAL encryption type
 * @aes_ksize: HAL AES key size
 * @key:       Encryption key
 * @xts_key:   XTS tweak key (for XTS mode)
 * @keylen:    Key length in bytes
 * @sa_idx:    SA cache index (-1 if not cached)
 */
struct al_ssm_ctx {
	struct al_ssm_dev *dev;
	enum al_crypto_sa_enc_type enc_type;
	enum al_crypto_sa_aes_ksize aes_ksize;
	u8 key[AES_MAX_KEY_SIZE];
	u8 xts_key[AES_MAX_KEY_SIZE];
	unsigned int keylen;
	int sa_idx;
};

/* ========================================================================
 * SA (Security Association) management
 * ======================================================================== */

/**
 * al_ssm_sa_alloc - Allocate a free SA cache slot
 * @dev: SSM device
 *
 * Returns SA index (0..SA_CACHE_SIZE-1) or -1 if cache is full.
 */
static int __maybe_unused al_ssm_sa_alloc(struct al_ssm_dev *dev)
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&dev->sa_lock, flags);
	for (i = 0; i < AL_SSM_SA_CACHE_SIZE; i++) {
		if (!dev->sa_cache[i].in_use) {
			dev->sa_cache[i].in_use = true;
			spin_unlock_irqrestore(&dev->sa_lock, flags);
			return i;
		}
	}
	spin_unlock_irqrestore(&dev->sa_lock, flags);
	return -1;
}

/**
 * al_ssm_sa_free - Release an SA cache slot
 * @dev:    SSM device
 * @sa_idx: SA index to free
 */
static void al_ssm_sa_free(struct al_ssm_dev *dev, int sa_idx)
{
	unsigned long flags;

	if (sa_idx < 0 || sa_idx >= AL_SSM_SA_CACHE_SIZE)
		return;

	spin_lock_irqsave(&dev->sa_lock, flags);
	dev->sa_cache[sa_idx].in_use = false;
	spin_unlock_irqrestore(&dev->sa_lock, flags);
}

/* ========================================================================
 * Core crypto operation (synchronous, single-buffer)
 * ======================================================================== */

/**
 * al_ssm_do_crypt - Execute a single crypto operation
 * @ctx:    Transform context with key/mode
 * @src:    Source scatterlist
 * @dst:    Destination scatterlist
 * @nbytes: Number of bytes
 * @iv:     Initialization vector
 * @dir:    Direction (encrypt or decrypt)
 *
 * This function linearizes scatter/gather lists, submits to the hardware
 * crypto engine via HAL, polls for completion, then copies back.
 *
 * For dm-crypt, requests are typically single-page (4KB) or contiguous,
 * so the linearization overhead is minimal.
 */
static int al_ssm_do_crypt(struct al_ssm_ctx *ctx,
			   struct scatterlist *src,
			   struct scatterlist *dst,
			   unsigned int nbytes,
			   u8 *iv,
			   enum al_crypto_dir dir)
{
	struct al_ssm_dev *dev = ctx->dev;
	struct al_ssm_chan *chan;
	struct al_crypto_transaction xaction;
	struct al_crypto_sa sa;
	struct al_crypto_hw_sa *hw_sa = NULL;
	struct al_buf src_buf, dst_buf;
	struct al_block src_block, dst_block;
	dma_addr_t src_dma, dst_dma, sa_dma, iv_dma;
	void *src_virt = NULL, *dst_virt = NULL;
	u8 *iv_buf = NULL;
	int rc = 0;
	uint32_t comp_status;
	unsigned long timeout;
	unsigned long flags;
	int qid = 0; /* Use queue 0 for now */

	if (!dev || !dev->bar0)
		return -ENODEV;

	if (nbytes == 0 || nbytes > AL_SSM_MAX_DATA_SIZE)
		return -EINVAL;

	chan = &dev->channels[qid];

	/* Allocate bounce buffers for DMA */
	src_virt = dma_alloc_coherent(dev->dev, nbytes, &src_dma, GFP_KERNEL);
	if (!src_virt)
		return -ENOMEM;

	dst_virt = dma_alloc_coherent(dev->dev, nbytes, &dst_dma, GFP_KERNEL);
	if (!dst_virt) {
		rc = -ENOMEM;
		goto out_free_src;
	}

	/* Allocate IV DMA buffer */
	iv_buf = dma_alloc_coherent(dev->dev, AES_BLOCK_SIZE, &iv_dma, GFP_KERNEL);
	if (!iv_buf) {
		rc = -ENOMEM;
		goto out_free_dst;
	}

	/* Copy source data from scatterlist */
	scatterwalk_map_and_copy(src_virt, src, 0, nbytes, 0);

	/* Copy IV */
	memcpy(iv_buf, iv, AES_BLOCK_SIZE);

	/* Set up SA */
	memset(&sa, 0, sizeof(sa));
	sa.sa_op = AL_CRYPT_ENC;
	sa.enc_type = ctx->enc_type;
	sa.aes_ksize = ctx->aes_ksize;

	if (ctx->enc_type == AL_CRYPT_AES_XTS)
		sa.cntr_size = AL_CRYPT_CNTR_128_BIT;
	else if (ctx->enc_type == AL_CRYPT_AES_CTR)
		sa.cntr_size = AL_CRYPT_CNTR_128_BIT;

	/* Copy encryption key */
	memcpy(sa.enc_key, ctx->key, min_t(unsigned int, ctx->keylen, sizeof(sa.enc_key)));

	/* For XTS mode, copy tweak key */
	if (ctx->enc_type == AL_CRYPT_AES_XTS)
		memcpy(sa.enc_xts_tweak_key, ctx->xts_key,
		       min_t(unsigned int, ctx->keylen, sizeof(sa.enc_xts_tweak_key)));

	/* Copy IV into SA */
	memcpy(sa.enc_iv, iv_buf, AES_BLOCK_SIZE);

	/*
	 * IMPORTANT:
	 * Don't DMA-map stack memory here. On kernels with VMAP_STACK,
	 * stack addresses are vmalloc-backed and dma_map_single() will
	 * reject them ("rejecting DMA map of vmalloc memory"), which can
	 * cascade into dm-crypt I/O failures.
	 */
	hw_sa = dma_alloc_coherent(dev->dev, sizeof(*hw_sa), &sa_dma, GFP_KERNEL);
	if (!hw_sa) {
		rc = -ENOMEM;
		goto out_free_iv;
	}
	memset(hw_sa, 0, sizeof(*hw_sa));
	al_crypto_hw_sa_init(&sa, hw_sa);

	/* Set up source block */
	src_buf.addr = src_dma;
	src_buf.len = nbytes;
	src_block.bufs = &src_buf;
	src_block.num = 1;

	/* Set up destination block */
	dst_buf.addr = dst_dma;
	dst_buf.len = nbytes;
	dst_block.bufs = &dst_buf;
	dst_block.num = 1;

	/* Set up transaction */
	memset(&xaction, 0, sizeof(xaction));
	xaction.dir = dir;
	xaction.flags = AL_SSM_INTERRUPT;
	xaction.src = src_block;
	xaction.src_size = nbytes;
	xaction.dst = dst_block;

	/* SA update - push our SA to the engine */
	xaction.sa_indx = 0; /* Use SA index 0 */
	xaction.sa_in.addr = sa_dma;
	xaction.sa_in.len = sizeof(*hw_sa);

	/* Set IV */
	xaction.enc_iv_in.addr = iv_dma;
	xaction.enc_iv_in.len = AES_BLOCK_SIZE;

	/* Submit to hardware */
	spin_lock_irqsave(&chan->lock, flags);

	rc = al_crypto_dma_prepare(&dev->ssm_dma, qid, &xaction);
	if (rc) {
		spin_unlock_irqrestore(&chan->lock, flags);
		dev_err(dev->dev, "crypto prepare failed: %d\n", rc);
		goto out_unmap_sa;
	}

	rc = al_crypto_dma_action(&dev->ssm_dma, qid, xaction.tx_descs_count);
	if (rc) {
		spin_unlock_irqrestore(&chan->lock, flags);
		dev_err(dev->dev, "crypto action failed: %d\n", rc);
		goto out_unmap_sa;
	}

	spin_unlock_irqrestore(&chan->lock, flags);

	/* Poll for completion */
	timeout = jiffies + msecs_to_jiffies(AL_SSM_TIMEOUT_MS);
	while (time_before(jiffies, timeout)) {
		spin_lock_irqsave(&chan->lock, flags);
		rc = al_crypto_dma_completion(&dev->ssm_dma, qid, &comp_status);
		spin_unlock_irqrestore(&chan->lock, flags);

		if (rc > 0) {
			/* Completed */
			if (comp_status) {
				dev_err(dev->dev, "crypto completion error: 0x%x\n",
					comp_status);
				rc = -EIO;
				goto out_unmap_sa;
			}
			break;
		}
		cpu_relax();
	}

	if (rc <= 0) {
		dev_err(dev->dev, "crypto operation timed out\n");
		rc = -ETIMEDOUT;
		goto out_unmap_sa;
	}

	/* Copy result back to destination scatterlist */
	scatterwalk_map_and_copy(dst_virt, dst, 0, nbytes, 1);
	rc = 0;

out_unmap_sa:
	if (hw_sa)
		dma_free_coherent(dev->dev, sizeof(*hw_sa), hw_sa, sa_dma);
out_free_iv:
	dma_free_coherent(dev->dev, AES_BLOCK_SIZE, iv_buf, iv_dma);
out_free_dst:
	dma_free_coherent(dev->dev, nbytes, dst_virt, dst_dma);
out_free_src:
	dma_free_coherent(dev->dev, nbytes, src_virt, src_dma);
	return rc;
}

/* ========================================================================
 * Crypto algorithm implementations
 * ======================================================================== */

static int al_ssm_init_tfm(struct crypto_skcipher *tfm)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);

	if (!g_ssm_dev)
		return -ENODEV;

	ctx->dev = g_ssm_dev;
	ctx->sa_idx = -1;

	/*
	 * Request extra space for the IV to be stored alongside the request.
	 * The crypto framework uses this for walking scatterlists.
	 */
	crypto_skcipher_set_reqsize(tfm, 0);

	return 0;
}

static void al_ssm_exit_tfm(struct crypto_skcipher *tfm)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);

	if (ctx->sa_idx >= 0 && ctx->dev)
		al_ssm_sa_free(ctx->dev, ctx->sa_idx);

	memzero_explicit(ctx->key, sizeof(ctx->key));
	memzero_explicit(ctx->xts_key, sizeof(ctx->xts_key));
}

/* ---- AES-XTS ---- */

static int al_ssm_xts_setkey(struct crypto_skcipher *tfm, const u8 *key,
			     unsigned int keylen)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	unsigned int half = keylen / 2;
	int ret;

	/* XTS uses two keys: data key + tweak key */
	ret = xts_verify_key(tfm, key, keylen);
	if (ret)
		return ret;

	ctx->enc_type = AL_CRYPT_AES_XTS;

	switch (half) {
	case AES_KEYSIZE_128:
		ctx->aes_ksize = AL_CRYPT_AES_128;
		break;
	case AES_KEYSIZE_256:
		ctx->aes_ksize = AL_CRYPT_AES_256;
		break;
	default:
		return -EINVAL;
	}

	ctx->keylen = half;
	memcpy(ctx->key, key, half);
	memcpy(ctx->xts_key, key + half, half);

	return 0;
}

static int al_ssm_xts_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	u8 iv[AES_BLOCK_SIZE];

	memcpy(iv, req->iv, AES_BLOCK_SIZE);
	return al_ssm_do_crypt(ctx, req->src, req->dst, req->cryptlen,
			       iv, AL_CRYPT_ENCRYPT);
}

static int al_ssm_xts_decrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	u8 iv[AES_BLOCK_SIZE];

	memcpy(iv, req->iv, AES_BLOCK_SIZE);
	return al_ssm_do_crypt(ctx, req->src, req->dst, req->cryptlen,
			       iv, AL_CRYPT_DECRYPT);
}

/* ---- AES-CBC ---- */

static int al_ssm_cbc_setkey(struct crypto_skcipher *tfm, const u8 *key,
			     unsigned int keylen)
{
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->enc_type = AL_CRYPT_AES_CBC;

	switch (keylen) {
	case AES_KEYSIZE_128:
		ctx->aes_ksize = AL_CRYPT_AES_128;
		break;
	case AES_KEYSIZE_192:
		ctx->aes_ksize = AL_CRYPT_AES_192;
		break;
	case AES_KEYSIZE_256:
		ctx->aes_ksize = AL_CRYPT_AES_256;
		break;
	default:
		return -EINVAL;
	}

	ctx->keylen = keylen;
	memcpy(ctx->key, key, keylen);

	return 0;
}

static int al_ssm_cbc_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	u8 iv[AES_BLOCK_SIZE];

	memcpy(iv, req->iv, AES_BLOCK_SIZE);
	return al_ssm_do_crypt(ctx, req->src, req->dst, req->cryptlen,
			       iv, AL_CRYPT_ENCRYPT);
}

static int al_ssm_cbc_decrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct al_ssm_ctx *ctx = crypto_skcipher_ctx(tfm);
	u8 iv[AES_BLOCK_SIZE];

	memcpy(iv, req->iv, AES_BLOCK_SIZE);
	return al_ssm_do_crypt(ctx, req->src, req->dst, req->cryptlen,
			       iv, AL_CRYPT_DECRYPT);
}

/* ========================================================================
 * Crypto algorithm registration
 * ======================================================================== */

static struct skcipher_alg al_ssm_algs[] = {
	{
		.base.cra_name		= "xts(aes)",
		.base.cra_driver_name	= "xts-aes-al-ssm",
		.base.cra_priority	= 400, /* Higher than ARM64 NEON (200) */
		.base.cra_flags		= CRYPTO_ALG_KERN_DRIVER_ONLY,
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
		.base.cra_flags		= CRYPTO_ALG_KERN_DRIVER_ONLY,
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

/* ========================================================================
 * DMA ring setup
 * ======================================================================== */

/**
 * al_ssm_init_channel - Initialize a single DMA channel (queue)
 * @dev:  SSM device
 * @qid:  Queue ID
 */
static int al_ssm_init_channel(struct al_ssm_dev *dev, int qid)
{
	struct al_ssm_chan *chan = &dev->channels[qid];
	struct al_udma_q_params tx_params, rx_params;
	int rc;

	spin_lock_init(&chan->lock);
	init_completion(&chan->comp);
	chan->dma = &dev->ssm_dma;
	chan->qid = qid;

	/* Allocate TX descriptor ring */
	chan->tx_ring = dma_alloc_coherent(dev->dev,
					  AL_SSM_RING_SIZE * sizeof(union al_udma_desc),
					  &chan->tx_ring_dma, GFP_KERNEL);
	if (!chan->tx_ring)
		return -ENOMEM;

	/* Allocate RX descriptor ring */
	chan->rx_ring = dma_alloc_coherent(dev->dev,
					  AL_SSM_RING_SIZE * sizeof(union al_udma_desc),
					  &chan->rx_ring_dma, GFP_KERNEL);
	if (!chan->rx_ring) {
		dma_free_coherent(dev->dev,
				  AL_SSM_RING_SIZE * sizeof(union al_udma_desc),
				  chan->tx_ring, chan->tx_ring_dma);
		return -ENOMEM;
	}

	/* Initialize TX queue */
	memset(&tx_params, 0, sizeof(tx_params));
	tx_params.size = AL_SSM_RING_SIZE;
	tx_params.desc_base = chan->tx_ring;
	tx_params.desc_phy_base = chan->tx_ring_dma;
	tx_params.cdesc_base = chan->tx_ring; /* TX completions reuse the ring */
	tx_params.cdesc_phy_base = chan->tx_ring_dma;
	tx_params.cdesc_size = sizeof(union al_udma_desc);

	/* Initialize RX queue */
	memset(&rx_params, 0, sizeof(rx_params));
	rx_params.size = AL_SSM_RING_SIZE;
	rx_params.desc_base = chan->rx_ring;
	rx_params.desc_phy_base = chan->rx_ring_dma;
	rx_params.cdesc_base = chan->rx_ring; /* RX completions reuse the ring */
	rx_params.cdesc_phy_base = chan->rx_ring_dma;
	rx_params.cdesc_size = sizeof(union al_udma_desc);

	rc = al_ssm_dma_q_init(&dev->ssm_dma, qid, &tx_params, &rx_params,
				AL_CRYPT_AUTH_Q);
	if (rc) {
		dev_err(dev->dev, "failed to init queue %d: %d\n", qid, rc);
		dma_free_coherent(dev->dev,
				  AL_SSM_RING_SIZE * sizeof(union al_udma_desc),
				  chan->rx_ring, chan->rx_ring_dma);
		dma_free_coherent(dev->dev,
				  AL_SSM_RING_SIZE * sizeof(union al_udma_desc),
				  chan->tx_ring, chan->tx_ring_dma);
		return rc;
	}

	return 0;
}

/**
 * al_ssm_free_channel - Free a single DMA channel
 * @dev:  SSM device
 * @qid:  Queue ID
 */
static void al_ssm_free_channel(struct al_ssm_dev *dev, int qid)
{
	struct al_ssm_chan *chan = &dev->channels[qid];

	if (chan->rx_ring) {
		dma_free_coherent(dev->dev,
				  AL_SSM_RING_SIZE * sizeof(union al_udma_desc),
				  chan->rx_ring, chan->rx_ring_dma);
		chan->rx_ring = NULL;
	}

	if (chan->tx_ring) {
		dma_free_coherent(dev->dev,
				  AL_SSM_RING_SIZE * sizeof(union al_udma_desc),
				  chan->tx_ring, chan->tx_ring_dma);
		chan->tx_ring = NULL;
	}
}

/* ========================================================================
 * PCI driver
 * ======================================================================== */

static const struct pci_device_id al_ssm_pci_tbl[] = {
	/*
	 * Match by vendor + device + class to distinguish the SSM crypto engine
	 * (class 0x100000) from the DMA/RAID engine (class 0x010400) which
	 * shares the same PCI device ID (0x0022).
	 */
	{ PCI_VDEVICE(AMAZON_ANNAPURNA_LABS, AL_SSM_DEVICE_ID),
	  .class = AL_SSM_CLASS_CRYPTO, .class_mask = 0xffffff },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, al_ssm_pci_tbl);

static int al_ssm_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *ent)
{
	struct al_ssm_dev *dev;
	struct al_ssm_dma_params dma_params;
	int rc;
	u8 rev_id;

	pci_read_config_byte(pdev, PCI_REVISION_ID, &rev_id);

	dev_info(&pdev->dev,
		 "Alpine V2 SSM crypto engine found (vendor=%04x dev=%04x class=%06x rev=%d)\n",
		 pdev->vendor, pdev->device, pdev->class, rev_id);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	dev->dev = &pdev->dev;
	spin_lock_init(&dev->sa_lock);

	rc = pci_enable_device(pdev);
	if (rc) {
		dev_err(&pdev->dev, "failed to enable PCI device: %d\n", rc);
		goto err_free;
	}

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc) {
		dev_err(&pdev->dev, "failed to request PCI regions: %d\n", rc);
		goto err_disable;
	}

	pci_set_master(pdev);

	/* Set up DMA mask for 40-bit addressing (Alpine V2 LPAE) */
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (rc) {
		dev_warn(&pdev->dev, "40-bit DMA not available, trying 32-bit\n");
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (rc) {
			dev_err(&pdev->dev, "failed to set DMA mask: %d\n", rc);
			goto err_regions;
		}
	}

	/* Map BAR 0 - UDMA registers (128KB) */
	dev->bar0 = pci_iomap(pdev, 0, 0);
	if (!dev->bar0) {
		dev_err(&pdev->dev, "failed to map BAR 0\n");
		rc = -ENOMEM;
		goto err_regions;
	}

	/* Map BAR 4 - Application/crypto registers (64KB) */
	dev->bar4 = pci_iomap(pdev, 4, 0);
	if (!dev->bar4) {
		dev_err(&pdev->dev, "failed to map BAR 4\n");
		rc = -ENOMEM;
		goto err_unmap_bar0;
	}

	dev_info(&pdev->dev,
		 "BAR0=%pR mapped at %p, BAR4=%pR mapped at %p\n",
		 &pdev->resource[0], dev->bar0,
		 &pdev->resource[4], dev->bar4);

	/* Set up BAR array for HAL */
	memset(dev->bars, 0, sizeof(dev->bars));
	dev->bars[0] = dev->bar0;
	dev->bars[4] = dev->bar4;

	/* Get unit register info from HAL */
	al_ssm_unit_regs_info_get(dev->bars, AL_CRYPTO_ALPINE_V2_DEV_ID,
				  AL_SSM_REV_ID_REV2, &dev->unit_info);

	/* Initialize unit adapter */
	memset(&dev->unit_adapter, 0, sizeof(dev->unit_adapter));

	/* Initialize SSM DMA */
	memset(&dma_params, 0, sizeof(dma_params));
	dma_params.rev_id = AL_SSM_REV_ID_REV2;
	dma_params.udma_regs_base = dev->bar0;
	dma_params.name = DRV_NAME;
	dma_params.num_of_queues = 1; /* Start with one queue */

	rc = al_ssm_dma_init(&dev->ssm_dma, &dma_params);
	if (rc) {
		dev_err(&pdev->dev, "failed to init SSM DMA: %d\n", rc);
		goto err_unmap_bar4;
	}

	/* Enable DMA */
	rc = al_ssm_dma_state_set(&dev->ssm_dma, UDMA_NORMAL);
	if (rc) {
		dev_err(&pdev->dev, "failed to enable SSM DMA: %d\n", rc);
		goto err_unmap_bar4;
	}

	/* Initialize channel 0 */
	rc = al_ssm_init_channel(dev, 0);
	if (rc) {
		dev_err(&pdev->dev, "failed to init channel 0: %d\n", rc);
		goto err_dma_disable;
	}
	dev->num_channels = 1;

	pci_set_drvdata(pdev, dev);
	g_ssm_dev = dev;

	/* Register crypto algorithms */
	rc = crypto_register_skciphers(al_ssm_algs, ARRAY_SIZE(al_ssm_algs));
	if (rc) {
		dev_err(&pdev->dev, "failed to register crypto algorithms: %d\n", rc);
		goto err_free_channels;
	}
	dev->crypto_registered = true;

	dev_info(&pdev->dev,
		 "Alpine V2 SSM crypto engine initialized: AES-XTS, AES-CBC (hw accelerated)\n");

	return 0;

err_free_channels:
	al_ssm_free_channel(dev, 0);
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
	int i;

	if (!dev)
		return;

	/* Unregister crypto algorithms */
	if (dev->crypto_registered) {
		crypto_unregister_skciphers(al_ssm_algs, ARRAY_SIZE(al_ssm_algs));
		dev->crypto_registered = false;
	}

	/* Free channels */
	for (i = 0; i < dev->num_channels; i++)
		al_ssm_free_channel(dev, i);

	/* Disable DMA */
	al_ssm_dma_state_set(&dev->ssm_dma, UDMA_DISABLE);

	/* Unmap BARs */
	if (dev->bar4)
		pci_iounmap(pdev, dev->bar4);
	if (dev->bar0)
		pci_iounmap(pdev, dev->bar0);

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
