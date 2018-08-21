/**
 * Copyright (c) 2018 Linaro, Lmtd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL CONFIG_SYS_LOG_WIFI_LEVEL
#define SYS_LOG_DOMAIN "dev/esp8266"
#include <logging/sys_log.h>

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <misc/printk.h>
#include <zephyr.h>
#include <kernel.h>
#include <device.h>
#include <net/net_if.h>
#include <net/wifi_mgmt.h>
#include <net/net_offload.h>
#include <uart.h>
#include <gpio.h>
#include <net/net_pkt.h>
#include <net/net_if.h>
#include <net/net_l2.h>
#include <net/net_context.h>
#include <net/net_offload.h>
#include <net/wifi_mgmt.h>

#define ESP8266_MAX_CONNECTIONS	4
#define ESP8266_BUF_SIZE	128

static char rx_buf[512];
static size_t rx_last;
static struct wifi_scan_result scan_result[10];
static int scan_count;
static int tried_parse;

enum request_state {
	ESP8266_IDLE = 0,
	ESP8266_POWER_UP,
	ESP8266_SCAN,
	ESP8266_CONNECT,
	ESP8266_DISCONNECT,
	ESP8266_RECEIVE_DATA,
	ESP8266_SEND_DATA,
	ESP8266_GET_IP,
	ESP8266_ECHO,
	ESP8266_GET_MAC,
};

enum request_result {
	ESP8266_OK = 0,
	ESP8266_ERROR,
	ESP8266_FAIL,
	ESP8266_READY,
	ESP8266_IN_PROGRESS,
	ESP8266_CONNECTING,
};

K_SEM_DEFINE(transfer_complete, 0, 1);
K_MUTEX_DEFINE(dev_mutex);

extern char *strtok_r(char *, const char *, char **);

#define EVENT_GOT_IP	 BIT(0)
#define EVENT_CONNECT	 BIT(1)
#define EVENT_DISCONNECT BIT(2)

struct socket_data {
	struct net_context	*context;
	net_context_connect_cb_t	connect_cb;
	net_tcp_accept_cb_t		accept_cb;
	net_context_send_cb_t		send_cb;
	net_context_recv_cb_t		recv_cb;
	void			*connect_data;
	void			*send_data;
	void			*recv_data;
	void			*accept_data;
	struct net_pkt		*rx_pkt;
	struct net_buf		*pkt_buf;
	int			ret;
	struct	k_sem		wait_sem;
	char tmp[16];
};

struct esp8266_data {
	struct net_if *iface;
	unsigned char mac[6];

	/* max sockets */
	struct socket_data socket_data[4];
	u8_t sock_map;

	/* fifos + accounting info */
	u8_t rx_buf[ESP8266_BUF_SIZE];	/* circular due to parsing */
	u8_t tx_buf[ESP8266_BUF_SIZE];
	u8_t rx_head;
	u8_t rx_tail;
	u8_t rx_full;
	size_t tx_head;
	size_t tx_tail;

	struct k_delayed_work work;
	u8_t event;

	int transaction;
	int resp;
	int initialized;
	int connected;

	/* delayed work requests */

	/* device management */
	struct device *uart_dev;
	struct device *gpio_dev;
};

static struct esp8266_data foo_data;

void esp8266_set_request_state(int trans)
{
	k_mutex_lock(&dev_mutex, K_FOREVER);
	if (foo_data.transaction != ESP8266_IDLE) {
		printk("collision - %d, %d\n", trans, foo_data.transaction);
	}
	foo_data.transaction = trans;
	foo_data.resp = ESP8266_IN_PROGRESS;
	k_mutex_unlock(&dev_mutex);
}

void esp8266_clear_request_state(void)
{
	k_mutex_lock(&dev_mutex, K_FOREVER);
	foo_data.transaction = ESP8266_IDLE;
	k_mutex_unlock(&dev_mutex);
}

static int esp8266_get(sa_family_t family,
			enum net_sock_type type,
			enum net_ip_protocol ip_proto,
			struct net_context **context)
{
	int i;
	struct net_context *c = *context;

	if (family != AF_INET) {
		return -1;
	}

	for (i = 0; i < 4 && foo_data.sock_map & BIT(i); i++){}
	if (i >= 4) {
		printk("ran out of sockets\n");
		return 1;
	}

	foo_data.sock_map |= 1 << i;
	c->user_data = (void *)&foo_data.socket_data[i];
	k_sem_init(&foo_data.socket_data[i].wait_sem, 0, 1);
	foo_data.socket_data[i].context = c;

