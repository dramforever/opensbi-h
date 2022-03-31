/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_double_trap.h>
#include <sbi/sbi_ecall.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_illegal_insn.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_irqchip.h>
#include <sbi/sbi_trap_ldst.h>
#include <sbi/sbi_pmu.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_sse.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_page_fault.h>

static void sbi_trap_error_one(const struct sbi_trap_context *tcntx,
			       const char *prefix, u32 hartid, u32 depth)
{
	const struct sbi_trap_info *trap = &tcntx->trap;
	const struct sbi_trap_regs *regs = &tcntx->regs;

	sbi_printf("\n");
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "mcause", trap->cause, "mtval", trap->tval);
	if (misa_extension('H')) {
		sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
			   hartid, depth, "mtval2", trap->tval2, "mtinst", trap->tinst);
	}
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "mepc", regs->mepc, "mstatus", regs->mstatus);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "ra", regs->ra, "sp", regs->sp);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "gp", regs->gp, "tp", regs->tp);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "s0", regs->s0, "s1", regs->s1);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "a0", regs->a0, "a1", regs->a1);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "a2", regs->a2, "a3", regs->a3);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "a4", regs->a4, "a5", regs->a5);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "a6", regs->a6, "a7", regs->a7);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "s2", regs->s2, "s3", regs->s3);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "s4", regs->s4, "s5", regs->s5);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "s6", regs->s6, "s7", regs->s7);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "s8", regs->s8, "s9", regs->s9);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "s10", regs->s10, "s11", regs->s11);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "t0", regs->t0, "t1", regs->t1);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "t2", regs->t2, "t3", regs->t3);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "t4", regs->t4, "t5", regs->t5);
	sbi_printf("%s: hart%d: trap%d: %s=0x%" PRILX "\n", prefix,
		   hartid, depth, "t6", regs->t6);
}

static void __noreturn sbi_trap_error(const char *msg, int rc,
				      const struct sbi_trap_context *tcntx)
{
	u32 depth = 0, hartid = current_hartid();
	const struct sbi_trap_context *tc;

	for (tc = tcntx; tc; tc = tc->prev_context)
		depth++;

	sbi_printf("\n");
	sbi_printf("%s: hart%d: trap%d: %s (error %d)\n", __func__,
		   hartid, depth - 1, msg, rc);
	for (tc = tcntx; tc; tc = tc->prev_context)
		sbi_trap_error_one(tc, __func__, hartid, --depth);

	sbi_hart_hang();
}

/**
 * Redirect trap to lower privledge mode (S-mode or U-mode)
 *
 * @param regs pointer to register state
 * @param trap pointer to trap details
 *
 * @return 0 on success and negative error code on failure
 */
