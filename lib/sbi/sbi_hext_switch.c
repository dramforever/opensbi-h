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

void sbi_hext_switch_virt(struct sbi_trap_regs *regs, struct hext_state *hext,
			  bool virt)
{
	bool tvm, tw, tsr;

	if (hext->virt == virt)
		return;

	hext->virt = virt;

	sbi_printf("%s: [ VM %s ]\n", __func__, virt ? "enter" : "exit");

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

		if (misa_extension('F') && (hext->sstatus & SSTATUS_FS) == 0)
			sbi_panic("%s: Impossible to enforce sstatus.FS = Off",
				  __func__);

		if (misa_extension('V') && (hext->sstatus & SSTATUS_VS) == 0)
			sbi_panic("%s: Impossible to enforce sstatus.VS = Off",
				  __func__);

		hext->sstatus &= SSTATUS_SIE;
		hext->sstatus |=
			(hext->sstatus & SSTATUS_SPIE) ? SSTATUS_SIE : 0;
		hext->sstatus |= SSTATUS_SPIE;

		hext->hstatus &= ~HSTATUS_SPV;
		hext->sstatus &= ~SSTATUS_SPP;

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
		tvm = false;
		tw  = false;
		tsr = false;

		hext->sstatus  = csr_swap(CSR_SSTATUS, hext->sstatus);
		hext->stvec    = csr_swap(CSR_STVEC, hext->stvec);
		hext->sscratch = csr_swap(CSR_SSCRATCH, hext->sscratch);
		hext->sepc     = csr_swap(CSR_SEPC, hext->sepc);
		hext->scause   = csr_swap(CSR_SCAUSE, hext->scause);
		hext->stval    = csr_swap(CSR_STVAL, hext->stval);
		hext->sie      = csr_swap(CSR_SIE, hext->sie);

		csr_set(CSR_SSTATUS, SSTATUS_FS | SSTATUS_VS);

		// FIXME: Interrupts don't actually work like this
		hext->sip = csr_swap(CSR_SIP, hext->sip);

		csr_write(CSR_SATP, hext->satp);
		__asm__ __volatile__("sfence.vma");

		csr_set(CSR_MIDELEG, MIP_SSIP | MIP_STIP | MIP_SEIP);

		csr_write(CSR_MEDELEG, hext->medeleg);
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