	return 0;
}

static const char type_tcp[] = "TCP";
static const char type_udp[] = "UDP";

static int esp8266_connect(struct net_context *context,
			const struct sockaddr *addr,
			socklen_t addrlen)
{
	int len;
	const char *type;
	int port, id;
	struct socket_data *data = context->user_data;

	if (net_context_get_type(context) == SOCK_STREAM) {
		type = type_tcp;
	} else {
		type = type_udp;
	}

	len = sprintf(foo_data.tx_buf, "AT+CIPSTART=%d,%s,%s,%d\r\n",
			id, type, data->tmp, port);
	foo_data.tx_head = 0;
	foo_data.tx_tail = len;
	esp8266_set_request_state(ESP8266_ECHO);

	uart_irq_rx_enable(foo_data.uart_dev);
	uart_irq_tx_enable(foo_data.uart_dev);
	k_sem_take(&transfer_complete, K_FOREVER);

	return 0;
}

static int esp8266_bind(struct net_context *context,
			const struct sockaddr *addr,
			socklen_t addrlen)
{
	return 0;
}

static int esp8266_put(struct net_context *context)
{
	struct socket_data *data = context->user_data;
	int id = data - foo_data.socket_data;
	foo_data.sock_map &= ~(1 << id);
	return 0;
}

static struct net_offload esp8266_offload = {
	.get            = esp8266_get,
	.bind		= esp8266_bind,
	.listen		= NULL,
	.connect	= NULL,
	.accept		= NULL,
	.send		= NULL,
	.sendto		= NULL,
	.recv		= NULL,
	.put		= esp8266_put,
};

static int esp8266_mgmt_echo(struct device *dev)
{
	int len;

	len = sprintf(foo_data.tx_buf, "ATE0\r\n");
	foo_data.tx_head = 0;
	foo_data.tx_tail = len;
	esp8266_set_request_state(ESP8266_ECHO);

	uart_irq_rx_enable(dev);
	uart_irq_tx_enable(dev);
	k_sem_take(&transfer_complete, K_FOREVER);

	return 0;
};

static int esp8266_mgmt_scan(struct device *dev, scan_result_cb_t cb)
{
	int len;
	int i;


	scan_count = 0;
	tried_parse = 0;
	len = sprintf(foo_data.tx_buf, "AT+CWLAP\r\n");
	foo_data.tx_head = 0;
	foo_data.tx_tail = len;
	esp8266_set_request_state(ESP8266_SCAN);
	uart_irq_rx_enable(foo_data.uart_dev);
	uart_irq_tx_enable(foo_data.uart_dev);

	k_sem_take(&transfer_complete, K_FOREVER);

	for (i = 0; i < scan_count; i++) {
		printk("sent\n");
		cb(foo_data.iface, 0, &scan_result[i]);
		k_sleep(200);
       }

	return 0;
};

void parse_scan()
{
	int index;
	char *parTok, *parSaveTok;
	char *comTok, *comSaveTok;
	char *ssidTok, *ssidSaveTok;

	/* parse contents */
	parTok = strtok_r(rx_buf, "()", &parSaveTok);
	tried_parse++;
	while (parTok) {
		if (strstr(parTok, ",")) {
			comTok = strtok_r(parTok, ",", &comSaveTok);
			index = 0;
			while (comTok) {
				if (index == 0) {
					switch(*comTok) {
						case '0':
						case '1':
						case '5':
						default:
							scan_result[scan_count].security = WIFI_SECURITY_TYPE_NONE;
							break;
						case '2':
						case '3':
						case '4':
							scan_result[scan_count].security = WIFI_SECURITY_TYPE_PSK;
							break;
					};
				}
				/* ssid */
				if (index == 1) {
					ssidTok = strtok_r(comTok, "\"", &ssidSaveTok);
					strncpy(scan_result[scan_count].ssid, ssidTok, strlen(ssidTok)+1);
					scan_result[scan_count].ssid_length = strlen(comTok);
				}
				if (index == 2) {
					scan_result[scan_count].rssi = atoi(comTok);
				}
				if (index == 4) {
					scan_result[scan_count].channel = atoi(comTok);
				}
				comTok = strtok_r(NULL, ",", &comSaveTok);
				index++;
			}
		}
		parTok = strtok_r(NULL, "()", &parSaveTok);
	}
}

