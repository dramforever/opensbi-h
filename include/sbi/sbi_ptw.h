/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __SBI_PTW_H__
#define __SBI_PTW_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_trap.h>

typedef unsigned long sbi_pte_t;
typedef unsigned long long sbi_addr_t;

struct sbi_ptw_csr {
	unsigned long vsatp;
	unsigned long hgatp;
};

typedef sbi_pte_t (*sbi_load_pte_func)(sbi_addr_t addr,
				       const struct sbi_ptw_csr *csr,
				       struct sbi_trap_info *trap);

struct sbi_ptw_out {
	sbi_addr_t base;
	sbi_addr_t len;
	sbi_pte_t leaf_pte;
};

int sbi_ptw_translate(sbi_addr_t gva, const struct sbi_ptw_csr *csr,
		      struct sbi_ptw_out *out, struct sbi_trap_info *trap);

#endif
