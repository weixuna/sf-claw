# 微信 ClawBot（iLink）接入

本目录实现手表侧的微信 ClawBot 客户端。它不是公众号，也不是 WebSocket；
登录和收发消息全部是发往 `ilinkai.weixin.qq.com` 的出站 HTTPS 请求，
因此可以直接跑在手机蓝牙共享网络（BT PAN，`b0`）后面，不需要公网 IP、
回调 URL 或 IP 白名单。

## 功能

- 手表屏幕直接显示微信登录二维码（LVGL RGB565 canvas + qrcodegen）。
- 扫码确认后保存 `bot_token` / `base_url` / `ilink_bot_id` 到 `prefdb`。
- `getupdates` 35 秒长轮询接收微信消息。
- 文本消息交给 rt-claw AI（独立 `AI_CHANNEL_WECHAT`），回复通过
  `sendmessage` 发回微信。
- 重启后自动恢复轮询，不需要重新扫码。
- 手机蓝牙共享断开后等待 `b0` 恢复，不退出服务。

## 文件

```text
src/wechat/
├── wechat_ilink.c       服务线程：扫码、KV、长轮询、AI、发送
├── wechat_ilink.h       对外状态快照 / 控制接口
├── wechat_http.c        自定义超时的 HTTPS 请求 + HTTP 响应解析
├── wechat_http.h
├── SConscript
└── README.md

src/gui_apps/wechat/
├── app_wechat.c         微信界面：二维码、状态、按钮
└── SConscript
```

相关改动：

| 文件 | 作用 |
|---|---|
| `src/ui_bridge/ui_bridge.c` | `ui_bridge_ai_chat()` 串行化 UI/微信对 AI 通道的切换 |
| `src/rtclaw/project_glue.c` | rt-claw 就绪后启动微信服务 |
| `third_party/rt-claw-port-sf32/include/claw/services/ai/ai_engine.h` | 增加 `AI_CHANNEL_WECHAT` |
| `third_party/rt-claw-port-sf32/port/claw_net_webclient.c` | 避免 webclient 重复连接造成的 TLS 上下文/套接字泄漏 |
| `src/gui_apps/chat/app_chat.c` | 聊天页快捷指令“微信连接”打开微信界面 |
| `src/resource/strings/*.json` | 主菜单“微信”标签 |

## 线程模型

```text
GUI 线程
  └─ WeChat app 每 500 ms 读取 wechat_ilink_get_status()
       └─ 只有未连接/未登录时才调用 login_start()

wechat 服务线程（12 KB 栈，优先级 24；含 TLS 发送路径）
  ├─ 从 KV 恢复 token，或等待登录请求
  ├─ QR 登录：get_bot_qrcode → get_qrcode_status
  ├─ 在线：getupdates（35 s）→ 逐条处理
  └─ 文本消息 → ui_bridge_ai_chat(AI_CHANNEL_WECHAT) → sendmessage

ui_ai 线程（原有）
  └─ 聊天界面消息 → ui_bridge_ai_chat(AI_CHANNEL_UI)
```

LVGL 只在 GUI 线程操作；服务线程不碰任何 `lv_*`。
二维码数据通过 `wechat_ilink_status_t` 快照传给 GUI。

## 协议

默认地址：

```text
https://ilinkai.weixin.qq.com
```

CDN 地址暂未使用（当前只支持文本）：

```text
https://novac2c.cdn.weixin.qq.com/c2c
```

### 1. 获取二维码

优先使用 ESP-Claw 的 GET 形式：

```http
GET /ilink/bot/get_bot_qrcode?bot_type=3
```

如果 GET 被拒绝或返回 JSON 业务错误，会再尝试当前腾讯插件使用的 POST：

```http
POST /ilink/bot/get_bot_qrcode?bot_type=3
Content-Type: application/json

{"local_token_list":[]}
```

响应：

```json
{
  "qrcode": "qrc_...",
  "qrcode_img_content": "https://weixin.qq.com/x/..."
}
```

