/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_ptw.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_types.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_unpriv.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_bitops.h>
#include <sbi/riscv_encoding.h>

static u8 sbi_hyp_load_u8(sbi_addr_t gva, const struct sbi_ptw_csr *csr,
			  sbi_pte_t access, struct sbi_trap_info *trap)
{
	struct hext_state *hext = sbi_hext_current_state();
	bool u_mode		= !(hext->hstatus & HSTATUS_SPVP);
	bool sum		= (hext->sstatus & SSTATUS_SUM) != 0;

	int ret;
	u8 result;
	unsigned long mstatus, gpa, pa;
	struct sbi_ptw_out vsout, gout;

	ret = sbi_ptw_translate(gva, csr, &vsout, &gout, trap);

	if (ret) {
		trap->cause = sbi_convert_access_type(trap->cause,
						      CAUSE_LOAD_PAGE_FAULT);
		return 0;
	}

	gpa = vsout.base | (gva & (vsout.len - 1));
	pa  = gout.base | (gpa & (gout.len - 1));

	if (sbi_ptw_check_access(&vsout, &gout, access, u_mode, sum, trap)) {
		trap->tval  = gva;
		trap->tval2 = gpa >> 2;
		trap->tinst = 0;
		return 0;
	}

	mstatus = csr_read_set(CSR_MSTATUS, MSTATUS_MPP);
	result	= sbi_load_u8((u8 *)pa, trap);
	csr_write(CSR_MSTATUS, mstatus);

	return result;
}

static int sbi_hyp_mem(unsigned long insn, const struct sbi_ptw_csr *csr,
		       struct sbi_trap_regs *regs)
{
	sbi_pte_t access;
	unsigned long data = 0;
	int i, shift;
	int sign, len;
	u8 res;
	sbi_addr_t gva;
	struct sbi_trap_info trap;

	if ((insn & INSN_MASK_HLVX_HU) == INSN_MATCH_HLVX_HU) {
		sign   = 0;
		len    = 2;
		access = PTE_X;
	} else {
		sbi_panic("%s: Unimplemented hypervisor load/store %08lx\n",
			  __func__, insn);
	}

	gva = GET_RS1(insn, regs);

	// sbi_printf("%s: Hypervisor load store, gva = 0x%llx\n", __func__, gva);

	for (i = 0; i < len; i++) {
		res = sbi_hyp_load_u8(gva + i, csr, access, &trap);
		if (trap.cause) {
			trap.epc = regs->mepc;
			return sbi_trap_redirect(regs, &trap);
		}
		data = data | (res << (i * 8));
	}

	if (sign) {
		shift = 8 * (sizeof(ulong) - len);
		data  = ((long)data << shift) >> shift;
	}

	SET_RD(insn, regs, data);
	regs->mepc += 4;
	return SBI_OK;
}

int sbi_hext_insn(unsigned long insn, struct sbi_trap_regs *regs)
{
	struct hext_state *hext = sbi_hext_current_state();
	struct sbi_ptw_csr csr = { .hgatp = hext->hgatp, .vsatp = hext->vsatp };
	struct sbi_trap_info trap;
	unsigned long mpp = (regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
	unsigned long funct3	= GET_RM(insn);
	unsigned long prv	= (insn >> 28) & 0x3;

	if (!sbi_hext_enabled())
		return SBI_ENOTSUPP;

	if (prv == 0x2) {
		/* Hypervisor-level instruction */
		if (hext->virt)
			return SBI_ENOTSUPP;

		switch (funct3) {
		case 0b000:
			if (mpp < PRV_S) {
				return SBI_EDENIED;
			}

			if ((insn & INSN_MASK_HFENCE_GVMA) ==
				    INSN_MATCH_HFENCE_GVMA ||
			    (insn & INSN_MASK_HFENCE_VVMA) ==
				    INSN_MATCH_HFENCE_VVMA) {
				/* Conservatively flush everything */
				sbi_hext_pt_flush_all(&hext->pt_area);

				regs->mepc += 4;
				return SBI_OK;
			} else {
				return SBI_ENOTSUPP;
			}

		case 0b100:
			if (mpp < PRV_S && !(hext->hstatus & HSTATUS_HU)) {
				return SBI_EDENIED;
			}

			return sbi_hyp_mem(insn, &csr, regs);

		default:
			return SBI_ENOTSUPP;
		}
	} else if (prv == PRV_S) {
		/* Supervisor-level instruction */

		if ((insn & INSN_MASK_WFI) == INSN_MATCH_WFI) {
			trap.cause = CAUSE_VIRTUAL_INST_FAULT;
			trap.epc   = regs->mepc;
			trap.tval  = insn;
			trap.tval2 = 0;
			trap.tinst = 0;
			return sbi_trap_redirect(regs, &trap);
		} else if ((insn & INSN_MASK_SRET) == INSN_MATCH_SRET) {
			if (hext->virt || !(hext->hstatus & HSTATUS_SPV)) {
				sbi_panic("%s: Unexpected trapped sret",
					  __func__);
			}

			sbi_hext_switch_virt(regs, hext, true);
			regs->mepc = hext->sepc;
			return SBI_OK;
		} else if ((insn & INSN_MASK_SFENCE_VMA) ==
				   INSN_MATCH_SFENCE_VMA ||
			   (insn & INSN_MASK_SINVAL_VMA) ==
				   INSN_MATCH_SINVAL_VMA) {
			if (!hext->virt)
				return SBI_ENOTSUPP;

			sbi_hext_pt_flush_all(&hext->pt_area);
			regs->mepc += 4;
			return SBI_OK;
		} else {
			return SBI_ENOTSUPP;
		}
	} else {
		return SBI_ENOTSUPP;
	}
}
