/**
 * @file claw_port_net.h
 * @brief Network helpers shared by the rt-claw port.
 */

#ifndef CLAW_PORT_NET_H
#define CLAW_PORT_NET_H

#include <stdbool.h>
#include <stdint.h>

bool claw_port_net_is_online(void);
int  claw_port_net_wait_online(uint32_t timeout_ms);

#endif /* CLAW_PORT_NET_H */
