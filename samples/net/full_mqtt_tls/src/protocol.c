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
#include <entropy.h>

#include "pdump.h"
#include <net/tls_credentials.h>
#include <net/mqtt.h>

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

/*
 * TODO: Properly export these.
 */
time_t k_time(time_t *ptr);


extern unsigned char zepfull_private_der[];
extern unsigned int zepfull_private_der_len;

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


static const char client_id[] = CONFIG_CLOUD_CLIENT_ID;
static const char *subs[] = {
	CONFIG_CLOUD_SUBSCRIBE_CONFIG,
};

#if 0

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
	struct device *dev;
	int ret;

	ARG_UNUSED(data);

	dev = device_get_binding(CONFIG_ENTROPY_NAME);
        if (!dev) {
                SYS_LOG_ERR("no entropy device");
                return -EINVAL;
        }

	ret = entropy_get_entropy(dev, output, len);

	if (ret) {
		SYS_LOG_ERR("error getting entropy");
		return ret;
	}

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

	SYS_LOG_INF("Got connack");
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

	SYS_LOG_INF("Got suback: id:%d, items:%d, qos[0]:%d",
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

	SYS_LOG_INF("Got publish: id:%d", incoming_publish.pkt_id);

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

	SYS_LOG_INF("Got puback: id:%d", pkt_id);
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
		SYS_LOG_INF("Ping response");
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
	SYS_LOG_DBG("Read: %d (%d) size=%d", res, recv_used, size);

	while (size > 0 && size <= recv_used) {
		if (size > sizeof(recv_buf)) {
			SYS_LOG_ERR("FAILURE: received packet larger than buffer: %d > %d",
				    size, sizeof(recv_buf));
			// TODO: Discard this packet, although there probably
			// isn't really a way to recover from this.
			return res;
		}

		SYS_LOG_INF("Process packet: %x", recv_buf[0]);
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

	//SYS_LOG_DBG("SEND: %d to %d", len, sock);

	int res = send(sock, buf, len, ZSOCK_MSG_DONTWAIT);
	if (res >= 0) {
		return res;
	}

	if (res != len) {
		SYS_LOG_ERR("Short send: %d", res);
	}

	switch errno {
	case EAGAIN:
		SYS_LOG_ERR("Waiting for write, res: %d", len);
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

	res = recv(sock, buf, len, ZSOCK_MSG_DONTWAIT);
	//if (res >= 0) {
	//	SYS_LOG_DBG("RECV: %d from %d", res, sock);
	//}

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

		res = poll(fds, 1, 250);
		if (res < 0) {
			SYS_LOG_ERR("Socket poll error: %d", errno);
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
s
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
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == -1) {
		SYS_LOG_ERR("Failed to create socket");
		return;
	}

	SYS_LOG_INF("connecting...");
	res = connect(sock, host->ai_addr, host->ai_addrlen);
	if (res == -1) {
		SYS_LOG_ERR("Failed to connect to socket");
		return;
	}
	SYS_LOG_INF("Connected");

	mbedtls_ssl_set_bio(&the_ssl, &sock, tcp_tx, tcp_rx, NULL);

	SYS_LOG_INF("Performing TLS handshake");
	SYS_LOG_INF("State: %d", the_ssl.state);

	res = tls_perform(action_handshake, &the_ssl);
	if (res != 0) {
		SYS_LOG_INF("Error on tls handshake: %d", res);
		return;
	}
	if (the_ssl.state != MBEDTLS_SSL_HANDSHAKE_OVER) {
		SYS_LOG_INF("SSL handshake did not complete: %d", the_ssl.state);
		return;
	}

	SYS_LOG_INF("Done with TCP client startup");
}

static void publish_state(void)
{
    char pubmsg[64];
	static unsigned int counter = 0;
    int res;
	u16_t send_len = 0;
	struct write_action wract = {
		.context = &the_ssl,
		.buf = send_buf,
		.len = send_len,
	};
    sprintf(pubmsg, "%s: %d\n", "time", counter++);
	/* Try sending a state update. */
	struct mqtt_publish_msg pmsg = {
		.dup = 0,
		.qos = MQTT_QoS1,
		.retain = 1,
		.pkt_id = 0xfd12,
		.topic = CONFIG_CLOUD_PUBLISH_TOPIC,
		.topic_len = strlen(CONFIG_CLOUD_PUBLISH_TOPIC),
		.msg = pubmsg,
		.msg_len = strlen(pubmsg),
	};
	res = mqtt_pack_publish(send_buf, &send_len, sizeof(send_buf),
				&pmsg);

	wract.buf = send_buf;
	wract.len = send_len;
	pdump(send_buf, send_len);
	res = tls_perform(action_write, &wract);
	SYS_LOG_DBG("Send result: %d", res);
	if (res < 0) {
		return;
	}
	if (res != send_len) {
		SYS_LOG_ERR("Short send");
	}
    SYS_LOG_INF("Publish packet: res=%d, len=%d", res, send_len);
}

void mqtt_startup(void)
{
	struct mqtt_connect_msg conmsg;
	struct jwt_builder jb;
	int res = 0;

	time_t now = k_time(NULL);

	res = jwt_init_builder(&jb, token, sizeof(token));
	if (res != 0) {
		SYS_LOG_ERR("Error with JWT token");
		return;
	}

	res = jwt_add_payload(&jb, now + 60 * 60, now,
			      CONFIG_CLOUD_AUDIENCE);
	if (res != 0) {
		SYS_LOG_ERR("Error with JWT token");
		return;
	}

	res = jwt_sign(&jb, zepfull_private_der, zepfull_private_der_len);

	if (res != 0) {
		SYS_LOG_ERR("Error with JWT token");
		return;
	}

	memset(&conmsg, 0, sizeof(conmsg));

	conmsg.clean_session = 1;
	conmsg.client_id = (char *)client_id;  /* Discard const */
	conmsg.client_id_len = strlen(client_id);
	conmsg.keep_alive = 60 * 2; /* Two minutes */
	conmsg.password = token;
	conmsg.password_len = jwt_payload_len(&jb);

	SYS_LOG_DBG("len1 = %d, len2 = %d", conmsg.password_len,
	       strlen(token));

	u16_t send_len = 0;
	res = mqtt_pack_connect(send_buf, &send_len, sizeof(send_buf),
				    &conmsg);
	SYS_LOG_DBG("build packet: res = %d, len=%d", res, send_len);

	struct write_action wract = {
		.context = &the_ssl,
		.buf = send_buf,
		.len = send_len,
	};

	pdump(send_buf, send_len);
	res = tls_perform(action_write, &wract);

	while (!got_reply) {
		SYS_LOG_INF("Waiting for CONNACT");
		struct idle_action idact = {
			.context = &the_ssl,
		};

		res = tls_perform(action_idle, &idact);
		if (res <= 0) {
			SYS_LOG_INF("Idle error: %d", res);
			return;
		}
	}

	SYS_LOG_INF("Done with connect");
	got_reply = false;

	static const enum mqtt_qos qoss[] = {
		MQTT_QoS1,
	};

	/* topics is defined in private_info/client_info.c */
	res = mqtt_pack_subscribe(send_buf, &send_len, sizeof(send_buf),
				  124, 1, subs, qoss);
	SYS_LOG_INF("Subscribe packet: res=%d, len=%d", res, send_len);

	wract.buf = send_buf;
	wract.len = send_len;
	pdump(send_buf, send_len);
	res = tls_perform(action_write, &wract);
	SYS_LOG_DBG("Send result: %d", res);
	if (res < 0) {
		return;
	}
	if (res != send_len) {
		SYS_LOG_ERR("Short send");
	}

	got_reply = 0;

	next_alive = k_uptime_get() + ALIVE_TIME;

	publish_state();
	while (1) {
		struct idle_action idact = {
			.context = &the_ssl,
		};

		res = tls_perform(action_idle, &idact);
		if (res <= 0) {
			SYS_LOG_INF("Idle error: %d", res);
			return;
		}

		while (puback_head != puback_tail) {
			SYS_LOG_DBG("head=%d, tail=%d", puback_head, puback_tail);
			/* Send the puback back so that the remote
			 * feels we have received the message. */
			res = mqtt_pack_puback(send_buf, &send_len, sizeof(send_buf),
					       puback_ids[puback_tail]);
			SYS_LOG_INF("Send Puback: res=%d, len=%d", res, send_len);

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
			publish_state();
			res = mqtt_pack_pingreq(send_buf, &send_len, sizeof(send_buf));
			SYS_LOG_INF("Send ping, res=%d, len=%d", res, send_len);

			wract.buf = send_buf;
			wract.len = send_len;
			pdump(send_buf, send_len);
			res = tls_perform(action_write, &wract);

			if (res != send_len) {
				SYS_LOG_ERR("Problem sending ping: %d", res);
			}

			next_alive = k_uptime_get() + ALIVE_TIME;
		}
	}
}
#endif

/* The mqtt client struct */
static struct mqtt_client client_ctx;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Buffers for MQTT client. */
static u8_t rx_buffer[128];
static u8_t tx_buffer[128];

static sec_tag_t m_sec_tags[] = {
#if defined(MBEDTLS_X509_CRT_PARSE_C)
		1,
#endif
#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
		APP_PSK_TAG,
#endif
};

void mqtt_evt_handler(struct mqtt_client *const client,
		      const struct mqtt_evt *evt)
{

}

void mqtt_startup(const char *hostname, struct zsock_addrinfo *host, int port)
{
	int err;
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;
	char foo[32];
	struct mqtt_client *client = &client_ctx;

printk("%s\n", __func__);
#if defined(CONFIG_MQTT_LIB_TLS)
	mbedtls_platform_set_time(k_time);

#if defined(MBEDTLS_X509_CRT_PARSE_C)
	err = tls_credential_add(1, TLS_CREDENTIAL_CA_CERTIFICATE,
				 globalsign_certificate, sizeof(globalsign_certificate));
	if (err < 0) {
		SYS_LOG_ERR("Failed to register public certificate: %d", err);
	}
#endif
#endif

	mqtt_client_init(client);

printk("after client init\n");

	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(port);
	net_ipaddr_copy(&broker4->sin_addr, &net_sin(host->ai_addr)->sin_addr);
//	inet_pton(AF_INET, "206.181.100.64", &broker4->sin_addr);
//	inet_pton(AF_INET, "64.181.233.206", &broker4->sin_addr);

	/* MQTT client configuration */
	client->broker = &broker;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = (u8_t *)client_id;
	client->client_id.size = strlen(client_id);
	client->password = NULL;
	client->user_name = NULL;
	client->protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* MQTT transport configuration */
#if defined(CONFIG_MQTT_LIB_TLS)
	client->transport.type = MQTT_TRANSPORT_SECURE;

	struct mqtt_sec_config *tls_config = &client->transport.tls.config;

	tls_config->peer_verify = 2;
	tls_config->cipher_list = NULL;
	tls_config->seg_tag_list = m_sec_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(m_sec_tags);
#if defined(MBEDTLS_X509_CRT_PARSE_C)
	tls_config->hostname = hostname;
#else
	tls_config->hostname = NULL;
#endif

#else
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

	err = mqtt_connect(client);
	if (err != 0) {
		SYS_LOG_ERR("could not connect\n");
	}

	printk("got somewhere\n");
}
