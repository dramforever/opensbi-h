/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hart.h>

#define MIP_VS_ALL (MIP_VSEIP | MIP_VSSIP | MIP_VSTIP)

#define HEDELEG_WRITABLE                                                \
	((1U << CAUSE_MISALIGNED_FETCH) | (1U << CAUSE_FETCH_ACCESS) |  \
	 (1U << CAUSE_ILLEGAL_INSTRUCTION) | (1U << CAUSE_BREAKPOINT) | \
	 (1U << CAUSE_MISALIGNED_LOAD) | (1U << CAUSE_LOAD_ACCESS) |    \
	 (1U << CAUSE_MISALIGNED_STORE) | (1U << CAUSE_STORE_ACCESS) |  \
	 (1U << CAUSE_USER_ECALL) | (1U << CAUSE_FETCH_PAGE_FAULT) |    \
	 (1U << CAUSE_LOAD_PAGE_FAULT) | (1U << CAUSE_STORE_PAGE_FAULT))

#define HSTATUS_WRITABLE                                         \
	(HSTATUS_GVA | HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_HU | \
	 HSTATUS_VTVM | HSTATUS_VTW | HSTATUS_VTSR)

/* Sanitize CSR value, assuming all fields are WARL. */
#define sanitize_csr(csr, old_val, new_val)                     \
	({                                                      \
		unsigned long saved = csr_swap(csr, (old_val)); \
		csr_write(csr, (new_val));                      \
		unsigned long sanitized = csr_swap(csr, saved); \
		sanitized;                                      \
	})

