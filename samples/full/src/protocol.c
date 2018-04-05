/* Protocol implementation. */
/*
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG

#include "protocol.h"

#include <zephyr.h>
#include <logging/sys_log.h>
#include <string.h>
#include <jwt.h>

#include "mqtt.h"
#include "pdump.h"

#include <mbedtls/platform.h>
#include <mbedtls/net.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#include <mbedtls/debug.h>

#ifdef CONFIG_STDOUT_CONSOLE
# include <stdio.h>
# define PRINT printf
#else
# define PRINT printk
#endif

static unsigned char zepfull_ec_private_der[] = {
	0x4c, 0x77, 0xb9, 0x99, 0x2d, 0x9d, 0x9f, 0x4d,
	0xd9, 0x67, 0x34, 0xc5, 0x33, 0x72, 0x86, 0x3a,
	0x98, 0x1f, 0xf1, 0xe5, 0xbd, 0x20, 0xdb, 0x9e,
	0x12, 0xcb, 0x43, 0xf8, 0xad, 0xba, 0x56, 0xa9,
};
unsigned int zepfull_ec_private_der_len = 32;


/*
 * TODO: Properly export these.
 */
time_t k_time(time_t *ptr);

/*
 * mbed TLS has its own "memory buffer alloc" heap, but it needs some
 * data.  This size can be tuned.
 */
#ifdef MBEDTLS_MEMORY_BUFFER_ALLOC_C
#  include <mbedtls/memory_buffer_alloc.h>
static unsigned char heap[56240];
#else
#  error "TODO: no memory buffer configured"
#endif

/*
 * This is the hard-coded root certificate that we accept.
 */
#include "globalsign.inc"

/*
 * Determine the length of an MQTT packet.
 *
 * Returns:
 *   > 0    The length, in bytes, of the MQTT packet this starts
 *   0      We don't have enough data to determine the length.
 *   -errno The packet is malformed.
 */
static int mqtt_length(u8_t *data, size_t len)
{
	u32_t size = 0;
	int shift = 0;
	int pos = 1;

	while (1) {
		if (pos >= 5) {
			return -EINVAL;
		}

		if (pos >= len) {
			return 0;
		}

		u8_t ch = data[pos];

		size |= (ch & 0x7F) << shift;

		if ((ch & 0x80) == 0) {
			break;
		}

		shift += 7;
		pos++;
	}

	return size + pos + 1;
}

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

static mbedtls_ssl_context the_ssl;
static mbedtls_ssl_config the_conf;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;
static int sock;

/* State information.  This really should be in a structure per
 * instance. */
static bool got_reply;

static u8_t send_buf[1024];
static u8_t recv_buf[1024];
static size_t recv_used;
static u8_t token[512];

/* A queue of publish replies we need to return. */
#define PUBACK_SIZE 8  /* Change macro if not power of two */
#define INC_PUBACK_Q(x__) (((x__) + 1) & (PUBACK_SIZE - 1))
static u16_t puback_ids[PUBACK_SIZE];
static u16_t puback_head, puback_tail;

/* The next time we should send a keep-alive packet. */
static u64_t next_alive;
#define ALIVE_TIME (60 * MSEC_PER_SEC)

static void process_connack(u8_t *buf, size_t size)
{
	u8_t session;
	u8_t connect_rc;

	int res = mqtt_unpack_connack(buf, size, &session, &connect_rc);
	if (res < 0) {
		SYS_LOG_ERR("Malformed CONNACK received");
		return;
	}

	if (connect_rc != 0) {
		SYS_LOG_ERR("Error establishing connection: %d", connect_rc);
		return;
	}

	printk("Got connack\n");
	got_reply = true;
}

static void process_suback(u8_t *buf, size_t size)
{
	u16_t pkt_id;
	u8_t items;
	enum mqtt_qos granted_qos[4];

	int res = mqtt_unpack_suback(buf, size, &pkt_id,
				     &items, ARRAY_SIZE(granted_qos),
				     granted_qos);
	if (res < 0) {
		SYS_LOG_ERR("Malformed SUBACK message");
		return;
	}

	printk("Got suback: id:%d, items:%d, qos[0]:%d\n",
	       pkt_id, items, granted_qos[0]);
	got_reply = true;
}

