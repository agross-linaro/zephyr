/* Full-stack IoT client example. */

/*
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG

#include <zephyr.h>
#include <logging/sys_log.h>

#include "dhcp.h"
#include "dns.h"
#include "protocol.h"

#include <net/sntp.h>
#include <net/net_config.h>
#include <net/net_event.h>

/* This comes from newlib. */
#include <time.h>
#include <inttypes.h>

#ifdef CONFIG_STDOUT_CONSOLE
# include <stdio.h>
# define PRINT printf
#else
# define PRINT printk
#endif

#include "net/wifi_mgmt.h"

struct k_sem sem;

#define SNTP_PORT 123

static int64_t time_base;

void resp_callback(struct sntp_ctx *ctx,
		   int status,
		   u64_t epoch_time,
		   void *user_data)
{
	int64_t stamp;

	stamp = k_uptime_get();
	SYS_LOG_INF("stamp: %lld", stamp);
	SYS_LOG_INF("time: %lld", epoch_time);
	SYS_LOG_INF("time1k: %lld", epoch_time * MSEC_PER_SEC);
	time_base = epoch_time * MSEC_PER_SEC - stamp;
	SYS_LOG_INF("base: %lld", time_base);
	SYS_LOG_INF("status: %d", status);

	/* Convert time to make sure. */
	time_t now = epoch_time;
	struct tm now_tm;

	gmtime_r(&now, &now_tm);
	SYS_LOG_INF("  year: %d", now_tm.tm_year);
	SYS_LOG_INF("  mon : %d", now_tm.tm_mon);
	SYS_LOG_INF("  day : %d", now_tm.tm_mday);
	SYS_LOG_INF("  hour: %d", now_tm.tm_hour);
	SYS_LOG_INF("  min : %d", now_tm.tm_min);
	SYS_LOG_INF("  sec : %d", now_tm.tm_sec);

	k_sem_give(&sem);
}

/* Zephyr implementation of POSIX `time`.  Has to be called k_time
 * because time is already taken by newlib.  The clock will be set by
 * the SNTP client when it receives the time.  We make no attempt to
 * adjust it smoothly, and it should not be used for measuring
 * intervals.  Use `k_uptime_get()` directly for that.   Also the
 * time_t defined by newlib is a signed 32-bit value, and will
 * overflow in 2037. */
time_t k_time(time_t *ptr)
{
	s64_t stamp;
	time_t now;

	stamp = k_uptime_get();
	now = (time_t)((stamp + time_base) / 1000);

	if (ptr) {
		*ptr = now;
	}

	return now;
}

void sntp(const char *ip)
{
	struct sntp_ctx ctx;
	int rc;

	k_sem_init(&sem, 0, 1);

	/* Initialize sntp */
	rc = sntp_init(&ctx,
		       ip,
		       SNTP_PORT,
		       K_FOREVER);
	if (rc < 0) {
		SYS_LOG_ERR("Unable to init sntp context: %d", rc);
		return;
	}

	rc = sntp_request(&ctx, K_FOREVER, resp_callback, NULL);
	if (rc < 0) {
		SYS_LOG_ERR("Failed to send sntp request: %d", rc);
		return;
	}

	/* TODO: This needs to retry. */
	k_sem_take(&sem, K_FOREVER);
	sntp_close(&ctx);

	SYS_LOG_INF("done");
}

#if defined (CONFIG_WIFI_ESP8266)
static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback dhcp_mgmt_cb;
static K_SEM_DEFINE(sem_comm, 0, 1);
static K_SEM_DEFINE(sem_ip, 0, 1);

static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb,
	struct net_if *iface)
{
	const struct wifi_status *status =
			(const struct wifi_status *) cb->info;

	if (status->status) {
		printk("\nConnection request failed (%d)\n", status->status);
	} else {
		printk("\nConnected\n");
	}
}

static void dhcp_event_handler(struct net_mgmt_event_callback *cb,
                                   u32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_IPV4_ADDR_ADD:
		k_sem_give(&sem_ip);
		break;
	default:
		break;
	}
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
	u32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		handle_wifi_connect_result(cb, iface);
		k_sem_give(&sem_comm);
		break;
	default:
		break;
	}
}

static const char ssid[] = "linaro-connect";
static const char psk[] = "LC0nN3c7";

