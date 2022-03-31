/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libfdt.h>
#include <sbi/sbi_hext.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_domain.h>

struct hext_state {
	unsigned long vsatp;
	unsigned long hgatp;
};

unsigned long hext_shadow_pt_start;
unsigned long hext_shadow_pt_size;

struct hext_state hart_hext_state[SBI_HARTMASK_MAX_BITS] = { 0 };

static int find_main_memory(void *fdt, unsigned long *addr, unsigned long *size) {
	int i, node, len;
	const void *device_type;
	int address_cells, size_cells;
	int memory_node = -1;
	const fdt32_t *reg;
	uint64_t full_addr = 0, full_size = 0;

	address_cells = fdt_address_cells(fdt, 0);
	size_cells = fdt_size_cells(fdt, 0);


	if (address_cells < 0 || size_cells < 0) {
		return SBI_EFAIL;
	}

	fdt_for_each_subnode(node, fdt, 0) {
		device_type = fdt_getprop(fdt, node, "device_type", &len);
		if (device_type && strncmp(device_type, "memory", strlen("memory")) == 0) {
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

	for (i = 0; i < address_cells; i ++) {
		full_addr = (full_addr << 32) | fdt32_to_cpu(reg[i]);
	}

	for (i = address_cells; i < address_cells + size_cells; i ++) {
		full_size = (full_size << 32) | fdt32_to_cpu(reg[i]);
	}

	if (addr) {
		*addr = (unsigned long) full_addr;
	}

	if (size) {
		*size = (unsigned long) full_size;
	}

	return SBI_OK;
}

int patch_fdt_cpu_isa(void *fdt) {
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
		if (! prop || strncmp(prop, "cpu", strlen("cpu")) != 0) {
			continue;
		}

		isa_string = fdt_getprop(fdt, cpu, "riscv,isa", &len);

		err = fdt_setprop_placeholder(fdt, cpu, "riscv,isa", len + 1, &new_isa_string);
		if (err < 0) {
			return SBI_EFAIL;
		}

		memmove(new_isa_string, isa_string, len - 1);
		((char*) new_isa_string)[len - 1] = 'h';
		((char*) new_isa_string)[len] = '\0';
	}

	if (cpu < 0 && cpu != -FDT_ERR_NOTFOUND) {
		return SBI_EFAIL;
	}


	return SBI_OK;
}

static int allocate_shadow_pt_space(struct sbi_scratch *scratch) {
	int rc;
	unsigned long mem_start, mem_size;
	unsigned long mem_end_aligned;
	struct sbi_domain_memregion region;

	rc = find_main_memory((void *) scratch->next_arg1, &mem_start, &mem_size);
	if (rc)
		return rc;

	mem_end_aligned = (mem_start + mem_size) & ~(SHADOW_PT_SPACE_SIZE - 1);

	if (mem_start + SHADOW_PT_SPACE_SIZE > mem_end_aligned) {
		sbi_printf("%s: No memory for shadow page tables.\n", __func__);
		return SBI_OK;
	}

	sbi_domain_memregion_init(
		mem_end_aligned - SHADOW_PT_SPACE_SIZE,
		SHADOW_PT_SPACE_SIZE,
		0,
		&region
	);

	rc = sbi_domain_root_add_memregion(&region);
	if (rc) {
		sbi_printf("%s: Failed to add memregion for shadow page tables\n", __func__);
		return SBI_ENOMEM;
	}

	hext_shadow_pt_start = mem_end_aligned - SHADOW_PT_SPACE_SIZE;
	hext_shadow_pt_size = SHADOW_PT_SPACE_SIZE;

	return SBI_OK;
}

int sbi_hext_init(struct sbi_scratch *scratch, bool cold_boot) {
	int rc;

	if (cold_boot) {
		if (misa_extension('H')) {
			sbi_printf("%s: Native hypervisor extension available.\n", __func__);
			return SBI_OK;
		}

		rc = allocate_shadow_pt_space(scratch);
		if (rc)
			return rc;

		rc = patch_fdt_cpu_isa((void *) scratch->next_arg1);
		if (rc)
			return rc;

		sbi_printf("%s: Hypervisor extension emulation enabled.\n", __func__);

	} else {
		if (! sbi_hext_enabled()) {
			return SBI_OK;
		}
	}

	return SBI_OK;
}

int sbi_hext_csr_read(int csr_num, struct sbi_trap_regs *regs, unsigned long *csr_val) {
	sbi_printf("%s: CSR 0x%03x: Not implemented\n", __func__, csr_num);
	return SBI_ENOTSUPP;
}

int sbi_hext_csr_write(int csr_num, struct sbi_trap_regs *regs, unsigned long csr_val) {
	sbi_printf("%s: CSR 0x%03x: Not implemented\n", __func__, csr_num);
	return SBI_ENOTSUPP;
}
