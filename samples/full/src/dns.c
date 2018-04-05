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

#include "dns.h"

#define DNS_TIMEOUT K_SECONDS(2)

static K_SEM_DEFINE(dns_sem, 0, 1);

/* The 'user_data' passed back to the context.  Filled in with the
 * first name the resolver finds. */
struct dns_ctx {
	char *name;
	size_t name_len;
	bool filled;
	int status;
};

void dns_result_cb(enum dns_resolve_status status,
		   struct dns_addrinfo *info,
		   void *user_data)
{
	void *addr;
	struct dns_ctx *ctx = (struct dns_ctx *)user_data;

	SYS_LOG_INF("dns result: status=%d", status);

	switch (status) {
	case DNS_EAI_CANCELED:
		SYS_LOG_INF("DNS query canceled");
		ctx->status = status;
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_FAIL:
		SYS_LOG_INF("DNS resolve failed");
		ctx->status = status;
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_NODATA:
		SYS_LOG_INF("Cannot resolve address");
		ctx->status = status;
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_ALLDONE:
		SYS_LOG_INF("DNS resolving finished");
		ctx->status = 0;
		k_sem_give(&dns_sem);
		return;
	case DNS_EAI_INPROGRESS:
		SYS_LOG_INF("progress");
		break;
	default:
		SYS_LOG_INF("DNS resolve error (%d)", status);
		ctx->status = status;
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

	if (ctx->filled) {
		return;
	}

	ctx->filled = true;

	SYS_LOG_INF("IPv4 address: %s", net_addr_ntop(info->ai_family, addr,
						      ctx->name, ctx->name_len));
}

/* Returns 0 on success, or some kind of error otherwise. */
int ipv4_lookup(const char *host, char *ip, size_t ip_len)
{
	u16_t dns_id;
	int ret;
	struct dns_ctx context;

	context.name = ip;
	context.name_len = ip_len;
	context.filled = false;

	ret = dns_get_addr_info(host,
				DNS_QUERY_TYPE_A,
				&dns_id,
				dns_result_cb,
				(void *)&context,
				DNS_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("cannot resolve IPv4 address (%d)", ret);
		return ret;
	}

	k_sem_take(&dns_sem, K_FOREVER);

	return context.status;
}
