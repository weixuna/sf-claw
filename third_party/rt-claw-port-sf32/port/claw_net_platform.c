/**
 * @file claw_net_platform.c
 * @brief rt-claw platform network hooks using the board-provided netdev.
 *
 * The board decides whether netdev_name is "b0" (BT PAN), "e0"
 * (Ethernet), or a WiFi netdev, and supplies net_prepare() if a link
 * needs to be triggered.
 */

#include <rtthread.h>
#include <stdbool.h>
#include <lwip/ip_addr.h>
#include <netdev.h>

#include "claw/platform_net.h"
#include "claw_port.h"
#include "claw_port_net.h"

#define CLAW_NET_DEFAULT_DEVICE "e0"

static const char *board_netdev_name(void)
{
    const struct claw_board_ops *b = claw_board_get();

    if (b && b->netdev_name && b->netdev_name[0] != '\0') {
        return b->netdev_name;
    }
    return CLAW_NET_DEFAULT_DEVICE;
}

const char *claw_platform_net_device_name(void)
{
    return board_netdev_name();
}

void claw_platform_net_prepare(void)
{
    const struct claw_board_ops *b = claw_board_get();

    if (b && b->net_prepare) {
        int rc = b->net_prepare();
        if (rc != 0) {
            rt_kprintf("[claw_net] board net_prepare failed: %d\n", rc);
        }
    }
}

bool claw_port_net_is_online(void)
{
    struct netdev *nd = netdev_get_by_name(board_netdev_name());

    if (!nd) {
        return false;
    }
    if (!netdev_is_link_up(nd)) {
        return false;
    }
    return nd->ip_addr.addr != 0;
}

int claw_port_net_wait_online(uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (waited < timeout_ms) {
        if (claw_port_net_is_online()) {
            return 0;
        }
        rt_thread_mdelay(500);
        waited += 500;
    }

    return -1;
}
