/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Driver for UART on ARM CMSDK APB UART.
 *
 * UART has two wires for RX and TX, and does not provide CTS or RTS.
 */

#include <kernel.h>
#include <arch/cpu.h>
#include <clock_control/arm_clock_control.h>
#include <misc/__assert.h>
#include <board.h>
#include <init.h>
#include <uart.h>
#include <linker/sections.h>

/* UART Registers */
#define UART_DR		(0x00+0x50102000)
#define UART_RSR	(0x04+0x50102000)
#define UART_ECR	(0x04+0x50102000)
#define UART_FR		(0x18+0x50102000)
#define UART_IBRD	(0x24+0x50102000)
#define UART_FBRD	(0x28+0x50102000)
#define UART_LCRH	(0x2c+0x50102000)
#define UART_CR		(0x30+0x50102000)
#define UART_IMSC	(0x38+0x50102000)
#define UART_MIS	(0x40+0x50102000)
#define UART_ICR	(0x44+0x50102000)

/* Flag register */
#define FR_RXFE		0x10	/* Receive FIFO empty */
#define FR_TXFF		0x20	/* Transmit FIFO full */

/* Masked interrupt status register */
#define MIS_RX		0x10	/* Receive interrupt */
#define MIS_TX		0x20	/* Transmit interrupt */

/* Interrupt clear register */
#define ICR_RX		0x10	/* Clear receive interrupt */
#define ICR_TX		0x20	/* Clear transmit interrupt */

/* Line control register (High) */
#define LCRH_WLEN8	0x60	/* 8 bits */
#define LCRH_FEN	0x10	/* Enable FIFO */

/* Control register */
#define CR_UARTEN	0x0001	/* UART enable */
#define CR_TXE		0x0100	/* Transmit enable */
#define CR_RXE		0x0200	/* Receive enable */

/* Interrupt mask set/clear register */
#define IMSC_RX		0x10	/* Receive interrupt mask */
#define IMSC_TX		0x20	/* Transmit interrupt mask */


/* UART Bits */
/* CTRL Register */
#define UART_TX_EN	(1 << 0)
#define UART_RX_EN	(1 << 1)

/* STATE Register */
#define UART_TX_BF	(1 << 0)
#define UART_RX_BF	(1 << 1)

/* Device data structure */
struct uart_cmsdk_apb_dev_data {
	u32_t baud_rate;	/* Baud rate */
};

/* convenience defines */
#define DEV_CFG(dev) \
	((const struct uart_device_config * const)(dev)->config->config_info)
#define DEV_DATA(dev) \
	((struct uart_cmsdk_apb_dev_data * const)(dev)->driver_data)
#define UART_STRUCT(dev) ((DEV_CFG(dev))->regs)

static const struct uart_driver_api uart_cmsdk_apb_driver_api;

/**
 * @brief Set the baud rate
 *
 * This routine set the given baud rate for the UART.
 *
 * @param dev UART device struct
 *
 * @return N/A
 */
static void baudrate_set(struct device *dev)
{
	u32_t divider, remainder, fraction;
	const struct uart_device_config * const dev_cfg = DEV_CFG(dev);
	struct uart_cmsdk_apb_dev_data *const dev_data = DEV_DATA(dev);

	/*
	 * Set baud rate:
	 * IBRD = UART_CLK / (16 * BAUD_RATE)
	 * FBRD = ROUND((64 * MOD(UART_CLK,(16 * BAUD_RATE))) / (16 * BAUD_RATE))
	 */
	divider = dev_cfg->sys_clk_freq / (16 * dev_data->baud_rate);
	remainder = dev_cfg->sys_clk_freq % (16 * dev_data->baud_rate);
	fraction = (8 * remainder / dev_data->baud_rate) >> 1;
	fraction += (8 * remainder / dev_data->baud_rate) & 1;
	sys_write32(divider, UART_IBRD);
	sys_write32(fraction, UART_FBRD);
}

