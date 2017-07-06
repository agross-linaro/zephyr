/*
 * Copyright (c) 2016 Open-RnD Sp. z o.o.
 * Copyright (c) 2016 Linaro Limited.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Driver for UART port on STM32F10x family processor.
 *
 * Based on reference manual:
 *   STM32F101xx, STM32F102xx, STM32F103xx, STM32F105xx and STM32F107xx
 *   advanced ARM ® -based 32-bit MCUs
 *
 * Chapter 27: Universal synchronous asynchronous receiver
 *             transmitter (USART)
 */

#include <kernel.h>
#include <arch/cpu.h>
#include <misc/__assert.h>
#include <board.h>
#include <init.h>
#include <uart.h>
#include <clock_control.h>

#include <linker/sections.h>
#include <clock_control/stm32_clock_control.h>
#include "uart_stm32.h"
#include <st_stm32_usart_init.h>

/* convenience defines */
#define DEV_CFG(dev)							\
	((const struct st_stm32_usart_config * const)(dev)->config->config_info)
#define DEV_DATA(dev)							\
	((struct st_stm32_usart_data * const)(dev)->driver_data)
#define UART_STRUCT(dev)					\
	((USART_TypeDef *)(DEV_CFG(dev))->uconf.base)

#define TIMEOUT 1000

static int uart_stm32_poll_in(struct device *dev, unsigned char *c)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	if (HAL_UART_Receive(UartHandle, (u8_t *)c, 1, TIMEOUT) == HAL_OK) {
		return 0;
	} else {
		return -1;
	}
}

static unsigned char uart_stm32_poll_out(struct device *dev,
					unsigned char c)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	HAL_UART_Transmit(UartHandle, (u8_t *)&c, 1, TIMEOUT);

	return c;
}

static inline void __uart_stm32_get_clock(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	struct device *clk =
		device_get_binding(STM32_CLOCK_CONTROL_NAME);

	__ASSERT_NO_MSG(clk);

	data->clock = clk;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN

static int uart_stm32_fifo_fill(struct device *dev, const u8_t *tx_data,
				  int size)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;
	u8_t num_tx = 0;

	while ((size - num_tx > 0) && __HAL_UART_GET_FLAG(UartHandle,
		UART_FLAG_TXE)) {
		/* TXE flag will be cleared with byte write to DR register */

		/* Send a character (8bit , parity none) */
#if defined(CONFIG_SOC_SERIES_STM32F1X) || defined(CONFIG_SOC_SERIES_STM32F4X)
		/* Use direct access for F1, F4 until Low Level API is available
		 * Once it is we can remove the if/else
		 */
		UartHandle->Instance->DR = (tx_data[num_tx++] &
					(u8_t)0x00FF);
#else
		LL_USART_TransmitData8(UartHandle->Instance, tx_data[num_tx++]);
#endif
	}

	return num_tx;
}

static int uart_stm32_fifo_read(struct device *dev, u8_t *rx_data,
				  const int size)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;
	u8_t num_rx = 0;

	while ((size - num_rx > 0) && __HAL_UART_GET_FLAG(UartHandle,
		UART_FLAG_RXNE)) {
		/* Clear the interrupt */
		__HAL_UART_CLEAR_FLAG(UartHandle, UART_FLAG_RXNE);

		/* Receive a character (8bit , parity none) */
#if defined(CONFIG_SOC_SERIES_STM32F1X) || defined(CONFIG_SOC_SERIES_STM32F4X)
		/* Use direct access for F1, F4 until Low Level API is available
		 * Once it is we can remove the if/else
		 */
		rx_data[num_rx++] = (u8_t)(UartHandle->Instance->DR &
					(u8_t)0x00FF);
#else
		rx_data[num_rx++] = LL_USART_ReceiveData8(UartHandle->Instance);
#endif
	}
	return num_rx;
}

static void uart_stm32_irq_tx_enable(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	__HAL_UART_ENABLE_IT(UartHandle, UART_IT_TC);
}

static void uart_stm32_irq_tx_disable(struct device *dev)
{
	 struct st_stm32_usart_data *data = DEV_DATA(dev);
	 UART_HandleTypeDef *UartHandle = &data->huart;

	__HAL_UART_DISABLE_IT(UartHandle, UART_IT_TC);
}

static int uart_stm32_irq_tx_ready(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	return __HAL_UART_GET_FLAG(UartHandle, UART_FLAG_TXE);
}

static int uart_stm32_irq_tx_complete(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	return __HAL_UART_GET_FLAG(UartHandle, UART_FLAG_TXE);
}

static void uart_stm32_irq_rx_enable(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	__HAL_UART_ENABLE_IT(UartHandle, UART_IT_RXNE);
}

static void uart_stm32_irq_rx_disable(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	__HAL_UART_DISABLE_IT(UartHandle, UART_IT_RXNE);
}

static int uart_stm32_irq_rx_ready(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	return __HAL_UART_GET_FLAG(UartHandle, UART_FLAG_RXNE);
}

static void uart_stm32_irq_err_enable(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	/* Enable FE, ORE interruptions */
	__HAL_UART_ENABLE_IT(UartHandle, UART_IT_ERR);
	/* Enable Line break detection */
	__HAL_UART_ENABLE_IT(UartHandle, UART_IT_LBD);
	/* Enable parity error interruption */
	__HAL_UART_ENABLE_IT(UartHandle, UART_IT_PE);
}

