#include <sbi/sbi_page_fault.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_ptw.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>

int sbi_page_fault_handler(ulong tval, struct sbi_trap_regs *regs)
{
	int ret;
	struct hext_state *hext = sbi_hext_current_state();
	struct sbi_ptw_csr csr = { .hgatp = hext->hgatp, .vsatp = hext->vsatp };
	struct sbi_ptw_out out;
	struct sbi_trap_info trap;
	sbi_printf("%s: page fault 0x%lx\n", __func__, tval);
	ret = sbi_ptw_translate(tval, &csr, &out, &trap);
	if (ret) {
		sbi_printf("%s: redirecting page fault\n", __func__);
		return sbi_trap_redirect(regs, &trap);
	}

	return SBI_OK;
}
