/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_encoding.h>

int sbi_hext_insn(unsigned long insn, struct sbi_trap_regs *regs)
{
	struct hext_state *hext = sbi_hext_current_state();
	unsigned long mpp = (regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;

	if (!sbi_hext_enabled() || hext->virt)
		return SBI_ENOTSUPP;

	unsigned long funct3 = GET_RM(insn);
	unsigned long prv    = (insn >> 28) & 0x3;

	if (prv == 0x2) {
		/* Hypervisor-level instruction */

		switch (funct3) {
		case 0b000:
			if (mpp < PRV_S) {
				return SBI_EDENIED;
			}

			/* sbi_printf("%s: 0x%08lx: TODO: hfence.*\n", __func__, insn); */
			regs->mepc += 4;
			return SBI_OK;

		case 0b100:
			if (mpp < PRV_S && !(hext->hstatus & HSTATUS_HU)) {
				return SBI_EDENIED;
			}

			sbi_printf("%s: 0x%08lx: TODO: Hypervisor load/store\n",
				   __func__, insn);
			return SBI_ENOTSUPP;

		default:
			return SBI_ENOTSUPP;
		}
	} else if (prv == PRV_S) {
		/* Supervisor-level instruction */

		if ((insn & INSN_MASK_WFI) == INSN_MATCH_WFI) {
			sbi_panic("%s: TODO: Trapped wfi\n", __func__);
		} else if ((insn & INSN_MASK_SRET) == INSN_MATCH_SRET) {
			if (hext->virt || !(hext->hstatus & HSTATUS_SPV)) {
				sbi_panic("%s: Unexpected trapped sret",
					  __func__);
			}

			sbi_hext_switch_virt(insn, regs, hext, TRUE);
			regs->mepc = hext->sepc;
			return SBI_OK;
		} else if ((insn & INSN_MASK_SFENCE_VMA) ==
				   INSN_MATCH_SFENCE_VMA ||
			   (insn & INSN_MASK_SINVAL_VMA) ==
				   INSN_MATCH_SINVAL_VMA) {
			sbi_panic("%s: TODO: Trapped sfence.vma/sinval.vma\n",
				  __func__);
		} else {
			return SBI_ENOTSUPP;
		}
	} else {
		return SBI_ENOTSUPP;
	}
}
