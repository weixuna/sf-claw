/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 *
 * 项目级通用 FAL 分区表。
 *
 * 这里不再按板子复制 ptab.json / custom_mem_map.h：
 *   - Flash 分区由当前 BSP 的 ptab.json 决定；
 *   - 项目只在 KVDB_BLE 之后追加 64KB 的 prefdb 原始分区给 rt-claw。
 * 因此同一份工程可以配合任意包含 KVDB_BLE_REGION 的 SF32 BSP 构建。
 */

#ifndef __CUSTOM_MEM_MAP__
#define __CUSTOM_MEM_MAP__

#ifdef USING_PARTITION_TABLE
    #include "ptab.h"
#endif /* USING_PARTITION_TABLE */

#ifndef KVDB_BLE_REGION_OFFSET
    #error "BSP ptab must define KVDB_BLE_REGION for rt-claw prefdb"
#endif

/* prefdb 紧跟在 BLE KV 之后，占用 64KB 原始 Flash。 */
#define KVDB_PREFDB_REGION_SIZE    0x00010000
#ifndef KVDB_PREFDB_REGION_OFFSET
    #define KVDB_PREFDB_REGION_OFFSET \
        (KVDB_BLE_REGION_OFFSET + KVDB_BLE_REGION_SIZE)
#endif

#define FAL_PART_TABLE \
{ \
    {FAL_PART_MAGIC_WORD, "dfu",    NOR_FLASH2_DEV_NAME, KVDB_DFU_REGION_OFFSET,   KVDB_DFU_REGION_SIZE, 0}, \
    {FAL_PART_MAGIC_WORD, "ble",    NOR_FLASH2_DEV_NAME, KVDB_BLE_REGION_OFFSET,   KVDB_BLE_REGION_SIZE, 0}, \
    {FAL_PART_MAGIC_WORD, "prefdb", NOR_FLASH2_DEV_NAME, KVDB_PREFDB_REGION_OFFSET, KVDB_PREFDB_REGION_SIZE, 0}, \
}

#endif  /* __CUSTOM_MEM_MAP__ */
