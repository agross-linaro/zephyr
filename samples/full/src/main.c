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

struct k_sem sem;

void resp_callback(struct sntp_ctx *ctx,
		   int status,
		   u64_t epoch_time,
		   void *user_data)
{
	SYS_LOG_INF("time: %lld", epoch_time);
	SYS_LOG_INF("status: %d", status);

	k_sem_give(&sem);
}

void main(void)
{
	SYS_LOG_INF("Main entered");
	app_dhcpv4_startup();
	SYS_LOG_INF("Should have DHCPv4 lease at this point.");
	ipv4_lookup("time.google.com");
	ipv4_lookup("mqtt.googleapis.com");
	ipv4_lookup("invalid.example.com");
	SYS_LOG_INF("Done with DNS");
}

#if 0
void sntp(void)
{
	struct sntp_ctx ctx;
	int rc;

	k_sem_init(&sem, 0, 1);

	/* Initialize sntp */
	rc = sntp_init(&ctx,
		       CONFIG_NET_APP_PEER_IPV4_ADDR,
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
#endif
