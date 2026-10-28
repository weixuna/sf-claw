# SF32LB52-SF-Claw

Smartwatch firmware for the SF32LB52, built on SiFli-SDK, RT-Thread and LVGL v8. It integrates AI chat, WeChat ClawBot, Bluetooth PAN networking and HotUI dynamic interfaces.

> Only the `sf32lb52_osptek` board is supported today. Other boards have not been validated and require a matching BSP port plus on-device testing.

## Features

- **Watch UI**: LVGL v8 honeycomb menu, watch face, chat and WeChat screens.
- **HotUI**: AI-generated high-level `ui.*` JavaScript runs in QuickJS, with hot reload, rollback on failure, touch and swipe gestures.
- **AI chat**: rt-claw + DeepSeek with tool calls; generated UIs run immediately.
- **WeChat ClawBot**: QR-code login, HTTPS long polling, recent messages synced to the Chat screen.
- **Bluetooth PAN**: classic Bluetooth tethering, creates the `b0` network interface.
- **Local storage**: API key, WeChat token and settings live in the `prefdb` FlashDB partition.

## Build

```bash
cd project
scons --board=sf32lb52_osptek -j8
```

## Supported Boards

| Board | Status |
|---|---|
| sf32lb52_osptek | Ported and tested |
| Other boards | Not validated; requires BSP port and testing |

## Hardware

- `sf32lb52_osptek` development board (AMOLED, touch, PA34 key)

## Board Power

Power-pin initialization, power-on and deep-sleep power-down live in the osptek BSP. The application layer no longer touches these pins directly:

```text
customer/boards/sf32lb52_osptek_base/bsp_power.c
```

| Function | Pin | Enable level |
|---|---|---|
| AMOLED | PA9 | High |
| TF card | PA29 | High |
| Sensor LDO | PA11 | High |
| Charger | PA20 | Low |

At boot `BSP_IO_Init()` calls `BSP_Power_Up()` to configure and enable the rails. On deep sleep or shutdown the PM framework calls `BSP_IO_Power_Down()`.

> `CONFIG_BSP_USING_PM` is currently disabled, so the deep-sleep power-down callback is not invoked. Enable PM and validate sleep, wake-up and shutdown on hardware before relying on it.

## HotUI

`src/hotui/` runs a restricted QuickJS environment. The AI only sees the high-level `ui.*` API and never touches LVGL pointers.

```text
hot_ui status
hot_ui stop
hot_ui run
hot_ui reload
hot_ui demo static|timer|breakout|watch|2048
```

Scripts execute on the GUI thread. A syntax error keeps the previous UI alive; a runtime failure rolls back to the last working version.

## Bluetooth PAN

`src/net/bt_pan.c` provides classic Bluetooth PAN. A successful connection creates the `b0` interface:

```text
bt_pan status
bt_pan connect
bt_pan del_bond
bt_pan auto 0|1
```

After enabling Bluetooth tethering on the phone, the serial log shows `[bt_pan] pan connect successed`; run `ifconfig` to see the `b0` address. See [`src/net/README.md`](src/net/README.md).

## rt-claw

The project uses three layers:

```text
third_party/rt-claw              # upstream 0.2.0, kept clean
third_party/rt-claw-port-sf32    # reusable SF32/RT-Thread/SDK port
src/rtclaw                       # project board glue (b0, prefdb, PSRAM, GPIO)
```

`src/ui_bridge` and `src/gui_apps/chat` form the UI layer; `src/net/bt_pan.c` is the network service.

Common serial commands:

```text
net_test https://api.github.com/zen
claw_cfg show
claw_cfg url https://api.deepseek.com/chat/completions
claw_cfg key sk-...
claw_cfg model deepseek-chat
claw_cfg save
claw_ping
claw_test hello
claw_status
claw_mem
claw_ntp
claw_ui Chat
```

> `CONFIG_RT_USING_SAL` is disabled for webclient + mbedTLS compatibility; BT PAN uses lwIP sockets directly.
> The API key and model are stored in the board `prefdb` FlashDB partition; see [`src/rtclaw/README.md`](src/rtclaw/README.md).

## WeChat ClawBot (iLink)

`src/wechat/wechat_ilink.c` implements QR-code login and HTTPS long polling for WeChat ClawBot. The watch renders the QR code with an LVGL RGB565 true-color canvas plus qrcodegen, so no phone browser, public IP or callback URL is required.

- The bot token is stored in `prefdb` and polling resumes automatically after reboot.
- Incoming text messages go to rt-claw through `AI_CHANNEL_WECHAT`; replies are sent with `POST /ilink/bot/sendmessage`.
- The latest 10 WeChat messages are mirrored to the Chat screen.
- Open the QR page from the main menu (`WeChat`), the Chat quick phrase `微信连接`, or `claw_ui WeChat`.

```text
wechat status
wechat login
wechat qr
wechat logout
wechat token
wechat send hello
```

See [`src/wechat/README.md`](src/wechat/README.md) for the protocol, threading model and test procedure.

## Documentation

- [SiFli-SDK Quick Start](https://docs.sifli.com/projects/sdk/latest/sf32lb52x/quickstart/index.html)
- [LVGL Documentation](https://docs.lvgl.io/8.3/)
