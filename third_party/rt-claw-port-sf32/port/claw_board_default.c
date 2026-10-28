/**
 * @file claw_board_default.c
 * @brief Weak defaults for projects that do not provide a board adapter.
 */

#include "claw_port.h"

__attribute__((weak)) const struct claw_board_ops *claw_board_get(void)
{
    return NULL;
}

__attribute__((weak)) void claw_port_on_ready(int ok)
{
    (void)ok;
}