int sbi_hext_csr_read(int csr_num, struct sbi_trap_regs *regs,
		      unsigned long *csr_val)
{
	struct hext_state *hext = sbi_hext_current_state();
	unsigned long mpp = (regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;

	if (!sbi_hext_enabled() || (hext->virt && csr_num != CSR_SATP) ||
	    mpp < PRV_S) {
		return SBI_ENOTSUPP;
	}

	switch (csr_num) {
	case CSR_HSTATUS:
		*csr_val = hext->hstatus;
		return SBI_OK;

	case CSR_HTVAL:
		*csr_val = hext->htval;
		return SBI_OK;

	case CSR_HTINST:
		*csr_val = hext->htinst;
		return SBI_OK;

	case CSR_HEDELEG:
		*csr_val = hext->hedeleg;
		return SBI_OK;

	case CSR_HIDELEG:
		*csr_val = hext->hideleg;
		return SBI_OK;

	case CSR_HIE:
		*csr_val = hext->hie;
		return SBI_OK;

	case CSR_HIP:
		*csr_val = hext->hip;
		return SBI_OK;

	case CSR_HVIP:
		*csr_val = hext->hvip;
		return SBI_OK;

	case CSR_HGATP:
		*csr_val = hext->hgatp;
		return SBI_OK;

	case CSR_HCOUNTEREN:
		*csr_val = 0UL;
		return SBI_OK;

	case CSR_VSSTATUS:
		*csr_val = hext->sstatus;
		return SBI_OK;

	case CSR_VSTVEC:
		*csr_val = hext->stvec;
		return SBI_OK;

	case CSR_VSSCRATCH:
		*csr_val = hext->sscratch;
		return SBI_OK;

	case CSR_VSEPC:
		*csr_val = hext->sepc;
		return SBI_OK;

	case CSR_VSCAUSE:
		*csr_val = hext->scause;
		return SBI_OK;

	case CSR_VSTVAL:
		*csr_val = hext->stval;
		return SBI_OK;

	case CSR_VSIE:
		*csr_val = hext->sie;
		return SBI_OK;

	case CSR_VSIP:
		*csr_val = hext->sip;
		return SBI_OK;

	case CSR_VSATP:
		*csr_val = hext->vsatp;
		return SBI_OK;

	case CSR_SATP:
		if (!hext->virt)
			sbi_panic("%s: Read satp trap\n", __func__);

		*csr_val = hext->vsatp;
		return SBI_OK;

	case CSR_HENVCFG:
		*csr_val = 0;

		return SBI_OK;

	default:
		sbi_panic("%s: CSR read 0x%03x: Not implemented\n", __func__,
			  csr_num);
		break;
	}
}

int sbi_hext_csr_write(int csr_num, struct sbi_trap_regs *regs,
		       unsigned long csr_val)
{
	struct hext_state *hext = sbi_hext_current_state();
	unsigned long mode, ppn;
	unsigned long mpp = (regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;

	if (!sbi_hext_enabled() || (hext->virt && csr_num != CSR_SATP) ||
	    mpp < PRV_S)
		return SBI_ENOTSUPP;

	switch (csr_num) {
	case CSR_HSTATUS:
		csr_val = (csr_val & HSTATUS_WRITABLE) |
			  (hext->hstatus & ~HSTATUS_WRITABLE);

		if (!(hext_mstatus_features & MSTATUS_TW)) {
			csr_val &= ~HSTATUS_VTW;
		}

		hext->hstatus = csr_val;

		if (csr_val & HSTATUS_SPV) {
			// Next sret should go to V = 0, need to emulate this
			regs->mstatus |= MSTATUS_TSR;
		} else {
			regs->mstatus &= ~MSTATUS_TSR;
		}

		return SBI_OK;

	case CSR_HTVAL:
		hext->htval = csr_val;
		return SBI_OK;

	case CSR_HTINST:
		hext->htinst = csr_val;
		return SBI_OK;

	case CSR_HGATP:
		/* VMIDLEN = 0 */
		csr_val &= ~HGATP_VMID_MASK;

		mode = csr_val >> HGATP_MODE_SHIFT;
		ppn  = csr_val & HGATP_PPN;

		if ((mode == HGATP_MODE_OFF && ppn == 0) ||
		    (mode == HGATP_MODE_SV39X4)) {
			hext->hgatp = csr_val;
		} else {
			/* Unsupported mode, do nothing */
		}

		return SBI_OK;

	case CSR_HEDELEG:
		csr_val &= HEDELEG_WRITABLE;
		hext->hedeleg = csr_val;
		return SBI_OK;

	case CSR_HIDELEG:
		csr_val &= MIP_VS_ALL;
		hext->hideleg = csr_val;
		return SBI_OK;

	case CSR_HIE:
		csr_val &= MIP_VS_ALL;
		hext->hie = csr_val;
		return SBI_OK;

	case CSR_HIP:
		csr_val &= MIP_VS_ALL;
		hext->hip = csr_val;
		return SBI_OK;

	case CSR_HVIP:
		csr_val &= MIP_VS_ALL;
		hext->hvip = csr_val;
		return SBI_OK;

	case CSR_HCOUNTEREN:
		/* FIXME: Can hcounteren be always read-only all-zeros? */
		return SBI_OK;

	case CSR_VSSTATUS:
		hext->sstatus =
			sanitize_csr(CSR_SSTATUS, hext->sstatus, csr_val);
		return SBI_OK;

	case CSR_VSTVEC:
		hext->stvec = sanitize_csr(CSR_STVEC, hext->stvec, csr_val);
		return SBI_OK;

	case CSR_VSSCRATCH:
		hext->sscratch =
			sanitize_csr(CSR_SSCRATCH, hext->sscratch, csr_val);
		return SBI_OK;

	case CSR_VSEPC:
		hext->sepc = sanitize_csr(CSR_SEPC, hext->sepc, csr_val);
		return SBI_OK;

	case CSR_VSCAUSE:
		hext->scause = sanitize_csr(CSR_SCAUSE, hext->scause, csr_val);
		return SBI_OK;

	case CSR_VSTVAL:
		hext->stval = sanitize_csr(CSR_STVAL, hext->stval, csr_val);
		return SBI_OK;

	case CSR_VSIE:
		hext->sie = sanitize_csr(CSR_SIE, hext->sie, csr_val);
		return SBI_OK;

	case CSR_VSIP:
		// FIXME: Interrupts don't actually work like this
		hext->sip = sanitize_csr(CSR_SIP, hext->sip, csr_val);
		return SBI_OK;

	case CSR_VSATP:
		csr_val = sanitize_csr(CSR_SATP, hext->vsatp, csr_val);
		asm volatile("sfence.vma" ::: "memory");

		/* ASIDLEN = 0 */
		csr_val &= ~SATP_ASID_MASK;

		mode = csr_val >> SATP_MODE_SHIFT;
		ppn  = csr_val & SATP_PPN;

		if ((mode == SATP_MODE_OFF && ppn == 0) ||
		    (mode == SATP_MODE_SV39)) {
			hext->vsatp = csr_val;
		} else {
			/* Unsupported mode, do nothing */
		}

		return SBI_OK;

	case CSR_SATP:
		if (!hext->virt)
			sbi_panic("%s: Write satp trap\n", __func__);

		/* No ASID */
		csr_val &= SATP_PPN | SATP_MODE;

		mode = csr_val >> SATP_MODE_SHIFT;
		ppn  = csr_val & SATP_PPN;

		if ((mode == SATP_MODE_OFF && ppn == 0) ||
		    (mode == SATP_MODE_SV39)) {
			hext->vsatp = csr_val;
			sbi_hext_pt_flush_all(&hext->pt_area);
		} else {
			/* Unsupported mode, do nothing */
		}

		return SBI_OK;

	case CSR_HENVCFG:
		/* hardwire to 0 */

		return SBI_OK;

	default:
		sbi_printf("%s: CSR write 0x%03x: Not implemented\n", __func__,
			   csr_num);
		sbi_hart_hang();
		return SBI_ENOTSUPP;
	}
}
