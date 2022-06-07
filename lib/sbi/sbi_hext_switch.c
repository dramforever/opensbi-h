/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_encoding.h>

#define HEDELEG_MASK                                                          \
	((1U << CAUSE_LOAD_PAGE_FAULT) | (1U << CAUSE_STORE_PAGE_FAULT) |     \
	 (1U << CAUSE_FETCH_PAGE_FAULT) | (1U << CAUSE_ILLEGAL_INSTRUCTION) | \
	 (1U << CAUSE_SUPERVISOR_ECALL))

void sbi_hext_switch_virt(unsigned long insn, struct sbi_trap_regs *regs,
			  struct hext_state *hext, bool virt)
{
	bool tvm, tw, tsr;

	if (hext->virt == virt)
		return;

	hext->virt = virt;

	if (virt) {
		tvm = (hext->hgatp >> HGATP_MODE_SHIFT) != HGATP_MODE_OFF ||
		      (hext->hstatus & HSTATUS_VTVM) != 0;
		tw  = (hext->hstatus & HSTATUS_VTW) != 0;
		tsr = (hext->hstatus & HSTATUS_VTSR) != 0;

		hext->sstatus  = csr_swap(CSR_SSTATUS, hext->sstatus);
		hext->stvec    = csr_swap(CSR_STVEC, hext->stvec);
		hext->sscratch = csr_swap(CSR_SSCRATCH, hext->sscratch);
		hext->sepc     = csr_swap(CSR_SEPC, hext->sepc);
		hext->scause   = csr_swap(CSR_SCAUSE, hext->scause);
		hext->stval    = csr_swap(CSR_STVAL, hext->stval);
		hext->sie      = csr_swap(CSR_SIE, hext->sie);

		// FIXME: Interrupts don't actually work like this
		hext->sip = csr_swap(CSR_SIP, hext->sip);

		hext->satp = csr_swap(
			CSR_SATP,
			(SATP_MODE_SV39 << SATP_MODE_SHIFT) |
				((unsigned long)hext->pt_area.pt_start >> 12));
		__asm__ __volatile__("sfence.vma");

		csr_clear(CSR_MIDELEG, MIP_SSIP | MIP_STIP | MIP_SEIP);

		hext->medeleg = csr_read_clear(
			CSR_MEDELEG, ~(hext->hedeleg & ~HEDELEG_MASK));
	} else {
		sbi_panic("%s: TODO: VM exit", __func__);
	}

	if (tvm)
		regs->mstatus |= MSTATUS_TVM;
	else
		regs->mstatus &= ~MSTATUS_TVM;

	if (tw)
		regs->mstatus |= MSTATUS_TW;
	else
		regs->mstatus &= ~MSTATUS_TW;

	if (tsr)
		regs->mstatus |= MSTATUS_TSR;
	else
		regs->mstatus &= ~MSTATUS_TSR;
}
