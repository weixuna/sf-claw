/**
 * @file claw_port.h
 * @brief Board interface injected by the SF32 project into rt-claw.
 *
 * The port package never owns board configuration.  A project provides a
 * strong claw_board_get() (and optionally claw_port_on_ready()) in its
 * own board_port.c.  All fields are optional; sane defaults are used when
 * the board returns NULL.
 */

#ifndef CLAW_PORT_H
#define CLAW_PORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct claw_board_ops {
    const char *name;              /* "sf32lb52-osptek" */
    const char *netdev_name;       /* "b0", "e0", ... */
    int  (*net_prepare)(void);     /* bring up BT PAN / WiFi / Ethernet */
    const char *kv_partition;      /* "prefdb"; NULL disables persistence */
    size_t psram_size;             /* bytes reserved for claw extras, 0=none */
    bool (*gpio_allowed)(int pin); /* LLM GPIO whitelist */
    void (*early_init)(void);      /* board clocks / power, optional */
};

/*
 * Implemented by the project.  The port package provides a weak default
 * returning NULL, so a board without an adapter still builds and runs
 * with the generic defaults.
 */
const struct claw_board_ops *claw_board_get(void);

/*
 * Called by the rt-claw boot thread after claw_init().  The project uses
 * this to tell the UI bridge whether AI is ready.  Weak no-op default.
 */
void claw_port_on_ready(int ok);

/*
 * Replace the upstream RT-Thread system_info/memory_info tool
 * implementations with SF32-aware ones (real chip/board/revision/PSRAM
 * values instead of the vexpress-a9 reference strings).  Called by the
 * port boot thread after the linker-section tools have been collected.
 */
void claw_port_tools_override(void);

#ifdef __cplusplus
}
#endif

#endif /* CLAW_PORT_H */
