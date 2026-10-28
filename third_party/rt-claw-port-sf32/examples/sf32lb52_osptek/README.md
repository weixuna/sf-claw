# sf32lb52_osptek reference

This directory is intentionally **not** part of the default build.

The real board adapter for this project lives in:

```text
watch/src/rtclaw/board_port.c
watch/src/rtclaw/README.md
```

That file shows the complete `claw_board_ops` implementation for:

- `b0` BT PAN netdev
- `bt_pan_connect()` via `src/net/bt_pan.c`
- `prefdb` FlashDB partition
- 512KB PSRAM arena
- PA32 RGB LED GPIO whitelist
- `claw_port_on_ready()` → `ui_bridge_set_ai_ready()`

To port another SF32 board, copy that pattern into the new project; do not
add a new board directory to `rt-claw-port-sf32`.
