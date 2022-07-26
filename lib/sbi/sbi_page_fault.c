#include <sbi/sbi_page_fault.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_ptw.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>

static inline ulong convert_access_type(ulong cause, ulong orig_cause)
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

int sbi_page_fault_handler(ulong tval, ulong cause, struct sbi_trap_regs *regs)
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
		trap.cause = convert_access_type(trap.cause, cause);
		return sbi_trap_redirect(regs, &trap);
	}

	return SBI_OK;
}