- `qrcode` 只用于轮询扫码状态。
- `qrcode_img_content` 是扫码 payload；手表用 qrcodegen 直接编码它，
  不下载图片。

### 2. 轮询扫码状态

```http
GET /ilink/bot/get_qrcode_status?qrcode=<urlencoded qrcode>
```

状态：

| status | 处理 |
|---|---|
| `wait` | 等待扫码 |
| `scanned` / `scaned` | 已扫码，提示在手机上确认 |
| `scaned_but_redirect` | 切换 `redirect_host` 后继续 |
| `confirmed` | 保存 `bot_token`、`baseurl`、`ilink_bot_id`、`ilink_user_id` |
| `expired` | 重新获取二维码（最多 3 次） |
| `binded_redirect` | 若带 `bot_token` 按成功处理，否则报错 |
| `need_verifycode` | 暂不支持输入验证码，提示重试 |

二维码本地 TTL 为 4 分钟，避免服务端不返回 `expired` 时一直显示旧码。

### 3. 收消息

```http
POST /ilink/bot/getupdates
Authorization: Bearer <bot_token>
Content-Type: application/json

{
  "get_updates_buf": "<上一次响应里的游标，首次为空>",
  "base_info": { "channel_version": "rt-claw-wechat" }
}
```

- 服务端会保持连接约 35 秒。`wechat_http.c` 会把本次 socket 超时设成 50 秒，
  覆盖 webclient 默认的 6 秒，避免长轮询在 6 秒被 `-0x4C` 打断。
- 响应 `msgs[]` 中取 `from_user_id`（群聊取 `group_id`）作为 `chat_id`。
- 每条消息保存 `context_token`，回复时必须原样带回。
- 只处理 `item_list[]` 里 `type == 1` 的 `text_item.text`；
  图片/语音/文件/视频只回一句“目前只支持文字消息”。
- 用 `message_id` / `client_id` 做 16 槽去重，防止同一批消息重复回复。

### 4. 发消息

```http
POST /ilink/bot/sendmessage
Authorization: Bearer <bot_token>
Content-Type: application/json

{
  "msg": {
    "from_user_id": "",
    "to_user_id": "<chat_id>",
    "client_id": "rtclaw-xxxxxxxx",
    "message_type": 2,
    "message_state": 2,
    "context_token": "<收到的 context_token>",
    "item_list": [
      { "type": 1, "text_item": { "text": "..." } }
    ]
  },
  "base_info": { "channel_version": "rt-claw-wechat" }
}
```

长回复按 3000 字节分片，并在 UTF-8 字符边界切分。

### 公共请求头

```text
iLink-App-Id: bot
iLink-App-ClientVersion: 131329
X-WECHAT-UIN: base64(随机 uint32 的十进制字符串)
AuthorizationType: ilink_bot_token
Authorization: Bearer <bot_token>   # 仅在已登录接口
SKRouteTag: <可选，本工程未配置>
```

## KV 持久化

命名空间 `wechat`，存在板级 `prefdb` 分区：

```text
wechat.token
wechat.base_url
wechat.bot_id
wechat.user_id
```

重启后 `wx_load_credentials()` 读取 token，直接进入轮询。
只有扫码成功和登出/会话过期时才会写 Flash，不会在每条消息后写。

## 二维码显示

`src/gui_apps/wechat/app_wechat.c`：

- 466×466 圆屏安全区内放 248×248 白色卡片，内部 220×220 QR。
- **不能使用 LVGL 自带 `lv_qrcode`**：它生成 1-bit indexed canvas，
  Sifli EPIC GPU 不支持该格式，且 GPU 路径不会回退到软件绘制，屏幕会是空白。
- 本工程使用 `lv_canvas` + `LV_IMG_CF_TRUE_COLOR`（RGB565），
  QR 模块由 `qrcodegen_encodeText()` 直接画进 true-color 缓冲区。
  之前尝试过 L8 indexed 格式，但在该屏上出现蓝色和行列断裂，因此改为 RGB565。
