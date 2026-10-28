# 手表界面v8

源码路径: example/multimedia/lvgl/watch
这是一个基于LVGL v8实现的智能手表界面示例，包含多种交互界面和字体配置功能。该示例展示了如何使用SiFli-SDK的LVGL图形库组件构建嵌入式设备的用户界面。
基于此示例，开发者可以构建各种智能穿戴设备的UI界面，如运动手表、健康监测设备等。

> 本工程当前只维护 `sf32lb52_osptek` 板级配置，其他板级配置已从 `project/` 中删除。

## 用法

```bash
cd project
scons --board=sf32lb52_osptek -j8
```

## 支持的开发板

- sf32lb52_osptek

## 板级电源

`src/app_utils/power_en.c` 控制 AMOLED、TF 卡、传感器 LDO 和充电器使能，仅适用于 `sf32lb52_osptek`：

| 功能 | 引脚 | 开启电平 |
|---|---|---|
| AMOLED | PA9 | 高 |
| TF 卡 | PA29 | 高 |
| 传感器 LDO | PA11 | 高 |
| 充电器 | PA20 | 低 |

## 蓝牙 PAN

`src/net/bt_pan.c` 提供经典蓝牙 PAN 接入，连接成功后生成 `b0` 网卡：

```text
bt_pan status
bt_pan connect
bt_pan del_bond
bt_pan auto 0|1
```

手机打开蓝牙网络共享后，串口应出现 `[bt_pan] pan connect successed`，执行 `ifconfig` 可查看 `b0` 的 IP。详细说明见 `src/net/README.md`。

## rt-claw

工程采用三层结构：

```text
third_party/rt-claw              # 上游 0.2.0，保持干净
third_party/rt-claw-port-sf32    # 可复用 SF32/RT-Thread/SDK 适配
src/rtclaw                       # 本项目 board 注入（b0、prefdb、PSRAM、GPIO）
```

`src/ui_bridge` 和 `src/gui_apps/chat` 是 UI 层；`src/net/bt_pan.c` 是项目网络服务。
完整说明见 `docs/rt-claw-port-layout.md`。

常用串口命令：

```text
net_test https://api.github.com/zen
claw_cfg show
claw_cfg url https://api.deepseek.com/chat/completions
claw_cfg key sk-...
claw_cfg model deepseek-chat
claw_cfg save
claw_ping
claw_test 你好
claw_status
claw_mem
claw_ntp
claw_ui Chat
```

> 本工程为 webclient + mbedTLS 兼容关闭了 `CONFIG_RT_USING_SAL`；BT PAN 直接使用 lwIP socket。
> API Key/Model 保存在板级 `prefdb` FlashDB 分区，分区配置见 `src/rtclaw/README.md`。

## 微信 ClawBot（iLink）

`src/wechat/wechat_ilink.c` 实现了微信 ClawBot 扫码登录和 HTTPS 长轮询，
手表屏幕用 LVGL RGB565 true-color canvas + qrcodegen 直接显示二维码（避开 EPIC 不支持的 1-bit QR canvas），不需要手机浏览器或公网回调。

- 登录后 `bot_token` 存在 `prefdb`，重启自动恢复轮询。
- 微信文本消息走独立 `AI_CHANNEL_WECHAT` 交给 rt-claw，回复用
  `POST /ilink/bot/sendmessage` 发回。
- 二维码入口：主菜单“微信”、聊天页快捷指令“微信连接”，或串口
  `claw_ui WeChat`。

常用串口命令：

```text
wechat status
wechat login
wechat qr
wechat logout
wechat token
wechat send 你好
```

完整协议、线程模型、联调步骤和已知限制见
[`src/wechat/README.md`](src/wechat/README.md)。

## 硬件需求

- `sf32lb52_osptek` 开发板（含 AMOLED、触摸和 PA34 按键）

## 1. 指定字体

