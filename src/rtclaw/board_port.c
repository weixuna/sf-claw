/**
 * @file board_port.c
 * @brief sf32lb52_osptek board adapter for the reusable rt-claw SF32 port.
 *
 * All board-specific facts live here, not in third_party/rt-claw-port-sf32:
 *   - network device is the BT PAN interface "b0"
 *   - PAN bring-up is the project's src/net/bt_pan.c
 *   - KV lives in the board's "prefdb" FAL partition
 *   - PSRAM arena size selected by Kconfig
 */

#include <rtconfig.h>
#include <stdbool.h>

#include "claw_port.h"
#include "bt_pan.h"

#define BOARD_NAME          "sf32lb52-osptek"
#define BOARD_NETDEV_NAME   BT_PAN_NETIF_NAME
#define BOARD_KV_PARTITION  "prefdb"

/* RGB LED on PA32 is the only GPIO exposed to the LLM in v1. */
#define BOARD_GPIO_RGB_LED  32

static int board_net_prepare(void)
{
    if (!bt_pan_is_connected()) {
        return bt_pan_connect();
    }
    return 0;
}

static bool board_gpio_allowed(int pin)
{
    return pin == BOARD_GPIO_RGB_LED;
}

static const struct claw_board_ops s_board_ops = {
    .name         = BOARD_NAME,
    .netdev_name  = BOARD_NETDEV_NAME,
    .net_prepare  = board_net_prepare,
    .kv_partition = BOARD_KV_PARTITION,
#ifdef RTCLAW_PORT_PSRAM_SIZE
    .psram_size   = RTCLAW_PORT_PSRAM_SIZE,
#else
    .psram_size   = 512 * 1024,
#endif
    .gpio_allowed = board_gpio_allowed,
    .early_init   = NULL,
};

const struct claw_board_ops *claw_board_get(void)
{
    return &s_board_ops;
}