static void process_publish(u8_t *buf, size_t size)
{
	struct mqtt_publish_msg incoming_publish;

	int res = mqtt_unpack_publish(buf, size, &incoming_publish);
	if (res < 0) {
		SYS_LOG_ERR("Malformed PUBLISH message");
		return;
	}

	if (incoming_publish.qos != MQTT_QoS1) {
		SYS_LOG_ERR("Unsupported QOS on publish");
		return;
	}

	printk("Got publish: id:%d\n", incoming_publish.pkt_id);

	/* Queue this up. */
	puback_ids[puback_head] = incoming_publish.pkt_id;
	puback_head = INC_PUBACK_Q(puback_head);

	if (puback_head == puback_tail) {
		SYS_LOG_ERR("Too many pub ack replies came in!");
	}
}

static void process_puback(u8_t *buf, size_t size)
{
	u16_t pkt_id;

	int res = mqtt_unpack_puback(buf, size, &pkt_id);
	if (res < 0) {
		SYS_LOG_ERR("Malformed PUBACK message");
		return;
	}

	printk("Got puback: id:%d\n", pkt_id);
	got_reply = true;
}

static void process_packet(u8_t *buf, size_t size)
{
	switch (MQTT_PACKET_TYPE(buf[0])) {
	case MQTT_CONNACK:
		process_connack(buf, size);
		break;
	case MQTT_SUBACK:
		process_suback(buf, size);
		break;
	case MQTT_PUBLISH:
		process_publish(buf, size);
		break;
	case MQTT_PUBACK:
		process_puback(buf, size);
		break;
	case MQTT_PINGRESP:
		printk("Ping response\n");
		break;
	default:
		SYS_LOG_ERR("Unsupported packet received: %x", buf[0]);
		break;
	}
}

static int check_read(mbedtls_ssl_context *context)
{
	int res = mbedtls_ssl_read(context,
				   recv_buf + recv_used,
				   sizeof(recv_buf) - recv_used);
	if (res <= 0) {
		return res;
	}

	recv_used += res;

	int size = mqtt_length(recv_buf, recv_used);
	printk("Read: %d (%d) size=%d\n", res, recv_used, size);

	while (size > 0 && size <= recv_used) {
		if (size > sizeof(recv_buf)) {
			SYS_LOG_ERR("FAILURE: received packet larger than buffer: %d > %d",
				    size, sizeof(recv_buf));
			// TODO: Discard this packet, although there probably
			// isn't really a way to recover from this.
			return res;
		}

		printk("Process packet: %x\n", recv_buf[0]);
		pdump(recv_buf, recv_used);

		process_packet(recv_buf, size);

		/* Consume this part of the buffer, moving any
		 * remaining to the start. */

		if (recv_used > size) {
			memmove(recv_buf, recv_buf + size, recv_used - size);
		}

		recv_used -= size;

		size = mqtt_length(recv_buf, recv_used);
	}

	return res;
}

