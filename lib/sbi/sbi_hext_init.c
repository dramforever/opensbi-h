/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libfdt.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_page_fault.h>

#include <sbi_utils/fdt/fdt_helper.h>

#define MSTATUS_TRY_FEATURES (MSTATUS_TVM | MSTATUS_TW | MSTATUS_TSR)
#define MSTATUS_NEED_FEATURES (MSTATUS_TVM | MSTATUS_TSR)

unsigned long hext_pt_start;
unsigned long hext_pt_size;

unsigned long hext_mstatus_features;

struct hext_state hart_hext_state[SBI_HARTMASK_MAX_BITS] = { 0 };

static int find_main_memory(void *fdt, unsigned long *addr, unsigned long *size)
{
	int i, node, len;
	const void *device_type;
	int address_cells, size_cells;
	int memory_node = -1;
	const fdt32_t *reg;
	uint64_t full_addr = 0, full_size = 0;

	address_cells = fdt_address_cells(fdt, 0);
	size_cells    = fdt_size_cells(fdt, 0);

	if (address_cells < 0 || size_cells < 0) {
		return SBI_EFAIL;
	}

	fdt_for_each_subnode(node, fdt, 0) {
		device_type = fdt_getprop(fdt, node, "device_type", &len);
		if (device_type &&
		    strncmp(device_type, "memory", strlen("memory")) == 0) {
			memory_node = node;
			break;
		}
	}

	if (node < 0 && node != -FDT_ERR_NOTFOUND) {
		return SBI_EFAIL;
	}

	if (memory_node < 0) {
		return SBI_ENODEV;
	}

	reg = fdt_getprop(fdt, memory_node, "reg", &len);

	if (len < address_cells + size_cells) {
		return SBI_EFAIL;
	}

	for (i = 0; i < address_cells; i++) {
		full_addr = (full_addr << 32) | fdt32_to_cpu(reg[i]);
	}

	for (i = address_cells; i < address_cells + size_cells; i++) {
		full_size = (full_size << 32) | fdt32_to_cpu(reg[i]);
	}

	if (addr) {
		*addr = (unsigned long)full_addr;
	}

	if (size) {
		*size = (unsigned long)full_size;
	}

	return SBI_OK;
}

int patch_fdt_cpu_isa(void *fdt)
{
	int err, cpu, cpus_offset, len;
	int total_new_strings = 0;
	const void *prop, *isa_string;
	void *new_isa_string;
	char *pos;
	int append_len;

	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0) {
		return SBI_ENODEV;
	}

	fdt_for_each_subnode(cpu, fdt, cpus_offset) {
		fdt_getprop(fdt, cpu, "riscv,isa", &len);
		total_new_strings += len + sizeof(struct fdt_property) + 2;
	}

	if (cpu < 0 && cpu != -FDT_ERR_NOTFOUND) {
		return SBI_EFAIL;
	}

	err = fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + total_new_strings);
	if (err < 0) {
		return SBI_ENOMEM;
	}

	fdt_for_each_subnode(cpu, fdt, cpus_offset) {
		prop = fdt_getprop(fdt, cpu, "device_type", &len);
		if (!prop || strncmp(prop, "cpu", strlen("cpu")) != 0) {
			continue;
		}

		isa_string = fdt_getprop(fdt, cpu, "riscv,isa", &len);

		/**
		 * If riscv,isa has no underscore:
		 * 	rv64imafdc -> rv64imafdch
		 * If riscv,isa has underscore:
		 * 	rv64imafdc_zicsr -> rv64imafdcu_zicsr_h
		 */

		pos = sbi_strchr(isa_string, '_');

		if (pos)
			append_len = 2;
		else
			append_len = 1;

		err = fdt_setprop_placeholder(fdt, cpu, "riscv,isa",
					      len + append_len,
					      &new_isa_string);
		if (err < 0) {
			return SBI_EFAIL;
		}

		memmove(new_isa_string, isa_string, len - 1);

		if (pos) {
			((char *)new_isa_string)[len - 1] = '_';
			((char *)new_isa_string)[len]	  = 'h';
			((char *)new_isa_string)[len + 1] = '\0';
		} else {
			((char *)new_isa_string)[len - 1] = 'h';
			((char *)new_isa_string)[len]	  = '\0';
		}
	}

	if (cpu < 0 && cpu != -FDT_ERR_NOTFOUND) {
		return SBI_EFAIL;
	}

	return SBI_OK;
}

