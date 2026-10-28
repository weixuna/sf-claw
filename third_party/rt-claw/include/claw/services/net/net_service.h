/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef CLAW_SERVICES_NET_SERVICE_H
#define CLAW_SERVICES_NET_SERVICE_H

#include "osal/claw_os.h"

int net_service_init(void);

/*
 * Print IP address info to stdout.
 * Output format varies by platform but always includes IP, netmask, gateway.
 */
void net_print_ipinfo(void);

#endif /* CLAW_SERVICES_NET_SERVICE_H */
