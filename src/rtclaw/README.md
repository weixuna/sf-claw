# sf32lb52_osptek board adapter for rt-claw

This directory is the **only** place where board differences live:

```text
src/rtclaw/
├─ SConscript     # thin entry: points the port package at third_party/rt-claw
├─ board_port.c   # claw_board_ops: b0 / bt_pan / prefdb / PSRAM / GPIO
├─ project_glue.c # claw_port_on_ready() -> ui_bridge_set_ai_ready()
└─ README.md      # board wiring notes (this file)
```

`third_party/rt-claw` is upstream and stays clean.  
`third_party/rt-claw-port-sf32` is the reusable SF32/RT-Thread/SDK layer.  
`src/net/bt_pan.c` remains a project/board network service.  
`src/ui_bridge` and `src/gui_apps/chat` are UI-layer code, not SF32 port code.

## Board facts injected here

| Item | Value |
|---|---|
| Board name | `sf32lb52-osptek` |
| Network device | `b0` (BT PAN) |
| Network bring-up | `src/net/bt_pan.c` → `bt_pan_connect()` |
| KV partition | `prefdb` (FlashDB/FAL) |
| PSRAM arena | `CONFIG_RTCLAW_PORT_PSRAM_SIZE` |
| LLM GPIO whitelist | PA32 RGB LED only |

## Required board configuration (project-local, no SDK edits)

The reusable port package does **not** add partitions.  This board needs a
`prefdb` FAL partition, but it is supplied by project-local overrides that the
SiFli build system already supports:

- `project/sf32lb52_osptek_hcpu/ptab.json`
- `project/sf32lb52_osptek_hcpu/custom_mem_map.h`

The SDK `customer/boards/sf32lb52_osptek/...` files are left untouched.

### 1. `prefdb` partition

`project/sf32lb52_osptek_hcpu/ptab.json`:

```json
{
    "offset": "0x00CA8000",
    "max_size": "0x00010000",
    "tags": ["KVDB_PREFDB_REGION"]
}
```

`project/sf32lb52_osptek_hcpu/custom_mem_map.h`:

```c
#define FAL_PART_TABLE \
{ \
    {FAL_PART_MAGIC_WORD, "dfu",    NOR_FLASH2_DEV_NAME, KVDB_DFU_REGION_OFFSET,    KVDB_DFU_REGION_SIZE, 0}, \
    {FAL_PART_MAGIC_WORD, "ble",    NOR_FLASH2_DEV_NAME, KVDB_BLE_REGION_OFFSET,    KVDB_BLE_REGION_SIZE, 0}, \
    {FAL_PART_MAGIC_WORD, "prefdb", NOR_FLASH2_DEV_NAME, KVDB_PREFDB_REGION_OFFSET, KVDB_PREFDB_REGION_SIZE, 0}, \
}
```

If another board uses a different partition name or offset, change only its board files and set
`claw_board_ops.kv_partition` accordingly.  The port package itself has no board table.

### 2. No linker script changes required

The port package shadows `claw/core/claw_class.h` and turns the
`CLAW_TOOL_REGISTER` / `CLAW_SERVICE_REGISTER` / `CLAW_DRIVER_REGISTER`
macros into `__attribute__((constructor))` registrations.  The standard
SiFli `.init_array` startup applies them before `claw_init()`, so the
project works with a **stock SDK linker script**.

If you ever switch the port back to upstream section-based registration,
then the `claw_tools` / `claw_services` / `claw_drivers` input sections must
be placed inside `.data` (with `KEEP` and explicit `__start_/__stop_`
symbols), otherwise `claw_init()` sees garbage pointers at runtime.

`third_party/rt-claw-port-sf32/docs/README.md` documents both modes.

## Network

`board_net_prepare()` only calls `bt_pan_connect()` when PAN is not connected.  The BT PAN
state machine, pairing, retry and netdev setup stay in `src/net/bt_pan.c`.  Boards without
BT PAN can implement `net_prepare()` for WiFi/Ethernet, or leave it `NULL` when the netdev
is brought up by the SDK.

## API key input

The Chat app's Settings page uses a password-mode textarea and is the preferred way to enter
the API key.  `claw_cfg key ...` is for UART bring-up only and will appear in Finsh history;
do not use it on production units.

## Adding another SF32 board

1. Add a `src/rtclaw`-equivalent directory (or a new board file) that implements
   `claw_board_get()`.
2. Fill in `netdev_name`, `net_prepare`, `kv_partition`, `psram_size`, `gpio_allowed`.
3. If persistent KV is wanted, add the board's `prefdb` partition through a
   project-local `project/<board>_hcpu/ptab.json` and `custom_mem_map.h`
   override (the SDK board files stay untouched).
4. Include `third_party/rt-claw-port-sf32/SConscript` with `RT_CLAW_ROOT` pointing at
   `third_party/rt-claw`.

No changes to `third_party/rt-claw-port-sf32` are required for a normal board port.
