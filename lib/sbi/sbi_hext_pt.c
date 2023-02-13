/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sbi/sbi_error.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_string.h>
#include <sbi/riscv_locks.h>
#include <sbi/riscv_asm.h>

int sbi_hext_pt_init(unsigned long pt_start, unsigned long nodes_per_hart)
{
	u32 hart_count;

	hart_count = sbi_platform_hart_count(sbi_platform_thishart_ptr());

	for (u32 index = 0; index < hart_count; index++) {
		struct hext_state *hext = &hart_hext_state[index];

		if (!hext->available)
			continue;

		struct pt_area_info *pt_area = &hext->pt_area;

		pt_area->pt_start =
			pt_start + index * nodes_per_hart * PT_NODE_SIZE;

		pt_area->alloc_top = pt_area->pt_start + PT_NODE_SIZE;

		pt_area->alloc_limit =
			pt_area->pt_start + nodes_per_hart * PT_NODE_SIZE;

		pt_area->free_list = (unsigned long)-1;

		sbi_memset((void *)pt_area->pt_start, 0, PT_NODE_SIZE);
	}

	return SBI_OK;
}

/**
 * Allocate page table nodes from a shadow page table area
 *
 * Allocating page table nodes can invalidate previously allocated nodes. The
 * expected use is to allocate the maximum number of nodes before inserting page
 * table entries, and return unused ones after.
 *
 * This function cannot fail.
 *
 * @param pt_area Shadow page table area
 * @param num Number of nodes to allocate
 * @param addrs Output array of physical addresses of each node
 */
void sbi_hext_pt_alloc(struct pt_area_info *pt_area, size_t num,
		       unsigned long *addrs)
{
	unsigned long addr;

	for (size_t i = 0; i < num; i++) {
		if (pt_area->free_list != (unsigned long)-1) {
			addr		   = pt_area->free_list;
			pt_area->free_list = *(unsigned long *)addr;
		} else if (pt_area->alloc_top < pt_area->alloc_limit) {
			addr = pt_area->alloc_top;
			pt_area->alloc_top += PT_NODE_SIZE;
		} else {
			sbi_printf("%s: Running out of PT nodes, flushing\n",
				   __func__);
			sbi_hext_pt_flush_all(pt_area);
			return sbi_hext_pt_alloc(pt_area, num, addrs);
		}

		addrs[i] = addr;
	}

	for (size_t i = 0; i < num; i++)
		sbi_memset((void *)addrs[i], 0, PT_NODE_SIZE);
}

/**
 * Deallocate page table nodes back to a shadow page table area
 *
 * This function cannot fail.
 *
 * @param pt_area Shadow page table area
 * @param num Number of nodes to deallocate
 * @param addrs Array of physical addresses of each node
 */
void sbi_hext_pt_dealloc(struct pt_area_info *pt_area, size_t num,
			 const unsigned long *addrs)
{
	for (size_t i = 0; i < num; i++) {
		*(unsigned long *)addrs[i] = pt_area->free_list;
		pt_area->free_list	   = addrs[i];
	}
}

/**
 * Invalidate and deallocate all nodes in a shadow page table area, and flush
 * translation caches.
 *
 * This function cannot fail.
 *
 * @param pt_area Shadow page table area
 */
void sbi_hext_pt_flush_all(struct pt_area_info *pt_area)
{
	pt_area->alloc_top = pt_area->pt_start + PT_NODE_SIZE;
	pt_area->free_list = (unsigned long)-1;
	sbi_memset((void *)pt_area->pt_start, 0, PT_NODE_SIZE);
	asm volatile("sfence.vma" ::: "memory");
}
