#include <sbi/sbi_ptw.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_unpriv.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_domain.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_asm.h>

const char prot_names[] = "vrwxugad";

struct sbi_ptw_mode {
	/** Function to load PTE based on address */
	sbi_load_pte_func load_pte;

	/** Is the address sign-extended? */
	bool addr_signed;

	/**
	 * Number of address bits in each segment, starting LSB first, with the
	 * number of bits in a page offset, then number of bits for each level,
	 * then a terminating 0.
	 */
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

static int sbi_pt_walk(sbi_addr_t addr, sbi_addr_t pt_root,
		       const struct sbi_ptw_csr *csr,
		       const struct sbi_ptw_mode *mode, struct sbi_ptw_out *out,
		       struct sbi_trap_info *trap);

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
	struct sbi_ptw_mode *mode = &sbi_ptw_sv39x4;
	unsigned long pt_root, pa = -1, mstatus;
	struct sbi_ptw_out out;
	int ret;
	sbi_pte_t res = 0x3000;

	if ((csr->hgatp >> HGATP_MODE_SHIFT) != HGATP_MODE_SV39X4)
		sbi_panic("%s: Not Sv39x4 mode\n", __func__);

	pt_root = (csr->hgatp & HGATP_PPN) << PAGE_SHIFT;

	ret = sbi_pt_walk(addr, pt_root, csr, mode, &out, trap);

	if (ret) {
		trap->cause = convert_pf_to_gpf(trap->cause);
		goto trap;
	}

	pa = (out.base & ~(out.len - 1)) | (addr & (out.len - 1));

	mstatus = csr_read_set(CSR_MSTATUS, MSTATUS_MPP);
	res	= sbi_load_ulong((unsigned long *)pa, trap);
	csr_write(CSR_MSTATUS, mstatus);

trap:
	if (trap->cause) {
		trap->tval2 = addr;
		trap->tinst = INSN_PSEUDO_VS_LOAD;
	}

	// sbi_printf("%s: 0x%llx -> 0x%lx: 0x%016lx\n", __func__, addr, pa, res);

	return res;
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

/**
 * Perform a page table based virtual address translation.
 *
 * Returned trap cause is 'load page fault' for all page table related faults.
 * Caller should convert it to the original access type, and possibly convert
 * page faults to guest-page faults.
 *
 * @param addr Address to translate
 * @param pt_root Root node address of the page table
 * @param csr Relevant CSR state for this translation
 * @param mode Mode to use for this translation
 * @param out Physical address region info for successful translation
 * @param trap Trap info for unsuccessful translation
 * @return Zero if successful, non-zero if unsuccessful
 */
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

		if ((pte & PTE_V) != 1) {
			goto invalid;
		}

		ppn = ((pte >> PTE_PPN_SHIFT) & PTE_PPN_MASK);

#if __riscv_xlen == 64
		if ((pte >> PTE64_RESERVED_SHIFT) != 0)
			goto invalid;
#endif

		if (pte & (PTE_R | PTE_W | PTE_X)) {
			if (ppn & ((1 << (shift - PAGE_SHIFT)) - 1))
				goto invalid;

			out->base = ppn << PAGE_SHIFT;
			out->len  = 1UL << shift;
			out->prot = pte;

			return SBI_OK;
		} else {
			/* D, A, U bits are reserved for non-leaf PTEs */
			if (pte & (PTE_A | PTE_D | PTE_U))
				goto invalid;
			node = ppn << PAGE_SHIFT;
		}
	}

	sbi_printf("%s: no leaf found\n", __func__);

invalid:
	trap->cause = CAUSE_LOAD_PAGE_FAULT;
	trap->tinst = 0;
	trap->tval  = 0;
	trap->tval2 = 0;

	return SBI_EINVAL;
}

/**
 * Map a page into shadow page table.
 *
 * This function cannot fail.
 *
 * FIXME: Handle non-Sv39.
 *
 * @param va Virtual address of the mapping
 * @param out Description of physical address region
 * @param pt_area Shadow page table region
 */
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
				*pte	 = PTE_V | ((new_node >> PAGE_SHIFT)
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

/**
 * Translate a guest virtual address based on vsatp and hgatp.
 *
 * Returned trap cause may have the wrong access type. Caller should convert it
 * to the original access type.
 *
 * @param gva Guest virtual address to translate
 * @param csr Relevant CSR state for this translation
 * @param out Physical address region info for successful translation
 * @param trap Trap info for unsuccessful translation
 * @return Zero if successful, non-zero if unsuccessful
 */
int sbi_ptw_translate(sbi_addr_t gva, const struct sbi_ptw_csr *csr,
		      struct sbi_ptw_out *vsout, struct sbi_ptw_out *gout,
		      struct sbi_trap_info *trap)
{
	int ret = 0;
	sbi_addr_t gpa;

	if (csr->hgatp >> HGATP_MODE_SHIFT != HGATP_MODE_SV39X4) {
		sbi_panic("%s: Unsupported hgatp mode\n", __func__);
	}

	if (csr->vsatp >> SATP_MODE_SHIFT == SATP_MODE_OFF) {
		vsout->prot = PROT_ALL & ~PTE_U;
		vsout->base = gva & PAGE_MASK;
		vsout->len  = 1 << PAGE_SHIFT;
		gpa	    = gva;
	} else if (csr->vsatp >> SATP_MODE_SHIFT == SATP_MODE_SV39) {
		ret = sbi_pt_walk(gva, (csr->vsatp & SATP_PPN) << PAGE_SHIFT,
				  csr, &sbi_ptw_sv39, vsout, trap);

		if (ret) {
			trap->tval = gva;
			return ret;
		}

	} else {
		sbi_panic("%s: Unsupported vsatp mode\n", __func__);
	}

	gpa = vsout->base + (gva & (vsout->len - 1));
	ret = sbi_pt_walk(gpa, (csr->hgatp & HGATP_PPN) << PAGE_SHIFT, csr,
			  &sbi_ptw_sv39x4, gout, trap);

	if (ret) {
		// sbi_printf("%s: Guest-page fault\n", __func__);
		trap->tval  = gva;
		trap->tval2 = gpa >> 2;
		trap->tinst = 0;
		trap->cause = convert_pf_to_gpf(trap->cause);
		return ret;
	}

	return SBI_OK;
}

static inline sbi_pte_t convert_access_dirty(sbi_pte_t pte)
{
	sbi_pte_t res = pte & (PTE_R | PTE_W | PTE_X);

	if (!(pte & PTE_A))
		return 0;

	if (!(pte & PTE_D))
		res &= ~PTE_W;

	return res;
}

int sbi_ptw_check_access(const struct sbi_ptw_out *vsout,
			 const struct sbi_ptw_out *gout, sbi_pte_t access,
			 bool u_mode, bool sum, struct sbi_trap_info *trap)
{
	struct hext_state *hext = sbi_hext_current_state();
	bool vsbare = (hext->vsatp >> SATP_MODE_SHIFT) == SATP_MODE_OFF;
	bool pte_u  = (vsout->prot & PTE_U) != 0;

	trap->cause = 0;

	if (!(gout->prot & PTE_U) ||
	    !(convert_access_dirty(gout->prot) & access)) {
		trap->cause = CAUSE_LOAD_GUEST_PAGE_FAULT;
		return SBI_EINVAL;
	}

	if (!vsbare &&
	    ((u_mode != pte_u && (u_mode || access == PTE_X || !sum)) ||
	     !(convert_access_dirty(vsout->prot) & access))) {
		trap->cause = CAUSE_LOAD_PAGE_FAULT;
		return SBI_EINVAL;
	}

	return 0;
}
