/*
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MQTT-based cloud protocol engine.
 */

#ifndef PROTOCOL_H__
#define PROTOCOL_H__

#include <net/socket.h>

void tls_client(const char *hostname, struct zsock_addrinfo *host, int port);
void mqtt_startup(void);

#endif
