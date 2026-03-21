/*******************************************************************************
Copyright (C) 2015 Annapurna Labs Ltd.

This file may be licensed under the terms of the Annapurna Labs Commercial
License Agreement.

Alternatively, this file can be distributed under the terms of the GNU General
Public License V2 as published by the Free Software Foundation and can be
found at http://www.gnu.org/licenses/gpl-2.0.html

Alternatively, redistribution and use in source and binary forms, with or
without modification, are permitted provided that the following conditions are
met:

*     Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

*     Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in
the documentation and/or other materials provided with the
distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

/**
 * @defgroup group_ssm_api API
 * Cryptographic / RAID Acceleration Engine common HAL API
 * @ingroup group_ssm
 * @{
 * @file   al_hal_ssm.h
 *
 * Adapted for Alpine V2 / kernel 6.12 out-of-tree build.
 * Removed V3/V4 queue support and unit_adapter dependency.
 */

#ifndef __AL_HAL_SSM_H__
#define __AL_HAL_SSM_H__

#include "al_hal_common.h"
#include "al_hal_udma.h"
#include "al_hal_udma_config.h"
#include "al_hal_m2m_udma.h"

#define AL_SSM_REV_ID_REV1		1	/* Alpine V1 */
#define AL_SSM_REV_ID_REV2		2	/* Alpine V2 */

#define AL_SSM_MAX_SRC_DESCS	31
#define AL_SSM_MAX_DST_DESCS	31

enum al_ssm_op_flags {
	AL_SSM_INTERRUPT = AL_BIT(0),     /* enable interrupt on completion */
	AL_SSM_BARRIER = AL_BIT(1),       /* data memory barrier */
	AL_SSM_SRC_NO_SNOOP = AL_BIT(2),  /* no snoop on source buffers */
	AL_SSM_DEST_NO_SNOOP = AL_BIT(3), /* no snoop on dest buffers */
};

/**
 * SSM queue types.
 */
enum al_ssm_q_type {
	AL_CRYPT_AUTH_Q,
	AL_MEM_CRC_MEMCPY_Q,
	AL_RAID_Q
};

/** SSM DMA private data structure */
struct al_ssm_dma {
	uint8_t rev_id;
	struct al_m2m_udma m2m_udma;
	enum al_ssm_q_type q_types[DMA_MAX_Q];
	unsigned int ssm_max_src_descs;
	unsigned int ssm_max_dst_descs;
};

/* SSM DMA parameters from upper layer */
struct al_ssm_dma_params {
	uint8_t rev_id;
	void __iomem *udma_regs_base;
	char *name;
	uint8_t num_of_queues;
};

/* SSM Unit info of register base address */
struct al_ssm_unit_regs_info {
	void __iomem *udma_regs_base;
	void __iomem *raid_regs_base;
};

/**
 * Initialize DMA for SSM operations
 */
int al_ssm_dma_init(struct al_ssm_dma *ssm_dma, struct al_ssm_dma_params *params);

/**
 * Initialize the m2s(tx) and s2m(rx) components of the queue
 */
int al_ssm_dma_q_init(struct al_ssm_dma		*ssm_dma,
		      uint32_t			qid,
		      struct al_udma_q_params	*tx_params,
		      struct al_udma_q_params	*rx_params,
		      enum al_ssm_q_type	q_type);

/**
 * Change the DMA state
 */
int al_ssm_dma_state_set(struct al_ssm_dma *ssm_dma, enum al_udma_state dma_state);

/**
 * Get udma handle
 */
int al_ssm_dma_handle_get(struct al_ssm_dma *ssm_dma,
			  enum al_udma_type type,
			  struct al_udma **udma);

struct al_udma *al_ssm_dma_tx_udma_handle_get(struct al_ssm_dma *ssm_dma);
struct al_udma *al_ssm_dma_rx_udma_handle_get(struct al_ssm_dma *ssm_dma);
struct al_udma_q *al_ssm_dma_tx_queue_handle_get(struct al_ssm_dma *ssm_dma, unsigned int qid);
struct al_udma_q *al_ssm_dma_rx_queue_handle_get(struct al_ssm_dma *ssm_dma, unsigned int qid);

/**
 * Start asynchronous execution of SSM transaction
 */
int al_ssm_dma_action(struct al_ssm_dma *ssm_dma, uint32_t qid, int tx_descs);

/* *INDENT-OFF* */
#ifdef __cplusplus
}
#endif
/* *INDENT-ON* */
/** @} end of SSM group */
#endif		/* __AL_HAL_SSM_H__ */
