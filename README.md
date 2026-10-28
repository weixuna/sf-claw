# SF32LB52-SF-Claw

本项目是基于RT-Claw进行修改，集成 AI 对话、微信 ClawBot、蓝牙 PAN 联网和 HotUI 动态界面。

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

- `sf32lb52_osptek` 自制板

## 串口配置 API

> AI 对话、微信和 NTP 都需要网络。手表没有 Wi-Fi，必须先通过手机的“蓝牙网络共享”建立 BT PAN，并确认 `b0` 拿到 IP，否则 API 会连接失败。

### 1. 先连接 BT PAN

```text
bt_pan status
bt_pan auto 1
bt_pan connect
ifconfig          # 确认 b0 获得 192.168.x.x 地址
```

手机需要先打开“蓝牙网络共享”。如果先配对、后开共享，执行 `bt_pan connect` 重新发起 PAN。

### 2. 配置 API

首次使用或更换模型时，通过串口配置 API：

```text
claw_cfg show
claw_cfg url https://api.deepseek.com/chat/completions
claw_cfg key sk-你的key
claw_cfg model deepseek-chat
claw_cfg save
```

### 3. 验证配置

```text
claw_cfg show     # key 只显示掩码，确认不是 (not set)
claw_ping         # 检查 API 是否可达
claw_test 你好    # 发起一次完整对话
```

说明：

- `claw_cfg url/key/model` 会立即更新运行时配置并写入 `prefdb`，`claw_cfg save` 会再保存一次当前值。
- 也可以在 Chat 界面的“设置”中直接填写 API URL、API Key 和 Model。
- `claw_cfg key` 会留在 Finsh 历史里，正式设备建议使用 Chat 设置页。
- API Key 在 `prefdb` 中是明文存储，UI 的密码模式只是显示掩码，不是加密。
- SDK 的 `webclient` 会把请求头和 POST body 打印到串口，可能包含 API Key 和聊天内容；调试完请轮换 Key。

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

`src/net/bt_pan.c` 提供经典蓝牙 PAN 接入，连接成功后生成 `b0` 网卡。AI 对话、微信和 NTP 都必须先建立这个连接：

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
