/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 starfivetech.com
 *
 * Authors:
 *   minda.chen <minda.chen@starfivetech.com>
 */

#include <sbi/riscv_io.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_console.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/i2c/fdt_i2c.h>
#include <libfdt.h>
#include <sbi/sbi_string.h>

#define DW_IC_CON		0x00
#define DW_IC_TAR		0x04
#define DW_IC_SAR		0x08
#define DW_IC_DATA_CMD		0x10
#define DW_IC_SS_SCL_HCNT	0x14
#define DW_IC_SS_SCL_LCNT	0x18
#define DW_IC_FS_SCL_HCNT	0x1c
#define DW_IC_FS_SCL_LCNT	0x20
#define DW_IC_HS_SCL_HCNT	0x24
#define DW_IC_HS_SCL_LCNT	0x28
#define DW_IC_INTR_STAT		0x2c
#define DW_IC_INTR_MASK		0x30
#define DW_IC_RAW_INTR_STAT	0x34
#define DW_IC_RX_TL		0x38
#define DW_IC_TX_TL		0x3c
#define DW_IC_CLR_INTR		0x40
#define DW_IC_CLR_RX_UNDER	0x44
#define DW_IC_CLR_RX_OVER	0x48
#define DW_IC_CLR_TX_OVER	0x4c
#define DW_IC_CLR_RD_REQ	0x50
#define DW_IC_CLR_TX_ABRT	0x54
#define DW_IC_CLR_RX_DONE	0x58
#define DW_IC_CLR_ACTIVITY	0x5c
#define DW_IC_CLR_STOP_DET	0x60
#define DW_IC_CLR_START_DET	0x64
#define DW_IC_CLR_GEN_CALL	0x68
#define DW_IC_ENABLE		0x6c
#define DW_IC_STATUS		0x70
#define DW_IC_TXFLR		0x74
#define DW_IC_RXFLR		0x78
#define DW_IC_SDA_HOLD		0x7c
#define DW_IC_TX_ABRT_SOURCE	0x80
#define DW_IC_ENABLE_STATUS	0x9c
#define DW_IC_CLR_RESTART_DET	0xa8
#define DW_IC_COMP_PARAM_1	0xf4
#define DW_IC_COMP_VERSION	0xf8


#define DRV_I2C_DEVADDR_DEPTH7          0
#define DRV_I2C_DEVADDR_DEPTH10         1

#define DRV_I2C_REG_DEPTH8              0
#define DRV_I2C_REG_DEPTH16             1

#define STARFIVE_I2C_STATUS_TXFIFO_EMPTY (1 << 2)
#define STARFIVE_I2C_STATUS_RXFIFO_NOT_EMPTY (1 << 3)
#define DW_IC_CON_10BITADDR_MASTER	(1 << 4)
#define I2C_APB_CLK_BASE 	0x13020228

#define STARFIVE_I2C_ADAPTER_MAX	7

#define IC_DATA_CMD_READ	(1 << 8)
#define IC_DATA_CMD_STOP	(1 << 9)
#define IC_DATA_CMD_RESTART	(1 << 10)
#define IC_INT_STATUS_STOPDET	(1 << 9)


struct starfive_i2c_adapter {
	unsigned long addr;
	int index;
	struct i2c_adapter adapter;
};

static unsigned int starfive_i2c_adapter_count;
static struct starfive_i2c_adapter
	starfive_i2c_adapter_array[STARFIVE_I2C_ADAPTER_MAX];

extern struct fdt_i2c_adapter fdt_i2c_adapter_starfive;

static inline void starfive_i2c_setreg(struct starfive_i2c_adapter *adap,
				     uint8_t reg, uint32_t value)
{
	writel(value, (volatile char *)adap->addr + reg);
}

static inline uint32_t starfive_i2c_getreg(struct starfive_i2c_adapter *adap,
					uint8_t reg)
{
	return readl((volatile char *)adap->addr + reg);
}

static int starfive_i2c_adapter_poll(struct starfive_i2c_adapter *adap,
				   uint32_t mask, uint32_t addr, bool inverted)
{
	unsigned int timeout = 10; /* [msec] */
	int count = 0;
	uint32_t val;

	do {
		val = starfive_i2c_getreg(adap, addr);
		if (inverted) {
			if (!(val & mask))
				return 0;
		} else {
			if (val & mask)
				return 0;
		}
		sbi_timer_udelay(2);
		count += 1;
		if (count == (timeout * 1000))
			return SBI_ETIMEDOUT;
	} while (1);
}

#define starfive_i2c_adapter_poll_rxrdy(adap)	\
	starfive_i2c_adapter_poll(adap, STARFIVE_I2C_STATUS_RXFIFO_NOT_EMPTY, DW_IC_STATUS, 0)
#define starfive_i2c_adapter_poll_txfifo_ready(adap)	\
	starfive_i2c_adapter_poll(adap, STARFIVE_I2C_STATUS_TXFIFO_EMPTY, DW_IC_STATUS, 0)