void start_esp8266(void)
{
	struct net_if *iface = net_if_get_default();
	static struct wifi_connect_req_params esp8266_params;
	char buf[NET_IPV4_ADDR_LEN];

	net_mgmt_init_event_callback(&wifi_mgmt_cb,
		wifi_mgmt_event_handler,
		NET_EVENT_WIFI_CONNECT_RESULT |
		NET_EVENT_WIFI_DISCONNECT_RESULT);

	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	net_mgmt_init_event_callback(&dhcp_mgmt_cb, dhcp_event_handler,
		NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&dhcp_mgmt_cb);

	esp8266_params.ssid = ssid;
	esp8266_params.ssid_length = strlen(ssid);
	esp8266_params.psk = psk;
	esp8266_params.psk_length = strlen(psk);
	esp8266_params.security = WIFI_SECURITY_TYPE_PSK;

	net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, NULL);

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &esp8266_params,
		sizeof(struct wifi_connect_req_params))) {
		printk("Connection request failed\n");
		return;
	}

	if (k_sem_take(&sem_comm, 10000)) {
		printk("timeout out connecting to AP: %s\n",
		esp8266_params.ssid);
		return;
	}

	if (k_sem_take(&sem_ip, 10000)) {
		printk("timeout out waiting for ip address\n");
		return;
	}

	printk("DHCP address from ESP8266:\n");
	printk("ip: %s\n",
		net_addr_ntop(AF_INET,
		&iface->config.ip.ipv4->unicast[0].address.in_addr,
		buf, sizeof(buf)));
	printk("gw: %s\n",
		net_addr_ntop(AF_INET,
		&iface->config.ip.ipv4->gw,
		buf, sizeof(buf)));
	printk("netmask: %s\n",
		net_addr_ntop(AF_INET,
		&iface->config.ip.ipv4->netmask,
		buf, sizeof(buf)));
}
#endif



/*
 * TODO: These need to be configurable.
 */
#define MBEDTLS_NETWORK_TIMEOUT 30000

static void show_addrinfo(struct zsock_addrinfo *addr)
{
top:
	printf("  flags   : %d\n", addr->ai_flags);
	printf("  family  : %d\n", addr->ai_family);
	printf("  socktype: %d\n", addr->ai_socktype);
	printf("  protocol: %d\n", addr->ai_protocol);
	printf("  addrlen : %d\n", addr->ai_addrlen);

	/* Assume two words. */
	printf("   addr[0]: 0x%lx\n", ((uint32_t *)addr->ai_addr)[0]);
	printf("   addr[1]: 0x%lx\n", ((uint32_t *)addr->ai_addr)[1]);

	if (addr->ai_next != 0) {
		addr = addr->ai_next;
		goto top;
	}
}

/*
 * Things that make sense in a demo app that would need to be more
 * robust in a real application:
 *
 * - DHCP happens once.  If it fails, or we change networks, the
 *   network will just stop working.
 *
 * - DNS lookups are tried once, and that address just used.  IP
 *   address changes, or DNS resolver problems will just break the
 *   demo.
 */

void main(void)
{
	char time_ip[NET_IPV6_ADDR_LEN];
	static struct zsock_addrinfo hints;
	struct zsock_addrinfo *haddr;
	int res;

	SYS_LOG_INF("Main entered");

#if defined(CONFIG_WIFI_OFFLOAD)
	k_sleep(5000);
	start_esp8266();
#else
	app_dhcpv4_startup();
#endif

	SYS_LOG_INF("Should have DHCPv4 lease at this point.");

	res = ipv4_lookup("time.google.com", time_ip, sizeof(time_ip));
	if (res == 0) {
		SYS_LOG_INF("time: %s", time_ip);
	} else {
		SYS_LOG_INF("Unable to lookup time.google.com, stopping");
		return;
	}

	SYS_LOG_INF("Done with DNS");

	/* TODO: Convert sntp to sockets with newer API. */
	sntp(time_ip);

	printk("sntp finished\n");
	/* After setting the time, spin periodically, and make sure
	 * the system clock keeps up reasonably.
	 */
	for (int count = 0; count < 0; count++) {
		time_t now;
		struct tm tm;
		uint32_t a, b, c;

		a = k_cycle_get_32();
		now = k_time(NULL);
		b = k_cycle_get_32();
		gmtime_r(&now, &tm);
		c = k_cycle_get_32();

		SYS_LOG_INF("time %d-%d-%d %d:%d:%d",
			    tm.tm_year + 1900,
			    tm.tm_mon + 1,
			    tm.tm_mday,
			    tm.tm_hour,
			    tm.tm_min,
			    tm.tm_sec);
		SYS_LOG_INF("time k_time(): %lu", b - a);
		SYS_LOG_INF("time gmtime_r(): %lu", c - b);

		k_sleep(990);
	}

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	res = zsock_getaddrinfo("mqtt.googleapis.com", "8883", &hints, &haddr);
	printf("getaddrinfo status: %d\n", res);

	if (res != 0) {
		printf("Unable to get address, exiting\n");
		return;
	}

	show_addrinfo(haddr);

	tls_client("mqtt.googleapis.com", haddr, 8883);
	mqtt_startup();
}
