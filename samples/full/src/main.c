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

#define SNTP_PORT 123

void resp_callback(struct sntp_ctx *ctx,
		   int status,
		   u64_t epoch_time,
		   void *user_data)
{
	SYS_LOG_INF("time: %lld", epoch_time);
	SYS_LOG_INF("status: %d", status);

	k_sem_give(&sem);
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
}
