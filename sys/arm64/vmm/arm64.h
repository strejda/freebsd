/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (C) 2015 Mihai Carabas <mihai.carabas@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#ifndef _VMM_ARM64_H_
#define _VMM_ARM64_H_

#include <machine/reg.h>
#include <machine/hypervisor.h>
#include <machine/pcpu.h>

#include "mmu.h"
#include "io/vgic_v3.h"
#include "io/vtimer.h"

struct vgic_v3;
struct vgic_v3_cpu;

/*
  Encode the index of a given register within VNCR into the enum value.
  We move the indices up by VNCR_START to not interfere with values for
  non-VNCR registers.
*/
#define VNCR_REG(reg) reg = (VNCR_START + VNCR_##reg / 8)
/* Retrieve the VNCR offset from the enum value */
#define REG_VNCR_OFFSET(val) ((val - VNCR_START) * 8)

enum hypctx_sysreg {
	/* VNCR Registers */
	VNCR_START,

	VNCR_REG(VTTBR_EL2),
	VNCR_REG(VSTTBR_EL2),
	VNCR_REG(VTCR_EL2),
	VNCR_REG(VSTCR_EL2),
	VNCR_REG(VMPIDR_EL2),	/* Virtualization Multiprocessor ID Register */
	VNCR_REG(CNTVOFF_EL2),
	VNCR_REG(HCR_EL2),	/* Hypervisor Configuration Register */
	VNCR_REG(HSTR_EL2),
	VNCR_REG(VPIDR_EL2),	/* Virtualization Processor ID Register */
	VNCR_REG(TPIDR_EL2),
	VNCR_REG(HCRX_EL2),	/* Extended Hypervisor Configuration Register */
	VNCR_REG(VNCR_EL2),
	VNCR_REG(CPACR_EL1),	/* Architectural Feature Access Control Register */
	VNCR_REG(CONTEXTIDR_EL1),	/* Current Process Identifier */
	VNCR_REG(SCTLR_EL1),	/* System Control Register */
	VNCR_REG(ACTLR_EL1),	/* Auxiliary Control Register */
	VNCR_REG(TCR_EL1),	/* Translation Control Register */
	VNCR_REG(AFSR0_EL1),	/* Auxiliary Fault Status Register 0 */
	VNCR_REG(AFSR1_EL1),	/* Auxiliary Fault Status Register 1 */
	VNCR_REG(ESR_EL1),	/* Exception Syndrome Register */
	VNCR_REG(MAIR_EL1),	/* Memory Attribute Indirection Register */
	VNCR_REG(AMAIR_EL1),	/* Auxiliary Memory Attribute Indirection Register */
	VNCR_REG(MDSCR_EL1),	/* Monitor Debug System Control Register */
	VNCR_REG(SPSR_EL1),	/* Saved Program Status Register */
	VNCR_REG(CNTV_CVAL_EL0),
	VNCR_REG(CNTV_CTL_EL0),
	VNCR_REG(CNTP_CVAL_EL0),
	VNCR_REG(CNTP_CTL_EL0),
	VNCR_REG(SCXTNUM_EL1),
	VNCR_REG(TFSR_EL1),
	VNCR_REG(HDFGRTR2_EL2),
	VNCR_REG(CNTPOFF_EL2),
	VNCR_REG(HDFGWTR2_EL2),
	VNCR_REG(HFGRTR_EL2),
	VNCR_REG(HFGWTR_EL2),
	VNCR_REG(HFGITR_EL2),
	VNCR_REG(HDFGRTR_EL2),
	VNCR_REG(HDFGWTR_EL2),
	VNCR_REG(ZCR_EL1),
	VNCR_REG(HAFGRTR_EL2),
	VNCR_REG(SMCR_EL1),
	VNCR_REG(SMPRIMAP_EL2),
	VNCR_REG(TTBR0_EL1),	/* Translation Table Base Register 0 */
	VNCR_REG(TTBR1_EL1),	/* Translation Table Base Register 1 */
	VNCR_REG(FAR_EL1),	/* Fault Address Register */
	VNCR_REG(ELR_EL1),	/* Exception Link Register */
	VNCR_REG(SP_EL1),
	VNCR_REG(VBAR_EL1),	/* Vector Base Address Register */
	VNCR_REG(TCR2_EL1),	/* Translation Control Register 2 */
	VNCR_REG(SCTLR2_EL1),
	VNCR_REG(MAIR2_EL1),
	VNCR_REG(AMAIR2_EL1),
	VNCR_REG(PIRE0_EL1),
	VNCR_REG(PIRE0_EL2),
	VNCR_REG(PIR_EL1),
	VNCR_REG(POR_EL1),
	VNCR_REG(S2PIR_EL2),
	VNCR_REG(S2POR_EL1),
	VNCR_REG(HFGRTR2_EL2),
	VNCR_REG(HFGWTR2_EL2),
	VNCR_REG(PFAR_EL1),
	VNCR_REG(HFGITR2_EL2),
	VNCR_REG(SCTLRMASK_EL1),
	VNCR_REG(CPACRMASK_EL1),
	VNCR_REG(SCTLR2MASK_EL1),
	VNCR_REG(TCRMASK_EL1),
	VNCR_REG(TCR2MASK_EL1),
	VNCR_REG(ACTLRMASK_EL1),
	VNCR_REG(ICH_HCR_EL2),
	VNCR_REG(ICH_VMCR_EL2),
	VNCR_REG(VDISR_EL2),
	VNCR_REG(VSESR_EL2),
	VNCR_REG(PMBLIMITR_EL1),
	VNCR_REG(PMBPTR_EL1),
	VNCR_REG(PMBSR_EL1),
	VNCR_REG(PMSCR_EL1),
	VNCR_REG(PMSEVFR_EL1),
	VNCR_REG(PMSICR_EL1),
	VNCR_REG(PMSIRR_EL1),
	VNCR_REG(PMSLATFR_EL1),
	VNCR_REG(PMSNEVFR_EL1),
	VNCR_REG(PMSDSFR_EL1),
	VNCR_REG(TRFCR_EL1),
	VNCR_REG(TRCITECR_EL1),
	VNCR_REG(GCSPR_EL1),
	VNCR_REG(GCSCR_EL1),
	VNCR_REG(BRBCR_EL1),
	VNCR_REG(SPMACCESSR_EL1),
};

