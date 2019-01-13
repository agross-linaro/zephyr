/* Protocol implementation. */
/*
 * Copyright (c) 2018-2019 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>

LOG_MODULE_DECLARE(net_full_mqtt_tls_sample, LOG_LEVEL_DBG);
#include "protocol.h"

#include <zephyr.h>
#include <string.h>
#include <jwt.h>
#include <entropy.h>

#include "pdump.h"
#include <net/tls_credentials.h>
#include <net/mqtt.h>
//#include <net/mqtt_legacy_types.h>

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

/* private key information */
extern unsigned char zepfull_private_der[];
extern unsigned int zepfull_private_der_len;

/*
 * This is the hard-coded root certificate that we accept.
 */
#include "globalsign.inc"

static u8_t client_id[] = CONFIG_CLOUD_CLIENT_ID;
static u8_t sub_topic[] = CONFIG_CLOUD_SUBSCRIBE_CONFIG;
static u8_t client_username[] = "none";
static u8_t pub_topic[] = CONFIG_CLOUD_PUBLISH_TOPIC;

static struct mqtt_publish_param pub_data;
static struct mqtt_topic topic;

static u8_t token[512];

static bool connected;
static u64_t next_alive;
static u32_t last_pub_ack;

/* The mqtt client struct */
static struct mqtt_client client_ctx;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Buffers for MQTT client. */
static u8_t rx_buffer[1024];
static u8_t tx_buffer[1024];

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
	struct mqtt_puback_param puback;

	switch (evt->type) {
	case MQTT_EVT_SUBACK:
		LOG_INF("[%s:%d] SUBACK packet id: %u", __func__, __LINE__,
				evt->param.suback.message_id);
		break;

	case MQTT_EVT_UNSUBACK:
		LOG_INF("[%s:%d] UNSUBACK packet id: %u", __func__, __LINE__,
				evt->param.suback.message_id);
		break;

	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed %d", evt->result);
			break;
		}

		connected = true;
		LOG_INF("[%s:%d] MQTT client connected!", __func__, __LINE__);

		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("[%s:%d] MQTT client disconnected %d", __func__,
		       __LINE__, evt->result);

		connected = false;

		break;

	case MQTT_EVT_PUBLISH:
		{
			u8_t d[32];
			int len = evt->param.publish.message.payload.len;
			LOG_INF("[%s:%d] MQTT publish received %d, %d bytes, id: %d, qos: %d",
				__func__, __LINE__, evt->result, len, evt->param.publish.message_id,
				evt->param.publish.message.topic.qos);
			mqtt_read_publish_payload(&client_ctx, d, 32);
		}
		puback.message_id = evt->param.publish.message_id;
		mqtt_publish_qos1_ack(&client_ctx, &puback);
		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error %d", evt->result);
			break;
		}

		last_pub_ack = evt->param.puback.message_id;
		pub_data.message_id += 1;
		LOG_INF("[%s:%d] PUBACK packet id: %u", __func__, __LINE__,
				evt->param.puback.message_id);

		break;

	default:
		break;
	}
}

static int wait_for_input(int timeout)
{
	int res;
	struct zsock_pollfd fds[1] = {
		[0] = {.fd = client_ctx.transport.tls.sock,
		      .events = ZSOCK_POLLIN,
		      .revents = 0},
	};

	res = zsock_poll(fds, 1, timeout);
	if (res < 0 ) {
		LOG_ERR("poll read event error");
		return -errno;
	}

	return res;
}

#define ALIVE_TIME	(60 * MSEC_PER_SEC)

static struct mqtt_utf8 password = {
	.utf8 = token
};

static struct mqtt_utf8 username = {
	.utf8 = client_username,
	.size = sizeof(client_username)
};

static struct mqtt_subscription_list subs_list;

void mqtt_startup(char *hostname, struct zsock_addrinfo *host, int port)
{
	int err;
	char pub_msg[64];
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;
	struct mqtt_client *client = &client_ctx;
	struct jwt_builder jb;
	int res = 0;

	mbedtls_platform_set_time(k_time);

	err = tls_credential_add(1, TLS_CREDENTIAL_CA_CERTIFICATE,
				 globalsign_certificate, sizeof(globalsign_certificate));
	if (err < 0) {
		LOG_ERR("Failed to register public certificate: %d", err);
	}

	mqtt_client_init(client);

	time_t now = k_time(NULL);

	res = jwt_init_builder(&jb, token, sizeof(token));
	if (res != 0) {
		LOG_ERR("Error with JWT token");
		return;
	}

	res = jwt_add_payload(&jb, now + 60 * 60, now,
			      CONFIG_CLOUD_AUDIENCE);
	if (res != 0) {
		LOG_ERR("Error with JWT token");
		return;
	}

	res = jwt_sign(&jb, zepfull_private_der, zepfull_private_der_len);

	if (res != 0) {
		LOG_ERR("Error with JWT token");
		return;
	}


	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(port);
	net_ipaddr_copy(&broker4->sin_addr, &net_sin(host->ai_addr)->sin_addr);

	/* MQTT client configuration */
	client->broker = &broker;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = client_id;
	client->client_id.size = strlen(client_id);
	client->password = &password;
	password.size = jwt_payload_len(&jb);
	client->user_name = &username;
	client->protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* MQTT transport configuration */
	client->transport.type = MQTT_TRANSPORT_SECURE;

	struct mqtt_sec_config *tls_config = &client->transport.tls.config;

	tls_config->peer_verify = 2;
	tls_config->cipher_list = NULL;
	tls_config->seg_tag_list = m_sec_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(m_sec_tags);
	tls_config->hostname = hostname;

	LOG_INF("Connecting to host: %s", hostname);

	err = mqtt_connect(client);
	if (err != 0) {
		LOG_ERR("could not connect, error %d", err);
		return;
	}

	if (wait_for_input(5000) > 0) {
		mqtt_input(client);
		if (!connected) {
			LOG_ERR("failed to connect to mqtt_broker");
			return;
		}
	} else {
		LOG_ERR("failed to connect to mqtt broker");
		return;
	}

	/* initialize publish structure */
	pub_data.message.topic.topic.utf8 = pub_topic;
	pub_data.message.topic.topic.size = strlen(pub_topic);
	pub_data.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	pub_data.message.payload.data = (u8_t*)pub_msg;
	pub_data.message_id = 1;
	pub_data.dup_flag = 0;
	pub_data.retain_flag = 1;

	/* send ping on connect and wait for response */
	mqtt_ping(client);
	wait_for_input(5000);

	next_alive = k_uptime_get() + ALIVE_TIME;

	while (1) {
		LOG_INF("Publishing data");
		sprintf(pub_msg, "%s: %d\n", "payload", pub_data.message_id);
		pub_data.message.payload.len = strlen(pub_msg);
		err = mqtt_publish(client, &pub_data);
		if (err) {
			LOG_ERR("could not publish, error %d", err);
			break;
		}

		/* idle and process messages */
		while (k_uptime_get() < next_alive) {
			if (wait_for_input(5000) > 0 ) {
				mqtt_input(client);
			}
		}

		LOG_INF("Send keep alive");
		mqtt_ping(client);
		wait_for_input(5000);
		next_alive += ALIVE_TIME;
	}
}
