#include <sbi/sbi_ptw.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_unpriv.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_domain.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_asm.h>

/* We don't use the G bit yet */
#define PROT_ALL (PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_U)

const char prot_names[] = "vrwxugad";

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

static sbi_pte_t sbi_load_pte_pa(sbi_addr_t addr, const struct sbi_ptw_csr *csr,
				 struct sbi_trap_info *trap)
{
	sbi_pte_t res;
	unsigned long mstatus;
	struct sbi_domain *dom = sbi_domain_thishart_ptr();

	if (!sbi_domain_check_addr(dom, addr, PRV_S, SBI_DOMAIN_READ)) {
		/* This load would fail a PMP check */
		trap->cause = CAUSE_LOAD_ACCESS;
		trap->tval  = 0;
		trap->tval2 = 0;
		trap->tinst = 0;
		return 0;
	}

	mstatus = csr_read_set(CSR_MSTATUS, MSTATUS_MPP);
	res	= sbi_load_ulong((unsigned long *)addr, trap);
	csr_write(CSR_MSTATUS, mstatus);
	return res;
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

		pte = mode->load_pte(node + addr_part * sizeof(sbi_pte_t), csr,
				     trap);

		if (trap->cause) {
			sbi_printf("%s: load pte failed %ld\n", __func__,
				   trap->cause);
			return SBI_EINVAL;
		}

		if ((pte & 1) != 1) {
			sbi_printf("%s: pte not valid\n", __func__);
			goto invalid;
		}

		ppn = ((pte >> PTE_PPN_SHIFT) & PTE_PPN_MASK);

		if (level != 1 && (pte & (PTE_R | PTE_W | PTE_X))) {
			if (ppn & ((1 << (shift - PAGE_SHIFT)) - 1))
				goto invalid;

#if __riscv_xlen == 64
			if ((pte >> PTE64_RESERVED_SHIFT) != 0)
				goto invalid;
#endif

			out->base     = ppn << PAGE_SHIFT;
			out->len      = 1UL << shift;
			out->prot     = pte;

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

static sbi_pte_t prot_translate(sbi_pte_t vsprot, sbi_pte_t gprot)
{
	sbi_pte_t prot = vsprot & (gprot & ~PTE_U) & PROT_ALL;

	if (!(gprot & PTE_U) || !(prot & PTE_A))
		return 0;

	if (!(prot & PTE_D))
		prot &= ~PTE_W;

	return prot | PTE_V;
}

void sbi_pt_map(sbi_addr_t va, const struct sbi_ptw_out *out,
		struct pt_area_info *pt_area)
{
	const struct sbi_ptw_mode *mode = &sbi_ptw_sv39;

	/* FIXME: Code duplication */

	int num_levels = 0, va_bits = 0;
	int level, shift, alloc_used = 0;
	sbi_addr_t addr_part, mask;
	sbi_pte_t *pte;
	unsigned long alloc[4];
	unsigned long node = pt_area->pt_start, new_node;

	while (mode->parts[num_levels]) {
		va_bits += mode->parts[num_levels];
		num_levels++;
	}

	shift = va_bits;

	if (!(out->len == (1 << PAGE_SHIFT)))
		sbi_panic("%s: Unhandled huge page size\n", __func__);

	sbi_hext_pt_alloc(pt_area, num_levels - 1, alloc);

	for (level = num_levels - 1; level >= 1; level--) {
		shift -= mode->parts[level];
		mask	  = (1UL << mode->parts[level]) - 1;
		addr_part = (va >> shift) & mask;

		pte = (sbi_pte_t *)(node + addr_part * sizeof(sbi_pte_t));

		if (level > 1) {
			if (!(*pte & PTE_V)) {
				new_node = alloc[alloc_used++];
				*pte = PTE_V | ((new_node >> PAGE_SHIFT)
						<< PTE_PPN_SHIFT);
			}

			node = ((*pte >> PTE_PPN_SHIFT) & PTE_PPN_MASK)
			       << PAGE_SHIFT;
		} else {
			*pte = out->prot |
			       ((out->base >> PAGE_SHIFT) << PTE_PPN_SHIFT);
		}
	}

	sbi_hext_pt_dealloc(pt_area, num_levels - 1 - alloc_used,
			    alloc + alloc_used);
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
	struct sbi_ptw_out gout;

	if (csr->vsatp >> SATP_MODE_SHIFT != SATP_MODE_OFF) {
		sbi_panic("not bare");
	}

	gpa = gva;

	if (csr->hgatp >> HGATP_MODE_SHIFT != HGATP_MODE_SV39X4) {
		sbi_panic("not sv39x4");
	}

	ret = sbi_pt_walk(gpa, (csr->hgatp & HGATP_PPN) << PAGE_SHIFT, csr,
			  &sbi_ptw_sv39x4, &gout, trap);

	if (ret) {
		sbi_printf("%s: Guest-page fault\n", __func__);
		trap->tval  = gva;
		trap->tval2 = gpa >> 2;
		trap->cause = convert_pf_to_gpf(trap->cause);
		return ret;
	}

	out->base = gout.base;
	out->len  = gout.len;
	out->prot = prot_translate(PROT_ALL, gout.prot);

	pa = out->base + (gpa & (out->len - 1));
	sbi_printf("%s: gpa 0x%llx -> pa 0x%llx prot ", __func__, gpa, pa);

	for (int i = 0; i < 8; i++) {
		if ((out->prot >> i) & 1)
			sbi_printf("%c", prot_names[i]);
		else
			sbi_printf("-");
	}

	sbi_printf("\n");

	/* We only handle base-sized pages for now */
	out->base = pa & PAGE_MASK;
	out->len  = 1 << PAGE_SHIFT;

	return SBI_OK;
}
