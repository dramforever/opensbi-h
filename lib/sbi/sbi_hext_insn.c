/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_encoding.h>

int sbi_hext_insn(unsigned long insn, struct sbi_trap_regs *regs)
{
	struct hext_state *hext = &hart_hext_state[current_hartid()];

	if (!sbi_hext_enabled() || hext->virt)
		return SBI_ENOTSUPP;

	unsigned long funct3 = GET_RM(insn);

	switch (funct3) {
	case 0b000:
		sbi_printf("%s: 0x%08lx: TODO: hfence.*\n", __func__, insn);
		return SBI_OK;
	case 0b100:
		sbi_printf("%s: 0x%08lx: TODO: Hypervisor load/store\n",
			   __func__, insn);
		return SBI_ENOTSUPP;
	default:
		// Shouldn't be possible
		return SBI_ENOTSUPP;
	}
}
