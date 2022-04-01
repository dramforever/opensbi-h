/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_encoding.h>

int sbi_hext_insn(ulong insn, struct sbi_trap_regs *regs) {
	sbi_printf("%s: Instruction 0x%08lx: Not implemented\n", __func__, insn);
	return SBI_ENOTSUPP;
}
