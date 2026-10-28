/**
 * @file claw_mem_psram.c
 * @brief Route rt-claw large allocations to a dedicated PSRAM memheap.
 *
 * The board decides how much PSRAM to reserve through the
 * RTCLAW_PORT_PSRAM_SIZE Kconfig value (or a compile-time define).  Only
 * allocations >= CLAW_PORT_PSRAM_MIN_ALLOC are redirected; small objects
 * keep using the faster SRAM heap.  TLS and network stack buffers stay in
 * SRAM.
 */

#include <rtthread.h>
#include <rtconfig.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "mem_section.h"
#include "claw_port.h"
#include "claw_port_mem.h"

#ifndef RTCLAW_PORT_PSRAM_SIZE
#define RTCLAW_PORT_PSRAM_SIZE (512 * 1024)
#endif

#define CLAW_PORT_PSRAM_MIN_ALLOC 256

/* Placed in PSRAM by the SiFli linker script (.RW_PSRAM1). */
static uint8_t s_claw_psram[RTCLAW_PORT_PSRAM_SIZE]
    L2_RET_BSS_SECT(claw_psram);

static struct rt_memheap s_claw_heap;
static bool s_claw_heap_ready;

void claw_port_mem_init(void)
{
    const struct claw_board_ops *board;
    size_t size = sizeof(s_claw_psram);

    if (s_claw_heap_ready) {
        return;
    }

    board = claw_board_get();
    if (board && board->psram_size && board->psram_size < size) {
        size = board->psram_size;
    }

    if (rt_memheap_init(&s_claw_heap, "claw_psram",
                        s_claw_psram, size) == RT_EOK) {
        s_claw_heap_ready = true;
        rt_kprintf("[claw_mem] psram heap %u bytes ready\n",
                   (unsigned)size);
    } else {
        rt_kprintf("[claw_mem] psram heap init failed\n");
    }
}

void *claw_port_mem_alloc(size_t size)
{
    if (!s_claw_heap_ready || size < CLAW_PORT_PSRAM_MIN_ALLOC) {
        return NULL;
    }

    return rt_memheap_alloc(&s_claw_heap, size);
}

/*
 * Bypass the small-object threshold.  Used by the cJSON hooks so that
 * every JSON node and string stays out of the fragmented SRAM heap.
 */
void *claw_port_mem_alloc_any(size_t size)
{
    if (!s_claw_heap_ready || size == 0) {
        return NULL;
    }

    return rt_memheap_alloc(&s_claw_heap, size);
}

void *claw_port_mem_calloc(size_t nmemb, size_t size)
{
    size_t bytes;
    void *p;

    if (nmemb == 0 || size == 0) {
        return NULL;
    }
    if (nmemb > (SIZE_MAX / size)) {
        return NULL;
    }

    bytes = nmemb * size;
    p = claw_port_mem_alloc(bytes);
    if (p) {
        memset(p, 0, bytes);
    }
    return p;
}

int claw_port_mem_owns(void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)s_claw_psram;
    uintptr_t end = start + sizeof(s_claw_psram);

    return (p >= start && p < end) ? 1 : 0;
}

void claw_port_mem_free(void *ptr)
{
    if (ptr && claw_port_mem_owns(ptr)) {
        rt_memheap_free(ptr);
    }
}

void claw_port_mem_info(size_t *total, size_t *used, size_t *free_bytes,
                        size_t *largest)
{
    if (total) {
        *total = s_claw_heap_ready ? s_claw_heap.pool_size : 0;
    }
    if (free_bytes) {
        *free_bytes = s_claw_heap_ready ? s_claw_heap.available_size : 0;
    }
    if (used) {
        *used = s_claw_heap_ready
                    ? s_claw_heap.pool_size - s_claw_heap.available_size
                    : 0;
    }
    if (largest) {
        /* RT-Thread 3.1.3 memheap does not expose the largest block. */
        *largest = s_claw_heap_ready ? s_claw_heap.available_size : 0;
    }
}
