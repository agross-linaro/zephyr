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

/* This comes from newlib. */
#include <time.h>
#include <inttypes.h>

#ifdef CONFIG_STDOUT_CONSOLE
# include <stdio.h>
# define PRINT printf
#else
# define PRINT printk
#endif

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
	 app_dhcpv4_startup();
	// net_app_init(NULL, NET_CONFIG_NEED_IPV4, 30 * 1000);
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
