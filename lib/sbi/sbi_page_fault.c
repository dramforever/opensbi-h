#include <sbi/sbi_page_fault.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_ptw.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>

bool errata_cip_453 = 0;

static inline sbi_pte_t cause_to_access(unsigned long cause)
{
	switch (cause) {
	case CAUSE_LOAD_PAGE_FAULT:
		return PTE_R;
	case CAUSE_STORE_PAGE_FAULT:
		return PTE_W;
	case CAUSE_FETCH_PAGE_FAULT:
		return PTE_X;
	default:
		return 0;
	}
}

/**
 * Combine flags from VS-stage leaf PTE and G-stage leaf PTE.
 *
 * This function assumes software management of A and D bits.
 *
 * @param vsprot VS-stage leaf PTE. Only flag bits are considered.
 * @param gprot G-stage leaf PTE. Only flag bits are considered.
 * @return Combined flag bits from the entire translation process.
 */
static sbi_pte_t prot_translate(sbi_pte_t vsprot, sbi_pte_t gprot)
{
	sbi_pte_t prot =
		(vsprot & gprot & PROT_ALL & ~PTE_U) | (vsprot & PTE_U);

	if (!(gprot & PTE_U) || !(prot & PTE_A))
		return 0;

	if (!(prot & PTE_D))
		prot &= ~PTE_W;

	return prot | PTE_V;
}

int sbi_page_fault_handler(ulong tval, ulong cause, struct sbi_trap_regs *regs)
{
	int ret;
	struct hext_state *hext = sbi_hext_current_state();
	struct sbi_ptw_csr csr = { .hgatp = hext->hgatp, .vsatp = hext->vsatp };
	struct sbi_ptw_out vsout, gout, out;
	struct sbi_trap_info trap;
	bool u_mode =
		((regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT == PRV_U);
	bool sum = (regs->mstatus & MSTATUS_SUM) != 0;

	sbi_pte_t access = cause_to_access(cause);
	sbi_addr_t gpa, pa;

	if (errata_cip_453) {
		switch (cause) {
		case CAUSE_FETCH_PAGE_FAULT:
		case CAUSE_FETCH_ACCESS:
			tval = regs->mepc + ((tval ^ regs->mepc) & 2);
		}
	}

	// sbi_printf("%s: page fault 0x%lx cause %d at pc=0x%lx\n", __func__,
	// 	   tval, (int)cause, regs->mepc);
	ret = sbi_ptw_translate(tval, &csr, &vsout, &gout, &trap);

	if (ret) {
		trap.cause = sbi_convert_access_type(trap.cause, cause);
		goto trap;
	}

	gpa = vsout.base | (tval & (vsout.len - 1));
	pa  = gout.base | (gpa & (gout.len - 1));

	if (sbi_ptw_check_access(&vsout, &gout, access, u_mode, sum, &trap)) {
		trap.cause = sbi_convert_access_type(trap.cause, cause);
		trap.tval  = tval;
		trap.tval2 = gpa >> 2;
		trap.tinst = 0;
		goto trap;
	}

	out.base = pa & PAGE_MASK;
	out.len	 = 1 << PAGE_SHIFT;
	out.prot = prot_translate(vsout.prot, gout.prot);

	sbi_pt_map(tval, &out, &hext->pt_area);
	asm volatile("sfence.vma" ::: "memory");

	return SBI_OK;

trap:
	trap.epc = regs->mepc;
	return sbi_trap_redirect(regs, &trap);
}
