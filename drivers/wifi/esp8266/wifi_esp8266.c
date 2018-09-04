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
#include <net/socket.h>
#include <net/net_app.h>
#include <drivers/modem/modem_receiver.h>


#define ESP8266_MAX_CONNECTIONS	5
#define BUF_ALLOC_TIMEOUT K_SECONDS(1)
#define MDM_MAX_DATA_LENGTH	1500

struct cmd_handler {
	const char *cmd;
	u16_t cmd_len;
	void (*func)(struct net_buf **buf, u16_t len);
	int skip_cmd;
};

#define CMD_HANDLER(cmd_, cb_) { \
	.cmd = cmd_, \
	.cmd_len = (u16_t)sizeof(cmd_)-1, \
	.func = on_cmd_ ## cb_ \
}

static u8_t mdm_recv_buf[MDM_MAX_DATA_LENGTH];
static char rx_buf[512];

/* net bufs */
NET_BUF_POOL_DEFINE(esp8266_recv_pool, 30, 128, 0, NULL);
static struct k_delayed_work reset_work;
static struct k_work_q esp8266_workq;

/* RX thread structures */
K_THREAD_STACK_DEFINE(esp8266_rx_stack, 1028);

K_SEM_DEFINE(uart_rx_sem, 0, 1);
/* RX thread work queue */
K_THREAD_STACK_DEFINE(esp8266_workq_stack,
		      1028);

struct k_thread esp8266_rx_thread;

K_SEM_DEFINE(transfer_complete, 0, 1);
K_SEM_DEFINE(sock_sem, 1, 1);
K_MUTEX_DEFINE(dev_mutex);


struct socket_data {
	struct net_context	*context;
	sa_family_t family;
	enum net_sock_type type;
	enum net_ip_protocol ip_proto;
	net_tcp_accept_cb_t		accept_cb;
	net_context_send_cb_t		send_cb;
	net_context_recv_cb_t		recv_cb;

	struct k_work recv_cb_work;
	void *recv_user_data;

	struct sockaddr		src;
	struct sockaddr		dst;
	int connected;
	struct net_pkt		*rx_pkt;
	struct net_buf		*pkt_buf;
	int			ret;
	struct	k_sem		wait_sem;
	char tmp[16];
	/** semaphore */
	struct k_sem sock_send_sem;
	int socket_id;
};

struct esp8266_data {
	struct net_if *iface;
	struct in_addr ip;
	struct in_addr gw;
	struct in_addr netmask;
	u8_t mac[6];

	/* modem stuff */
	struct mdm_receiver_context mdm_ctx;
	int last_error;
	int last_socket_id;

	struct k_delayed_work ip_addr_work;

	/* semaphores */
	struct k_sem response_sem;


	/* max sockets */
	struct socket_data socket_data[ESP8266_MAX_CONNECTIONS];
	u8_t sock_map;

	/* wifi scan callbacks */
	scan_result_cb_t wifi_scan_cb;

	int transaction;
	int resp;
	int initialized;

	int data_id;
	int data_len;

	int debug;
	/* delayed work requests */

	/* device management */
	struct device *uart_dev;
	struct device *gpio_dev;
};

static struct esp8266_data foo_data;

#define MDM_CMD_TIMEOUT		K_SECONDS(5)

/* Send an AT command with a series of response handlers */
static int send_at_cmd(struct socket_data *sock,
			const u8_t *data, int timeout)
{
	int ret;

	foo_data.last_error = 0;

	SYS_LOG_DBG("OUT: [%s]", data);
	mdm_receiver_send(&foo_data.mdm_ctx, data, strlen(data));
	mdm_receiver_send(&foo_data.mdm_ctx, "\r\n", 2);

	if (timeout == K_NO_WAIT) {
		return 0;
	}

	if (!sock) {
		k_sem_reset(&foo_data.response_sem);
		ret = k_sem_take(&foo_data.response_sem, timeout);
	} else {
		k_sem_reset(&sock->sock_send_sem);
		ret = k_sem_take(&sock->sock_send_sem, timeout);
	}

	if (ret == 0) {
		ret = foo_data.last_error;
	} else if (ret == -EAGAIN) {
		ret = -ETIMEDOUT;
	}

	return ret;
}

static int net_buf_find_next_delimiter(struct net_buf *buf, const u8_t d,
	size_t index, size_t len)
{
	struct net_buf *frag = buf;
	u16_t offset = 0;
	size_t n = len - index;

	while (frag && index) {
		if (frag->len > index) {
			offset += index;
			break;
		} else {
			index -= frag->len;
			offset = 0;
			if (!frag->frags) {
				return -1;
			}
			frag = frag->frags;
		}
	}

	while(n && *(frag->data + offset) != d) {
		if (offset == frag->len) {
			if (!frag->frags) {
				return -1;
			}
			frag = frag->frags;
			offset = 0;
		} else {
			offset++;
		}
		n--;
	}

	return len - n;
}

static int net_buf_ncmp(struct net_buf *buf, const u8_t *s2, size_t n)
{
	struct net_buf *frag = buf;
	u16_t offset = 0;

	while ((n > 0) && (*(frag->data + offset) == *s2) && (*s2 != '\0')) {
		if (offset == frag->len) {
			if (!frag->frags) {
				break;
			}
			frag = frag->frags;
			offset = 0;
		} else {
			offset++;
		}

		s2++;
		n--;
	}

	return (n == 0) ? 0 : (*(frag->data + offset) - *s2);
}


