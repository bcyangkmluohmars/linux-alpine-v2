/*
 * Stub for UDMA gen_ex registers - Alpine V4+ extended register definitions
 *
 * On Alpine V2, these registers don't exist, but the struct is referenced
 * in the unit_regs layout. We define an empty struct so the code compiles.
 */

#ifndef __AL_HAL_UDMA_REGS_GEN_EX_H__
#define __AL_HAL_UDMA_REGS_GEN_EX_H__

struct udma_gen_ex_vmpr_v4 {
	uint32_t tx_sel;
	uint32_t rx_sel[2];
	uint32_t rsrvd[5];
};

struct udma_gen_ex_regs {
	uint32_t rsrvd[64];
	struct udma_gen_ex_vmpr_v4 vmpr_v4[16];
};

#endif /* __AL_HAL_UDMA_REGS_GEN_EX_H__ */
