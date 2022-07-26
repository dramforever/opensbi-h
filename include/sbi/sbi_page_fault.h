#ifndef __SBI_PAGE_FAULT_H__
#define __SBI_PAGE_FAULT_H__

#include <sbi/sbi_types.h>

struct sbi_trap_regs;

int sbi_page_fault_handler(ulong tval, struct sbi_trap_regs *regs);

#endif