- QR 缓冲区约 97 KB，使用 `claw_malloc()` 放在 PSRAM；应用退出时释放。
- 只在 payload 变化时重新生成 QR。
- 未连接时显示“点下方按钮生成微信二维码”；扫码后显示确认提示；
  连接成功显示“微信已连接 / 微信消息由 rt-claw 自动回复”。
- 主按钮随状态变化：生成二维码 / 取消登录 / 重新扫码；
  已连接时右侧出现“断开”。

入口：

- 主菜单里的“微信”。
- 聊天页快捷指令“微信连接”。
- 串口 `claw_ui WeChat`。

## 串口命令

```text
wechat status      查看状态、bot_id、收发计数、最近消息
wechat login       重新生成二维码（屏幕同步显示）
wechat qr          打印当前 qrcode 和 qr_payload
wechat cancel      取消当前扫码
wechat logout      删除 token，停止轮询
wechat token       打印 token/base_url（仅调试用）
wechat send <文本> 向最近一次微信会话发送文本
```

`wechat test <文本>` 与 `wechat send` 相同，方便最小联调。

## 联调步骤

1. 手机打开“蓝牙网络共享 / 蓝牙 tethering”，手表与手机配对。
2. 串口确认：

   ```text
   bt_pan status
   ifconfig
   net_test https://ilinkai.weixin.qq.com
   ```

   `b0` 必须拿到 IP，且 `net_test` 能完成 HTTPS。
3. 确认系统时间（TLS 证书校验需要）：

   ```text
   date
   ```

   SNTP 使用 UDP/123，有些手机共享网络会拦截；本工程会先尝试多个
   NTP 服务器，再尝试 HTTP `Date:` 头，最后用固件编译时间兜底，
   保证 TLS 证书校验能启动。后台仍会继续重试 SNTP 校正时间。

4. 打开手表主菜单 → 微信，或聊天页点“微信连接”。
   屏幕应显示二维码，串口同时打印：

   ```text
   [wechat] qrcode=...
   [wechat] qr_payload=...
   ```

5. 微信扫码并在手机上确认。屏幕状态依次变为：
   `等待微信扫码` → `已扫码，请在手机上确认` → `微信已连接`。
6. 在微信里给 ClawBot 发“你好”。串口应出现 getupdates 日志，
   手表通过 rt-claw AI 生成回复并调用 sendmessage；微信里收到回复。
7. 重启手表，确认无需重新扫码：

   ```text
   wechat status
   ```

   应看到 `state=connected` 和已保存的 `bot`。
8. 断开手机蓝牙共享再打开，观察服务是否在 `b0` 恢复后继续轮询。

## 已知限制

- 只支持文本；图片/语音/文件/视频未实现 CDN 上传下载。
- 不支持 `need_verifycode` 的验证码输入界面。
- 没有做 HTTP keep-alive：每次长轮询重新建立 TLS。35 秒一次握手，
  功耗和延迟高于连接复用，但实现简单、连接状态干净。
- 手机 NAT/蓝牙共享断开时，当前 HTTPS 调用可能等到超时/断链才返回；
  服务会重试，但恢复时间取决于 lwIP/TLS 的断链检测。
- `context_token` 只保留最近一条入站消息；主动给其他会话推送需要额外缓存。
- 重启后不恢复 `get_updates_buf` 游标，极端情况下可能重复处理最近消息，
  已用本地去重缓存降低影响。

## 安全提醒

- 微信请求使用本目录的 `wechat_http.c`，不会经过 webclient 的请求头
  打印路径，因此不会自动把 `Authorization: Bearer <bot_token>` 打到串口。
  但 `wechat token` 命令仍会打印 token，量产固件应移除该命令。
- `qrcode_img_content` 和 qrcode token 会打印到串口，便于最小联调；
  它们本身也是登录凭证，量产应关闭相关日志。
- token 以明文保存在 `prefdb`。如果手表会被他人物理接触，需要额外加密
  或使用安全分区。
