#include <sbi/sbi_ptw.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_unpriv.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_domain.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_asm.h>

struct sbi_ptw_mode {
	sbi_load_pte_func load_pte;
	bool addr_signed;
	char parts[8];
};

static sbi_pte_t sbi_load_pte_pa(sbi_addr_t addr, const struct sbi_ptw_csr *csr,
				 struct sbi_trap_info *trap);

static sbi_pte_t sbi_load_pte_gpa(sbi_addr_t addr,
				  const struct sbi_ptw_csr *csr,
				  struct sbi_trap_info *trap);

static struct sbi_ptw_mode sbi_ptw_sv39x4 = { .load_pte	   = sbi_load_pte_pa,
					      .addr_signed = false,
					      .parts = { 12, 9, 9, 11, 0 } };

static struct sbi_ptw_mode sbi_ptw_sv39 = { .load_pte	 = sbi_load_pte_gpa,
					    .addr_signed = true,
					    .parts	 = { 12, 9, 9, 9, 0 } };

static sbi_pte_t sbi_load_ulong_pa(sbi_addr_t addr, struct sbi_trap_info *trap)
{
	register ulong tinfo asm("a3");

	register ulong mtvec = sbi_hart_expected_trap_addr();
	sbi_pte_t ret	     = 0;
	trap->cause	     = 0;

	asm volatile(
		/* clang-format off */
		"add %[tinfo], %[taddr], zero\n"
		"csrrw %[mtvec], " STR(CSR_MTVEC) ", %[mtvec]\n"
		".option push\n"
		".option norvc\n"
		REG_L " %[ret], %[addr]\n"
		".option pop\n"
		"csrw " STR(CSR_MTVEC) ", %[mtvec]"
		/* clang-format on */
		: [mtvec] "+&r"(mtvec), [tinfo] "+&r"(tinfo), [ret] "=&r"(ret)
		: [addr] "m"(*(sbi_pte_t *)addr), [taddr] "r"((ulong)trap)
		: "a4", "memory");
	return ret;
}

static sbi_pte_t sbi_load_pte_pa(sbi_addr_t addr, const struct sbi_ptw_csr *csr,
				 struct sbi_trap_info *trap)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();

	if (!sbi_domain_check_addr(dom, addr, PRV_S, SBI_DOMAIN_READ)) {
		/* This load would fail a PMP check */
		trap->cause = CAUSE_LOAD_ACCESS;
		trap->tval  = 0;
		trap->tval2 = 0;
		trap->tinst = 0;
		return 0;
	}

	return sbi_load_ulong_pa(addr, trap);
}

static sbi_pte_t sbi_load_pte_gpa(sbi_addr_t addr,
				  const struct sbi_ptw_csr *csr,
				  struct sbi_trap_info *trap)
{
	trap->cause = 0;
	return 0;
}

static inline bool addr_valid(sbi_addr_t addr, const struct sbi_ptw_mode *mode,
			      int va_bits)
{
	if (mode->addr_signed) {
		int64_t a = ((int64_t)addr) >> (va_bits - 1);
		return a == 0 || a == -1;
	} else {
		return (addr >> va_bits) == 0;
	}
}

static int sbi_pt_walk(sbi_addr_t addr, sbi_addr_t pt_root,
		       const struct sbi_ptw_csr *csr,
		       const struct sbi_ptw_mode *mode, struct sbi_ptw_out *out,
		       struct sbi_trap_info *trap)
{
	int num_levels = 0, va_bits = 0;
	int level, shift;
	sbi_addr_t node, addr_part, mask, ppn;
	sbi_pte_t pte;

	while (mode->parts[num_levels]) {
		va_bits += mode->parts[num_levels];
		num_levels++;
	}

	if (!addr_valid(addr, mode, va_bits)) {
		goto invalid;
	}

	shift = va_bits;
	node  = pt_root;

	for (level = num_levels - 1; level >= 1; level--) {
		shift -= mode->parts[level];
		mask	  = (1UL << mode->parts[level]) - 1;
		addr_part = (addr >> shift) & mask;

		sbi_printf("%s: level %d load pte 0x%lx\n", __func__, level,
			   node + addr_part * sizeof(sbi_pte_t));

		pte = mode->load_pte(node + addr_part * sizeof(sbi_pte_t), csr,
				     trap);

		if (trap->cause) {
			sbi_printf("%s: load pte failed %ld\n", __func__,
				   trap->cause);
			return SBI_EINVAL;
		}

		sbi_printf("%s: pte is %016lx\n", __func__, pte);

		if ((pte & 1) != 1) {
			sbi_printf("%s: pte not valid\n", __func__);
			goto invalid;
		}

		ppn = ((pte >> PTE_PPN_SHIFT) & PTE_PPN_MASK);

		if (level != 1 && (pte & (PTE_R | PTE_W | PTE_X))) {
			sbi_printf(
				"%s: leaf pte ppn 0x%lx (pa 0x%lx) at level %d, shift = %d, va_bits = %d\n",
				__func__, ppn, ppn << PAGE_SHIFT, level, shift,
				va_bits);

			if (ppn & ((1 << (shift - PAGE_SHIFT)) - 1))
				goto invalid;

			out->base     = ppn << PAGE_SHIFT;
			out->len      = 1UL << shift;
			out->leaf_pte = pte;

			return SBI_OK;
		}

		node = ppn << PAGE_SHIFT;
	}

	sbi_printf("%s: no leaf found\n", __func__);

invalid:
	trap->cause = CAUSE_LOAD_PAGE_FAULT;
	trap->tinst = 0;
	trap->tval  = 0;
	trap->tval2 = 0;

	return SBI_EINVAL;
}

static ulong convert_pf_to_gpf(ulong cause)
{
	switch (cause) {
	case CAUSE_LOAD_PAGE_FAULT:
		return CAUSE_LOAD_GUEST_PAGE_FAULT;
	case CAUSE_STORE_PAGE_FAULT:
		return CAUSE_STORE_GUEST_PAGE_FAULT;
	case CAUSE_FETCH_PAGE_FAULT:
		return CAUSE_FETCH_GUEST_PAGE_FAULT;
	default:
		return cause;
	}
}

int sbi_ptw_translate(sbi_addr_t gva, const struct sbi_ptw_csr *csr,
		      struct sbi_ptw_out *out, struct sbi_trap_info *trap)
{
	int ret;
	sbi_addr_t gpa, pa;

	if (csr->vsatp >> SATP_MODE_SHIFT != SATP_MODE_OFF) {
		sbi_panic("not bare");
	}

	gpa = gva;

	if (csr->hgatp >> HGATP_MODE_SHIFT != HGATP_MODE_SV39X4) {
		sbi_panic("not sv39x4");
	}

	ret = sbi_pt_walk(gpa, (csr->hgatp & HGATP_PPN) << PAGE_SHIFT, csr,
			  &sbi_ptw_sv39x4, out, trap);

	if (ret) {
		trap->cause = convert_pf_to_gpf(trap->cause);
		return ret;
	}

	pa = out->base + (gpa & (out->len - 1));

	sbi_printf("%s: gpa 0x%lx -> pa 0x%lx\n", __func__, gpa, pa);
	sbi_panic("todo");
}
