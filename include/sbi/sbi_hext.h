/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __SBI_HEXT_H__
#define __SBI_HEXT_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_trap.h>

#define PT_NODE_SIZE (1UL << 12)
#define PT_ALIGN PT_NODE_SIZE
#define PT_SPACE_SIZE (4UL << 20)

typedef unsigned long sbi_pte_t;

struct pt_area_info {
	unsigned long pt_start;
	unsigned long alloc_top;
	unsigned long alloc_limit;
	unsigned long free_list;
};

struct hext_state {
	struct pt_area_info pt_area;

	unsigned long medeleg;

	/**
	 * HS-level CSRs
	 *
	 * These are only accessible in HS-mode.
	 */

	unsigned long hstatus;
	unsigned long htval;
	unsigned long htinst;
	unsigned long hedeleg;
	unsigned long hideleg;
	unsigned long hie;
	unsigned long hip;

	unsigned long hvip;

	unsigned long hgatp;

	/**
	 * Saved supervisor CSRs
	 *
	 * When emulating the hypervisor extension, we put the 'active' set of
	 * supervisor CSRs into real CSRs in hardware, while the 'inactive' set
	 * is saved here. To be more specific, given the current hart's
	 * emulation state:
	 *
	 *     struct hext_state *hext = ...;
	 *
	 * The supervisor CSR sfoo and vsfoo are handled thus:
	 *
	 * - When V = 0 (i.e., hext->virt == 1):
	 *   - HS-mode sfoo is the real CSR sfoo
	 *   - Accessing vsfoo is trapped and redirected to hext->sfoo
	 * - When V = 1:
	 *   - VS-mode sfoo is the real CSR sfoo
	 *   - HS-mode sfoo are saved in hext->sfoo
	 */

	unsigned long sstatus;
	unsigned long stvec;
	unsigned long sscratch;
	unsigned long sepc;
	unsigned long scause;
	unsigned long stval;
	unsigned long sie;
	unsigned long sip;

	/**
	 * - When V = 0:
	 *   - HS-mode satp is the real satp
	 *   - Accessing vsatp is trapped and redirected to hext->vsatp
	 * - When V = 1:
	 *   - HS-mode satp is saved in hext->satp
	 *   - If hgatp.MODE = Bare:
	 *     - VS-mode satp is the real satp
	 *   - If hgatp.MODE != Bare:
	 *     - Accessing satp is trapped and redirected to hext->vsatp
	 *     - Real satp points to a shadow page table
	 */
	unsigned long satp;
	unsigned long vsatp;

	bool virt;
	bool available;
};

extern unsigned long hext_mstatus_features;
extern struct hext_state hart_hext_state[];
extern unsigned long hext_pt_start;
extern unsigned long hext_pt_size;

int sbi_hext_init(struct sbi_scratch *scratch, bool cold_boot);

int sbi_hext_csr_read(int csr_num, struct sbi_trap_regs *regs,
		      unsigned long *csr_val);
int sbi_hext_csr_write(int csr_num, struct sbi_trap_regs *regs,
		       unsigned long csr_val);
int sbi_hext_insn(unsigned long insn, struct sbi_trap_regs *regs);

void sbi_hext_switch_virt(struct sbi_trap_regs *regs, struct hext_state *hext,
			  bool virt);

inline bool sbi_hext_enabled()
{
	return hext_pt_start != 0;
}

inline struct hext_state *sbi_hext_current_state()
{
	const struct sbi_platform *platform = sbi_platform_thishart_ptr();
	u32 index = sbi_platform_hart_index(platform, current_hartid());
	return &hart_hext_state[index];
}

int sbi_hext_pt_init(unsigned long pt_start, unsigned long nodes_per_hart);

void sbi_hext_pt_alloc(struct pt_area_info *pt_area, size_t num,
		       unsigned long *addrs);
void sbi_hext_pt_dealloc(struct pt_area_info *pt_area, size_t num,
			 const unsigned long *addrs);

void sbi_hext_pt_flush_all(struct pt_area_info *pt_area);

#endif