static inline void _hexdump(const u8_t *packet, size_t length)
{
	char output[sizeof("xxxxyyyy xxxxyyyy")];
	int n = 0, k = 0;
	u8_t byte;

	while (length--) {
		if (n % 16 == 0) {
			printk(" %08X ", n);
		}

		byte = *packet++;

		printk("%02X ", byte);

		if (byte < 0x20 || byte > 0x7f) {
			output[k++] = '.';
		} else {
			output[k++] = byte;
		}

		n++;
		if (n % 8 == 0) {
			if (n % 16 == 0) {
				output[k] = '\0';
				printk(" [%s]\n", output);
				k = 0;
			} else {
				printk(" ");
			}
		}
	}

	if (n % 16) {
		int i;

		output[k] = '\0';

		for (i = 0; i < (16 - (n % 16)); i++) {
			printk("   ");
		}

		if ((n % 16) < 8) {
			printk(" "); /* one extra delimiter after 8 chars */
		}

		printk(" [%s]\n", output);
	}
}

/* Echo Handler for commands without related sockets */
static void on_cmd_atcmdecho_nosock(struct net_buf **buf, u16_t len)
{
	/* clear last_socket_id */
	foo_data.last_socket_id = 0;
}

static void on_cmd_esp8266_ready(struct net_buf **buf, u16_t len)
{
	/* clear last_socket_id */
	foo_data.last_socket_id = 0;
	k_sem_give(&foo_data.response_sem);
}


static struct socket_data *socket_from_id(int socket_id)
{
	int i;
	struct socket_data *sock = NULL;

	if (socket_id < 1) {
		return NULL;
	}

	for (i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
		if (foo_data.socket_data[i].socket_id == socket_id) {
			sock = &foo_data.socket_data[i];
			break;
		}
	}

	return sock;
}

static void esp8266_ip_addr_work(struct k_work *work)
{
	int ret;

	ret = send_at_cmd(NULL, "AT+CIPSTA_CUR?", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to get ip address information\n");
		return;
	}

	/* update interface addresses */
	net_if_ipv4_set_gw(foo_data.iface, &foo_data.gw);
	net_if_ipv4_set_netmask(foo_data.iface,
			&foo_data.netmask);
	net_if_ipv4_addr_add(foo_data.iface, &foo_data.ip, NET_ADDR_DHCP,
		0);
}

static void on_cmd_ip_addr_get(struct net_buf **buf, u16_t len)
{
		k_delayed_work_submit_to_queue(&esp8266_workq,
			&foo_data.ip_addr_work, K_SECONDS(2));
}

static void on_cmd_send_ready(struct net_buf **buf, u16_t len)
{
}
static void on_cmd_wifi_scan_resp(struct net_buf **buf, u16_t len)
{
	int i;
	char temp[32];
	struct wifi_scan_result result;
	int delimiters[6];
	int slen;

	/* ecn, ssid, rssi, mac, channel, freq */

	delimiters[0] = 1;
	for (i = 1; i < 6; i++) {
		delimiters[i] = net_buf_find_next_delimiter(*buf, ',',
					delimiters[i-1] + 1, len);
		if (delimiters[i] == -1) {
			return;
		}
		delimiters[i]++;
	}

	/* ecn */
	net_buf_linearize(temp, 1, *buf, delimiters[0],
			  delimiters[1] - delimiters[0]);
	if (temp[0] != '0') {
		result.security = WIFI_SECURITY_TYPE_PSK;
	} else {
		result.security = WIFI_SECURITY_TYPE_PSK;
	}

	/* ssid */
	slen = delimiters[2] - delimiters[1] - 3;
	net_buf_linearize(result.ssid, 32, *buf, delimiters[1] + 1,
			  slen);
	result.ssid_length = slen;

	/* rssi */
	slen = delimiters[3] - delimiters[2];
	net_buf_linearize(temp, 32, *buf, delimiters[2], slen);
	temp[slen] = '\0';
	result.rssi = strtol(temp, NULL, 10);

	/* channel */
	slen = delimiters[5] - delimiters[4];
	net_buf_linearize(temp, 32, *buf, delimiters[4], slen);
	temp[slen] = '\0';
	result.channel = strtol(temp, NULL, 10);

	/* issue callback to report scan results */
	if (foo_data.wifi_scan_cb) {
		foo_data.wifi_scan_cb(foo_data.iface, 0, &result);
	}
}

static const char nm_label[] = "netmask";
static const char gw_label[] = "gateway";
static const char ip_label[] = "ip";

static void on_cmd_ip_addr_resp(struct net_buf **buf, u16_t len)
{
	char ip_addr[] = "xxx.xxx.xxx.xxx";
	int d[3];
	int slen;

	d[0] = net_buf_find_next_delimiter(*buf, ':', 0, len);
	d[1] = net_buf_find_next_delimiter(*buf, '\"', d[0] + 1, len);
	d[2] = net_buf_find_next_delimiter(*buf, '\"', d[1] + 1, len);

	slen = d[2] - d[1] - 1;

	net_buf_linearize(ip_addr, 16, *buf, d[0] + 2, slen);
	ip_addr[slen] = '\0';

	if (net_buf_ncmp(*buf, nm_label, strlen(nm_label)) == 0) {
		inet_pton(AF_INET, ip_addr,
			  &foo_data.netmask);
	} else if (net_buf_ncmp(*buf, ip_label, strlen(ip_label)) == 0) {
		inet_pton(AF_INET, ip_addr,
			  &foo_data.ip);
	} else if (net_buf_ncmp(*buf, gw_label, strlen(gw_label)) == 0) {
		inet_pton(AF_INET, ip_addr,
			  &foo_data.gw);
	} else {
		return;
	}
}

