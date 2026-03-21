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
 *  @{
 * @file   al_hal_ssm_raid_regs.h
 *
 * @brief RAID_Accelerator registers
 */

#ifndef __AL_HAL_RAID_ACCELERATOR_REGS_H__
#define __AL_HAL_RAID_ACCELERATOR_REGS_H__

#include "al_hal_plat_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct raid_accelerator_configuration {
	uint32_t unit_conf;
	uint32_t mem_test;
	uint32_t rsrvd[2];
};

struct raid_accelerator_log {
	uint32_t desc_word0;
	uint32_t desc_word1;
	uint32_t trans_info_1;
	uint32_t trans_info_2;
	uint32_t rsrvd[4];
};

struct raid_accelerator_raid_perf_counter {
	uint32_t exec_cnt;
	uint32_t m2s_active_cnt;
	uint32_t m2s_idle_cnt;
	uint32_t m2s_backp_cnt;
	uint32_t s2m_active_cnt;
	uint32_t s2m_idle_cnt;
	uint32_t s2m_backp_cnt;
	uint32_t cmd_dn_cnt;
	uint32_t src_blocks_cnt;
	uint32_t dst_blocks_cnt;
	uint32_t mem_cmd_dn_cnt;
	uint32_t recover_err_cnt;
	uint32_t src_data_beats;
	uint32_t dst_data_beats;
	uint32_t rsrvd[6];
};

struct raid_accelerator_perfm_cnt_cntl {
	uint32_t conf;
};

struct raid_accelerator_raid_status {
	uint32_t status;
};

struct raid_accelerator_unit_id {
	uint32_t unit_id;
};

struct raid_accelerator_gflog_table {
	uint32_t w0_raw;
	uint32_t w1_raw;
	uint32_t w2_raw;
	uint32_t w3_raw;
};

struct raid_accelerator_gfilog_table {
	uint32_t w0_r;
	uint32_t w1_r;
	uint32_t w2_r;
	uint32_t w3_r;
};

struct raid_accelerator_regs {
	struct raid_accelerator_configuration configuration; /* [0x0] */
	uint32_t rsrvd_0[4];
	struct raid_accelerator_log log;                     /* [0x20] */
	struct raid_accelerator_raid_perf_counter raid_perf_counter; /* [0x40] */
	struct raid_accelerator_perfm_cnt_cntl perfm_cnt_cntl; /* [0x90] */
	struct raid_accelerator_raid_status raid_status;     /* [0x94] */
	struct raid_accelerator_unit_id unit_id;             /* [0x98] */
	uint32_t rsrvd_1[25];
	struct raid_accelerator_gflog_table gflog_table[16]; /* [0x100] */
	struct raid_accelerator_gfilog_table gfilog_table[16]; /* [0x200] */
};

#ifdef __cplusplus
}
#endif

#endif

/** @} */
