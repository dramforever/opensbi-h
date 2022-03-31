#ifndef __SBI_PAGE_FAULT_H__
#define __SBI_PAGE_FAULT_H__

#include <sbi/sbi_types.h>

struct sbi_trap_regs;

extern bool errata_cip_453;

int sbi_page_fault_handler(ulong tval, ulong cause, struct sbi_trap_regs *regs);

#endif