static void on_cmd_mac_addr_resp(struct net_buf **buf, u16_t len)
{
	int i;
	char octet[] = "xx";
	for (i = 0; i < 6; i++) {
		net_buf_pull_u8(*buf);
		octet[0]= net_buf_pull_u8(*buf);
		octet[1]= net_buf_pull_u8(*buf);
		foo_data.mac[i] = strtoul(octet, NULL, 16);
	}

	net_if_set_link_addr(foo_data.iface, foo_data.mac,
			     sizeof(foo_data.mac),
			     NET_LINK_ETHERNET);

	atomic_set_bit(foo_data.iface->if_dev->flags, NET_IF_UP);
}

static void on_cmd_sock_send_ready(struct net_buf **buf, u16_t len)
{
	foo_data.last_error = 0;
	k_sem_give(&foo_data.response_sem);
}

static void on_cmd_receive(struct net_buf **buf, u16_t len)
{
}

static void on_cmd_sendok(struct net_buf **buf, u16_t len)
{
	foo_data.last_error = 0;
	k_sem_give(&foo_data.response_sem);
}

/* Handler: OK */
static void on_cmd_sockok(struct net_buf **buf, u16_t len)
{
	struct socket_data *sock = NULL;

	foo_data.last_error = 0;
	sock = socket_from_id(foo_data.last_socket_id);
	if (!sock) {
		k_sem_give(&foo_data.response_sem);
	} else {
		k_sem_give(&sock->sock_send_sem);
	}
}

/* Handler: ERROR */
static void on_cmd_sockerror(struct net_buf **buf, u16_t len)
{
	struct socket_data *sock = NULL;

	foo_data.last_error = -EIO;
	sock = socket_from_id(foo_data.last_socket_id);
	if (!sock) {
		k_sem_give(&foo_data.response_sem);
	} else {
		k_sem_give(&sock->sock_send_sem);
	}
}

static void on_cmd_wifi_connected_resp(struct net_buf **buf, u16_t len)
{
	wifi_mgmt_raise_connect_result_event(foo_data.iface, 0);
}

static void on_cmd_wifi_disconnected_resp(struct net_buf **buf, u16_t len)
{
	wifi_mgmt_raise_disconnect_result_event(foo_data.iface, 0);
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

	k_sem_take(&sock_sem, K_FOREVER);
	for (i = 0; i < ESP8266_MAX_CONNECTIONS && foo_data.sock_map & BIT(i); i++){}
	if (i >= ESP8266_MAX_CONNECTIONS) {
		k_sem_give(&sock_sem);
		return 1;
	}

	foo_data.sock_map |= 1 << i;
	c->offload_context = (void *)&foo_data.socket_data[i];
	k_sem_init(&foo_data.socket_data[i].wait_sem, 0, 1);
	foo_data.socket_data[i].context = c;
	foo_data.socket_data[i].family = family;
	foo_data.socket_data[i].type = type;
	foo_data.socket_data[i].ip_proto = ip_proto;

	k_sem_give(&sock_sem);
	return 0;
}

static const char type_tcp[] = "TCP";
static const char type_udp[] = "UDP";

static int esp8266_connect(struct net_context *context,
			const struct sockaddr *addr,
			socklen_t addrlen,
			net_context_connect_cb_t cb,
			s32_t timeout,
			void *user_data)
{
	char connect_msg[100];
	int len;
	const char *type;
	s32_t timeout_sec = timeout / MSEC_PER_SEC;
	struct socket_data *sock;
	char addr_str[32];
	int ret = -EFAULT;

	if (!context || !addr) {
		return -EINVAL;
	}

	sock = (struct socket_data *)context->offload_context;
	if (!sock) {
		SYS_LOG_ERR("Can't find socket info for ctx: %p\n", context);
		return -EINVAL;
	}

	sock = (struct socket_data *)context->offload_context;
	sock->dst.sa_family = addr->sa_family;
#if defined (CONFIG_NET_IPV4)
	if (addr->sa_family == AF_INET) {
		net_ipaddr_copy(&net_sin(&sock->dst)->sin_addr,
				&net_sin(addr)->sin_addr);
		net_sin(&sock->dst)->sin_port = net_sin(addr)->sin_port;
	} else
#endif
	{
		return -EINVAL;
	}

	if (net_sin(&sock->dst)->sin_port < 0) {
		SYS_LOG_ERR("invalid port: %d\n",
			    net_sin(&sock->dst)->sin_port);
		return -EINVAL;
	}

	if (net_context_get_type(context) == SOCK_STREAM) {
		type = type_tcp;
	} else {
		type = type_udp;
	}

	inet_ntop(sock->dst.sa_family, &net_sin(&sock->dst)->sin_addr,
			     addr_str, sizeof(addr_str));
	len = sprintf(connect_msg, "AT+CIPSTART=%d,\"%s\",\"%s\",%d",
			sock - foo_data.socket_data, type, addr_str,
			net_sin(&sock->dst)->sin_port);

	ret = send_at_cmd(NULL, connect_msg, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to send connect\n");
		ret = -EINVAL;
	}

	if (cb) {
		cb(context, ret, user_data);
	}

	return 0;
}

