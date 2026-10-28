/**
 * @file claw_port_mem.h
 * @brief PSRAM arena for rt-claw large allocations.
 */

#ifndef CLAW_PORT_MEM_H
#define CLAW_PORT_MEM_H

#include <stddef.h>

void claw_port_mem_init(void);

/* Hooks consumed by osal/rtthread/claw_os_rtthread.c. */
void *claw_port_mem_alloc(size_t size);
void *claw_port_mem_alloc_any(size_t size);
void *claw_port_mem_calloc(size_t nmemb, size_t size);
int   claw_port_mem_owns(void *ptr);
void  claw_port_mem_free(void *ptr);

void  claw_port_mem_info(size_t *total, size_t *used, size_t *free_bytes,
                         size_t *largest);

#endif /* CLAW_PORT_MEM_H */
