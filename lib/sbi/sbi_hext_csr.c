/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>

int sbi_hext_csr_read(int csr_num, struct sbi_trap_regs *regs,
		      unsigned long *csr_val)
{
	sbi_printf("%s: CSR read 0x%03x: Not implemented\n", __func__, csr_num);
	return SBI_ENOTSUPP;
}

int sbi_hext_csr_write(int csr_num, struct sbi_trap_regs *regs,
		       unsigned long csr_val)
{
	sbi_printf("%s: CSR write 0x%03x: Not implemented\n", __func__,
		   csr_num);
	return SBI_ENOTSUPP;
}
