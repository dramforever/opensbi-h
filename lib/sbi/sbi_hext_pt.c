/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_error.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_locks.h>
#include <sbi/riscv_asm.h>

int sbi_hext_pt_init(pte_t *pt_start, struct pt_meta *meta_start,
		     unsigned long nodes_per_hart)
{
	u32 hart_count;

	hart_count = sbi_platform_hart_count(sbi_platform_thishart_ptr());

	for (u32 index = 0; index < hart_count; index++) {
		struct pt_area_info *pt_area = &hart_hext_state[index].pt_area;

		pt_area->pt_start =
			pt_start +
			index * nodes_per_hart * (PT_NODE_SIZE / sizeof(pte_t));

		pt_area->meta_start = meta_start + (index * nodes_per_hart);

		pt_area->alloc_top   = PT_ROOT_SIZE / PT_NODE_SIZE;
		pt_area->alloc_limit = nodes_per_hart;
	}

	return SBI_OK;
}