/**
 * @brief Initialize UART channel
 *
 * This routine is called to reset the chip in a quiescent state.
 * It is assumed that this function is called only once per UART.
 *
 * @param dev UART device struct
 *
 * @return 0
 */
static int uart_cmsdk_apb_init(struct device *dev)
{
	sys_write32(0, UART_CR);	/* Disable everything */
	sys_write32(0x07ff, UART_ICR);	/* Clear all interrupt status */

	/* Set baud rate */
	baudrate_set(dev);

	/* Set N, 8, 1, FIFO enable */
	sys_write32((LCRH_WLEN8 | LCRH_FEN), UART_LCRH);

	/* Enable UART */
	sys_write32((CR_RXE | CR_TXE | CR_UARTEN), UART_CR);

	return 0;
}

/**
 * @brief Poll the device for input.
 *
 * @param dev UART device struct
 * @param c Pointer to character
 *
 * @return 0 if a character arrived, -1 if the input buffer if empty.
 */

static int uart_cmsdk_apb_poll_in(struct device *dev, unsigned char *c)
{
	while (sys_read32(UART_FR) & FR_RXFE) {
		;
	}
	*c = sys_read32(UART_DR) & 0xff;

	return 0;
}

/**
 * @brief Output a character in polled mode.
 *
 * Checks if the transmitter is empty. If empty, a character is written to
 * the data register.
 *
 * @param dev UART device struct
 * @param c Character to send
 *
 * @return Sent character
 */
static unsigned char uart_cmsdk_apb_poll_out(struct device *dev,
					     unsigned char c)
{
	while (sys_read32(UART_FR) & FR_TXFF) {
		; /* wait */
	}
	sys_write32((uint32_t)c, UART_DR);

	return c;
}

static const struct uart_driver_api uart_cmsdk_apb_driver_api = {
	.poll_in = uart_cmsdk_apb_poll_in,
	.poll_out = uart_cmsdk_apb_poll_out,
};

#ifdef CONFIG_UART_CMSDK_APB_PORT0

static const struct uart_device_config uart_cmsdk_apb_dev_cfg_0 = {
	.base = (u8_t *)CMSDK_APB_UART0,
	.sys_clk_freq = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
};

static struct uart_cmsdk_apb_dev_data uart_cmsdk_apb_dev_data_0 = {
	.baud_rate = CONFIG_UART_CMSDK_APB_PORT0_BAUD_RATE,
};

DEVICE_AND_API_INIT(uart_cmsdk_apb_0,
		    CONFIG_UART_CMSDK_APB_PORT0_NAME,
		    &uart_cmsdk_apb_init,
		    &uart_cmsdk_apb_dev_data_0,
		    &uart_cmsdk_apb_dev_cfg_0, PRE_KERNEL_1,
		    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_cmsdk_apb_driver_api);
#endif /* CONFIG_UART_CMSDK_APB_PORT0 */

#ifdef CONFIG_UART_CMSDK_APB_PORT1
static const struct uart_device_config uart_cmsdk_apb_dev_cfg_1 = {
	.base = (u8_t *)CMSDK_APB_UART1,
	.sys_clk_freq = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
};

static struct uart_cmsdk_apb_dev_data uart_cmsdk_apb_dev_data_1 = {
	.baud_rate = CONFIG_UART_CMSDK_APB_PORT1_BAUD_RATE,
};

DEVICE_AND_API_INIT(uart_cmsdk_apb_1,
		    CONFIG_UART_CMSDK_APB_PORT1_NAME,
		    &uart_cmsdk_apb_init,
		    &uart_cmsdk_apb_dev_data_1,
		    &uart_cmsdk_apb_dev_cfg_1, PRE_KERNEL_1,
		    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_cmsdk_apb_driver_api);
#endif /* CONFIG_UART_CMSDK_APB_PORT1 */
