/**
 * @file claw_port_cfg.h
 * @brief AI configuration persistence helpers.
 */

#ifndef CLAW_PORT_CFG_H
#define CLAW_PORT_CFG_H

#include <stddef.h>

void claw_port_ai_config_load(void);
int  claw_port_ai_config_save(void);
int  claw_port_ai_config_set(const char *url, const char *key,
                             const char *model);
void claw_port_ai_key_masked(char *buf, size_t size);

#endif /* CLAW_PORT_CFG_H */