int sbi_trap_redirect(struct sbi_trap_regs *regs,
		      const struct sbi_trap_info *trap)
{
	struct hext_state *hext = sbi_hext_current_state();
	bool elp = false;
	ulong hstatus, vsstatus, hedeleg, prev_mode;
	bool prev_virt;

	if (misa_extension('H')) {
		prev_virt = sbi_regs_from_virt(regs);
	} else {
		prev_virt = hext->available && hext->virt;
	}

	/* By default, we redirect to HS-mode */
	bool next_virt = false;

	/* Sanity check on previous mode */
	prev_mode = sbi_mstatus_prev_mode(regs->mstatus);
	if (prev_mode != PRV_S && prev_mode != PRV_U)
		return SBI_ENOTSUPP;

	/* If hart support for zicfilp, clear MPELP because redirecting to VS or (H)S */
	if (sbi_hart_has_extension(sbi_scratch_thishart_ptr(), SBI_HART_EXT_ZICFILP)) {
#if __riscv_xlen == 32
		elp = regs->mstatusH & MSTATUSH_MPELP;
		regs->mstatusH &= ~MSTATUSH_MPELP;
#else
		elp = regs->mstatus & MSTATUS_MPELP;
		regs->mstatus &= ~MSTATUS_MPELP;
#endif
	}

	/* If exceptions came from VS/VU-mode, redirect to VS-mode if
	 * delegated in hedeleg
	 */
	if (prev_virt) {
		if (misa_extension('H')) {
			hedeleg = csr_read(CSR_HEDELEG);
		} else {
			hedeleg = hext->hedeleg;
		}

		if ((trap->cause < __riscv_xlen) &&
		    (hedeleg & BIT(trap->cause))) {
			next_virt = true;
		}
	}

	/* Update MSTATUS MPV bits */
	if (misa_extension('H')) {
#if __riscv_xlen == 32
		regs->mstatusH &= ~MSTATUSH_MPV;
		regs->mstatusH |= (next_virt) ? MSTATUSH_MPV : 0UL;
#else
		regs->mstatus &= ~MSTATUS_MPV;
		regs->mstatus |= (next_virt) ? MSTATUS_MPV : 0UL;
#endif
	} else {
		sbi_hext_switch_virt(regs, hext, next_virt);
		/* V bit is updated now */
	}

	/* Update hypervisor CSRs if going to HS-mode */
	if ((misa_extension('H') || hext->available) && !next_virt) {
		if (misa_extension('H'))
			hstatus = csr_read(CSR_HSTATUS);
		else
			hstatus = hext->hstatus;

		if (prev_virt) {
			/* hstatus.SPVP is only updated if coming from VS/VU-mode */
			hstatus &= ~HSTATUS_SPVP;
			hstatus |= (prev_mode == PRV_S) ? HSTATUS_SPVP : 0;
		}
		hstatus &= ~HSTATUS_SPV;
		hstatus |= (prev_virt) ? HSTATUS_SPV : 0;
		hstatus &= ~HSTATUS_GVA;
		hstatus |= (trap->gva) ? HSTATUS_GVA : 0;

		if (misa_extension('H')) {
			csr_write(CSR_HSTATUS, hstatus);
			csr_write(CSR_HTVAL, trap->tval2);
			csr_write(CSR_HTINST, trap->tinst);
		} else {
			hext->hstatus = hstatus;
			hext->htval   = trap->tval2;
			hext->htinst  = trap->tinst;
		}
	}

	/* Update exception related CSRs */
	if (next_virt) {
		if (misa_extension('H')) {
			/* Update VS-mode exception info */
			csr_write(CSR_VSTVAL, trap->tval);
			csr_write(CSR_VSEPC, regs->mepc);
			csr_write(CSR_VSCAUSE, trap->cause);

			/* Set MEPC to VS-mode exception vector base */
			regs->mepc = csr_read(CSR_VSTVEC);
		} else {
			csr_write(CSR_STVAL, trap->tval);
			csr_write(CSR_SEPC, regs->mepc);
			csr_write(CSR_SCAUSE, trap->cause);
			regs->mepc = csr_read(CSR_STVEC);
		}

		/* Set MPP to VS-mode */
		regs->mstatus &= ~MSTATUS_MPP;
		regs->mstatus |= (PRV_S << MSTATUS_MPP_SHIFT);

		/* Get VS-mode SSTATUS CSR */
		if (misa_extension('H'))
			vsstatus = csr_read(CSR_VSSTATUS);
		else
			vsstatus = regs->mstatus & SSTATUS_WRITABLE_MASK;

		/* If elp was set, set it back in vsstatus */
		if (elp)
			vsstatus |= MSTATUS_SPELP;

		/* Set SPP for VS-mode */
		vsstatus &= ~SSTATUS_SPP;
		if (prev_mode == PRV_S)
			vsstatus |= (1UL << SSTATUS_SPP_SHIFT);

		/* Set SPIE for VS-mode */
		vsstatus &= ~SSTATUS_SPIE;
		if (vsstatus & SSTATUS_SIE)
			vsstatus |= (1UL << SSTATUS_SPIE_SHIFT);

		/* Clear SIE for VS-mode */
		vsstatus &= ~SSTATUS_SIE;

		/* Update VS-mode SSTATUS CSR */
		if (misa_extension('H')) {
			csr_write(CSR_VSSTATUS, vsstatus);
		} else {
			regs->mstatus &= ~SSTATUS_WRITABLE_MASK;
			regs->mstatus |= SSTATUS_WRITABLE_MASK & vsstatus;
		}
	} else {
		/* Update S-mode exception info */
		csr_write(CSR_STVAL, trap->tval);
		csr_write(CSR_SEPC, regs->mepc);
		csr_write(CSR_SCAUSE, trap->cause);

		/* Set MEPC to S-mode exception vector base */
		regs->mepc = csr_read(CSR_STVEC);

		/* Set MPP to S-mode */
		regs->mstatus &= ~MSTATUS_MPP;
		regs->mstatus |= (PRV_S << MSTATUS_MPP_SHIFT);

		/* Set SPP for S-mode */
		regs->mstatus &= ~MSTATUS_SPP;
		if (prev_mode == PRV_S)
			regs->mstatus |= (1UL << MSTATUS_SPP_SHIFT);

		/* Set SPIE for S-mode */
		regs->mstatus &= ~MSTATUS_SPIE;
		if (regs->mstatus & MSTATUS_SIE)
			regs->mstatus |= (1UL << MSTATUS_SPIE_SHIFT);

		/* Clear SIE for S-mode */
		regs->mstatus &= ~MSTATUS_SIE;

		/* If elp was set, set it back in mstatus */
		if (elp)
			regs->mstatus |= MSTATUS_SPELP;
	}

	return 0;
}

