/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * WiFi STA manager for Espressif SoCs (ESP32-C3, ESP32-S3, etc.).
 */

#ifndef CLAW_DRIVERS_NET_ESPRESSIF_WIFI_MANAGER_H
#define CLAW_DRIVERS_NET_ESPRESSIF_WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Initialize WiFi subsystem (STA mode). */
esp_err_t wifi_manager_init(void);

/* Start WiFi connection using NVS or build-time credentials. */
esp_err_t wifi_manager_start(void);

/* Block until connected or timeout. */
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);

/* Check connection state. */
bool wifi_manager_is_connected(void);

/* Get current IP address string (or "0.0.0.0"). */
const char *wifi_manager_get_ip(void);

/* Save WiFi credentials to NVS. */
esp_err_t wifi_manager_set_credentials(const char *ssid,
                                       const char *password);

/* Update runtime WiFi config and trigger reconnect. */
esp_err_t wifi_manager_reconnect(const char *ssid, const char *password);

/* Scan and print nearby APs. */
void wifi_manager_scan_and_print(void);

#endif /* CLAW_DRIVERS_NET_ESPRESSIF_WIFI_MANAGER_H */