static int esp8266_bind(struct net_context *context,
			const struct sockaddr *addr,
			socklen_t addrlen)
{
	struct socket_data *sock = NULL;

	if (!context) {
		return -EINVAL;
	}

	sock = (struct socket_data *)context->offload_context;
	if (!sock) {
		SYS_LOG_ERR("Missing socket for ctx: %p\n", context);
		return -EINVAL;
	}

	sock->src.sa_family = addr->sa_family;
#if defined (CONFIG_NET_IPV4)
	if (addr->sa_family == AF_INET) {
		net_ipaddr_copy(&net_sin(&sock->src)->sin_addr,
				&net_sin(addr)->sin_addr);
		net_sin(&sock->src)->sin_port = net_sin(addr)->sin_port;
	} else
#endif
	{
		return -EPFNOSUPPORT;
	}
	return 0;
}

static int esp8266_listen(struct net_context *context, int backlog)
{
	return -EPFNOSUPPORT;
}

static int esp8266_accept(struct net_context *context,
			net_tcp_accept_cb_t cb,
			s32_t timeout,
			void *user_data)
{
	return -EPFNOSUPPORT;
}

static u8_t send_msg[32];

static int esp8266_send(struct net_pkt *pkt,
		net_context_send_cb_t cb,
		s32_t timeout,
		void *token,
		void *user_data)
{
	struct net_context *context = net_pkt_context(pkt);
	struct socket_data *data;
	struct net_buf *frag;
	int id, len;
	int ret = -EFAULT;

	if (!context || !context->offload_context) {
		return -EINVAL;
	}

	data = context->offload_context;
	id = data - foo_data.socket_data;

	frag = pkt->frags;
	len = sprintf(send_msg, "AT+CIPSEND=%d,%d\r\n", id,
		      net_buf_frags_len(frag));

	ret = send_at_cmd(NULL, send_msg, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to send send\n");
		ret = -EINVAL;
	}

	while (frag) {
		mdm_receiver_send(&foo_data.mdm_ctx, frag->data,
				  frag->len);
		frag = frag->frags;
	}

	k_sem_reset(&foo_data.response_sem);
	ret = k_sem_take(&foo_data.response_sem, MDM_CMD_TIMEOUT);

	if (ret == 0) {
		ret = foo_data.last_error;
	} else if (ret == -EAGAIN) {
		ret = -ETIMEDOUT;
	}

	net_pkt_unref(pkt);
	if (cb) {
		cb(context, ret, token, user_data);
	}

	return 0;
}

static int esp8266_put(struct net_context *context)
{
	struct socket_data *data;
	u8_t msg[20];
	int len;

	if (!context || !context->offload_context) {
		return -EINVAL;
	}

	data = context->offload_context;
	int id = data - foo_data.socket_data;
	foo_data.sock_map &= ~(1 << id);

	foo_data.socket_data[id].recv_cb = NULL;
	foo_data.socket_data[id].send_cb = NULL;
	foo_data.socket_data[id].accept_cb = NULL;

	if (foo_data.socket_data[id].connected) {
		len = sprintf(msg, "AT+CIPCLOSE=%d", id);
		msg[len] = '\0';

		if (send_at_cmd(NULL, msg, MDM_CMD_TIMEOUT) < 0) {
			SYS_LOG_ERR("failed to close\n");
		}
	}

	net_context_unref(context);

	data->context = NULL;
	memset(&data->src, 0, sizeof(struct sockaddr));
	memset(&data->dst, 0, sizeof(struct sockaddr));
	return 0;
}

static int esp8266_sendto(struct net_pkt *pkt,
		const struct sockaddr *dst_addr,
		socklen_t addrlen,
		net_context_send_cb_t cb,
		s32_t timeout,
		void *token,
		void *user_data)
{

	return 0;
}

static int esp8266_recv(struct net_context *context,
		net_context_recv_cb_t cb,
		s32_t timeout,
		void *user_data)
{
	struct socket_data *data;

	if (!context || !context->offload_context) {
		return -EINVAL;
	}

	data = context->offload_context;

	data->recv_cb = cb;
	data->recv_user_data = user_data;

	return 0;
}

static struct net_offload esp8266_offload = {
	.get            = esp8266_get,
	.bind		= esp8266_bind,
	.listen		= esp8266_listen,
	.connect	= esp8266_connect,
	.accept		= esp8266_accept,
	.send		= esp8266_send,
	.sendto		= esp8266_sendto,
	.recv		= esp8266_recv,
	.put		= esp8266_put,
};

static int esp8266_mgmt_scan(struct device *dev, scan_result_cb_t cb)
{
	int ret;

	foo_data.wifi_scan_cb = cb;

	ret = send_at_cmd(NULL, "AT+CWLAP", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to send scan\n");
		ret = -EINVAL;
	}

	foo_data.wifi_scan_cb = NULL;
	return 0;
};

static u8_t connect_msg[100];

static int esp8266_mgmt_connect(struct device *dev,
				   struct wifi_connect_req_params *params)
{
	int len;
	int ret;

	if (params->security == WIFI_SECURITY_TYPE_PSK) {
		len = sprintf(connect_msg, "AT+CWJAP_CUR=\"");
		memcpy(&connect_msg[len], params->ssid,
		       params->ssid_length);
		len += params->ssid_length;
		len += sprintf(&connect_msg[len], "\",\"");
		memcpy(&connect_msg[len], params->psk,
		       params->psk_length);
		len += params->psk_length;
		len += sprintf(&connect_msg[len], "\"");
		connect_msg[len] = '\0';
	} else {
		len = sprintf(connect_msg, "AT+CWJAP_CUR=\"");
		memcpy(&connect_msg[len], params->ssid,
		       params->ssid_length);
		len += params->ssid_length;
		len += sprintf(&connect_msg[len], "\"");
		connect_msg[len] = '\0';
	}