static int esp8266_mgmt_connect(struct device *dev,
				   struct wifi_connect_req_params *params)
{
	int len;

	if (params->security == WIFI_SECURITY_TYPE_PSK) {
		len = sprintf(foo_data.tx_buf, "AT+CWJAP_CUR=\"");
		memcpy(&foo_data.tx_buf[len], params->ssid,
		       params->ssid_length);
		len += params->ssid_length;
		len += sprintf(&foo_data.tx_buf[len], "\",\"");
		memcpy(&foo_data.tx_buf[len], params->psk,
		       params->psk_length);
		len += params->ssid_length;
		len += sprintf(&foo_data.tx_buf[len], "\"\r\n");
	} else {
		len = sprintf(foo_data.tx_buf, "AT+CWJAP_CUR=\"");
		memcpy(&foo_data.tx_buf[len], params->ssid,
		       params->ssid_length);
		len += params->ssid_length;
		len += sprintf(&foo_data.tx_buf[len], "\"\r\n");
	}

	foo_data.tx_head = 0;
	foo_data.tx_tail = len;
	esp8266_set_request_state(ESP8266_CONNECT);
	uart_irq_rx_enable(foo_data.uart_dev);
	uart_irq_tx_enable(foo_data.uart_dev);

	k_sem_take(&transfer_complete, K_FOREVER);
	if (foo_data.resp == ESP8266_OK) {
		k_delayed_work_submit(&foo_data.work, 200);
		return 0;
	} else {
		return 1;
	}
}

static int esp8266_mgmt_disconnect(struct device *dev)
{
	int len;

	len = sprintf(foo_data.tx_buf, "AT+CWQAP\r\n");
	foo_data.tx_head = 0;
	foo_data.tx_tail = len;
	esp8266_set_request_state(ESP8266_DISCONNECT);
	uart_irq_tx_enable(foo_data.uart_dev);

	k_sem_take(&transfer_complete, K_FOREVER);
	return 0;
}

static void esp8266_work_handler(struct k_work *work)
{
	int len;

	/* get current IP */
	if (foo_data.event & EVENT_GOT_IP) {
		if (foo_data.transaction |= ESP8266_IDLE) {
			k_delayed_work_submit(&foo_data.work, 500);
		} else {
			esp8266_set_request_state(ESP8266_GET_IP);
			len = sprintf(foo_data.tx_buf, "AT+CIPSTA_CUR?\r\n");
			foo_data.tx_head = 0;
			foo_data.tx_tail = len;
			uart_irq_tx_enable(foo_data.uart_dev);
			k_sem_take(&transfer_complete, 1000);

			foo_data.event &= ~EVENT_GOT_IP;
		}
	}
	if (foo_data.event & EVENT_CONNECT) {
		wifi_mgmt_raise_connect_result_event(foo_data.iface,
			0);
		foo_data.event &= ~EVENT_CONNECT;
		foo_data.connected = 1;
	}
	if (foo_data.event & EVENT_DISCONNECT) {
		wifi_mgmt_raise_disconnect_result_event(foo_data.iface,
			0);
		foo_data.event &= ~EVENT_DISCONNECT;
		foo_data.connected = 0;
	}
}

static void esp8266_iface_init(struct net_if *iface)
{
	int len;

	net_if_set_link_addr(iface, foo_data.mac,
			     sizeof(foo_data.mac),
			     NET_LINK_ETHERNET);

	/* TBD: Pending support for socket offload: */
	iface->if_dev->offload = &esp8266_offload;

	foo_data.iface = iface;
}

static const struct net_wifi_mgmt_offload esp8266_api = {
	.iface_api.init = esp8266_iface_init,
	.scan		= esp8266_mgmt_scan,
	.connect	= esp8266_mgmt_connect,
	.disconnect	= esp8266_mgmt_disconnect,
};

static const char esp8266_resp_ready[] = "ready";

const u8_t *ready_tok = esp8266_resp_ready;

static int check_for_ready(u8_t c)
{
	if (c == *ready_tok) {
		ready_tok++;
		if (*ready_tok == 0) {
			foo_data.resp = ESP8266_READY;
			return 1;
		}
	} else {
		ready_tok = esp8266_resp_ready;
	}
	return 0;
}