/*
 * Per-vCPU hypervisor state.
 */
struct hypctx {
	struct trapframe tf;

	/*
	 * EL1 & EL0 registers.
	 */
	uint64_t	sp_el0;		/* Stack pointer */
	uint64_t	tpidr_el0;	/* EL0 Software ID Register */
	uint64_t	tpidrro_el0;	/* Read-only Thread ID Register */
	uint64_t	tpidr_el1;	/* EL1 Software ID Register */
	uint64_t	csselr_el1;	/* Cache Size Selection Register */
	uint64_t	mdccint_el1;	/* Monitor DCC Interrupt Enable Register */
	uint64_t	par_el1;	/* Physical Address Register */

	uint64_t	pmcr_el0;	/* Performance Monitors Control Register */
	uint64_t	pmccntr_el0;
	uint64_t	pmccfiltr_el0;
	uint64_t	pmuserenr_el0;
	uint64_t	pmselr_el0;
	uint64_t	pmxevcntr_el0;
	uint64_t	pmcntenset_el0;
	uint64_t	pmintenset_el1;
	uint64_t	pmovsset_el0;
	uint64_t	pmevcntr_el0[31];
	uint64_t	pmevtyper_el0[31];

	uint64_t	dbgclaimset_el1;
	uint64_t	dbgbcr_el1[16];	/* Debug Breakpoint Control Registers */
	uint64_t	dbgbvr_el1[16];	/* Debug Breakpoint Value Registers */
	uint64_t	dbgwcr_el1[16];	/* Debug Watchpoint Control Registers */
	uint64_t	dbgwvr_el1[16];	/* Debug Watchpoint Value Registers */

	/* EL2 control registers */
	uint64_t	cptr_el2;	/* Architectural Feature Trap Register */
	uint64_t	hcr_el2;	/* Hypervisor Configuration Register */
	uint64_t	hcrx_el2;	/* Extended Hypervisor Configuration Register */
	uint64_t	mdcr_el2;	/* Monitor Debug Configuration Register */
	uint64_t	vpidr_el2;	/* Virtualization Processor ID Register */
	uint64_t	vmpidr_el2;	/* Virtualization Multiprocessor ID Register */
	/* On systems without NV2 this still points to the register storage memory page */
	uint64_t	vncr_el2;	/* Virtual Nested Control Register */