static void uart_stm32_irq_err_disable(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	/* Disable FE, ORE interruptions */
	__HAL_UART_DISABLE_IT(UartHandle, UART_IT_ERR);
	/* Disable Line break detection */
	__HAL_UART_DISABLE_IT(UartHandle, UART_IT_LBD);
	/* Disable parity error interruption */
	__HAL_UART_DISABLE_IT(UartHandle, UART_IT_PE);
}

static int uart_stm32_irq_is_pending(struct device *dev)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	return __HAL_UART_GET_FLAG(UartHandle, UART_FLAG_TXE | UART_FLAG_RXNE);
}

static int uart_stm32_irq_update(struct device *dev)
{
	return 1;
}

static void uart_stm32_irq_callback_set(struct device *dev,
					uart_irq_callback_t cb)
{
	struct st_stm32_usart_data *data = DEV_DATA(dev);

	data->user_cb = cb;
}

static void st_stm32_usart_isr(void *arg)
{
	struct device *dev = arg;
	struct st_stm32_usart_data *data = DEV_DATA(dev);

	if (data->user_cb) {
		data->user_cb(dev);
	}
}

#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static const struct uart_driver_api st_stm32_usart_api = {
	.poll_in = uart_stm32_poll_in,
	.poll_out = uart_stm32_poll_out,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = uart_stm32_fifo_fill,
	.fifo_read = uart_stm32_fifo_read,
	.irq_tx_enable = uart_stm32_irq_tx_enable,
	.irq_tx_disable = uart_stm32_irq_tx_disable,
	.irq_tx_ready = uart_stm32_irq_tx_ready,
	.irq_tx_complete = uart_stm32_irq_tx_complete,
	.irq_rx_enable = uart_stm32_irq_rx_enable,
	.irq_rx_disable = uart_stm32_irq_rx_disable,
	.irq_rx_ready = uart_stm32_irq_rx_ready,
	.irq_err_enable = uart_stm32_irq_err_enable,
	.irq_err_disable = uart_stm32_irq_err_disable,
	.irq_is_pending = uart_stm32_irq_is_pending,
	.irq_update = uart_stm32_irq_update,
	.irq_callback_set = uart_stm32_irq_callback_set,
#endif	/* CONFIG_UART_INTERRUPT_DRIVEN */
};

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
static int st_stm32_usart_init(struct device *dev)
{
	const struct st_stm32_usart_config *config = DEV_CFG(dev);
	struct st_stm32_usart_data *data = DEV_DATA(dev);
	UART_HandleTypeDef *UartHandle = &data->huart;

	__uart_stm32_get_clock(dev);
	/* enable clock */
	clock_control_on(data->clock,
			(clock_control_subsys_t *)&config->pclken);

	UartHandle->Instance = UART_STRUCT(dev);
	UartHandle->Init.WordLength = UART_WORDLENGTH_8B;
	UartHandle->Init.StopBits = UART_STOPBITS_1;
	UartHandle->Init.Parity = UART_PARITY_NONE;
	UartHandle->Init.HwFlowCtl = UART_HWCONTROL_NONE;
	UartHandle->Init.Mode = UART_MODE_TX_RX;
	UartHandle->Init.OverSampling = UART_OVERSAMPLING_16;

	HAL_UART_Init(UartHandle);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	config->uconf.irq_config_func(dev);
#endif
	return 0;
}

 /*
  * WARNING:
  * Initialisation code for this driver is now generated at compilation
  * Structure information is proided in dts/arm/yaml/st,stm32-usart.yaml
  * Structures will be feed data extracted from dts files
  *
  * Generated file is available here:
  * outdir/<board>/include/generated/st_stm32_usart_init.h
  * Generated code should match the following
  *
  * #ifdef CONFIG_UART_INTERRUPT_DRIVEN
  * static void st_stm32_usart_irq_config_UART_1(struct device *dev);
  * #endif

  * static struct st_stm32_usart_config st_stm32_usart_config_UART_1 = {
  * 	.uconf = {
  * 		.base = (u8_t *)0x40013800,
  * #ifdef CONFIG_UART_INTERRUPT_DRIVEN
  *		.irq_config_func = st_stm32_usart_irq_config_UART_1,
  * #endif
  *		},
  *	.pclken = {
  *		.bus = STM32_CLOCK_BUS_APB2,
  *		.enr = LL_APB2_GRP1_PERIPH_USART1,
  *	},
  *};
  *
  *static struct st_stm32_usart_data st_stm32_usart_data_UART_1 = {
  *    .huart.Init.BaudRate = 115200,
  *};
  *
  *DEVICE_AND_API_INIT(st_stm32_usart_dev_UART_1,  "UART_1",
  *		       &st_stm32_usart_init,
  *		       &st_stm32_usart_data_UART_1, &st_stm32_usart_config_UART_1,
  *		       PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
  *		       &st_stm32_usart_api);
  *
  *#ifdef CONFIG_UART_INTERRUPT_DRIVEN
  *static void st_stm32_usart_irq_config_UART_1(struct device *dev)
  *{
  *    IRQ_CONNECT(37, 0, st_stm32_usart_isr,
  *	        DEVICE_GET(st_stm32_usart_dev_UART_1), 0);
  *   irq_enable(37);
  *}
  *
  */