static int starfive_i2c_write_addr(struct starfive_i2c_adapter *adap,
				    uint8_t addr)
{
	unsigned long clock_base = (I2C_APB_CLK_BASE + adap->index * 4);
	unsigned int val;

	val = readl((volatile char *)clock_base);
	if (!val) {
		writel(0x1 << 31, (volatile char *)(clock_base));
	}

	starfive_i2c_setreg(adap, DW_IC_ENABLE, 0);
	starfive_i2c_setreg(adap, DW_IC_TAR, addr);
	starfive_i2c_setreg(adap, DW_IC_ENABLE, 1);

	return 0;
}


static int starfive_i2c_adapter_read(struct i2c_adapter *ia, uint8_t addr,
				    uint8_t reg, uint8_t *buffer, int len)
{
	struct starfive_i2c_adapter *adap =
		container_of(ia, struct starfive_i2c_adapter, adapter);
	int rc;

	starfive_i2c_write_addr(adap, addr); /* only support 8bit value device now */

	rc = starfive_i2c_adapter_poll_txfifo_ready(adap);
	if (rc) {
		sbi_printf("i2c read: write daddr %x to\n", addr);
		return rc;
	}

	/* set register address */
	starfive_i2c_setreg(adap, DW_IC_DATA_CMD, reg);

	/* set value */
	while (len) {
		/*
		 * Avoid writing to ic_cmd_data multiple times
		 * in case this loop spins too quickly and the
		 * ic_status RFNE bit isn't set after the first
		 * write. Subsequent writes to ic_cmd_data can
		 * trigger spurious i2c transfer.
		 */
		if (len == 1)
			starfive_i2c_setreg(adap, DW_IC_DATA_CMD, IC_DATA_CMD_READ | IC_DATA_CMD_STOP);
		else
			starfive_i2c_setreg(adap, DW_IC_DATA_CMD, IC_DATA_CMD_READ);

		rc = starfive_i2c_adapter_poll_rxrdy(adap);
		if (rc) {
			sbi_printf("i2c read: read reg %x to\n", reg);
			return rc;
		}
		*buffer = starfive_i2c_getreg(adap, DW_IC_DATA_CMD) & 0xff;
		buffer++;
		len--;
	}

	return 0;
}

static int starfive_i2c_adapter_write(struct i2c_adapter *ia, uint8_t addr,
				   uint8_t reg, uint8_t *buffer, int len)
{
	struct starfive_i2c_adapter *adap =
		container_of(ia, struct starfive_i2c_adapter, adapter);
	int rc;
	uint8_t val = 0;

	starfive_i2c_write_addr(adap, addr);

	rc = starfive_i2c_adapter_poll_txfifo_ready(adap);
	if (rc) {
		sbi_printf("i2c write: write daddr %x to\n", addr);
		return rc;
	}
	/* set register address */
	starfive_i2c_setreg(adap, DW_IC_DATA_CMD, reg);

	while (len) {

		rc = starfive_i2c_adapter_poll_txfifo_ready(adap);
		if (rc) {
			sbi_printf("i2c write: write reg %x to\n", reg);
			return rc;
		}

		if (len == 1)
			starfive_i2c_setreg(adap, DW_IC_DATA_CMD, *buffer | IC_DATA_CMD_STOP);
		else
			starfive_i2c_setreg(adap, DW_IC_DATA_CMD, *buffer);

		val = *buffer;
		buffer++;
		len--;
	}
	rc = starfive_i2c_adapter_poll_txfifo_ready(adap);
	if (rc) {
		sbi_printf("i2c write: write reg %x val %x to\n", reg, val);
		return rc;
	}
	return 0;
}

static int starfive_i2c_init(void *fdt, int nodeoff,
			    const struct fdt_match *match)
{
	int rc;
	struct starfive_i2c_adapter *adapter;
	uint64_t addr;
	const char *name;

	if (starfive_i2c_adapter_count >= STARFIVE_I2C_ADAPTER_MAX)
		return SBI_ENOSPC;

	adapter = &starfive_i2c_adapter_array[starfive_i2c_adapter_count];

	rc = fdt_get_node_addr_size(fdt, nodeoff, 0, &addr, NULL);
	if (rc)
		return rc;

	name = fdt_get_name(fdt, nodeoff, NULL);
	if (!sbi_strncmp(name, "i2c", 3)) {
		adapter->index = name[3] - '0';
	} else
		return SBI_EINVAL;

	adapter->addr = addr;
	adapter->adapter.driver = &fdt_i2c_adapter_starfive;
	adapter->adapter.id = nodeoff;
	adapter->adapter.write = starfive_i2c_adapter_write;
	adapter->adapter.read = starfive_i2c_adapter_read;
	rc = i2c_adapter_add(&adapter->adapter);
	if (rc)
		return rc;

	starfive_i2c_adapter_count++;

	return 0;
}

static const struct fdt_match starfive_i2c_match[] = {
	{ .compatible = "snps,designware-i2c" },
	{ },
};

struct fdt_i2c_adapter fdt_i2c_adapter_starfive = {
	.match_table = starfive_i2c_match,
	.init = starfive_i2c_init,
};


