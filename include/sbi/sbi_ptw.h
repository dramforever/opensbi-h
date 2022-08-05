/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __SBI_PTW_H__
#define __SBI_PTW_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_hext.h>

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
	sbi_pte_t prot;
};

int sbi_ptw_translate(sbi_addr_t gva, const struct sbi_ptw_csr *csr,
		      struct sbi_ptw_out *out, struct sbi_trap_info *trap);
void sbi_pt_map(sbi_addr_t va, const struct sbi_ptw_out *out,
		struct pt_area_info *pt_area);

static inline ulong sbi_convert_access_type(ulong cause, ulong orig_cause)
{
	switch (cause) {

#define access_type_case(ty)                     \
	case CAUSE_FETCH_##ty:                   \
	case CAUSE_LOAD_##ty:                    \
	case CAUSE_STORE_##ty:                   \
		switch (orig_cause) {            \
		case CAUSE_LOAD_PAGE_FAULT:      \
			return CAUSE_LOAD_##ty;  \
		case CAUSE_STORE_PAGE_FAULT:     \
			return CAUSE_STORE_##ty; \
		case CAUSE_FETCH_PAGE_FAULT:     \
			return CAUSE_FETCH_##ty; \
		default:                         \
			return cause;            \
		}

		access_type_case(ACCESS);
		access_type_case(PAGE_FAULT);
		access_type_case(GUEST_PAGE_FAULT);

	default:
		return cause;

#undef access_type_case
	}
}
#endif
