/*
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Simplistic MQTT driver.
 *
 * The existing Zephyr MQTT code is divided into mqtt_pkt, which has
 * been copied to this directory, and the plain mqtt.c driver, which
 * is based on the netbuf code.  Since we need to use TLS, this is a
 * different driver that uses the same code for assembling and
 * processing packegs.
 */

#ifndef SAMPLE_MQTT_H__
#define SAMPLE_MQTT_H__

#include <net/mqtt_types.h>
#include "mqtt_pkt.h"

#endif
