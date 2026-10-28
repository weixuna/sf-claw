/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Swarm service - node discovery, heartbeat, and task distribution.
 */

#ifndef CLAW_SERVICES_SWARM_H
#define CLAW_SERVICES_SWARM_H

#include "osal/claw_os.h"
#include "claw_config.h"

enum swarm_node_state {
    SWARM_NODE_OFFLINE = 0,
    SWARM_NODE_DISCOVERING,
    SWARM_NODE_ONLINE,
};

/* Node role in the swarm — declared in heartbeat */
enum swarm_role {
    SWARM_ROLE_WORKER = 0,      /* default: tool execution */
    SWARM_ROLE_THINKER,         /* AI + internet capable */
    SWARM_ROLE_COORDINATOR,     /* task decomposition + aggregation */
    SWARM_ROLE_OBSERVER,        /* read-only monitoring */
};

struct swarm_node {
    uint32_t id;
    enum swarm_node_state state;
    uint32_t last_seen;
    uint32_t ip_addr;
    uint16_t port;
    uint8_t  capabilities;
    uint8_t  load;
    uint32_t uptime_s;
    uint8_t  role;              /* enum swarm_role */
    uint8_t  active_tasks;      /* running task count */
};

/* 20-byte heartbeat packet (packed) */
struct __attribute__((packed)) swarm_heartbeat {
    uint32_t magic;         /* 0x434C4157 "CLAW" */
    uint32_t node_id;
    uint32_t uptime_s;
    uint8_t  capabilities;
    uint8_t  load;
    uint16_t port;
    uint8_t  role;          /* enum swarm_role */
    uint8_t  active_tasks;
    uint16_t reserved;
};

#define SWARM_HEARTBEAT_MAGIC   0x434C4157  /* "CLAW" */

/* Capability bitmap — advertised in heartbeat packets */
#define SWARM_CAP_GPIO      (1 << 0)  /* controllable GPIO */
#define SWARM_CAP_LCD       (1 << 1)  /* LCD display */
#define SWARM_CAP_SENSOR    (1 << 2)  /* sensor input */
#define SWARM_CAP_CAMERA    (1 << 3)  /* camera */
#define SWARM_CAP_SPEAKER   (1 << 4)  /* audio output */
#define SWARM_CAP_AI        (1 << 5)  /* AI engine available */
#define SWARM_CAP_INTERNET  (1 << 6)  /* external network */
#define SWARM_CAP_MOUSE     (1 << 7)  /* USB HID mouse */

/* --- Remote tool invocation (RPC) --- */

#define SWARM_RPC_MAGIC         0x52504321  /* "RPC!" */
#define SWARM_RPC_PAYLOAD_MAX   188
#define SWARM_RPC_TOOL_NAME_MAX 32
#define SWARM_RPC_TIMEOUT_MS    5000

enum swarm_rpc_type {
    SWARM_RPC_REQUEST  = 0,
    SWARM_RPC_RESPONSE = 1,
};

enum swarm_rpc_status {
    SWARM_RPC_OK        = 0,
    SWARM_RPC_NOT_FOUND = 1,
    SWARM_RPC_ERROR     = 2,
};

struct __attribute__((packed)) swarm_rpc_msg {
    uint32_t magic;
    uint32_t src_node;
    uint32_t dst_node;
    uint16_t seq;
    uint8_t  type;       /* enum swarm_rpc_type */
    uint8_t  status;     /* enum swarm_rpc_status (response only) */
    char     tool_name[SWARM_RPC_TOOL_NAME_MAX];
    char     payload[SWARM_RPC_PAYLOAD_MAX];
};

/* Retry config for RPC calls */
#define SWARM_RPC_MAX_RETRIES   3
#define SWARM_RPC_RETRY_BASE_MS 500

int  swarm_init(void);
int  swarm_start(void);
void swarm_stop(void);
uint32_t swarm_self_id(void);
int  swarm_node_count(void);
void swarm_list_nodes(void);

/**
 * Execute a tool on a remote swarm node.
 * Blocks up to SWARM_RPC_TIMEOUT_MS waiting for the response.
 *
 * @param tool_name  Tool to invoke (e.g. "gpio_set")
 * @param params     JSON parameters string
 * @param result     Output buffer for JSON result
 * @param result_sz  Size of result buffer
 * @return CLAW_OK on success, CLAW_ERROR on timeout or no capable node
 */
int swarm_rpc_call(const char *tool_name, const char *params,
                   char *result, size_t result_sz);

#endif /* CLAW_SERVICES_SWARM_H */
