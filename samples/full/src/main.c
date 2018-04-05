/* Full-stack IoT client example. */

/*
 * Copyright (c) 2018 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
#define SYS_LOG_DOMAIN "full"
#define NET_SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#define NET_LOG_ENABLED 1
*/

#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG

#include <zephyr.h>
#include <logging/sys_log.h>

#include "dhcp.h"
// #include <stdio.h>

/* DHCP is done through the core network interface. */
/*
#include <net/net_if.h>
#include <net/net_core.h>
#include <net/net_context.h>
#include <net/net_mgmt.h>
*/

void main(void)
{
	SYS_LOG_INF("Main entered");
	app_dhcpv4_startup();
	SYS_LOG_INF("Should have DHCPv4 lease at this point.");
}