static int tcp_tx(void *ctx,
		  const unsigned char *buf,
		  size_t len)
{
	int sock = *((int *) ctx);

	/* Ideally, don't try to send more than is allowed.  TLS will
	 * reassemble on the other end. */

//	mbedtls_debug_print_buf(&the_ssl, 4, __FILE__, __LINE__, "tcp_tx", buf, len);
	printf("SEND: %d to %d\n", len, sock);

	int res = zsock_send(sock, buf, len, ZSOCK_MSG_DONTWAIT);
	if (res >= 0) {
		return res;
	}

	if (res != len) {
		printf("Short send: %d\n", res);
	}

	// printk("----- SEND -----\n");
	// pdump(buf, res);
	// printk("----- END SEND -----\n");

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
//	mbedtls_debug_print_buf(&the_ssl, 4, __FILE__, __LINE__, "tcp_rx", buf, res);
	if (res >= 0) {
		printf("RECV: %d from %d\n", res, sock);
		// printk("----- RECV -----\n");
		// pdump(buf, res);
		// printk("----- END RECV -----\n");
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

const char *pers = "mini_client";  // What is this?

typedef int (*tls_action)(void *data);

/*
 * A driving loop for mbed TLS.  Invokes 'op' with 'data'.  This is
 * expected to return one of the MBEDTLS errors, with
 * MBEDTLS_ERR_SSL_WANT_READ and MBEDTLS_ERR_SSL_WANT_WRITE handled
 * specifically by this code.  This will return when the operation
 * returns any other status result.
 */
static int tls_perform(tls_action action, void *data)
{
	int res = action(data);
	short events = 0;
	while (1) {
		// SYS_LOG_INF("tls_perform loop: %d", res);
		switch (res) {
		case MBEDTLS_ERR_SSL_WANT_READ:
			events = ZSOCK_POLLIN;
			break;

		case MBEDTLS_ERR_SSL_WANT_WRITE:
			events = ZSOCK_POLLOUT;
			break;

		default:
			/* Any other result is directly returned. */
			return res;
		}

		struct zsock_pollfd fds[1] = {
			[0] = {
				.fd = sock,
				.events = events,
				.revents = 0,
			},
		};

		// SYS_LOG_INF("polling: %d", events);
		res = zsock_poll(fds, 1, 250);
		if (res < 0) {
			SYS_LOG_ERR("Socket poll error: %d\n", errno);
			return -errno;
		}

		res = action(data);
	}
}

/* An action to perform the TLS handshaking.  Data should be a pointer
 * to the mbedtls_ssl_context. */
static int action_handshake(void *data)
{
	mbedtls_ssl_context *ssl = data;

	return mbedtls_ssl_handshake(ssl);
}

struct write_action {
	mbedtls_ssl_context *context;
	const unsigned char *buf;
	size_t len;
};

/* An action to write data over the connection.  The data should be a
 * pointer to a struct write_action.  This will also try reading data
 * and processing it as MQTT received data.
 *
 * It is a little complex if we get blocked on both read and write.
 * Currently, this doesn't ever happen (writes don't block in the
 * Zephyr socket code as currently implemented).
 */
static int action_write(void *data)
{
	struct write_action *act = data;

	/* Try the read first, in order to process the received data.
	 */
	int res = check_read(act->context);
	if (res == MBEDTLS_ERR_SSL_WANT_READ ||
		   res == MBEDTLS_ERR_SSL_WANT_WRITE) {
		/* This is kind of the "normal" case of no data being
		 * available. */
	} else if (res < 0) {
		/* Some kind of error, so return that. */
		return res;
	}

	/* At this point, we read, so now try the write. */
	return mbedtls_ssl_write(act->context, act->buf, act->len);
}

struct idle_action {
	mbedtls_ssl_context *context;
};

/* An action when we have nothing to do.  This will try reading, and
 * delay for a single polling interval.  The polling will then allow
 * for other operations to happen.  The main loop will not return
 * unless there is an error. */
static int action_idle(void *data)
{
	struct idle_action *act = data;

	/* If we need to send a keep alive, just return immediately.
	 */
	if (next_alive != 0 && k_uptime_get() >= next_alive) {
		return 1;
	}

	int res = check_read(act->context);
	if (res > 0 && !got_reply && (puback_head == puback_tail)) {
		/* In the valid case, just wait for more data. */
		res = MBEDTLS_ERR_SSL_WANT_READ;
	}

	return res;
}

/*
 * A TLS client, using mbed TLS.
 */
void tls_client(const char *hostname, struct zsock_addrinfo *host, int port)
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

	/*
	 * Never disable root cert verification, like this.
	 */
#if 1
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
#else
	mbedtls_ssl_conf_authmode(&the_conf, MBEDTLS_SSL_VERIFY_NONE);
#endif

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

	res = tls_perform(action_handshake, &the_ssl);
	if (res != 0) {
		SYS_LOG_INF("Error on tls handshake: %d", res);
		return;
	}
	if (the_ssl.state != MBEDTLS_SSL_HANDSHAKE_OVER) {
		SYS_LOG_INF("SSL handshake did not complete: %d", the_ssl.state);
		return;
	}

	SYS_LOG_ERR("Done with TCP client startup");
}

//extern const unsigned char zepfull_private_der[];
//extern const unsigned int zepfull_private_der_len;

//extern const unsigned char zepfull_ec_private_der[];
//extern const unsigned int zepfull_ec_private_der_len;

static const char client_id[] = "projects/macro-precinct-211108/locations/us-central1/"
		"registries/agross-registry/devices/agross";
#define AUDIENCE "macro-precinct-211108"

#if 0
static void show_stack(void)
{
	static bool once = false;

	extern u8_t _main_stack[];

	if (!once) {
		once = true;
		// pdump(_main_stack, CONFIG_MAIN_STACK_SIZE);
		// pdump(heap, sizeof(heap));
		mbedtls_memory_buffer_alloc_status();
	}
}
#endif

void mqtt_startup(void)
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

#ifdef CONFIG_JWT_SIGN_RSA
	res = jwt_sign(&jb, zepfull_private_der, zepfull_private_der_len);
#endif
#ifdef CONFIG_JWT_SIGN_ECDSA
	res = jwt_sign(&jb, zepfull_ec_private_der, zepfull_ec_private_der_len);
#endif
	if (res != 0) {
		printk("Error with JWT token\n");
		return;
	}

	memset(&conmsg, 0, sizeof(conmsg));

	conmsg.clean_session = 1;
	conmsg.client_id = (char *)client_id;  /* Discard const */
	conmsg.client_id_len = strlen(client_id);
	conmsg.keep_alive = 60 * 2; /* Two minutes */
	conmsg.password = token;
	conmsg.password_len = jwt_payload_len(&jb);

	printk("len1 = %d, len2 = %d\n", conmsg.password_len,
	       strlen(token));

	u16_t send_len = 0;
	res = mqtt_pack_connect(send_buf, &send_len, sizeof(send_buf),
				    &conmsg);
	printk("build packet: res = %d, len=%d\n", res, send_len);

	struct write_action wract = {
		.context = &the_ssl,
		.buf = send_buf,
		.len = send_len,
	};

	pdump(send_buf, send_len);
	res = tls_perform(action_write, &wract);

	while (!got_reply) {
		printk("Waiting for CONNACT\n");
		struct idle_action idact = {
			.context = &the_ssl,
		};

		res = tls_perform(action_idle, &idact);
		if (res <= 0) {
			printk("Idle error: %d\n", res);
			return;
		}
	}

	printk("Done with connect\n");
	got_reply = false;

#if 1
	/* Try subscribing to the device state message. */
	static const char *topics[] = {
		"/devices/agross/config",
	};
	static const enum mqtt_qos qoss[] = {
		MQTT_QoS1,
	};
	res = mqtt_pack_subscribe(send_buf, &send_len, sizeof(send_buf),
				  124, 1, topics, qoss);
	printk("Subscribe packet: res=%d, len=%d\n", res, send_len);
#else
#define TOPIC "/devices/zepfull/state"
#define MESSAGE "Some more state"
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
	wract.buf = send_buf;
	wract.len = send_len;
	pdump(send_buf, send_len);
	res = tls_perform(action_write, &wract);
	printk("Send result: %d\n", res);
	if (res < 0) {
		return;
	}
	if (res != send_len) {
		printk("Short send\n");
	}

#if 0
	while (!got_reply) {
		printk("Waiting for SUBACK\n");
		struct idle_action idact = {
			.context = &the_ssl,
		};

		res = tls_perform(action_idle, &idact);
		if (res <= 0) {
			printk("Idle error: %d\n", res);
			return;
		}
	}
#endif

	got_reply = 0;

	next_alive = k_uptime_get() + ALIVE_TIME;

	while (1) {
		struct idle_action idact = {
			.context = &the_ssl,
		};

		res = tls_perform(action_idle, &idact);
		if (res <= 0) {
			printk("Idle error: %d\n", res);
			return;
		}

		while (puback_head != puback_tail) {
			printk("head=%d, tail=%d\n", puback_head, puback_tail);
			/* Send the puback back so that the remote
			 * feels we have received the message. */
			res = mqtt_pack_puback(send_buf, &send_len, sizeof(send_buf),
					       puback_ids[puback_tail]);
			printk("Send Puback: res=%d, len=%d\n", res, send_len);

			puback_tail = INC_PUBACK_Q(puback_tail);

			wract.buf = send_buf;
			wract.len = send_len;
			pdump(send_buf, send_len);
			res = tls_perform(action_write, &wract);

			if (res != send_len) {
				SYS_LOG_ERR("Problem sending PUBACK: %d", res);
			}

			next_alive = k_uptime_get() + ALIVE_TIME;
		}

		if (k_uptime_get() >= next_alive) {
			res = mqtt_pack_pingreq(send_buf, &send_len, sizeof(send_buf));
			printk("Send ping, res=%d, len=%d\n", res, send_len);

			wract.buf = send_buf;
			wract.len = send_len;
			pdump(send_buf, send_len);
			res = tls_perform(action_write, &wract);

			if (res != send_len) {
				SYS_LOG_ERR("Problem sending ping: %d", res);
			}

			next_alive = k_uptime_get() + ALIVE_TIME;

			// show_stack();
		}
	}
}

