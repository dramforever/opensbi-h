/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libfdt.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_domain.h>

#define SHADOW_PT_SPACE_SIZE (2UL << 20)

#define MSTATUS_TRY_FEATURES (MSTATUS_TVM | MSTATUS_TW | MSTATUS_TSR)
#define MSTATUS_NEED_FEATURES (MSTATUS_TVM | MSTATUS_TSR)

unsigned long hext_shadow_pt_start;
unsigned long hext_shadow_pt_size;
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

	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0) {
		return SBI_ENODEV;
	}

	fdt_for_each_subnode(cpu, fdt, cpus_offset) {
		fdt_getprop(fdt, cpu, "riscv,isa", &len);
		total_new_strings += len + sizeof(struct fdt_property) + 1;
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

		err = fdt_setprop_placeholder(fdt, cpu, "riscv,isa", len + 1,
					      &new_isa_string);
		if (err < 0) {
			return SBI_EFAIL;
		}

		memmove(new_isa_string, isa_string, len - 1);
		((char *)new_isa_string)[len - 1] = 'h';
		((char *)new_isa_string)[len]	  = '\0';
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
	unsigned long relocate_base = hext_shadow_pt_start;

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

	return SBI_OK;
}

static int allocate_shadow_pt_space(struct sbi_scratch *scratch)
{
	int rc;
	unsigned long mem_start, mem_size;
	unsigned long mem_end_aligned;
	struct sbi_domain_memregion region;

	rc = find_main_memory((void *)scratch->next_arg1, &mem_start,
			      &mem_size);
	if (rc)
		return rc;

	mem_end_aligned = (mem_start + mem_size) & ~(SHADOW_PT_SPACE_SIZE - 1);

	if (mem_start + SHADOW_PT_SPACE_SIZE > mem_end_aligned) {
		sbi_printf("%s: No memory for shadow page tables.\n", __func__);
		return SBI_OK;
	}

	sbi_domain_memregion_init(mem_end_aligned - SHADOW_PT_SPACE_SIZE,
				  SHADOW_PT_SPACE_SIZE,
				  SBI_DOMAIN_MEMREGION_READABLE, &region);

	rc = sbi_domain_root_add_memregion(&region);
	if (rc) {
		sbi_printf(
			"%s: Failed to add memregion for shadow page tables\n",
			__func__);
		return SBI_ENOMEM;
	}

	hext_shadow_pt_start = mem_end_aligned - SHADOW_PT_SPACE_SIZE;
	hext_shadow_pt_size  = SHADOW_PT_SPACE_SIZE;

	patch_fdt_reserve((void *)scratch->next_arg1, hext_shadow_pt_start,
			  hext_shadow_pt_size);

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

		if (!sbi_hext_mstatus_features()) {
			sbi_printf(
				"%s: No virtualization support in mstatus.{TVM,TW,TSR}\n",
				__func__);
			return SBI_OK;
		}

		rc = allocate_shadow_pt_space(scratch);
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

	struct hext_state *hext = &hart_hext_state[current_hartid()];
	sbi_hext_init_state(hext);

	return SBI_OK;
}
