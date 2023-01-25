// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Renesas Electronics Corp.
 *
 */

#include <platform_override.h>
#include <sbi/sbi_domain.h>
#include <sbi_utils/fdt/fdt_helper.h>

int renesas_rzfive_early_init(bool cold_boot, const struct fdt_match *match)
{
	/*
	 * Renesas RZ/Five RISC-V SoC has Instruction local memory and
	 * Data local memory (ILM & DLM) mapped between region 0x30000
	 * to 0x4FFFF. When a virtual address falls within this range,
	 * the MMU doesn't trigger a page fault; it assumes the virtual
	 * address is a physical address which can cause undesired
	 * behaviours for statically linked applications/libraries. To
	 * avoid this, add the ILM/DLM memory regions to the root domain
	 * region of the PMPU with permissions set to 0x0 for S/U modes
	 * so that any access to these regions gets blocked and for M-mode
	 * we grant full access.
	 */
	return sbi_domain_root_add_memrange(0x30000, 0x20000, 0x1000,
					    SBI_DOMAIN_MEMREGION_M_RWX);
}

static const struct fdt_match renesas_rzfive_match[] = {
	{ .compatible = "renesas,r9a07g043f01" },
	{ /* sentinel */ }
};

const struct platform_override renesas_rzfive = {
	.match_table = renesas_rzfive_match,
	.early_init = renesas_rzfive_early_init,
};