static int relocate_initrd(struct sbi_scratch *scratch,
			   unsigned long *relocate_base)
{
	int chosen, len, rc;
	void *fdt = (void *)scratch->next_arg1;

	unsigned long initrd_start = 0, initrd_end = 0, initrd_new_start;
	const char *res;

	chosen = fdt_path_offset(fdt, "/chosen");

	if (chosen < 0)
		goto not_found;

	res = fdt_getprop(fdt, chosen, "linux,initrd-start", &len);
	if (!res || len > sizeof(unsigned long))
		goto not_found;

	for (int i = 0; i < len; i++)
		initrd_start = (initrd_start << 8) | res[i];

	res = fdt_getprop(fdt, chosen, "linux,initrd-end", &len);
	if (!res || len > sizeof(unsigned long))
		goto not_found;

	for (int i = 0; i < len; i++)
		initrd_end = (initrd_end << 8) | res[i];

	if (initrd_end > *relocate_base) {
		initrd_new_start = *relocate_base - (initrd_end - initrd_start);
		initrd_new_start &= PAGE_MASK;
		*relocate_base = initrd_new_start;

		sbi_printf("%s: Moving initrd 0x%lx -> 0x%lx\n", __func__,
			   initrd_start, initrd_new_start);

		memmove((void *)initrd_new_start, (void *)initrd_start,
			(initrd_end - initrd_start));

		rc = fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + 32);
		if (rc < 0)
			return SBI_EFAIL;

		fdt_setprop_u64(fdt, chosen, "linux,initrd-start",
				initrd_new_start);
		fdt_setprop_u64(fdt, chosen, "linux,initrd-end",
				initrd_new_start + (initrd_end - initrd_start));

		return SBI_OK;
	} else {
		return SBI_OK;
	}

not_found:
	return SBI_OK;
}

static int sbi_hext_relocate(struct sbi_scratch *scratch)
{
	int rc;
	unsigned long relocate_base = (unsigned long)hext_pt_start;

	rc = relocate_initrd(scratch, &relocate_base);
	if (rc)
		return rc;

	// TODO: Maybe also relocate FDT?

	return SBI_OK;
}

static int patch_fdt_reserve(void *fdt, unsigned long addr, unsigned long size)
{
	int rc;
	int parent;
	int na = fdt_address_cells(fdt, 0);
	int ns = fdt_size_cells(fdt, 0);
	fdt32_t addr_high, addr_low;
	fdt32_t size_high, size_low;
	int subnode;
	fdt32_t reg[4];
	fdt32_t *val;

	rc = fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + 128);
	if (rc < 0)
		return SBI_EFAIL;

	parent = fdt_path_offset(fdt, "/reserved-memory");
	if (parent < 0) {
		parent = fdt_add_subnode(fdt, 0, "reserved-memory");
		if (parent < 0)
			return SBI_EFAIL;

		rc = fdt_setprop_empty(fdt, parent, "ranges");
		if (rc < 0)
			return SBI_EFAIL;

		rc = fdt_setprop_u32(fdt, parent, "#size-cells", ns);
		if (rc < 0)
			return SBI_EFAIL;

		rc = fdt_setprop_u32(fdt, parent, "#address-cells", na);
		if (rc < 0)
			return SBI_EFAIL;
	}

	subnode = fdt_add_subnode(fdt, parent, "shadow-pt-resv");
	if (subnode < 0)
		return SBI_EFAIL;

	addr_high = (u64)addr >> 32;
	addr_low  = addr;
	size_high = (u64)size >> 32;
	size_low  = size;

	/* encode the <reg> property value */
	val = reg;
	if (na > 1)
		*val++ = cpu_to_fdt32(addr_high);
	*val++ = cpu_to_fdt32(addr_low);
	if (ns > 1)
		*val++ = cpu_to_fdt32(size_high);
	*val++ = cpu_to_fdt32(size_low);

	rc = fdt_setprop(fdt, subnode, "reg", reg, (na + ns) * sizeof(fdt32_t));
	if (rc < 0)
		return SBI_EFAIL;

	rc = fdt_setprop_empty(fdt, subnode, "no-map");
	if (rc < 0)
		return SBI_EFAIL;

	return SBI_OK;
}

static int hart_with_mmu_count(void *fdt)
{
	int err, cpu_offset, cpus_offset, len, count = 0;
	const struct sbi_platform *platform = sbi_platform_thishart_ptr();
	const char *mmu_type;
	u32 hartid, hart_index;

	err = fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + 32);
	if (err < 0)
		return SBI_EFAIL;

	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0)
		return SBI_EFAIL;

	fdt_for_each_subnode(cpu_offset, fdt, cpus_offset) {
		err = fdt_parse_hart_id(fdt, cpu_offset, &hartid);
		if (err)
			continue;

		if (!fdt_node_is_enabled(fdt, cpu_offset))
			continue;

		mmu_type = fdt_getprop(fdt, cpu_offset, "mmu-type", &len);
		if (!mmu_type || !len)
			continue;

		hart_index = sbi_platform_hart_index(platform, hartid);
		if (hart_index == -1u)
			continue;

		count++;

		hart_hext_state[hart_index].available = true;
	}

	if ((cpu_offset < 0) && (cpu_offset != -FDT_ERR_NOTFOUND))
		return SBI_EFAIL;

	return count;
}

