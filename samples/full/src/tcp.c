/*
 * Copyright (c) 2016 Intel Corporation
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG

#include <logging/sys_log.h>
#include <string.h>
#include <errno.h>
#include <misc/printk.h>

#include "tcp.h"

#include <net/net_core.h>
#include <net/net_context.h>
#include <net/net_pkt.h>
#include <net/net_if.h>

#ifndef CONFIG_MBEDTLS_CFG_FILE
# include "mbedtls/config.h"
#else
# include CONFIG_MBEDTLS_CFG_FILE
#endif

#include <mbedtls/ssl.h>

#define INET_FAMILY AF_INET

#define TCP_BUF_CTR 5
#define TCP_BUF_SIZE 1024

NET_BUF_POOL_DEFINE(tcp_msg_pool, TCP_BUF_CTR, TCP_BUF_SIZE, 0, NULL);

int tcp_tx(void *context, const unsigned char *buf, size_t size)
{
	SYS_LOG_ERR("tcp_tx");
	return -1;
}

int tcp_rx(void *context, unsigned char *buf, size_t size)
{
	SYS_LOG_ERR("tcp_rx");
	return -1;
}

static int set_addr(struct sockaddr *sock_addr, const char *addr,
		    u16_t server_port)
{
	void *ptr = NULL;
	int rc;

#ifdef CONFIG_NET_IPV6
	net_sin6(sock_addr)->sin6_port = htons(server_port);
	sock_addr->sa_family = AF_INET6;
	ptr = &(net_sin6(sock_addr)->sin6_addr);
	rc = net_addr_pton(AF_INET6, addr, ptr);
#else
	net_sin(sock_addr)->sin_port = htons(server_port);
	sock_addr->sa_family = AF_INET;
	ptr = &(net_sin(sock_addr)->sin_addr);
	rc = net_addr_pton(AF_INET, addr, ptr);
#endif

	if (rc) {
		SYS_LOG_ERR("Invalid IP address: %s", addr);
	}

	return rc;
}

int tcp_init(struct tcp_context *ctx, const char *server_addr,
	     u16_t server_port)
{
	struct sockaddr server_sock;
	int rc;

#ifdef CONFIG_NET_IPV6
	socklen_t addr_len = sizeof(struct sockaddr_in6);
	sa_family_t family = AF_INET6;
#else
	socklen_t addr_len = sizeof(struct sockaddr_in);
	sa_family_t family = AF_INET;
#endif

	rc = net_context_get(family, SOCK_STREAM, IPPROTO_TCP, &ctx->net_ctx);
	if (rc) {
		SYS_LOG_ERR("net_context_get error: %d", rc);
		return rc;
	}

	rc = set_addr(&server_sock, server_addr, server_port);
	if (rc) {
		SYS_LOG_ERR("set_addr error");
		goto lb_exit;
	}

	rc = net_context_connect(ctx->net_ctx, &server_sock, addr_len, NULL,
				 ctx->timeout, NULL);
	if (rc) {
		SYS_LOG_ERR("net_context_connect error");
		goto lb_exit;
	}

	return 0;

lb_exit:
	net_context_put(ctx->net_ctx);

	return rc;
}
