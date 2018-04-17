/* DNS resolution. */

/*
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG

#include <zephyr.h>
#include <logging/sys_log.h>

#include <net/net_if.h>
#include <net/net_core.h>
#include <net/net_context.h>
#include <net/net_mgmt.h>
#include <net/dns_resolve.h>

#define DNS_TIMEOUT K_SECONDS(2)

static K_SEM_DEFINE(dns_sem, 0, 1);

void dns_result_cb(enum dns_resolve_status status,
		   struct dns_addrinfo *info,
		   void *user_data)
{
	char hr_addr[NET_IPV6_ADDR_LEN];
	void *addr;

	switch (status) {
	case DNS_EAI_CANCELED:
		SYS_LOG_INF("DNS query canceled");
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_FAIL:
		SYS_LOG_INF("DNS resolve failed");
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_NODATA:
		SYS_LOG_INF("Cannot resolve address");
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_ALLDONE:
		SYS_LOG_INF("DNS resolving finished");
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_INPROGRESS:
		SYS_LOG_INF("progress");
		break;
	default:
		SYS_LOG_INF("DNS resolve error (%d)", status);
		k_sem_give(&dns_sem);
		return;
	}

	if (!info) {
		return;
	}

	if (info->ai_family == AF_INET) {
		addr = &net_sin(&info->ai_addr)->sin_addr;
	} else {
		SYS_LOG_ERR("Unknown DNS result family %d", info->ai_family);
		return;
	}

	SYS_LOG_INF("IPv4 address: %s", net_addr_ntop(info->ai_family, addr,
						      hr_addr, sizeof(hr_addr)));
}

void ipv4_lookup(const char *host)
{
	u16_t dns_id;
	int ret;

	ret = dns_get_addr_info(host,
				DNS_QUERY_TYPE_A,
				&dns_id,
				dns_result_cb,
				(void *)host,
				DNS_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("cannot resolve IPv4 address (%d)", ret);
		return;
	}

	k_sem_take(&dns_sem, K_FOREVER);

	SYS_LOG_INF("DNS id %u", dns_id);
}
