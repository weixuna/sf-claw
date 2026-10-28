# Watch Interface

Source Code Path: example/multimedia/lvgl/watch
This is a smartwatch interface example implemented based on LVGL v8, featuring various interactive interfaces and font configuration capabilities. It demonstrates how to use the LVGL graphics library component of SiFli-SDK to build user interfaces for embedded devices.
Developers can use this example as a foundation to build UI interfaces for various smart wearable devices such as sports watches and health monitoring devices.

> This project currently only maintains the `sf32lb52_osptek` board configuration. Other board configurations have been removed from `project/`.

## Usage

```bash
cd project
scons --board=sf32lb52_osptek -j8
```

## Supported Boards

- sf32lb52_osptek

## Board Power

`src/app_utils/power_en.c` controls the AMOLED, TF card, sensor LDO and charger enables. It is only valid for `sf32lb52_osptek`:

| Function | Pin | Enable level |
|---|---|---|
| AMOLED | PA9 | High |
| TF card | PA29 | High |
| Sensor LDO | PA11 | High |
| Charger | PA20 | Low |

## WeChat ClawBot (iLink)

`src/wechat/wechat_ilink.c` implements QR-code login and HTTPS long polling
for WeChat ClawBot. The watch renders the QR code with an LVGL RGB565 true-color canvas plus
qrcodegen (the Sifli EPIC GPU does not support the 1-bit canvas used by
`lv_qrcode`), so no phone browser, public IP or callback URL is required.

- The bot token is stored in the board `prefdb` partition and polling resumes
  automatically after reboot.
- Incoming text messages are sent to rt-claw through `AI_CHANNEL_WECHAT`;
  replies are delivered with `POST /ilink/bot/sendmessage`.
- Open the QR page from the main menu (`WeChat`), the Chat quick phrase
  `微信连接`, or the serial command `claw_ui WeChat`.

```text
wechat status
wechat login
wechat qr
wechat logout
wechat token
wechat send hello
```

See [`src/wechat/README.md`](src/wechat/README.md) for the protocol,
threading model and test procedure.

## Hardware Requirements

- `sf32lb52_osptek` development board (AMOLED, touch and PA34 key)

## Specifying Fonts

Refer to `src/resource/fonts/SConscript`. By adding the `FREETYPE_FONT_NAME` macro definition in CPPDEFINES, you can register the corresponding TTF font to LVGL
```python
CPPDEFINES += ["FREETYPE_FONT_NAME={}".format(font_name)]
```
If `font_name` is `DroidSansFallback`, it is equivalent to adding the following macro definition
```c
#define FREETYPE_FONT_NAME   DroidSansFallback
```
During compilation, it will search for font files with `.ttf` suffix in the `freetype` subdirectory and convert them to C files for compilation
```python
objs = Glob('freetype/{}.ttf'.format(font_name))
objs = Env.FontFile(objs)
```
Macros like `FREETYPE_TINY_FONT_FULL` are defined in `Kconfig.proj` in the project directory, similar to the following:
```kconfig
config FREETYPE_TINY_FONT_FULL
    bool
    default y
```

## Example Output

After successful execution, the development board screen will display the watch main interface, including a honeycomb menu and watch face display. You can switch between different interfaces via touch or buttons.

## Reference Documentation

- [SiFli-SDK Quick Start](https://docs.sifli.com/projects/sdk/latest/sf32lb52x/quickstart/index.html)
- [LVGL Official Documentation](https://docs.lvgl.io/8.3/)