static int allocate_pt_space(struct sbi_scratch *scratch)
{
	int rc;
	int hart_count;
	unsigned long mem_start, mem_size;
	unsigned long mem_end_aligned;
	unsigned long alloc_size;
	struct sbi_domain_memregion region;

	rc = find_main_memory((void *)scratch->next_arg1, &mem_start,
			      &mem_size);
	if (rc)
		return rc;

	mem_end_aligned = (mem_start + mem_size) & ~(PT_ALIGN - 1);

	hart_count = hart_with_mmu_count(sbi_scratch_thishart_arg1_ptr());

	if (hart_count < 0)
		return hart_count;

	alloc_size = hart_count * PT_SPACE_SIZE;

	/* A really conservative sanity check. Make sure we have enough memory */
	if (mem_start + 3 * alloc_size > mem_end_aligned) {
		sbi_printf("%s: No memory for shadow page tables.\n", __func__);
		return SBI_OK;
	}

	sbi_domain_memregion_init(mem_end_aligned - alloc_size, alloc_size,
				  SBI_DOMAIN_MEMREGION_READABLE, &region);

	rc = sbi_domain_root_add_memregion(&region);
	if (rc) {
		sbi_printf(
			"%s: Failed to add memregion for shadow page tables\n",
			__func__);
		return SBI_ENOMEM;
	}

	hext_pt_start = region.base;
	hext_pt_size  = (1UL << region.order) / PT_NODE_SIZE;

	patch_fdt_reserve((void *)scratch->next_arg1,
			  (unsigned long)hext_pt_start, (1UL << region.order));

	rc = sbi_hext_pt_init(hext_pt_start, hext_pt_size / hart_count);

	if (rc)
		return rc;

	return SBI_OK;
}

static bool sbi_hext_mstatus_features()
{
	unsigned long saved_mstatus =
		csr_read_set(CSR_MSTATUS, MSTATUS_TRY_FEATURES);
	unsigned long new_mstatus = csr_read(CSR_MSTATUS);
	csr_write(CSR_MSTATUS, saved_mstatus);

	hext_mstatus_features = new_mstatus & MSTATUS_TRY_FEATURES;

	return MSTATUS_NEED_FEATURES ==
	       (hext_mstatus_features & MSTATUS_NEED_FEATURES);
}

static void sbi_hext_init_state(struct hext_state *hext)
{
	hext->virt    = 0;
	hext->hgatp   = 0;
	hext->hedeleg = 0;
	hext->hideleg = 0;
	hext->hie     = 0;
	hext->hvip    = 0;

#if __riscv_xlen == 32
	hext->hstatus = 0;
#else
	/* hstatus.VSXL = RV64, read-only */
	hext->hstatus = 2UL << HSTATUS_VSXL_SHIFT;
#endif
}

static bool check_errata_cip_453()
{
	unsigned long vendorid = csr_read(CSR_MVENDORID);
	unsigned long arch_id  = csr_read(CSR_MARCHID);
	unsigned long impid    = csr_read(CSR_MIMPID);

	if (vendorid != 0x489)
		return false;

	if (arch_id != 0x8000000000000007 ||
	    (impid < 0x20181004 || impid > 0x20191105))
		return false;
	return true;
}

int sbi_hext_init(struct sbi_scratch *scratch, bool cold_boot)
{
	int rc;

	if (!misa_extension('S')) {
		// No supervisor mode, no need to emulate HS
		return SBI_OK;
	}

	if (cold_boot) {
		if (misa_extension('H')) {
			sbi_printf(
				"%s: Native hypervisor extension available.\n",
				__func__);
			return SBI_OK;
		}

		errata_cip_453 = check_errata_cip_453();

		if (!sbi_hext_mstatus_features()) {
			sbi_printf(
				"%s: No virtualization support in mstatus.{TVM,TW,TSR}\n",
				__func__);
			return SBI_OK;
		}

		rc = allocate_pt_space(scratch);
		if (rc)
			return rc;

		if (!sbi_hext_enabled()) {
			return SBI_OK;
		}

		rc = patch_fdt_cpu_isa((void *)scratch->next_arg1);
		if (rc)
			return rc;

		sbi_hext_relocate(scratch);

		sbi_printf("%s: Hypervisor extension emulation enabled.\n",
			   __func__);

	} else {
		if (!sbi_hext_enabled()) {
			return SBI_OK;
		}
	}

	struct hext_state *hext = sbi_hext_current_state();
	sbi_hext_init_state(hext);

	return SBI_OK;
}
