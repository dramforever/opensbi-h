/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 StarFive
 *
 * Authors:
 *   Wei Liang Lim <weiliang.lim@starfivetech.com>
 *   Minda chen <minda.chen@starfivetech.com>
 */

#include <libfdt.h>
#include <platform_override.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_system.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hsm.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_domain.h>
#include <sbi/riscv_io.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/fdt/fdt_fixup.h>
#include <sbi_utils/reset/fdt_reset.h>
#include <sbi_utils/i2c/fdt_i2c.h>

struct pmic {
	struct i2c_adapter *adapter;
	uint32_t dev_addr;
	const char *compatible;
};

static struct pmic pmic_inst;
static u32 selected_hartid = -1;

#define PMU_REG_BASE 0x17030000
#define SYS_CRG_BASE 0x13020000
#define SYS_CRG_CORE_CLK_BASE 0x13020064
#define SYS_CRG_CORE_TRACE_CLK_BASE 0x13020080

/* PMU register define */
#define HW_EVENT_TURN_ON_MASK		0x04
#define HW_EVENT_TURN_OFF_MASK		0x08
#define SW_TURN_ON_POWER_MODE		0x0C
#define SW_TURN_OFF_POWER_MODE		0x10
#define SW_ENCOURAGE			0x44
#define PMU_INT_MASK			0x48
#define PCH_BYPASS			0x4C
#define PCH_PSTATE			0x50
#define PCH_TIMEOUT			0x54
#define LP_TIMEOUT			0x58
#define HW_TURN_ON_MODE			0x5C
#define CURR_POWER_MODE			0x80
#define PMU_EVENT_STATUS		0x88
#define PMU_INT_STATUS			0x8C

/* sw encourage cfg */
#define SW_MODE_ENCOURAGE_EN_LO		0x05
#define SW_MODE_ENCOURAGE_EN_HI		0x50
#define SW_MODE_ENCOURAGE_DIS_LO	0x0A
#define SW_MODE_ENCOURAGE_DIS_HI	0xA0
#define SW_MODE_ENCOURAGE_ON		0xFF

#define DEVICE_PD_MASK			0xfc
#define SYSTOP_CPU_PD_MASK		0x3

#define TIMEOUT_COUNT			100000
#define AXP15060_POWER_REG		0x32
#define AXP15060_POWER_OFF_BIT		(0x1 << 7)
#define AXP15060_RESET_BIT		(0x1 << 6)

static int pm_system_reset_check(u32 type, u32 reason)
{
	switch (type) {
	case SBI_SRST_RESET_TYPE_SHUTDOWN:
		return 1;
	case SBI_SRST_RESET_TYPE_COLD_REBOOT:
		if (pmic_inst.adapter)
			return 255;
		break;
	}

	return 0;
}

static int wait_pmu_pd_state(uint32_t mask)
{
	int count = 0;
	uint32_t val;

	do {
		val = readl((volatile void *)(PMU_REG_BASE + CURR_POWER_MODE));
		if (val == mask)
			return 0;

		sbi_timer_udelay(2);
		count += 1;
		if (count == TIMEOUT_COUNT)
			return SBI_ETIMEDOUT;
	} while (1);
}

static int shutdown_device_power_domain()
{
	unsigned long addr = PMU_REG_BASE;
	uint32_t curr_mode;
	int ret;

	curr_mode = readl((volatile void *)(addr + CURR_POWER_MODE));
	curr_mode &= DEVICE_PD_MASK;

	if (curr_mode) {
		writel(curr_mode, (volatile void *)(addr + SW_TURN_OFF_POWER_MODE));
		writel(SW_MODE_ENCOURAGE_ON, (volatile void *)(addr + SW_ENCOURAGE));
		writel(SW_MODE_ENCOURAGE_DIS_LO, (volatile void *)(addr + SW_ENCOURAGE));
		writel(SW_MODE_ENCOURAGE_DIS_HI, (volatile void *)(addr + SW_ENCOURAGE));
		ret = wait_pmu_pd_state(SYSTOP_CPU_PD_MASK);
		if (ret)
			sbi_printf("shutdown device power %x error\n", curr_mode);
	}
	return ret;
}

static int shutdown_cpu_systop_domain()
{
	unsigned long addr = PMU_REG_BASE;
	uint32_t curr_mode;
	int ret;

	curr_mode = readl((volatile void *)(addr + CURR_POWER_MODE));

	if (curr_mode != SYSTOP_CPU_PD_MASK) {
		shutdown_device_power_domain();
	}
	if (curr_mode) {
		writel(curr_mode, (volatile void *)(addr + SW_TURN_OFF_POWER_MODE));
		writel(SW_MODE_ENCOURAGE_ON, (volatile void *)(addr + SW_ENCOURAGE));
		writel(SW_MODE_ENCOURAGE_DIS_LO, (volatile void *)(addr + SW_ENCOURAGE));
		writel(SW_MODE_ENCOURAGE_DIS_HI, (volatile void *)(addr + SW_ENCOURAGE));
	}
	while (1) {
		wfi(); /* wait for power down */
	}
	return ret;
}