	ret = send_at_cmd(NULL, connect_msg, MDM_CMD_TIMEOUT * 2);
	if (ret < 0) {
		SYS_LOG_ERR("failed to send scan\n");
		return -EINVAL;
	}

	return 0;
}

static void esp8266_get_mac_addr(void)
{
	int ret;

	ret = send_at_cmd(NULL, "AT+CIPAPMAC_CUR?", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to query mac address\n");
		return;
	}
}


static int esp8266_mgmt_disconnect(struct device *dev)
{
	int ret;

	ret = send_at_cmd(NULL, "AT+CWQAP", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to query mac address\n");
		return;
	}

	return 0;
}

static void esp8266_iface_init(struct net_if *iface)
{
	atomic_clear_bit(iface->if_dev->flags, NET_IF_UP);

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

static inline struct net_buf *read_rx_allocator(s32_t timeout, void *user_data)
{
	return net_buf_alloc((struct net_buf_pool *)user_data, timeout);
}

static void esp8266_read_rx(struct net_buf **buf)
{
	u8_t uart_buffer[128];
	size_t bytes_read = 0;
	int ret;
	u16_t rx_len;

	/* read all of the data from mdm_receiver */
	while (true) {
		ret = mdm_receiver_recv(&foo_data.mdm_ctx,
					uart_buffer,
					sizeof(uart_buffer),
					&bytes_read);
		if (ret < 0) {
			/* mdm_receiver buffer is empty */
			break;
		}
		_hexdump(uart_buffer, bytes_read);

		/* make sure we have storage */
		if (!*buf) {
			*buf = net_buf_alloc(&esp8266_recv_pool, BUF_ALLOC_TIMEOUT);
			if (!*buf) {
				SYS_LOG_ERR("Can't allocate RX data! "
					    "Skipping data!");
				break;
			}
		}

		rx_len = net_buf_append_bytes(*buf, bytes_read, uart_buffer,
					      BUF_ALLOC_TIMEOUT,
					      read_rx_allocator,
					      &esp8266_recv_pool);
		if (rx_len < bytes_read) {
			SYS_LOG_ERR("Data was lost! read %u of %u!",
				    rx_len, bytes_read);
		}
	}
}


static bool is_crlf(u8_t c)
{
	if (c == '\n' || c == '\r') {
		return true;
	} else {
		return false;
	}
}

static void net_buf_skipcrlf(struct net_buf **buf)
{
	/* chop off any /n or /r */
	while (*buf && is_crlf(*(*buf)->data)) {
		net_buf_pull_u8(*buf);
		if (!(*buf)->len) {
			*buf = net_buf_frag_del(NULL, *buf);
		}
	}
}

static u16_t net_buf_findcrlf(struct net_buf *buf, struct net_buf **frag,
			      u16_t *offset)
{
	u16_t len = 0, pos = 0;

	while (buf && !is_crlf(*(buf->data + pos))) {
		if (pos + 1 >= buf->len) {
			len += buf->len;
			buf = buf->frags;
			pos = 0;
		} else {
			pos++;
		}
	}

	if (buf && is_crlf(*(buf->data + pos))) {
		len += pos;
		*offset = pos;
		*frag = buf;
		return len;
	}

	return 0;
}
/* Setup IP header data to be used by some network applications.
 * While much is dummy data, some fields such as dst, port and family are
 * important.
 * Return the IP + protocol header length.
 */
static int net_pkt_setup_ip_data(struct net_pkt *pkt,
	enum net_ip_protocol proto,
	struct sockaddr *src,
	struct sockaddr *dst)
{
	int hdr_len = 0;
	u16_t src_port = 0, dst_port = 0;

#if defined(CONFIG_NET_IPV6)
	if (net_pkt_family(pkt) == AF_INET6) {
		net_buf_add(pkt->frags, NET_IPV6H_LEN);

		/* set IPv6 data */
		NET_IPV6_HDR(pkt)->vtc = 0x60;
		NET_IPV6_HDR(pkt)->tcflow = 0;
		NET_IPV6_HDR(pkt)->flow = 0;
		net_ipaddr_copy(&NET_IPV6_HDR(pkt)->src,
			&((struct sockaddr_in6 *)dst)->sin6_addr);
		net_ipaddr_copy(&NET_IPV6_HDR(pkt)->dst,
			&((struct sockaddr_in6 *)src)->sin6_addr);
		NET_IPV6_HDR(pkt)->nexthdr = proto;

		src_port = net_sin6(&dst)->sin6_port;
		dst_port = net_sin6(&src)->sin6_port;

		net_pkt_set_ip_hdr_len(pkt, sizeof(struct net_ipv6_hdr));
		net_pkt_set_ipv6_ext_len(pkt, 0);
		hdr_len = sizeof(struct net_ipv6_hdr);
	} else
#endif
#if defined(CONFIG_NET_IPV4)
	if (net_pkt_family(pkt) == AF_INET) {
		net_buf_add(pkt->frags, NET_IPV4H_LEN);

		/* set IPv4 data */
		NET_IPV4_HDR(pkt)->vhl = 0x45;
		NET_IPV4_HDR(pkt)->tos = 0x00;
		net_ipaddr_copy(&NET_IPV4_HDR(pkt)->src,
			&((struct sockaddr_in *)dst)->sin_addr);
		net_ipaddr_copy(&NET_IPV4_HDR(pkt)->dst,
			&((struct sockaddr_in *)src)->sin_addr);
		NET_IPV4_HDR(pkt)->proto = proto;

		src_port = net_sin(dst)->sin_port;
		dst_port = net_sin(src)->sin_port;

		net_pkt_set_ip_hdr_len(pkt, sizeof(struct net_ipv4_hdr));
		hdr_len = sizeof(struct net_ipv4_hdr);
	} else
#endif
	{
		/* no error here as hdr_len is checked later for 0 value */
	}

#if defined(CONFIG_NET_UDP)
	if (proto == IPPROTO_UDP) {
		struct net_udp_hdr hdr, *udp;

		net_buf_add(pkt->frags, NET_UDPH_LEN);
		udp = net_udp_get_hdr(pkt, &hdr);
		memset(udp, 0, NET_UDPH_LEN);

		/* Setup UDP header */
		udp->src_port = src_port;
		udp->dst_port = dst_port;
		net_udp_set_hdr(pkt, udp);
		hdr_len += NET_UDPH_LEN;
	} else
#endif
#if defined(CONFIG_NET_TCP)
	if (proto == IPPROTO_TCP) {
		struct net_tcp_hdr hdr, *tcp;

		net_buf_add(pkt->frags, NET_TCPH_LEN);
		tcp = net_tcp_get_hdr(pkt, &hdr);
		memset(tcp, 0, NET_TCPH_LEN);

		/* Setup TCP header */
		tcp->src_port = src_port;
		tcp->dst_port = dst_port;
		net_tcp_set_hdr(pkt, tcp);
		hdr_len += NET_TCPH_LEN;
	} else
#endif /* CONFIG_NET_TCP */
	{
		/* no error here as hdr_len is checked later for 0 value */
	}

	return hdr_len;
}

static struct net_buf * esp8266_read_data(struct net_buf *buf)
{
	struct net_buf *frag = buf;
	struct socket_data *sock = &foo_data.socket_data[foo_data.data_id];
	int pos;

	if (!sock->rx_pkt) {
		pos = net_buf_frags_len(buf);
		if (pos > foo_data.data_len) {
			frag = net_buf_skip(buf, foo_data.data_len);
			foo_data.data_len = 0;
		} else {
			frag = net_buf_skip(buf, pos);
			foo_data.data_len -= pos;
		}
		return frag;
	}

	while (frag && foo_data.data_len) {
		if (frag->len > foo_data.data_len) {
			pos = net_pkt_append(sock->rx_pkt, foo_data.data_len,
				frag->data, BUF_ALLOC_TIMEOUT);
			if (pos != foo_data.data_len) {
				SYS_LOG_ERR("unable to add data\n");
				net_pkt_unref(sock->rx_pkt);
				sock->rx_pkt = NULL;
				break;
			}
			foo_data.data_len = 0;
			net_buf_skip(frag, foo_data.data_len);
		} else {
			pos = net_pkt_append(sock->rx_pkt, frag->len, frag->data,
				BUF_ALLOC_TIMEOUT);
			if (pos != frag->len) {
				SYS_LOG_ERR("unable to add data\n");
				net_pkt_unref(sock->rx_pkt);
				sock->rx_pkt = NULL;
				break;
			}
			foo_data.data_len -= frag->len;
			frag = net_buf_skip(frag, frag->len);
		}
	}

	if (foo_data.data_len == 0) {
		if (sock->rx_pkt) {
			k_work_submit_to_queue(&esp8266_workq, &sock->recv_cb_work);
		}
	}
	return frag;
}

static void esp8266_process_setup_read(struct net_buf **buf, int end)
{
	struct socket_data *sock;
	struct net_buf *frag, *rbuf;
	u8_t temp[32];
	u16_t pos;
	int d[6];
	int i;
	int slen;
	int len = net_buf_frags_len(*buf);
	int hdr_len;

	d[0] = net_buf_find_next_delimiter(*buf, ',', 0, end);
	d[1] = net_buf_find_next_delimiter(*buf, ',', d[0] + 1, end);
	d[2] = net_buf_find_next_delimiter(*buf, ',', d[1] + 1, end);

	slen = d[1] - d[0] - 1;
	net_buf_linearize(temp, 32, *buf, d[0] + 1, slen);
	temp[slen] = '\0';
	foo_data.data_id = strtoul(temp, NULL, 10);

	slen = end - d[1] - 1;
	net_buf_linearize(temp, 32, *buf, d[1] + 1, slen);
	temp[slen] = '\0';
	foo_data.data_len = strtoul(temp, NULL, 10);
	*buf = net_buf_skip(*buf, end + 1);

	printk("MATCH +IPD (len:%u)\n", foo_data.data_len + end + 1);

	sock = &foo_data.socket_data[foo_data.data_id];
	sock->rx_pkt = net_pkt_get_rx(sock->context, BUF_ALLOC_TIMEOUT);
	if (!sock->rx_pkt) {
		printk("Failed to get net pkt\n");
		return;
	}

	/* set up packet data */
	net_pkt_set_context(sock->rx_pkt, sock->context);
	net_pkt_set_family(sock->rx_pkt, sock->family);

	frag = net_pkt_get_frag(sock->rx_pkt, BUF_ALLOC_TIMEOUT);
	if (!frag) {
		printk("Failed to get frag\n");
		net_pkt_unref(sock->rx_pkt);
		sock->rx_pkt = NULL;
		return;
	}

	net_pkt_frag_add(sock->rx_pkt, frag);
	net_pkt_set_appdatalen(sock->rx_pkt, foo_data.data_len);

	hdr_len = net_pkt_setup_ip_data(sock->rx_pkt, sock->ip_proto,
			&sock->src, &sock->dst);
	if (hdr_len > 0) {
		frag = net_frag_get_pos(sock->rx_pkt, hdr_len, &pos);
		NET_ASSERT(frag);
		net_pkt_set_appdata(sock->rx_pkt, frag->data + pos);
	} else {
		net_pkt_set_appdata(sock->rx_pkt,
				    sock->rx_pkt->frags->data);
	}

	return;
}

static void sockreadrecv_cb_work(struct k_work *work)
{
	struct socket_data *sock = NULL;
	struct net_pkt *pkt;

	sock = CONTAINER_OF(work, struct socket_data, recv_cb_work);

	/* return data */
	pkt = sock->rx_pkt;
	sock->rx_pkt = NULL;
	if (sock->recv_cb) {
		sock->recv_cb(sock->context, pkt, 0, sock->recv_user_data);
	} else {
		net_pkt_unref(pkt);
	}
}

/* RX thread */
static void esp8266_rx(void)
{
	struct net_buf *rx_buf = NULL;
	struct net_buf *frag = NULL;
	int i;
	u16_t offset, len;

	static const struct cmd_handler handlers[] = {
		CMD_HANDLER("AT+RST", atcmdecho_nosock),
		CMD_HANDLER("ATE1", atcmdecho_nosock),
		CMD_HANDLER("OK\r\n>", sock_send_ready),
		CMD_HANDLER("OK", sockok),
		CMD_HANDLER("ERROR", sockerror),
		CMD_HANDLER("FAIL", sockerror),
		CMD_HANDLER("WIFI GOT IP", ip_addr_get),
		CMD_HANDLER("AT+CWJAP_CUR=", atcmdecho_nosock),
		CMD_HANDLER("WIFI CONNECTED", wifi_connected_resp),
		CMD_HANDLER("WIFI DISCONNECT",wifi_disconnected_resp),
		CMD_HANDLER("SEND OK", sendok),
		CMD_HANDLER("link is not valid", atcmdecho_nosock),
		CMD_HANDLER("busy p...", atcmdecho_nosock),
		CMD_HANDLER("busy s...", atcmdecho_nosock),
		CMD_HANDLER("ready", esp8266_ready),
		CMD_HANDLER("AT+CIPAPMAC_CUR?", atcmdecho_nosock),
		CMD_HANDLER("+CIPAPMAC_CUR:", mac_addr_resp),
		CMD_HANDLER("AT+CIPSTA_CUR?", atcmdecho_nosock),
		CMD_HANDLER("+CIPSTA_CUR:", ip_addr_resp),
		CMD_HANDLER("AT+CWLAP", atcmdecho_nosock),
		CMD_HANDLER("+CWLAP:", wifi_scan_resp),
		CMD_HANDLER("+CWLAP:", wifi_scan_resp),
	//	CMD_HANDLER("+IPD", receive),
		CMD_HANDLER("AT+CIPSEND=", atcmdecho_nosock),
		CMD_HANDLER("0,CONNECT", atcmdecho_nosock),
		CMD_HANDLER("1,CONNECT", atcmdecho_nosock),
		CMD_HANDLER("2,CONNECT", atcmdecho_nosock),
		CMD_HANDLER("3,CONNECT", atcmdecho_nosock),
		CMD_HANDLER("4,CONNECT", atcmdecho_nosock),
		CMD_HANDLER("0,CLOSED", atcmdecho_nosock),
		CMD_HANDLER("1,CLOSED", atcmdecho_nosock),
		CMD_HANDLER("2,CLOSED", atcmdecho_nosock),
		CMD_HANDLER("3,CLOSED", atcmdecho_nosock),
		CMD_HANDLER("4,CLOSED", atcmdecho_nosock),
	};

	while (true) {
		k_sem_take(&foo_data.mdm_ctx.rx_sem, K_FOREVER);

		esp8266_read_rx(&rx_buf);

		while (rx_buf) {

			if (foo_data.data_len) {
				rx_buf = esp8266_read_data(rx_buf);
			}

			net_buf_skipcrlf(&rx_buf);
			if (!rx_buf) {
				break;
			}

			/* check for incoming data */
			if (net_buf_ncmp(rx_buf, "+IPD,", 5) == 0) {
				i = net_buf_find_next_delimiter(rx_buf, ':',
					0, net_buf_frags_len(rx_buf));
				if (i < 0) {
					continue;
				}
				esp8266_process_setup_read(&rx_buf, i);
				rx_buf = esp8266_read_data(rx_buf);
				if (!rx_buf) {
					break;
				}
				continue;
			}

			frag = NULL;
			len = net_buf_findcrlf(rx_buf, &frag, &offset);
			if (!frag) {
				break;
			}

			/* look for matching data handlers */
			i = -1;
			for (i = 0; i < ARRAY_SIZE(handlers); i++) {
				if (net_buf_ncmp(rx_buf, handlers[i].cmd,
						 handlers[i].cmd_len) == 0) {
					/* found a matching handler */
					printk("MATCH %s (len:%u)\n",
						    handlers[i].cmd, len);

					/* skip cmd_len */
					rx_buf = net_buf_skip(rx_buf,
							handlers[i].cmd_len);

					/* locate next cr/lf */
					frag = NULL;
					len = net_buf_findcrlf(rx_buf,
							       &frag, &offset);

#if 0
					if (!frag) {
						printk("skipping %s\n",
handlers[i].cmd);
						break;
					}
#endif

					/* call handler */
					if (handlers[i].func) {
						handlers[i].func(&rx_buf, len);
					}

					frag = NULL;
					/* make sure buf still has data */
					if (!rx_buf) {
						break;
					}

					/* locate next cr/lf */
					len = net_buf_findcrlf(rx_buf,
							       &frag, &offset);
					if (!frag) {
						break;
					}

					break;
				}
			}

			if (frag && rx_buf) {
				/* clear out processed line (buffers) */
				while (frag && rx_buf != frag) {
					rx_buf = net_buf_frag_del(NULL, rx_buf);
				}

				net_buf_pull(rx_buf, offset);
			}
		}

		/* give up time if we have a solid stream of data */
		k_yield();
	}
}

#if defined(CONFIG_WIFI_ESP8266_HAS_ENABLE_PIN)
static void esp8266_gpio_reset(void)
{
	struct device *gpio_dev = device_get_binding(CONFIG_WIFI_ESP8266_GPIO_DEVICE);

	if (!gpio_dev) {
		SYS_LOG_ERR("gpio device is not found: %s",
			    CONFIG_WIFI_ESP8266_GPIO_DEVICE);
	}

	gpio_pin_configure(gpio_dev, CONFIG_WIFI_ESP8266_GPIO_ENABLE_PIN,
			   GPIO_DIR_OUT);

	/* disable device until we want to configure it*/
	gpio_pin_write(drv_data->gpio_dev, CONFIG_WIFI_ESP8266_GPIO_ENABLE_PIN, 0);

	/* enable device and check for ready */
	k_sleep(100);
	gpio_pin_write(drv_data->gpio_dev, CONFIG_WIFI_ESP8266_GPIO_ENABLE_PIN, 1);
}
#endif

static void esp8266_reset_work(struct k_work *work)
{
	int ret = -1;
	int retry_count = 3;

#if defined(CONFIG_WIFI_ESP8266_HAS_ENABLE_PIN)
	esp8266_gpio_reset();
#else
	/* send AT+RST command */
	while(retry_count-- && ret < 0) {
		k_sleep(K_MSEC(100));
		ret = send_at_cmd(NULL, "AT+RST", MDM_CMD_TIMEOUT);
		if (ret < 0 && ret != -ETIMEDOUT) {
			break;
		}
	}

	if (ret < 0) {
		SYS_LOG_ERR("cannot send reset %d\n", retry_count);
		return;
	}
#endif
	k_sem_reset(&foo_data.response_sem);
	if (k_sem_take(&foo_data.response_sem, MDM_CMD_TIMEOUT)) {
		SYS_LOG_ERR("timed out waiting for device to become ready\n");
		return;
	}

	ret = send_at_cmd(NULL, "ATE1", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to set echo mode\n");
		return;
	}

	ret = send_at_cmd(NULL, "AT+CIPMUX=1", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to set multiple socket support\n");
		return;
	}

	ret = send_at_cmd(NULL, "AT+CIPAPMAC_CUR?", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to set multiple socket support\n");
		return;
	}

	ret = send_at_cmd(NULL, "AT+CWQAP", MDM_CMD_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("failed to set multiple socket support\n");
		return;
	}

	foo_data.initialized = 1;
}


static int esp8266_init(struct device *dev)
{
	int i, ret;

	memset(&foo_data, 0, sizeof(foo_data));
	for (i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
		k_work_init(&foo_data.socket_data[i].recv_cb_work,
			sockreadrecv_cb_work);
		k_sem_init(&foo_data.socket_data[i].sock_send_sem, 0, 1);
	}

	k_sem_init(&foo_data.response_sem, 0, 1);

	k_delayed_work_init(&foo_data.ip_addr_work, esp8266_ip_addr_work);

	/* initialize the work queue */
	k_work_q_start(&esp8266_workq,
		       esp8266_workq_stack,
		       K_THREAD_STACK_SIZEOF(esp8266_workq_stack),
		       K_PRIO_COOP(7));

	foo_data.last_socket_id = 0;


	if (mdm_receiver_register(&foo_data.mdm_ctx, CONFIG_WIFI_ESP8266_UART_DEVICE,
				  mdm_recv_buf, sizeof(mdm_recv_buf)) < 0) {
		SYS_LOG_ERR("Error registering modem receiver");
		return -EINVAL;
	}

	/* start RX thread */
	k_thread_create(&esp8266_rx_thread, esp8266_rx_stack,
			K_THREAD_STACK_SIZEOF(esp8266_rx_stack),
			(k_thread_entry_t) esp8266_rx,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	/* Let's start the modem reset in a workq so that init can proceed */
	k_delayed_work_init(&reset_work, esp8266_reset_work);
	ret = k_delayed_work_submit_to_queue(&esp8266_workq,
					     &reset_work, K_MSEC(10));

	SYS_LOG_INF("ESP8266 initialized\n");
	return 0;
}

NET_DEVICE_OFFLOAD_INIT(esp8266, "ESP8266",
			esp8266_init, &foo_data, NULL,
			80, &esp8266_api,
			1500);
