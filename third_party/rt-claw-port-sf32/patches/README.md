# Upstream patches

The SF32 port does **not** patch `third_party/rt-claw` in place.

Compatibility is handled by the port package instead:

- `port/claw_os_rtthread_sf32.c` replaces the upstream RT-Thread OSAL
  (`rt_mq_send_wait` compatibility, PSRAM allocation hooks).
- `include/claw_config.h` shadows the upstream config with SF32 defaults.
- `include/claw/services/ai/ai_engine.h` adds the UI channel via
  `#include_next`, without editing the upstream header.

If a future upstream change cannot be handled by a shim, add a patch here
and document how it is applied. Do not edit `third_party/rt-claw` directly.