	/* FEAT_FGT registers */
	/*uint64_t	hafgrtr_el2; *//* For FEAT_AMUv1 (not supported) */
	uint64_t	hdfgrtr_el2;
	uint64_t	hdfgwtr_el2;
	uint64_t	hfgitr_el2;
	uint64_t	hfgrtr_el2;
	uint64_t	hfgwtr_el2;

	/* FEAT_FGT2 registers */
	uint64_t	hdfgrtr2_el2;
	uint64_t	hdfgwtr2_el2;
	uint64_t	hfgitr2_el2;
	uint64_t	hfgrtr2_el2;
	uint64_t	hfgwtr2_el2;

	struct hyp	*hyp;
	struct vcpu	*vcpu;
	struct {
		uint64_t	far_el2;	/* Fault Address Register */
		uint64_t	hpfar_el2;	/* Hypervisor IPA Fault Address Register */
	} exit_info;

	struct vtimer_cpu 	vtimer_cpu;

	uint64_t		setcaps;	/* Currently enabled capabilities. */

	/* vCPU state used to handle guest debugging. */
	uint64_t		debug_spsr;		/* Saved guest SPSR */
	uint64_t		debug_mdscr;		/* Saved guest MDSCR */

	struct vgic_v3_regs	vgic_v3_regs;
	struct vgic_v3_cpu	*vgic_cpu;
	bool			has_exception;
	bool			dbg_oslock;

	/*
	 * Memory page pointed at by VNCR_EL2. Contains storage for registers
	 * the accesses to which are redirected to memory by FEAT_NV2.
	 * NOTE: The storage is still used even if FEAT_NV2 is not present.
	 */
	void *vncr_regs;
	/* Memory page used to store the host's values of VNCR registers. */
	void *host_vncr_regs;

	uint64_t	el2_addr;	/* The address of this in el2 space */
	uint64_t	el2_vncr_addr;	/* The address of vncr_regs in el2 space */
	uint64_t	el2_host_vncr_addr;
};

/* For non-VHE vmm_hyp.c, this will already be defined in vmm_nvhe.c */
#ifndef __hypctx_vncr_sysreg
#define __hypctx_vncr_sysreg(hypctx, reg)       \
	((uint64_t *)((char *)hypctx->vncr_regs + REG_VNCR_OFFSET(reg)))
#endif

static inline uint64_t *
hypctx_sys_reg(struct hypctx *hypctx, int reg /* enum hypctx_sysreg  */)
{
	if (reg > VNCR_START)
		return (__hypctx_vncr_sysreg(hypctx, reg));
	/* TODO: uniform handling for non-VNCR registers */
	return (NULL);
}

static inline void
hypctx_write_sys_reg(struct hypctx *hypctx, enum hypctx_sysreg reg,
    uint64_t val)
{
	*hypctx_sys_reg(hypctx, reg) = val;
}

static inline uint64_t
hypctx_read_sys_reg(struct hypctx *hypctx, enum hypctx_sysreg reg)
{
	return (*hypctx_sys_reg(hypctx, reg));
}

struct hyp {
	struct vm	*vm;
	struct vtimer	vtimer;
	uint64_t	vmid_generation;
	uint64_t	vttbr_el2;
	uint64_t	el2_addr;	/* The address of this in el2 space */
	uint64_t	feats;		/* Which features are enabled */
#define	HYP_FEAT_HCX		(0x1ul << 0)
#define	HYP_FEAT_ECV_POFF	(0x1ul << 1)
#define	HYP_FEAT_FGT		(0x1ul << 2)
#define	HYP_FEAT_FGT2		(0x1ul << 3)
	bool		vgic_attached;
	struct vgic_v3	*vgic;
	struct hypctx	*ctx[];
};

uint64_t	vmm_call_hyp(uint64_t, ...);

#if 0
#define	eprintf(fmt, ...)	printf("%s:%d " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
#define	eprintf(fmt, ...)	do {} while(0)
#endif

struct hypctx *arm64_get_active_vcpu(void);
void raise_data_insn_abort(struct hypctx *, uint64_t, bool, int);

#endif /* !_VMM_ARM64_H_ */
