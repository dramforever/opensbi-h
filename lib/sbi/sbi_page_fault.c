#include <sbi/sbi_page_fault.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_ptw.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>

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

int sbi_page_fault_handler(ulong tval, ulong cause, struct sbi_trap_regs *regs)
{
	int ret;
	struct hext_state *hext = sbi_hext_current_state();
	struct sbi_ptw_csr csr = { .hgatp = hext->hgatp, .vsatp = hext->vsatp };
	struct sbi_ptw_out out;
	struct sbi_trap_info trap;
	sbi_pte_t access = cause_to_access(cause);
	bool u_mode =
		((regs->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT == PRV_U);
	bool sum = (regs->mstatus & MSTATUS_SUM) != 0;
	// bool vsbare = (csr.vsatp >> SATP_MODE_SHIFT) == SATP_MODE_OFF;

	bool pte_u;

	// sbi_printf("%s: page fault 0x%lx cause %d at pc=0x%lx\n", __func__,
	// 	   tval, (int)cause, regs->mepc);
	ret = sbi_ptw_translate(tval, &csr, &out, &trap);

	pte_u = (out.prot & PTE_U) != 0;

	if (ret) {
		trap.cause = sbi_convert_access_type(trap.cause, cause);
		goto trap;
	}

	if ((access & out.prot) != access ||
	    (u_mode != pte_u && (u_mode || access == PTE_X || !sum))) {
		// sbi_printf(
		// 	"%s: Access trap va %lx, access = %lx, prot = %lx, u_mode = %d, pte.U = %d, sum = %d\n",
		// 	__func__, tval, access, out.prot, u_mode, pte_u, sum);
		trap.cause = cause;
		trap.tval  = tval;
		trap.tval2 = 0;
		trap.tinst = 0;
		goto trap;
	}

	sbi_pt_map(tval, &out, &hext->pt_area);
	asm volatile("sfence.vma" ::: "memory");

	return SBI_OK;

trap:
	trap.epc = regs->mepc;
	return sbi_trap_redirect(regs, &trap);
}
