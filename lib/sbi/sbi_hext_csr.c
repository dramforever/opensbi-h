/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hart.h>

#define MIP_VS_ALL (MIP_VSEIP | MIP_VSSIP | MIP_VSTIP)
#define HEDELEG_WRITABLE 0xb1ff

// TODO: Nested virtualization requires writable hstatus.{TVM,TW,TSR}
#define HSTATUS_WRITABLE (HSTATUS_GVA | HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_HU)

int sbi_hext_csr_read(int csr_num, struct sbi_trap_regs *regs,
		      unsigned long *csr_val)
{
	struct hext_state *hext = &hart_hext_state[current_hartid()];

	if (!sbi_hext_enabled() || hext->virt)
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

	if (!sbi_hext_enabled() || hext->virt)
		return SBI_ENOTSUPP;

	switch (csr_num) {
	case CSR_HSTATUS:
		csr_val = (csr_val & HSTATUS_WRITABLE) |
			  (hext->hstatus & ~HSTATUS_WRITABLE);

		// TODO: hstatus.SPV = 0 requires mstatus.TSR
		hext->hstatus = csr_val;

		return SBI_OK;

	case CSR_HGATP:
		// VMIDLEN = 0
		csr_val &= ~HGATP32_VMID_MASK;

		unsigned long mode = csr_val >> HGATP_MODE_SHIFT;
		unsigned long ppn  = csr_val & HGATP_PPN;

		if ((mode == HGATP_MODE_OFF && ppn == 0) ||
		    (mode == HGATP_MODE_SV39X4)) {
			hext->hgatp = csr_val;
		} else {
			// Unsupported mode, do nothing
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
