# BT PAN 网络服务

## 目录

```text
src/net/
├── bt_pan.c        PAN 状态机、BT 事件处理、自动重连
├── bt_pan.h        对外接口
└── SConscript      仅在 BLUETOOTH + CFG_PAN 时编译
```

## 线程模型

- `bt_pan` 线程负责启动 BT 栈、发起 PAN 连接和处理重试。
- BT 协议栈回调只复制 MAC 并投递邮箱消息，不直接连接、不操作 LVGL。
- GUI 线程不参与本模块，PAN 连接失败不会阻塞界面。
- 开机后先等 3 秒让手机主动连入；若 ACL 仍未建立，直接对最后一个已配对设备发起 ACL page。ACL 成功后才启动 PAN/SDP，避免 SDP 在未建立的链路上留下 `gn_sdp_pending` 卡死状态。
- ACL 建立后不会立刻发起 PAN/SDP：先等待手机完成认证/配对（`PAIR_STATE`/`PAIR_IND`/`ENCRYPTION`），避免 SDP 打断配对；已认证链路则在 8 秒 grace 后自动开始 PAN。

## 对外接口

```c
int  bt_pan_service_init(void);
int  bt_pan_connect(void);
bool bt_pan_is_connected(void);
void bt_pan_set_auto_reconnect(bool enable);
```

`bt_pan_service_init()` 通过 `INIT_APP_EXPORT` 自动调用，通常不需要手动调用。

## 串口命令

```text
bt_pan status          查看 ACL/PAN 状态、远端地址和网卡名
bt_pan connect         主动发起连接（ACL 未连接时会先发起 ACL page）
bt_pan dhcp            强制重启 b0 的 DHCP 客户端
bt_pan del_bond        删除已配对信息
bt_pan auto 1          打开断开后自动重连
bt_pan auto 0          关闭自动重连
```

PAN 网卡固定为 `b0`，连接成功后执行：

```text
ifconfig
```

应能看到 `b0` 和手机共享网络分配的 IP。

PAN 连接成功后会自动启动 DHCP 看门狗：每 5 秒检查一次 `b0` 是否拿到 IP，没有就强制 `dhcp_stop` / `dhcp_start` 重试，最多 24 次（约 2 分钟）。因此开机时 SDK 早于 BNEP 链路启动 DHCP 导致的 `0.0.0.0` 会自动恢复，不需要手动 `ifconfig b0 dhcp`。如果手机共享是后开的，可以执行 `bt_pan dhcp` 立即重试。

## 使用顺序

> 首次配对建议先打开手机的“蓝牙网络共享”。这台手机在共享未打开时不会正常接受 PAN，PAN 失败后也可能断开 ACL，表现为“配对不成功”。  
> 如果先配对、后开共享，执行 `bt_pan connect`，或执行 `bt_pan dhcp`；PAN 连接成功后 DHCP 看门狗也会自动重试。

### 常见现象

| 手机蓝牙共享 | 配对 | PAN | b0 IP |
|---|---|---|---|
| 关闭 | 可以成功 | 连不上/建立后断开 | 0.0.0.0 |
| 打开 | 可以成功 | 成功 | 手机分配的 192.168.x.x |

这是手机 NAP 的行为，不是手表配对失败。PAN 需要手机提供 NAP 服务，配对本身不需要。

> 如果日志出现 `[bt_pan] W: pair failed: 0x...` 或 `ACL disconnected (reason=0x8)`，说明手机和手表保存的 link key 不一致。执行 `bt_pan del_bond`，在手机蓝牙设置里“忽略/取消配对”手表，然后重新配对，并在手机上点“配对”确认。之后重启手表验证自动连接。

1. 手机打开“蓝牙网络共享 / 蓝牙 tethering”。
2. 手表开机，手机蓝牙搜索并配对 `sf-claw-watch`。
3. 等待串口出现：

```text
[bt_pan] pan connect successed
```

4. 执行 `ifconfig`，确认 `b0` 有 IP。
5. 如果先配对、后打开共享，执行一次：

```text
bt_pan connect
```

## MAC 地址

如果 `ifconfig` 没有 IP，先检查蓝牙 MAC 的 OUI：

```text
nvds get_mac
```

前三个字节必须是 IEEE 分配的有效 OUI。需要手工设置时，按 SDK PAN 例程说明执行：

```text
nvds update addr 6 xxxxxxYYYYYY
```

`YYYYYY` 是 OUI 的字节反序。工程已关闭：

```text
# CONFIG_NVDS_AUTO_UPDATE_MAC_ADDRESS is not set
```

避免自动更新覆盖手工地址。

## 构建配置

`project/proj.conf` 中与 PAN 相关的最小配置：

```text
CONFIG_BLUETOOTH=y
CONFIG_BSP_BLE_SIBLES=y
CONFIG_BT_PROFILE_CUSTOMIZE=y
CONFIG_CFG_PAN=y
CONFIG_BT_FINSH=y
CONFIG_BSP_BT_CONNECTION_MANAGER=y
CONFIG_BT_AUTO_CONNECT_LAST_DEVICE=y
CONFIG_RT_USING_LWIP=y
CONFIG_RT_USING_LWIP212=y
CONFIG_RT_USING_NETDEV=y
```

> 说明：SAL 不是 PAN 的必需项。SiFli PAN 例程和本工程都直接使用 lwIP socket。
> 本工程为兼容 webclient + mbedTLS，关闭了 `CONFIG_RT_USING_SAL`，避免 SAL/lwIP socket
> 头文件冲突。

`project/SConstruct` 需要通过 `AddLCPU()` 编译并嵌入蓝牙控制器固件。

## 验证

```text
bt_pan status
ifconfig
free
```

重点记录：

- `b0` 是否出现；
- `b0` 是否获得 IP；
- `pan connect successed` 是否出现；
- 手机断开共享后，重新打开是否在 30 秒内恢复；
- `free` 剩余堆是否满足后续 HTTPS 测试。

## 实测记录

2026-10-27，`sf32lb52_osptek`：

```text
bt_pan: acl=1 pan=1 auto=1 retry=0
remote: f4:63:fc:c7:ab:3f
netif: b0 mac=cd:ab:78:56:34:12 ip=192.168.44.140

ping www.baidu.com
4 packets transmitted, 4 received, 0% packet loss
min/avg/max = 56/70/77 ms

free:
total 194,520 / used 76,016 / free ~118,504 B
```

结论：手机打开蓝牙共享后，PAN、DHCP、DNS 和外网 ICMP 均正常。MAC OUI 仍不建议用于量产。
