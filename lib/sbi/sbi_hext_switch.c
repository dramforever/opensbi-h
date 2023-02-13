/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_bitops.h>

#define HEDELEG_MASK                                                          \
	((1U << CAUSE_LOAD_PAGE_FAULT) | (1U << CAUSE_STORE_PAGE_FAULT) |     \
	 (1U << CAUSE_FETCH_PAGE_FAULT) | (1U << CAUSE_ILLEGAL_INSTRUCTION) | \
	 (1U << CAUSE_SUPERVISOR_ECALL))

#define MIP_S_ALL (MIP_SEIP | MIP_STIP | MIP_SSIP)

void sbi_hext_switch_virt(struct sbi_trap_regs *regs, struct hext_state *hext,
			  bool virt)
{
	bool tvm, tw, tsr;
	unsigned long sstatus, vsip;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	if (hext->virt == virt)
		return;

	hext->virt = virt;

	if (virt) {
		tvm = true;
		tw  = (hext->hstatus & HSTATUS_VTW) != 0;
		tsr = (hext->hstatus & HSTATUS_VTSR) != 0;

		sstatus = regs->mstatus & SSTATUS_WRITABLE_MASK;
		regs->mstatus &= ~SSTATUS_WRITABLE_MASK;
		regs->mstatus |= SSTATUS_WRITABLE_MASK & hext->sstatus;
		hext->sstatus = sstatus;

		hext->stvec    = csr_swap(CSR_STVEC, hext->stvec);
		hext->sscratch = csr_swap(CSR_SSCRATCH, hext->sscratch);
		hext->sepc     = csr_swap(CSR_SEPC, hext->sepc);
		hext->scause   = csr_swap(CSR_SCAUSE, hext->scause);
		hext->stval    = csr_swap(CSR_STVAL, hext->stval);
		hext->sie      = csr_swap(CSR_SIE, hext->sie);

		/*
		 * On implementations supporting RVH, (HS-level) sstatus.FS
		 * overrides vsstatus.FS. If sstatus.FS = Off, no matter what
		 * state vsstatus.FS is in, operations that modify floating
		 * point state raise illegal instruction exceptions.
		 *
		 * However, mstatus.FS does *not* override sstatus.FS. So
		 * there's now way for us to emulate this behavior for HS-mode.
		 * For now, just check if this emulation is needed and panic.
		 *
		 * Similarly for sstatus.VS.
		 */

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

		/*
		 * FIXME: Why is reading the CSR needed? Why doesn't
		 * regs->mstatus work?
		 */
		regs->mstatus &= ~MSTATUS_MPP;
		regs->mstatus |=
			((csr_read(CSR_MSTATUS) & SSTATUS_SPP) ? PRV_S : PRV_U)
			<< MSTATUS_MPP_SHIFT;
		hext->sstatus &= ~SSTATUS_SPP;

		// FIXME: Interrupts don't actually work like this
		hext->sip = csr_read_clear(CSR_MIP, MIP_S_ALL) & MIP_S_ALL;
		csr_set(CSR_MIP, hext->hvip >> 1);

		hext->satp = csr_swap(
			CSR_SATP,
			(SATP_MODE_SV39 << SATP_MODE_SHIFT) |
				((unsigned long)hext->pt_area.pt_start >> 12));
		__asm__ __volatile__("sfence.vma");

		hext->medeleg = csr_read_clear(
			CSR_MEDELEG, ~(hext->hedeleg & ~HEDELEG_MASK));

		// trap CSR_TIME
		if (sbi_hart_priv_version(scratch) >= SBI_HART_PRIV_VER_1_10)
			csr_clear(CSR_MCOUNTEREN, BIT(CSR_TIME - CSR_CYCLE));
	} else {
		tvm = false;
		tw  = false;
		tsr = (hext->hstatus & HSTATUS_SPV) != 0;

		sstatus = regs->mstatus & SSTATUS_WRITABLE_MASK;
		regs->mstatus &= ~SSTATUS_WRITABLE_MASK;
		regs->mstatus |= SSTATUS_WRITABLE_MASK & hext->sstatus;
		hext->sstatus = sstatus;

		hext->stvec    = csr_swap(CSR_STVEC, hext->stvec);
		hext->sscratch = csr_swap(CSR_SSCRATCH, hext->sscratch);
		hext->sepc     = csr_swap(CSR_SEPC, hext->sepc);
		hext->scause   = csr_swap(CSR_SCAUSE, hext->scause);
		hext->stval    = csr_swap(CSR_STVAL, hext->stval);
		hext->sie      = csr_swap(CSR_SIE, hext->sie);

		/*
		 * If RVF is implemented, sstatus.FS must not be Off prior to
		 * entering VS/VU-mode. This is asserted above.
		 *
		 * Since VS-mode has full control over sstatus.FS, and
		 * sstatus.FS is just an alias of mstatus.FS, we could not know,
		 * for example, if the guest touched floating point state and
		 * put sstatus.FS back to Clean. Therefore, we must assume that
		 * sstatus.FS should be dirty now.
		 *
		 * Similarly for sstatus.VS.
		 */
		regs->mstatus |= SSTATUS_FS | SSTATUS_VS;

		// FIXME: Interrupts don't actually work like this
		vsip = csr_read_clear(CSR_MIP, MIP_S_ALL);
		csr_set(CSR_MIP, hext->sip & ~MIP_SEIP);

		hext->hvip &= ~MIP_VSSIP;
		hext->hvip |= (vsip & MIP_SSIP) ? MIP_VSSIP : 0;

		csr_write(CSR_SATP, hext->satp);
		__asm__ __volatile__("sfence.vma");

		csr_write(CSR_MEDELEG, hext->medeleg);

		// Do not trap CSR_TIME
		if (sbi_hart_priv_version(scratch) >= SBI_HART_PRIV_VER_1_10)
			csr_set(CSR_MCOUNTEREN, BIT(CSR_TIME - CSR_CYCLE));
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