void parse_mac(void)
{
	int index;
	char *parTok, *parSaveTok;
	char *colTok, *colSaveTok;

	/* parse contents */
	parTok = strtok_r(rx_buf, ":\"", &parSaveTok);
	while (parTok) {
		if (strstr(parTok, ":")) {
			colTok = strtok_r(parTok, ":", &colSaveTok);
			index = 0;
			while (colTok) {
				foo_data.mac[index] = strtoul(colTok, NULL, 16);
				index++;
				colTok = strtok_r(NULL, ":", &colSaveTok);
			}
		}
		parTok = strtok_r(NULL, "\"", &parSaveTok);
	}
}

void parse_ip(void)
{
	int index;
	char *parTok, *ipTok, *addrTok, *octetTok, *parSaveTok, *octSaveTok;
	struct in_addr addr;

	/* parse contents */
	parTok = strtok_r(rx_buf, ":", &parSaveTok);
	ipTok = strtok_r(NULL, ":", &parSaveTok);
	addrTok = strtok_r(NULL, ":\"", &parSaveTok);
	octetTok = strtok_r(addrTok, ".", &octSaveTok);

	index = 0;
	while(octetTok) {
		addr.s4_addr[index] = atoi(octetTok);
		octetTok = strtok_r(NULL, ".", &octSaveTok);
		index++;
	}

	if (strstr(ipTok, "ip")) {
		net_if_ipv4_addr_add(foo_data.iface, &addr, NET_ADDR_DHCP, 0);
	} else if (strstr(ipTok, "gateway")) {
		net_if_ipv4_set_gw(foo_data.iface, &addr);
	}
}

void scan_line(void)
{
	if (rx_buf[rx_last - 1] == '\n') {
		if (strstr(rx_buf, "OK")) {
			foo_data.resp = ESP8266_OK;
			esp8266_clear_request_state();
			k_sem_give(&transfer_complete);
		} else if (strstr(rx_buf, "ERROR")) {
			foo_data.resp = ESP8266_ERROR;
			esp8266_clear_request_state();
			k_sem_give(&transfer_complete);
		} else if (strstr(rx_buf, "FAIL")) {
			foo_data.resp = ESP8266_FAIL;
			esp8266_clear_request_state();
			k_sem_give(&transfer_complete);
		} else if (strstr(rx_buf, "+CWLAP")) {
			parse_scan();
			scan_count++;
		} else if (strstr(rx_buf, "WIFI GOT IP")) {
			foo_data.event |= EVENT_GOT_IP;
			if (foo_data.transaction == ESP8266_IDLE) {
				k_delayed_work_submit(&foo_data.work, 0);
			}
		} else if (strstr(rx_buf, "WIFI DISCONNECT")) {
			foo_data.event |= EVENT_DISCONNECT;
			k_delayed_work_submit(&foo_data.work, 0);
		} else if (strstr(rx_buf, "WIFI CONNECTED")) {
			foo_data.event |= EVENT_CONNECT;
			k_delayed_work_submit(&foo_data.work, 0);
		} else if (strstr(rx_buf, "+CIPAPMAC_CUR")) {
			parse_mac();
		} else if (strstr(rx_buf, "+CIPSTA_CUR")) {
			parse_ip();
		} else if (strstr(rx_buf, "busy s...")) {
			printk("busy send\n");
		} else if (strstr(rx_buf, "busy p...")) {
			printk("busy p");
		}
		rx_last = 0;
	}
}

static void esp8266_parse_rx(void)
{
	int i;
	u8_t end;

	switch(foo_data.transaction) {
		case ESP8266_POWER_UP:
			/*
			 * board is powering up, discard all input until
			 * after the ready
			 */
			/* look for 'ready' */
			for(; foo_data.rx_head != foo_data.rx_tail;) {
				end = foo_data.rx_buf[foo_data.rx_tail];
				foo_data.rx_tail = (foo_data.rx_tail + 1) &
						(ESP8266_BUF_SIZE - 1);
				if (check_for_ready(end)) {
						foo_data.initialized = 1;
						esp8266_clear_request_state();
						rx_last = 0;
						k_sem_give(&transfer_complete);
						break;
				}
			}
			foo_data.rx_full = 0;
			break;
		case ESP8266_RECEIVE_DATA:
		case ESP8266_SEND_DATA:
		case ESP8266_SCAN:
		case ESP8266_CONNECT:
		case ESP8266_DISCONNECT:
		default:
			for(; foo_data.rx_head != foo_data.rx_tail; i++) {
				end = foo_data.rx_buf[foo_data.rx_tail];
				rx_buf[rx_last++] = end == '\r'? 0: end;
				scan_line();
				foo_data.rx_tail = (foo_data.rx_tail + 1) & (ESP8266_BUF_SIZE - 1);
			}
			foo_data.rx_full = 0;
			break;
	}
}

