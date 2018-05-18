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
#include <net/socket.h>

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

#include <mbedtls/debug.h>

#ifdef MBEDTLS_MEMORY_BUFFER_ALLOC_C
# include <mbedtls/memory_buffer_alloc.h>
static unsigned char heap[65536];
#else
# error "TODO: no memory buffer"
#endif

#include "globalsign.inc"

struct k_sem sem;

#define SNTP_PORT 123

static int64_t time_base;

static void my_debug(void *ctx, int level,
		     const char *file, int line, const char *str)
{
	const char *p, *basename;
	int len;

	ARG_UNUSED(ctx);

	/* Extract basename from file */
	for (p = basename = file; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			basename = p + 1;
		}

	}

	/* Avoid printing double newlines */
	len = strlen(str);
	if (str[len - 1] == '\n') {
		((char *)str)[len - 1] = '\0';
	}

	SYS_LOG_INF("%s:%04d: |%d| %s", basename, line, level, str);
}

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

const char *pers = "mini_client";  // What is this?

static int entropy_source(void *data, unsigned char *output, size_t len,
			  size_t *olen)
{
	u32_t seed;

	// TODO: Don't use sys_rand32_get(), but instead use the
	// entropy device, and fail if it isn't available.

	ARG_UNUSED(data);

	seed = sys_rand32_get();

	if (len > sizeof(seed)) {
		len = sizeof(seed);
	}

	memcpy(output, &seed, len);

	*olen = len;

	return 0;
}

static mbedtls_ssl_context *the_ssl;

static int tcp_tx(void *ctx,
		  const unsigned char *buf,
		  size_t len)
{
	int sock = *((int *) ctx);

	mbedtls_debug_print_buf(the_ssl, 4, __FILE__, __LINE__, "tcp_tx", buf, len);

	return zsock_send(sock, buf, len, 0);
}

static int tcp_rx(void *ctx,
		  unsigned char *buf,
		  size_t len)
{
	int rlen;
	int sock = *((int *) ctx);

	rlen = zsock_recv(sock, buf, len, 0);
	mbedtls_debug_print_buf(the_ssl, 4, __FILE__, __LINE__, "tcp_tx", buf, rlen);
	return rlen;
}

/*
 * A TLS client, using mbed TLS.
 */
static void tls_client(const char *hostname, struct zsock_addrinfo *host, int port)
{
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	int sock;
	int res;

	mbedtls_platform_set_time(k_time);

#ifdef MBEDTLS_X509_CRT_PARSE_C
	mbedtls_x509_crt ca;
#else
#	error "Must define MBEDTLS_X509_CRT_PARSE_C"
#endif

	the_ssl = &ssl;
	mbedtls_platform_set_printf(PRINT);

	/*
	 * 0. Initialize mbed TLS.
	 */
	mbedtls_ctr_drbg_init(&ctr_drbg);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_x509_crt_init(&ca);

	SYS_LOG_INF("Seeding the random number generator...");
	mbedtls_entropy_init(&entropy);
	mbedtls_entropy_add_source(&entropy, entropy_source, NULL,
				   MBEDTLS_ENTROPY_MAX_GATHER,
				   MBEDTLS_ENTROPY_SOURCE_STRONG);

	if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
				  (const unsigned char *)pers,
				  strlen(pers)) != 0) {
		SYS_LOG_ERR("Unable to init drbg");
		return;
	}

	SYS_LOG_INF("Setting up the TLS structure");
	if (mbedtls_ssl_config_defaults(&conf,
					MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		SYS_LOG_ERR("Unable to setup ssl config");
		return;
	}

	mbedtls_ssl_conf_dbg(&conf, my_debug, NULL);

	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

	/* MBEDTLS_MEMORY_BUFFER_ALLOC_C */
	mbedtls_memory_buffer_alloc_init(heap, sizeof(heap));

	/* Load the intended root cert in. */
	if (mbedtls_x509_crt_parse_der(&ca, globalsign_certificate,
				       sizeof(globalsign_certificate)) != 0) {
		SYS_LOG_ERR("Unable to decode root cert");
		return;
	}

	/* And configure tls to require the other side of the
	 * connection to use a cert signed by this certificate.
	 * This makes things fragile, as we are tied to a specific
	 * certificate. */
	mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);

	// mbedtls_debug_set_threshold(2);
	if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
		SYS_LOG_ERR("Error running mbedtls_ssl_setup");
		return;
	}

	/* Certificate verification requires matching against an
	 * expected hostname.  Use the one we looked up.
	 * TODO: Make this only occur once in the code.
	 */
	if (mbedtls_ssl_set_hostname(&ssl, hostname) != 0) {
		SYS_LOG_ERR("Error setting target hostname");
	}

	SYS_LOG_INF("tls init done");

	SYS_LOG_INF("Connecting to tcp '%s'", hostname);
	SYS_LOG_INF("Creating socket");
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == -1) {
		SYS_LOG_ERR("Failed to create socket");
		return;
	}

	SYS_LOG_INF("connecting...");
	res = zsock_connect(sock, host->ai_addr, host->ai_addrlen);
	if (res == -1) {
		SYS_LOG_ERR("Failed to connect to socket");
		return;
	}
	SYS_LOG_INF("Connected");

	mbedtls_ssl_set_bio(&ssl, &sock, tcp_tx, tcp_rx, NULL);

	SYS_LOG_INF("Performing TLS handshake");
	// mbedtls_debug_set_threshold(4);

	if (mbedtls_ssl_handshake(&ssl) != 0) {
		SYS_LOG_ERR("TLS handshake failed");
		return;
	}

	SYS_LOG_ERR("Done with TCP client");
}

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
	// app_dhcpv4_startup();
	// net_app_init();
	SYS_LOG_INF("Should have DHCPv4 lease at this point.");

	res = ipv4_lookup("time.google.com", time_ip, sizeof(time_ip));
	if (res == 0) {
		SYS_LOG_INF("time: %s", time_ip);
	} else {
		SYS_LOG_INF("Unable to lookup time.google.com, stopping");
		return;
	}

#if 0
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
#endif

	SYS_LOG_INF("Done with DNS");

	/* TODO: Convert sntp to sockets with newer API. */
	sntp(time_ip);

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
}
