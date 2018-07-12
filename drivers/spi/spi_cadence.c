/*
 * Copyright (c) 2018 Linaro Lmtd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL CONFIG_SYS_LOG_SPI_LEVEL
#include <logging/sys_log.h>

#include <clock_control/arm_clock_control.h>

#include <arch/cpu.h>
#include "spi_context.h"
#include <errno.h>
#include <device.h>
#include <spi.h>
#include <soc.h>
#include <board.h>
#include <arch/arm/cortex_m/cmsis.h>


#define CADENCE_REGS_CR	0
#define CADENCE_REGS_ISR	1
#define CADENCE_REGS_IER	2
#define CADENCE_REGS_IDR	3
#define CADENCE_REGS_IMR	4
#define CADENCE_REGS_ER	5
#define CADENCE_REGS_DR	6
#define CADENCE_REGS_TXD	7
#define CADENCE_REGS_RXD 8

/* Config register definitions and masks */
#define CADENCE_CR_MASTER_ENABLE	BIT(0)	/* Master Enable Mask */
#define CADENCE_CR_CPOL			BIT(1)	/* Clock Polarity Control */
#define CADENCE_CR_CPHA			BIT(2)	/* Clock Phase Control */
#define CADENCE_CR_BAUD_DIV_MASK	0x38	/* Baud Rate Divisor Mask */
#define CADENCE_CR_BAUD_DIV_SHIFT	3
#define CADENCE_CR_CLK_SELECT		BIT(8)	/* reference clock select */
#define CADENCE_CR_PERI_SEL		BIT(9)	/* Peripheral Select Decode */
#define CADENCE_CR_SSCTRL_MASK		0x3C00	/* Slave Select Mask */
#define CADENCE_CR_MANUAL_CS		BIT(14)	/* Manual CS Enable Mask */
#define CADENCE_CR_MANUAL_ENABLE	BIT(15)	/* Manual TX Enable Mask */
#define CADENCE_CR_MANUAL_START		BIT(16)	/* Manual TX Start */

/* ISR register */
#define CADENCE_ISR_ROF		BIT(0)	/* RX FIFO overflow */
#define CADENCE_ISR_MF		BIT(1)	/* Mode fail */
#define CADENCE_ISR_TNF		BIT(2)	/* TX FIFO Not Full */
#define CADENCE_ISR_TF		BIT(3)	/* TX FIFO Full */
#define CADENCE_ISR_RNE		BIT(4)	/* RX FIFO Not Empty */
#define CADENCE_ISR_RF		BIT(5)	/* RX FIFO Full */
#define CADENCE_ISR_TUF		BIT(6)	/* TX FIFO Underflow */

/* SPI Enable Register */
#define CADENCE_ER_ENABLE	BIT(0)

#define CADENCE_MAX_BAUD_RATE_DIVISOR	7	/* 3 bits of encoding */

#define CADENCE_MIN_SPI_RATE	(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC >> \
				 CADENCE_MAX_BAUD_RATE_DIVISOR)

/* Device constant configuration parameters */
struct spi_cadence_config {
        volatile uint32_t *regs;
};

struct spi_cadence_data {
	struct spi_context ctx;
};

static unsigned int calculate_divisor(uint32_t hz)
{
	uint32_t current_rate = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
	uint32_t i;

	/*
	 * Divisors go from 0 to 7 where the definition is:
	 * Maximum SPI clk rate is SysClk / 2 - 0
	 * ..........
	 * Minimum SPI clk rate is SysClk / 256 - 7
	 */

	for (i = 0, current_rate >>= 1; hz < current_rate; i++) {
		current_rate >>= 1;
	}

	return i;
}

