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

#include <net/sntp.h>

/* This comes from newlib. */
#include <time.h>
#include <inttypes.h>

#ifdef CONFIG_STDOUT_CONSOLE
# include <stdio.h>
# define PRINT printf
#else
# define PRINT printk
#endif

#ifndef CONFIG_MBEDTLS_CFG_FILE
# include <mbedtls/config.h>
#else
# include CONFIG_MBEDTLS_CFG_FILE
#endif

#ifdef MBEDTLS_PLATFORM_C
# include <mbedtls/platform.h>
#else
# error "platform not defined"
#endif

#include <mbedtls/net.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#ifdef MBEDTLS_MEMORY_BUFFER_ALLOC_C
# include <mbedtls/memory_buffer_alloc.h>
static unsigned char heap[20480];
#else
# error "TODO: no memory buffer"
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

/*
 * A TLS client, using mbed TLS.
 */
static void tls_client(char *host, int port)
{
	//mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
// 	struct tcp_context ctx;
//
// 	ctx.timeout = MBEDTLS_NETWORK_TIMEOUT;

#ifdef MBEDTLS_X509_CRT_PARSE_C
	mbedtls_x509_crt ca;
#else
#	error "Must define MBEDTLS_X509_CRT_PARSE_C"
#endif

	mbedtls_platform_set_printf(PRINT);

	/*
	 * 0. Initialize mbed TLS.
	 */
	mbedtls_ctr_drbg_init(&ctr_drbg);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_x509_crt_init(&ca);
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
	char mqtt_ip[NET_IPV6_ADDR_LEN];
	char invalid_ip[NET_IPV6_ADDR_LEN];

	int res;

	SYS_LOG_INF("Main entered");
	app_dhcpv4_startup();
	SYS_LOG_INF("Should have DHCPv4 lease at this point.");

	res = ipv4_lookup("time.google.com", time_ip, sizeof(time_ip));
	if (res == 0) {
		SYS_LOG_INF("time: %s", time_ip);
	} else {
		SYS_LOG_INF("Unable to lookup time.google.com, stopping");
		return;
	}

	res = ipv4_lookup("mqtt.googleapis.com", mqtt_ip, sizeof(mqtt_ip));
	if (res == 0) {
		SYS_LOG_INF("mqtt: %s", time_ip);
	} else {
		SYS_LOG_INF("Unable to lookup mqtt.googleapis.com, stopping");
		return;
	}

	res = ipv4_lookup("invalid.example.com", invalid_ip, sizeof(invalid_ip));
	if (res == 0) {
	} else {
		SYS_LOG_INF("No invalid response");
	}

	SYS_LOG_INF("Done with DNS");

	sntp(time_ip);

	/* After setting the time, spin periodically, and make sure
	 * the system clock keeps up reasonably.
	 */
	for (int count = 0; count < 1; count++) {
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

	tls_client(mqtt_ip, 8883);
}