static int pmic_ops(struct pmic *pmic, int type)
{
	int ret = 0;
	uint8_t val;
	int retry = 10;

	shutdown_device_power_domain();

	if (!sbi_strcmp("stf,axp15060-regulator", pmic->compatible)) {
		do {
			ret = i2c_adapter_reg_read(pmic->adapter, pmic->dev_addr,
				AXP15060_POWER_REG, &val);
			retry--;
		} while (ret && retry);

		if (ret) {
			sbi_printf("cannot read pmic power register\n");
			goto err;
		}

		retry = 10;
		val |= AXP15060_POWER_OFF_BIT;
		if (type == SBI_SRST_RESET_TYPE_SHUTDOWN)
			val |= AXP15060_POWER_OFF_BIT;
		else
			val |= AXP15060_RESET_BIT;

		do {
			ret = i2c_adapter_reg_write(pmic->adapter, pmic->dev_addr,
				AXP15060_POWER_REG, val);
			retry--;
		} while (ret && retry);

		if (ret) {
			sbi_printf("cannot write pmic power register\n");
		}
	}

err:
	while (1) {
		wfi();
	}
	return 0;
}

static int pmu_shutdown()
{
	shutdown_device_power_domain();
	shutdown_cpu_systop_domain();

	return 0;
}

static void pm_system_reset(u32 type, u32 reason)
{
	if (pmic_inst.adapter) {
		pmic_ops(&pmic_inst, type);
		return;
	}

	switch (type) {
	case SBI_SRST_RESET_TYPE_SHUTDOWN:
		pmu_shutdown();
		break;
	default:
		goto skip_reset;
	}
	return;

skip_reset:
	sbi_hart_hang();
}


static struct sbi_system_reset_device pm_reset = {
	.name = "pm-reset",
	.system_reset_check = pm_system_reset_check,
	.system_reset = pm_system_reset
};

static int pmic_reset_init(void *fdt, int nodeoff,
			     const struct fdt_match *match)
{
	int rc;
	int i2c_bus;
	struct i2c_adapter *adapter;
	uint64_t addr;

	rc = fdt_get_node_addr_size(fdt, nodeoff, 0, &addr, NULL);
	if (rc)
		return rc;

	pmic_inst.dev_addr = addr;
	pmic_inst.compatible = match->compatible;

	i2c_bus = fdt_parent_offset(fdt, nodeoff);
	if (i2c_bus < 0)
		return i2c_bus;

	/* i2c adapter get */
	rc = fdt_i2c_adapter_get(fdt, i2c_bus, &adapter);
	if (rc)
		return rc;

	pmic_inst.adapter = adapter;

	sbi_system_reset_add_device(&pm_reset);

	return 0;
}

static int pm_reset_init(void *fdt, int nodeoff,
			     const struct fdt_match *match)
{
	if (!sbi_strcmp(match->compatible, "starfive,jh7110-pmu")) {
		sbi_system_reset_add_device(&pm_reset);
		return 0;
	}
	return pmic_reset_init(fdt, nodeoff, match);
}

static const struct fdt_match pm_reset_match[] = {
	{ .compatible = "stf,axp15060-regulator", .data = (void *)true },
	{ },
};

struct fdt_reset fdt_reset_pmic = {
	.match_table = pm_reset_match,
	.init = pm_reset_init,
};

static int starfive_jh7110_hart_suspend(u32 suspend_type)
{
	wfi();
	return SBI_ENOTSUPP;	/* 7110 not support STR */
}

static void starfive_jh7110_hart_resume(void)
{
}

static const struct sbi_hsm_device jh7110_hsm_device = {
	.name		= "jh7110-hsm",
	.hart_suspend	= starfive_jh7110_hart_suspend,
	.hart_resume = starfive_jh7110_hart_resume,
};

static int starfive_jh7110_final_init(bool cold_boot,
				   const struct fdt_match *match)
{
	void *fdt = fdt_get_address();

	if (cold_boot) {
		sbi_hsm_set_device(&jh7110_hsm_device);
		fdt_reset_driver_init(fdt, &fdt_reset_pmic);
	}

	return 0;
}

static bool starfive_jh7110_cold_boot_allowed(u32 hartid,
				   const struct fdt_match *match)
{
	if (selected_hartid != -1)
		return (selected_hartid == hartid);

	return true;
}

static void starfive_jh7110_fw_init(void *fdt, const struct fdt_match *match)
{
	const fdt32_t *val;
	int len, coff;

	coff = fdt_path_offset(fdt, "/chosen");
	if (-1 < coff) {
		val = fdt_getprop(fdt, coff, "starfive,boot-hart-id", &len);
		if (val && len >= sizeof(fdt32_t))
			selected_hartid = (u32) fdt32_to_cpu(*val);
	}
}

static const struct fdt_match starfive_jh7110_match[] = {
	{ .compatible = "starfive,jh7110" },
	{ },
};

const struct platform_override starfive_jh7110 = {
	.match_table = starfive_jh7110_match,
	.cold_boot_allowed = starfive_jh7110_cold_boot_allowed,
	.fw_init = starfive_jh7110_fw_init,
	.final_init = starfive_jh7110_final_init,
};