static int spi_cadence_configure(struct device *dev,
			      const struct spi_config *spi_cfg)
{
	const struct spi_cadence_config *dev_cfg = dev->config->config_info;
	struct spi_cadence_data *data = dev->driver_data;


	if (spi_context_configured(&data->ctx, spi_cfg)) {
		/* Already configured. No need to do it again. */
		return 0;
	}

	if (SPI_OP_MODE_GET(spi_cfg->operation) != SPI_OP_MODE_MASTER) {
		SYS_LOG_ERR("Slave mode is not supported on %s",
			    dev->config->name);
		return -EINVAL;
	}

	if (spi_cfg->operation & SPI_MODE_LOOP) {
		SYS_LOG_ERR("Loopback mode is not supported");
		return -EINVAL;
	}

	if ((spi_cfg->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		SYS_LOG_ERR("Only single line mode is supported");
		return -EINVAL;
	}

	if (SPI_WORD_SIZE_GET(spi_cfg->operation) != 8) {
		SYS_LOG_ERR("Word sizes other than 8 bits"
			    " are not supported");
		return -EINVAL;
	}

	if (spi_cfg->frequency < CADENCE_MIN_SPI_RATE) {
		SYS_LOG_ERR("Frequencies lower than %d kHz are not supported",
			    CADENCE_MIN_SPI_RATE);
		return -EINVAL;
	}

	data->ctx.config = spi_cfg;
	spi_context_cs_configure(&data->ctx);

	/* disable SPI to be sure it's disabled */
	dev_cfg->regs[CADENCE_REGS_ER] = 0;
	__ISB();

	/* enabled master mode, manual start, manual chip select */
	dev_cfg->regs[CADENCE_REGS_CR] = CADENCE_CR_MASTER_ENABLE |
		   CADENCE_CR_MANUAL_ENABLE;

	/* clear all peripheral select lines */
	dev_cfg->regs[CADENCE_REGS_CR] &= ~(CADENCE_CR_SSCTRL_MASK);

	/* set baud rate */
	dev_cfg->regs[CADENCE_REGS_CR] &= ~(CADENCE_CR_BAUD_DIV_MASK);
	dev_cfg->regs[CADENCE_REGS_CR] |= calculate_divisor(spi_cfg->frequency) <<
			 CADENCE_CR_BAUD_DIV_SHIFT;

	/* disable interrupts */
//	dev_cfg->regs[CADENCE_REGS_IER] = 0xff;
//	dev_cfg->regs[CADENCE_REGS_IDR] = 0xff;
//	dev_cfg->regs[CADENCE_REGS_IMR] = 0xff;

	__ISB();
	/* enable SPI */
	dev_cfg->regs[CADENCE_REGS_ER] = CADENCE_ER_ENABLE;
	return 0;
}

static int transceive(struct device *dev,
		      const struct spi_config *config,
		      const struct spi_buf_set *tx_bufs,
		      const struct spi_buf_set *rx_bufs,
		      bool asynchronous,
		      struct k_poll_signal *signal)
{
	const struct spi_cadence_config *info = dev->config->config_info;
	struct spi_cadence_data *data = dev->driver_data;
	int ret;
	u8_t rxd;

	spi_context_lock(&data->ctx, asynchronous, signal);

	/* Configure */
	ret = spi_cadence_configure(dev, config);
	if (ret) {
		goto out;
	}

	spi_context_cs_control(&data->ctx, true);
	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	/* do one set of tx/rx spi_bufs at a time */
	while (spi_context_longest_current_buf(&data->ctx) > 0) {

			while(!(info->regs[CADENCE_REGS_ISR] & CADENCE_ISR_TNF)) {}

			if (spi_context_tx_on(&data->ctx)) {
				info->regs[CADENCE_REGS_TXD] = *(u8_t *)data->ctx.tx_buf;
				spi_context_update_tx(&data->ctx, 1, 1);
			} else {
				info->regs[CADENCE_REGS_TXD] = 0;
			}

			info->regs[CADENCE_REGS_CR] |= CADENCE_CR_MANUAL_START;
			while(!(info->regs[CADENCE_REGS_ISR] & CADENCE_ISR_RNE)) {}

			rxd = info->regs[CADENCE_REGS_RXD];

			/* don't overflow read read */
			if (spi_context_rx_on(&data->ctx)) {
				if (data->ctx.rx_buf)
					*data->ctx.rx_buf = rxd;
				spi_context_update_rx(&data->ctx, 1, 1);
		}

	}

	spi_context_cs_control(&data->ctx, false);

out:
	spi_context_release(&data->ctx, ret);

	return ret;
}


static int spi_cadence_transceive(struct device *dev,
                      const struct spi_config *config,
                             const struct spi_buf_set *tx_bufs,
                             const struct spi_buf_set *rx_bufs)
{
        SYS_LOG_DBG("%p, %p, %p", dev, tx_bufs, rx_bufs);

        return transceive(dev, config, tx_bufs, rx_bufs, false, NULL);
}

#ifdef CONFIG_SPI_ASYNC
static int spi_cadence_transceive_async(struct device *dev,
                                   const struct spi_config *config,
                                   const struct spi_buf_set *tx_bufs,
                                   const struct spi_buf_set *rx_bufs,
                                   struct k_poll_signal *async)
{
        SYS_LOG_DBG("%p, %p, %p, %p", dev, tx_bufs, rx_bufs, async);

        return transceive(dev, config, tx_bufs, rx_bufs, true, async);
}
#endif /* CONFIG_SPI_ASYNC */

static int spi_cadence_release(struct device *dev,
			    const struct spi_config *config)
{
	struct spi_cadence_data *data = dev->driver_data;

	printk("released\n");
	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static int spi_cadence_init(struct device *dev)
{
	struct spi_cadence_data *data = dev->driver_data;

	spi_context_unlock_unconditionally(&data->ctx);

	/* The device will be configured and enabled when transceive
	 * is called.
	 */
	return 0;
}


static const struct spi_driver_api spi_cadence_driver_api = {
	.transceive = spi_cadence_transceive,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = spi_cadence_transceive_async,
#endif
	.release = spi_cadence_release,
};

#ifdef CONFIG_SPI_0

struct spi_cadence_data spi_cadence_data_port_0 = {
	SPI_CONTEXT_INIT_LOCK(spi_cadence_data_port_0, ctx),
	SPI_CONTEXT_INIT_SYNC(spi_cadence_data_port_0, ctx),
};

const struct spi_cadence_config spi_cadence_config_0 = {
	.regs = (volatile u32_t *)CADENCE_SPI_0_BASE_ADDRESS,
};

DEVICE_AND_API_INIT(spi_cadence_port_0, CADENCE_SPI_0_LABEL, spi_cadence_init,
		    &spi_cadence_data_port_0, &spi_cadence_config_0,
		    POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,
		    &spi_cadence_driver_api);

#endif /* CONFIG_SPI_0 */

#ifdef CONFIG_SPI_1

struct spi_cadence_data spi_cadence_data_port_1 = {
	SPI_CONTEXT_INIT_LOCK(spi_cadence_data_port_1, ctx),
	SPI_CONTEXT_INIT_SYNC(spi_cadence_data_port_1, ctx),
};

const struct spi_cadence_config spi_cadence_config_1 = {
	.regs = (volatile u32_t *)CADENCE_SPI_1_BASE_ADDRESS,
};

DEVICE_AND_API_INIT(spi_cadence_port_1, CADENCE_SPI_1_LABEL, spi_cadence_init,
		    &spi_cadence_data_port_1, &spi_cadence_config_1,
		    POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,
		    &spi_cadence_driver_api);

#endif /* CONFIG_SPI_1 */