static int sbi_trap_nonaia_irq(unsigned long irq)
{
	switch (irq) {
	case IRQ_M_TIMER:
		sbi_timer_process();
		break;
	case IRQ_M_SOFT:
		sbi_ipi_process();
		break;
	case IRQ_M_EXT:
		return sbi_irqchip_process();
	default:
		if (irq == sbi_pmu_irq_bit()) {
			sbi_pmu_ovf_irq();
			return 0;
		}
		return SBI_ENOENT;
	}

	return 0;
}

static int sbi_trap_aia_irq(void)
{
	int rc;
	unsigned long mtopi;

	while ((mtopi = csr_read(CSR_MTOPI))) {
		mtopi = mtopi >> TOPI_IID_SHIFT;
		switch (mtopi) {
		case IRQ_M_TIMER:
			sbi_timer_process();
			break;
		case IRQ_M_SOFT:
			sbi_ipi_process();
			break;
		case IRQ_M_EXT:
			rc = sbi_irqchip_process();
			if (rc)
				return rc;
			break;
		default:
			if (mtopi == sbi_pmu_irq_bit()) {
				sbi_pmu_ovf_irq();
				break;
			}

			return SBI_ENOENT;
		}
	}

	return 0;
}

static int sbi_trap_check_interrupt(struct sbi_trap_regs *regs)
{
	struct hext_state *hext = sbi_hext_current_state();
	struct sbi_trap_info trap;

	if (hext->virt && (hext->sip & MIP_STIP) && (hext->sie & MIP_STIP)) {
		trap.cause = IRQ_S_TIMER | BIT(__riscv_xlen - 1);
	} else if (hext->virt && (hext->sip & MIP_SEIP) &&
		   (hext->sie & MIP_SEIP)) {
		trap.cause = IRQ_S_EXT | BIT(__riscv_xlen - 1);
	} else if (hext->virt && (hext->sip & MIP_SSIP) &&
		   (hext->sie & MIP_SSIP)) {
		trap.cause = IRQ_S_SOFT | BIT(__riscv_xlen - 1);
	} else {
		return SBI_OK;
	}

	// sbi_printf("%s: Preempt from %lx %lx pc=0x%lx\n", __func__,
	// 	   (regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT,
	// 	   (csr_read(CSR_MSTATUS) & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT,
	// 	   regs->mepc);

	trap.tval  = 0;
	trap.tval2 = 0;
	trap.tinst = 0;

	return sbi_trap_redirect(regs, &trap);
}