static void esp8266_uart_isr(struct device *dev)
{
	int count;
	u8_t c;
	int rlen;

	uart_irq_update(dev);

	while(uart_irq_rx_ready(foo_data.uart_dev)){

		if (foo_data.rx_full) {
			/* drain over flow */
			printk("overflow on read - discarding\n");
			uart_fifo_read(dev, &c, 1);
			continue;
		}

		rlen = (foo_data.rx_head >= foo_data.rx_tail) ?
				ESP8266_BUF_SIZE - foo_data.rx_head:
				foo_data.rx_tail - foo_data.rx_head;

		/* read into circular buffer onto head */
		count = uart_fifo_read(dev, &foo_data.rx_buf[foo_data.rx_head],
				       rlen);

		if (count) {
			foo_data.rx_head = (foo_data.rx_head + count) & (ESP8266_BUF_SIZE - 1);
			if (foo_data.rx_head == foo_data.rx_tail) {
				foo_data.rx_full = 1;
			}
		}

		esp8266_parse_rx();
	}

	while(uart_irq_tx_ready(dev)){
		if (foo_data.tx_head == foo_data.tx_tail) {
			uart_irq_tx_disable(dev);
			break;
		}
		/* try to send some data */
		count = uart_fifo_fill(dev, foo_data.tx_buf + foo_data.tx_head,
				       foo_data.tx_tail - foo_data.tx_head);
		foo_data.tx_head += count;
	}
}

static int esp8266_init(struct device *dev)
{
	struct esp8266_data *drv_data = dev->driver_data;
	int len;

	drv_data->uart_dev = device_get_binding(CONFIG_WIFI_ESP8266_UART_DEVICE);

	if (!drv_data->uart_dev) {
		SYS_LOG_ERR("uart device is not found: %s",
			    CONFIG_WIFI_ESP8266_UART_DEVICE);
		return -EINVAL;
	}

        /* We use system workqueue to deal with things outside isr: */
        k_delayed_work_init(&foo_data.work,
                            esp8266_work_handler);
	uart_irq_callback_set(drv_data->uart_dev, esp8266_uart_isr);

	uart_irq_rx_enable(drv_data->uart_dev);
	drv_data->gpio_dev = device_get_binding(CONFIG_WIFI_ESP8266_GPIO_DEVICE);
	if (!drv_data->gpio_dev) {
		SYS_LOG_ERR("gpio device is not found: %s",
			    CONFIG_WIFI_ESP8266_GPIO_DEVICE);
		return -EINVAL;
	}

	gpio_pin_configure(drv_data->gpio_dev, CONFIG_WIFI_ESP8266_GPIO_ENABLE_PIN,
			   GPIO_DIR_OUT);

	/* disable device until we want to configure it*/
	gpio_pin_write(drv_data->gpio_dev, CONFIG_WIFI_ESP8266_GPIO_ENABLE_PIN, 0);

	esp8266_set_request_state(ESP8266_POWER_UP);

	/* enable device and check for ready */
	k_sleep(100);
	gpio_pin_write(drv_data->gpio_dev, CONFIG_WIFI_ESP8266_GPIO_ENABLE_PIN, 1);

	k_sleep(1000);
	k_sem_take(&transfer_complete, 3000);
	if (!foo_data.initialized) {
		SYS_LOG_ERR("esp8266 never became ready\n");
		/* disable device until we want to configure it*/
		gpio_pin_write(drv_data->gpio_dev, CONFIG_WIFI_ESP8266_GPIO_ENABLE_PIN, 0);
		return -EINVAL;
	}

	k_sleep(200);
	esp8266_mgmt_echo(drv_data->uart_dev);
	k_sleep(200);

	len = sprintf(foo_data.tx_buf, "AT+CIPAPMAC_CUR?\r\n");
	foo_data.tx_head = 0;
	foo_data.tx_tail = len;
	esp8266_set_request_state(ESP8266_GET_MAC);
	uart_irq_tx_enable(foo_data.uart_dev);
	k_sem_take(&transfer_complete, 3000);

	printk("ESP8266 initialized\n");

	return 0;
}

NET_DEVICE_OFFLOAD_INIT(esp8266, "ESP8266",
			esp8266_init, &foo_data, NULL,
			CONFIG_WIFI_INIT_PRIORITY, &esp8266_api,
			2048);
