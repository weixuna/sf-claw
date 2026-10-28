/**
 * @file bt_pan.h
 * @brief 蓝牙 PAN 服务：配对、自动回连和 b0 网卡建立。
 *
 * [线程] 服务内部创建独立线程，不阻塞 LVGL/GUI 线程。
 * [约束] 所有 BT 回调只投递消息，实际连接动作在 PAN 线程执行。
 */

#ifndef __BT_PAN_H__
#define __BT_PAN_H__

#include <stdbool.h>

#define BT_PAN_NETIF_NAME "b0"
#define BT_PAN_LOCAL_NAME "sf-claw-watch"

/**
 * @brief 启动 PAN 服务线程。
 *
 * @return 0 成功，负数失败。
 * @note 可重复调用；首次调用创建线程、邮箱和定时器。
 */
int bt_pan_service_init(void);

/**
 * @brief 请求连接当前已配对手机的 PAN。
 *
 * @return 0 已投递请求，负数失败。
 * @note 仅当 ACL 已连接且已保存远端地址时才会真正发起连接。
 */
int bt_pan_connect(void);

/**
 * @brief 查询 PAN 是否已连接。
 */
bool bt_pan_is_connected(void);

/**
 * @brief 强制重启 b0 的 DHCP 客户端（PAN 已连接时）。
 *
 * @return 0 成功，负数失败。
 */
int bt_pan_request_dhcp(void);

/**
 * @brief 打开或关闭 PAN 断开后的自动重连。
 */
void bt_pan_set_auto_reconnect(bool enable);

#endif /* __BT_PAN_H__ */