例如当 `font_name` 是 `DroidSansFallback` 时，则相当于添加了如下宏定义：
```c
#define FREETYPE_FONT_NAME   DroidSansFallback
```

编译时会在 `freetype` 子目录里查找以 `.ttf` 为后缀的字体文件，并将其转换为 C 文件加入编译过程：
```python
objs = Glob('freetype/{}.ttf'.format(font_name))
objs = Env.FontFile(objs)
```

编译完成后，TTF 文件会被转换成 C 文件，生成于 `build_xxx_hcpu/src/resource/fonts/freetype` 目录下，文件名为 `{font_name}.c`。该 C 文件中调用了字体注册宏，将字体注册进 LVGL，从而可以在 LVGL 中使用：
```c
LVSF_FREETYPE_FONT_REGISTER(tiny55_full)
```

像 `FREETYPE_TINY_FONT_FULL` 这样的宏是在工程目录下的 `Kconfig.proj` 中定义的，形式如下：
```kconfig
config FREETYPE_TINY_FONT_FULL
    bool
    default y
```

## 2. 完整 `src/resource/fonts/SConscript` 实例（支持自定义多字体使用）

```python
CPPDEFINES = []

font_name = ''  # 默认字体选项，根据工程目录下的 [Kconfig.proj] 中定义
font_name2 = 'SourceHanSansCN_Normal'  # 自定义 TTF 字体文件名，需放置在 `src/resource/fonts/freetype` 目录下

if GetDepend('FREETYPE_TINY_FONT_FULL'):
    font_name = 'tiny55_full'
elif GetDepend('FREETYPE_TINY_FONT_LITE'):
    font_name = 'tiny55_lite'
elif GetDepend('FREETYPE_HANSANS_FONT'):
    font_name = 'SourceHanSansCN_Normal'
elif GetDepend('FREETYPE_ARIAL_FONT'):
    font_name = 'arial'
else:
    font_name = 'DroidSansFallback'

objs = Glob('freetype/{}.ttf'.format(font_name))
objs = Env.FontFile(objs)

objs2 = Glob('freetype/{}.ttf'.format(font_name2))  # 寻找自定义字体文件
objs += Env.FontFile(objs2)  # 将自定义 TTF 文件转为 C 文件
```

## 函数使用
```c
//使用默认配置font_name字体接口
void lv_ext_set_local_font(lv_obj_t *obj, uint16_t size, lv_color_t color)

//根据配置注册字体名称，使用自定义字体接口
void lv_ext_set_font_local_by_name(lv_obj_t *obj, uint16_t size, lv_color_t color, char *fontname)

```
## 异常诊断
在多字体使用时，ttf 文件总和可能过大，超出事先预准备的内存大小，例如以下编译报错，此时需要修改 `project/xxx_hcpu/ptab.yaml` 中字体资源分区的大小，注意修改大小需要保证前后段的地址连续，另外还需考虑不同开发板所支持的存储容量。

对于该 watch 示例，ptab v3 的资源分区现在使用 `type: app`、`subtype: ex`，并通过 partition 的 `sections` 描述资源收集规则：

- 图片资源分区默认收集分区名对应的 section，同时可以通过 `sections` 兼容旧工程里遗留的 `.ROM1_IMG*`、`.ROM3_IMG*` 输入 section
- 字体资源分区通过 `sections` 收集 `lvsf_font_*` 对象中的 `.rodata*` 等输入 section

资源 bin 文件名仍按分区名生成，但链接与切包不再依赖固定的 `.rom2/.rom3` MEMORY 布局。
```
region'ROM' overflowed by 7880732 bytes
```
## 示例输出

成功运行后，开发板屏幕将显示手表主界面，包含蜂窝菜单和表盘显示。通过触摸或按键可以在不同界面间切换。

## 参考文档

- [SiFli-SDK 快速入门](https://docs.sifli.com/projects/sdk/latest/sf32lb52x/quickstart/index.html)
- [LVGL官方文档](https://docs.lvgl.io/8.3/)
