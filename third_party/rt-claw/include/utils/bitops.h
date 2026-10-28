/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Bit manipulation and common arithmetic macros.
 */

#ifndef CLAW_UTILS_BITOPS_H
#define CLAW_UTILS_BITOPS_H

#include <stdint.h>
#include <stddef.h>

/* ---- Bit operations ---- */

#define BIT(n)                  (1UL << (n))
#define BIT64(n)                (1ULL << (n))

#define SET_BIT(x, n)           ((x) |= BIT(n))
#define CLR_BIT(x, n)           ((x) &= ~BIT(n))
#define TEST_BIT(x, n)          (!!((x) & BIT(n)))
#define TOGGLE_BIT(x, n)        ((x) ^= BIT(n))

#define BITS_SET(x, mask)       ((x) |= (mask))
#define BITS_CLR(x, mask)       ((x) &= ~(mask))
#define BITS_TEST_ALL(x, mask)  (((x) & (mask)) == (mask))
#define BITS_TEST_ANY(x, mask)  (!!((x) & (mask)))

/* Extract bits [hi:lo] from val */
#define BITS_GET(val, hi, lo) \
    (((val) >> (lo)) & ((1UL << ((hi) - (lo) + 1)) - 1))

/* ---- Alignment ---- */

#define ALIGN_UP(x, a)          (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a)        ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a)        (((x) & ((a) - 1)) == 0)

/* ---- Common helpers ---- */

#define ARRAY_SIZE(arr)         (sizeof(arr) / sizeof((arr)[0]))

#define MIN(a, b)               (((a) < (b)) ? (a) : (b))
#define MAX(a, b)               (((a) > (b)) ? (a) : (b))
#define CLAMP(val, lo, hi)      MIN(MAX(val, lo), hi)

#define UNUSED(x)               ((void)(x))

/* Compile-time assertion (C99 compatible) */
#define STATIC_ASSERT(cond, msg) \
    typedef char static_assert_##msg[(cond) ? 1 : -1]

/* container_of — get pointer to enclosing struct from member pointer */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* ---- Byte swap ---- */

static inline uint16_t bswap16(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint32_t bswap32(uint32_t x)
{
    return ((x >> 24) & 0x000000FFUL) |
           ((x >>  8) & 0x0000FF00UL) |
           ((x <<  8) & 0x00FF0000UL) |
           ((x << 24) & 0xFF000000UL);
}

#endif /* CLAW_UTILS_BITOPS_H */