/**
 * Handle trap/interrupt
 *
 * This function is called by firmware linked to OpenSBI
 * library for handling trap/interrupt. It expects the
 * following:
 * 1. The 'mscratch' CSR is pointing to sbi_scratch of current HART
 * 2. The 'mcause' CSR is having exception/interrupt cause
 * 3. The 'mtval' CSR is having additional trap information
 * 4. The 'mtval2' CSR is having additional trap information
 * 5. The 'mtinst' CSR is having decoded trap instruction
 * 6. Stack pointer (SP) is setup for current HART
 * 7. Interrupts are disabled in MSTATUS CSR
 *
 * @param tcntx pointer to trap context
 */
struct sbi_trap_context *sbi_trap_handler(struct sbi_trap_context *tcntx)
{
	int rc		= SBI_ENOTSUPP;
	const char *msg = "trap handler failed";
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	const struct sbi_trap_info *trap = &tcntx->trap;
	struct sbi_trap_regs *regs = &tcntx->regs;
	ulong mcause = tcntx->trap.cause;

	/* Update trap context pointer */
	tcntx->prev_context = sbi_trap_get_context(scratch);
	sbi_trap_set_context(scratch, tcntx);

	if (mcause & MCAUSE_IRQ_MASK) {
		if (sbi_hart_has_extension(sbi_scratch_thishart_ptr(),
					   SBI_HART_EXT_SMAIA))
			rc = sbi_trap_aia_irq();
		else
			rc = sbi_trap_nonaia_irq(mcause & ~MCAUSE_IRQ_MASK);
		if (rc) {
			msg = "unhandled local interrupt";
			goto trap_done;
		}
		rc = sbi_trap_check_interrupt(regs);
		msg = "trap check interrupt";
		goto trap_done;
	}

	switch (mcause) {
	case CAUSE_ILLEGAL_INSTRUCTION:
		rc  = sbi_illegal_insn_handler(tcntx);
		msg = "illegal instruction handler failed";
		break;
	case CAUSE_MISALIGNED_LOAD:
		sbi_pmu_ctr_incr_fw(SBI_PMU_FW_MISALIGNED_LOAD);
		rc  = sbi_misaligned_load_handler(tcntx);
		msg = "misaligned load handler failed";
		break;
	case CAUSE_MISALIGNED_STORE:
		sbi_pmu_ctr_incr_fw(SBI_PMU_FW_MISALIGNED_STORE);
		rc  = sbi_misaligned_store_handler(tcntx);
		msg = "misaligned store handler failed";
		break;
	case CAUSE_SUPERVISOR_ECALL:
	case CAUSE_MACHINE_ECALL:
		rc  = sbi_ecall_handler(tcntx);
		msg = "ecall handler failed";
		break;
	case CAUSE_LOAD_PAGE_FAULT:
	case CAUSE_STORE_PAGE_FAULT:
	case CAUSE_FETCH_PAGE_FAULT:
		rc  = sbi_page_fault_handler(trap->tval, trap->cause, regs);
		msg = "page fault handler failed";
		break;
	case CAUSE_LOAD_ACCESS:
		sbi_pmu_ctr_incr_fw(SBI_PMU_FW_ACCESS_LOAD);
		rc  = sbi_load_access_handler(tcntx);
		msg = "load fault handler failed";
		break;
	case CAUSE_STORE_ACCESS:
		sbi_pmu_ctr_incr_fw(SBI_PMU_FW_ACCESS_STORE);
		rc  = sbi_store_access_handler(tcntx);
		msg = "store fault handler failed";
		break;
	case CAUSE_DOUBLE_TRAP:
		rc  = sbi_double_trap_handler(tcntx);
		msg = "double trap handler failed";
		break;
	default:
		/* If the trap came from S or U mode, redirect it there */
		msg = "trap redirect failed";
		rc  = sbi_trap_redirect(regs, trap);
		break;
	}

	rc = sbi_trap_check_interrupt(regs);
	msg = "trap check interrupt";

trap_done:
	if (rc)
		sbi_trap_error(msg, rc, tcntx);

	if (sbi_mstatus_prev_mode(regs->mstatus) != PRV_M)
		sbi_sse_process_pending_events(regs);

	sbi_trap_set_context(scratch, tcntx->prev_context);
	return tcntx;
}
