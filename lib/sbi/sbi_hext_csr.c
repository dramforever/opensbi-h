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

int sbi_hext_csr_read(int csr_num, struct sbi_trap_regs *regs,
		      unsigned long *csr_val)
{
	struct hext_state *hext = &hart_hext_state[current_hartid()];
	unsigned long mpp = (regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;

	if (!sbi_hext_enabled() || hext->virt || mpp < PRV_S)
		return SBI_ENOTSUPP;

	switch (csr_num) {
	case CSR_HSTATUS:
		*csr_val = hext->hstatus;
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

	case CSR_HVIP:
		*csr_val = hext->hvip;
		return SBI_OK;

	case CSR_HGATP:
		*csr_val = hext->hgatp;
		return SBI_OK;

	case CSR_HCOUNTEREN:
		return 0;

	default:
		sbi_printf("%s: CSR read 0x%03x: Not implemented\n", __func__,
			   csr_num);
		sbi_hart_hang();
		return SBI_ENOTSUPP;
	}
}

int sbi_hext_csr_write(int csr_num, struct sbi_trap_regs *regs,
		       unsigned long csr_val)
{
	struct hext_state *hext = &hart_hext_state[current_hartid()];
	unsigned long mpp = (regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;

	if (!sbi_hext_enabled() || hext->virt || mpp < PRV_S)
		return SBI_ENOTSUPP;

	switch (csr_num) {
	case CSR_HSTATUS:
		csr_val = (csr_val & HSTATUS_WRITABLE) |
			  (hext->hstatus & ~HSTATUS_WRITABLE);

		if (!(hext_mstatus_features & MSTATUS_TW)) {
			csr_val &= ~HSTATUS_VTW;
		}

		/* TODO: hstatus.SPV = 0 requires mstatus.TSR */
		hext->hstatus = csr_val;

		return SBI_OK;

	case CSR_HGATP:
		/* VMIDLEN = 0 */
		csr_val &= ~HGATP32_VMID_MASK;

		unsigned long mode = csr_val >> HGATP_MODE_SHIFT;
		unsigned long ppn  = csr_val & HGATP_PPN;

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

	case CSR_HVIP:
		csr_val &= MIP_VS_ALL;
		hext->hvip = csr_val;
		return SBI_OK;

	case CSR_HCOUNTEREN:
		return SBI_OK;

	default:
		sbi_printf("%s: CSR write 0x%03x: Not implemented\n", __func__,
			   csr_num);
		sbi_hart_hang();
		return SBI_ENOTSUPP;
	}
}
