/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __SBI_HEXT_H__
#define __SBI_HEXT_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_trap.h>

#define SHADOW_PT_SPACE_SIZE (2UL << 20)

extern unsigned long hext_shadow_pt_start;
extern unsigned long hext_shadow_pt_size;

struct hext_state {
	unsigned long hgatp;
	// TODO
};

extern struct hext_state hart_hext_state[];

int sbi_hext_init(struct sbi_scratch *scratch, bool cold_boot);

int sbi_hext_csr_read(int csr_num, struct sbi_trap_regs *regs,
		      unsigned long *csr_val);
int sbi_hext_csr_write(int csr_num, struct sbi_trap_regs *regs,
		       unsigned long csr_val);
int sbi_hext_insn(ulong insn, struct sbi_trap_regs *regs);

inline bool sbi_hext_enabled()
{
	return hext_shadow_pt_start != 0;
}

#endif
