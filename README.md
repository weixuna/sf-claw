# SF32LB52-SF-Claw

SF32LB52 智能手表固件，基于 SiFli-SDK、RT-Thread 和 LVGL v8，集成 AI 对话、微信 ClawBot、蓝牙 PAN 联网和 HotUI 动态界面。

> 目前仅适配 `sf32lb52_osptek` 开发板。其他板级配置尚未验证，需要补充对应的 BSP 适配并上机测试后再开放。

## 功能

- **手表主界面**：LVGL v8 蜂窝菜单、表盘、聊天、微信等界面。
- **HotUI**：通过 QuickJS 执行 AI 生成的高层 `ui.*` JavaScript，支持热重载、失败回滚、触摸和滑动手势。
- **AI 对话**：rt-claw + DeepSeek，支持工具调用，可生成并直接运行界面。
- **微信 ClawBot**：扫码登录、HTTPS 长轮询，聊天记录同步到手表 Chat 界面。
- **蓝牙 PAN**：经典蓝牙网络共享，连接后生成 `b0` 网卡。
- **本地存储**：API Key、微信 token 等保存在 `prefdb` FlashDB 分区。

## 构建

```bash
cd project
scons --board=sf32lb52_osptek -j8
```

## 支持的开发板

| 开发板 | 状态 |
|---|---|
| sf32lb52_osptek | 已适配并验证 |
| 其他板卡 | 未验证，需要移植 BSP 后测试 |

## 硬件需求

- `sf32lb52_osptek` 开发板（AMOLED、触摸和 PA34 按键）

## 板级电源

电源初始化、上电和深睡下电都由 osptek BSP 负责，应用层不再直接操作电源引脚：

```text
customer/boards/sf32lb52_osptek_base/bsp_power.c
```

| 功能 | 引脚 | 开启电平 |
|---|---|---|
| AMOLED | PA9 | 高 |
| TF 卡 | PA29 | 高 |
| 传感器 LDO | PA11 | 高 |
| 充电器 | PA20 | 低 |

启动时 `BSP_IO_Init()` 调用 `BSP_Power_Up()` 完成初始化并上电；深睡/关机时由 PM 框架调用 `BSP_IO_Power_Down()` 关闭板级电源。

> 当前未开启 `CONFIG_BSP_USING_PM`，因此深睡下电回调不会触发。需要低功耗时再启用 PM 并上机验证休眠、唤醒和关机流程。

## HotUI

`src/hotui/` 提供受限的 QuickJS 运行环境，AI 只能使用高层 `ui.*` API，不会接触 LVGL 指针。

```text
hot_ui status
hot_ui stop
hot_ui run
hot_ui reload
hot_ui demo static|timer|breakout|watch|2048
```

界面代码在 GUI 线程执行；语法错误不会破坏旧界面，运行时异常会回滚到上一个可用版本。

## 蓝牙 PAN

`src/net/bt_pan.c` 提供经典蓝牙 PAN 接入，连接成功后生成 `b0` 网卡：

```text
bt_pan status
bt_pan connect
bt_pan del_bond
bt_pan auto 0|1
```

手机打开蓝牙网络共享后，串口应出现 `[bt_pan] pan connect successed`，执行 `ifconfig` 可查看 `b0` 的 IP。详细说明见 [`src/net/README.md`](src/net/README.md)。

## rt-claw

工程采用三层结构：

```text
third_party/rt-claw              # 上游 0.2.0，保持干净
third_party/rt-claw-port-sf32    # 可复用 SF32/RT-Thread/SDK 适配
src/rtclaw                       # 本项目 board 注入（b0、prefdb、PSRAM、GPIO）
```

`src/ui_bridge` 和 `src/gui_apps/chat` 是 UI 层，`src/net/bt_pan.c` 是项目网络服务。

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
> API Key/Model 保存在板级 `prefdb` FlashDB 分区，分区配置见 [`src/rtclaw/README.md`](src/rtclaw/README.md)。

## 微信 ClawBot（iLink）

`src/wechat/wechat_ilink.c` 实现微信 ClawBot 扫码登录和 HTTPS 长轮询，手表屏幕用 LVGL RGB565 true-color canvas + qrcodegen 直接显示二维码，不需要手机浏览器或公网回调。

- 登录后 `bot_token` 存在 `prefdb`，重启自动恢复轮询。
- 微信文本消息走独立 `AI_CHANNEL_WECHAT` 交给 rt-claw，回复用 `POST /ilink/bot/sendmessage` 发回。
- 最近 10 条微信消息会同步到 Chat 界面。
- 二维码入口：主菜单“微信”、聊天页快捷指令“微信连接”，或串口 `claw_ui WeChat`。

常用串口命令：

```text
wechat status
wechat login
wechat qr
wechat logout
wechat token
wechat send 你好
```

完整协议、线程模型和联调步骤见 [`src/wechat/README.md`](src/wechat/README.md)。

## 文档

- [SiFli-SDK 快速入门](https://docs.sifli.com/projects/sdk/latest/sf32lb52x/quickstart/index.html)
- [LVGL 官方文档](https://docs.lvgl.io/8.3/)
