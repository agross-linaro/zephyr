/*
 * Copyright (c) 2016 Intel Corporation
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __TCP_H__
#define __TCP_H__

#include <net/net_context.h>
#include <net/net_ip.h>
#include <net/net_pkt.h>

struct tcp_context {
	struct net_context *net_ctx;
	struct sockaddr local_sock;
	struct net_pkt *rx_pkt;
	s32_t timeout;
};

int tcp_init(struct tcp_context *ctx, const char *server_addr,
	     u16_t server_port);

int tcp_tx(void *ctx, const unsigned char *buf, size_t size);
int tcp_rx(void *ctx, unsigned char *buf, size_t size);

#endif /* not __TCP_H__ */
