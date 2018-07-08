/* Full-stack IoT client example. */

/*
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG

#include <zephyr.h>
#include <logging/sys_log.h>
#include <jwt.h>

#include "dhcp.h"
#include "dns.h"
#include "mqtt.h"

#include "pdump.h"

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

#if 0
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
#endif

#include <mbedtls/platform.h>
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

static mbedtls_ssl_context the_ssl;
static mbedtls_ssl_config the_conf;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;
static int sock;

static int tcp_tx(void *ctx,
		  const unsigned char *buf,
		  size_t len)
{
	int sock = *((int *) ctx);

	/* Ideally, don't try to send more than is allowed.  TLS will
	 * reassemble on the other end. */

	mbedtls_debug_print_buf(&the_ssl, 4, __FILE__, __LINE__, "tcp_tx", buf, len);
	printf("SEND: %d to %d\n", len, sock);

	int res = zsock_send(sock, buf, len, ZSOCK_MSG_DONTWAIT);
	if (res >= 0) {
		return res;
	}

	switch errno {
	case EAGAIN:
		printf("Waiting for write, res: %d\n", len);
		return MBEDTLS_ERR_SSL_WANT_WRITE;

	default:
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
}

static int tcp_rx(void *ctx,
		  unsigned char *buf,
		  size_t len)
{
	int res;
	int sock = *((int *) ctx);

	res = zsock_recv(sock, buf, len, ZSOCK_MSG_DONTWAIT);
	mbedtls_debug_print_buf(&the_ssl, 4, __FILE__, __LINE__, "tcp_rx", buf, res);
	if (res >= 0) {
		printf("RECV: %d from %d\n", res, sock);
	}

	if (res >= 0) {
		return res;
	}

	switch errno {
	case EAGAIN:
		return MBEDTLS_ERR_SSL_WANT_READ;

	default:
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
}

/*
 * A TLS client, using mbed TLS.
 */
static void tls_client(const char *hostname, struct zsock_addrinfo *host, int port)
{
	int res;

	mbedtls_platform_set_time(k_time);

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
	mbedtls_ssl_init(&the_ssl);
	mbedtls_ssl_config_init(&the_conf);
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
	if (mbedtls_ssl_config_defaults(&the_conf,
					MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		SYS_LOG_ERR("Unable to setup ssl config");
		return;
	}

	mbedtls_ssl_conf_dbg(&the_conf, my_debug, NULL);

	mbedtls_ssl_conf_rng(&the_conf, mbedtls_ctr_drbg_random, &ctr_drbg);

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
	mbedtls_ssl_conf_ca_chain(&the_conf, &ca, NULL);
	mbedtls_ssl_conf_authmode(&the_conf, MBEDTLS_SSL_VERIFY_REQUIRED);

	// mbedtls_debug_set_threshold(2);
	if (mbedtls_ssl_setup(&the_ssl, &the_conf) != 0) {
		SYS_LOG_ERR("Error running mbedtls_ssl_setup");
		return;
	}

	/* Certificate verification requires matching against an
	 * expected hostname.  Use the one we looked up.
	 * TODO: Make this only occur once in the code.
	 */
	if (mbedtls_ssl_set_hostname(&the_ssl, hostname) != 0) {
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

	mbedtls_ssl_set_bio(&the_ssl, &sock, tcp_tx, tcp_rx, NULL);

	SYS_LOG_INF("Performing TLS handshake");
	SYS_LOG_INF("State: %d", the_ssl.state);
	// mbedtls_debug_set_threshold(2);

	res = mbedtls_ssl_handshake(&the_ssl);
	while (1) {
		if (res != 0) {
			if (res != MBEDTLS_ERR_SSL_WANT_READ) {
				SYS_LOG_ERR("TLS handshake failed");
				SYS_LOG_ERR("state: %d, result: %x", the_ssl.state, res);
				return;
			}
		} else {
			if (the_ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER) {
				break;
			} else {
				SYS_LOG_ERR("Shouldn't really get here");
				return;
			}
		}

		/* We need to wait for data on the incoming socket,
		 * and keep trying. */
		struct zsock_pollfd fds[1] = {
			[0] = {
				.fd = sock,
				.events = ZSOCK_POLLIN,
				.revents = 0,
			},
		};

		res = zsock_poll(fds, 1, 250);
		if (res < 0) {
			SYS_LOG_ERR("Socket poll error: %d\n", errno);
			return;
		}
		if (res > 1) {
			SYS_LOG_ERR("Weird return from poll: %d\n", res);
			return;
		}

		SYS_LOG_ERR("Waited, try next step");
		res = mbedtls_ssl_handshake(&the_ssl);
		SYS_LOG_ERR("Step returned: %d (state=%d)", res, the_ssl.state);
	}

	SYS_LOG_INF("State: %d", the_ssl.state);

	SYS_LOG_ERR("Done with TCP client startup");
}

static const char client_id[] = "projects/iot-work-199419/locations/us-central1/"
	"registries/my-registry/devices/zepfull";
#define AUDIENCE "iot-work-199419"

static u8_t send_buf[1024];
static u8_t recv_buf[1024];
static u8_t token[512];

extern unsigned char zepfull_private_der[];
extern unsigned int zepfull_private_der_len;

static void mqtt_startup(void)
{
	struct mqtt_connect_msg conmsg;
	struct jwt_builder jb;

	time_t now = k_time(NULL);

	int res = jwt_init_builder(&jb, token, sizeof(token));
	if (res != 0) {
		printk("Error with JWT token\n");
		return;
	}

	res = jwt_add_payload(&jb, now + 60 * 60, now,
			      AUDIENCE);
	if (res != 0) {
		printk("Error with JWT token\n");
		return;
	}

	res = jwt_sign(&jb, zepfull_private_der, zepfull_private_der_len);
	if (res != 0) {
		printk("Error with JWT token\n");
		return;
	}

	memset(&conmsg, 0, sizeof(conmsg));

	conmsg.clean_session = 1;
	conmsg.client_id = (char *)client_id;  /* Discard const */
	conmsg.client_id_len = strlen(client_id);
	conmsg.keep_alive = 60 * 60; /* One hour */
	conmsg.password = token;
	conmsg.password_len = jwt_payload_len(&jb);

	printk("len1 = %d, len2 = %d\n", conmsg.password_len,
	       strlen(token));

	u16_t send_len = 0;
	res = mqtt_pack_connect(send_buf, &send_len, sizeof(send_buf),
				    &conmsg);
	printk("build packet: res = %d, len=%d\n", res, send_len);

	pdump(send_buf, send_len);
	res = mbedtls_ssl_write(&the_ssl, send_buf, send_len);
	printk("Send result: %d\n", res);
	if (res < 0) {
		return;
	}
	if (res != send_len) {
		printk("Short send\n");
	}

	/* Try to receive something. */
	while (1) {
		struct zsock_pollfd fds[1] = {
			[0] = {
				.fd = sock,
				.events = ZSOCK_POLLIN,
				.revents = 0,
			},
		};

		res = zsock_poll(fds, 1, 5000);
		if (res < 0) {
			printk("Socket poll error: %d\n", errno);
			return;
		}
		if (res == 0) {
			printk("Moving on\n");
			break;
		}

		res = mbedtls_ssl_read(&the_ssl, recv_buf, sizeof(recv_buf));
		if (res < 0) {
			// printk("Read error: %d\n", res);
		} else {
			printk("Read data: %d bytes:\n", res);
			pdump(recv_buf, res);
		}
	}

#if 1
	/* Try subscribing to the device state message. */
	static const char *topics[] = {
		"/devices/zepfull/config",
	};
	static const enum mqtt_qos qoss[] = {
		MQTT_QoS1,
	};
	res = mqtt_pack_subscribe(send_buf, &send_len, sizeof(send_buf),
				  123, 1, topics, qoss);
	printk("Subscribe packet: res=%d, len=%d\n", res, send_len);
#else
#define TOPIC "/devices/zepfull/state"
#define MESSAGE "Hereismystate"
	/* Try sending a state update. */
	struct mqtt_publish_msg pmsg = {
		.dup = 0,
		.qos = MQTT_QoS1,
		.retain = 1,
		.pkt_id = 0xfd12,
		.topic = TOPIC,
		.topic_len = strlen(TOPIC),
		.msg = MESSAGE,
		.msg_len = strlen(MESSAGE),
	};
	res = mqtt_pack_publish(send_buf, &send_len, sizeof(send_buf),
				&pmsg);
	printk("Publish packet: res=%d, len=%d\n", res, send_len);
#endif
	pdump(send_buf, send_len);
	res = mbedtls_ssl_write(&the_ssl, send_buf, send_len);
	printk("Send result: %d\n", res);
	if (res < 0) {
		return;
	}
	if (res != send_len) {
		printk("Short send\n");
	}

	/* Try to receive something. */
	while (1) {
		struct zsock_pollfd fds[1] = {
			[0] = {
				.fd = sock,
				.events = ZSOCK_POLLIN,
				.revents = 0,
			},
		};

		res = zsock_poll(fds, 1, 5000);
		if (res < 0) {
			printk("Socket poll error: %d\n", errno);
			return;
		}
		if (res == 0) {
			printk(".");
		}

		res = mbedtls_ssl_read(&the_ssl, recv_buf, sizeof(recv_buf));
		if (res < 0) {
			// printk("Read error: %d\n", res);
		} else {
			printk("Read data: %d bytes:\n", res);
			pdump(recv_buf, res);
		}
	}
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
	mqtt_startup();
}
