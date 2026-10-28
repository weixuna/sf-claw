/**
 * @file bt_pan.c
 * @brief 蓝牙 PAN 服务实现。
 *
 * [线程] PAN 线程负责 BT 栈启动、连接请求和定时重试；BT 回调只投递消息。
 * [约束] 不在回调中操作 LVGL、文件系统或阻塞等待。
 * [约束] 只有 ACL 已连接且已保存远端地址时才发起 PAN 连接。
 */

#include "bt_pan.h"

#include <rtthread.h>
#include <lwip/ip_addr.h>
#include <netdev.h>
#include <string.h>

#define BT_PAN_INFO(fmt, ...) rt_kprintf("[bt_pan] " fmt "\n", ##__VA_ARGS__)
#define BT_PAN_WARN(fmt, ...) rt_kprintf("[bt_pan] W: " fmt "\n", ##__VA_ARGS__)
#define BT_PAN_ERR(fmt, ...)  rt_kprintf("[bt_pan] E: " fmt "\n", ##__VA_ARGS__)

#include "bf0_sibles.h"
#include "bts2_app_inc.h"
#include "bt_connection_manager.h"

#define BT_PAN_THREAD_STACK_SIZE 4096
#define BT_PAN_MAILBOX_SIZE      8
#define BT_PAN_CONNECT_DELAY_MS  3000
#define BT_PAN_RETRY_MAX         10
#define BT_PAN_DHCP_CHECK_MS     5000
#define BT_PAN_DHCP_RETRY_MAX    24
#define BT_PAN_ACL_DELAY_MS      3000
#define BT_PAN_ACL_RETRY_MAX     20
#define BT_PAN_PAIR_GRACE_MS     8000

typedef enum
{
    BT_PAN_MSG_STACK_READY = 1,
    BT_PAN_MSG_SCHEDULE_CONNECT,
    BT_PAN_MSG_CONNECT,
    BT_PAN_MSG_RETRY,
    BT_PAN_MSG_NETIF_UP,
    BT_PAN_MSG_DHCP_CHECK,
    BT_PAN_MSG_ACL_CONNECT,
    BT_PAN_MSG_PAN_FALLBACK,
} bt_pan_msg_t;

typedef struct
{
    struct rt_mailbox mailbox;
    rt_ubase_t mailbox_pool[BT_PAN_MAILBOX_SIZE];

    struct rt_timer connect_timer;
    struct rt_timer dhcp_timer;
    struct rt_timer acl_timer;
    struct rt_timer pan_grace_timer;
    struct rt_thread thread;

    bt_notify_device_mac_t remote;
    uint8_t retry_count;
    uint8_t dhcp_retry;
    uint8_t acl_retry;

    bool initialized;
    bool remote_valid;
    bool acl_connected;
    bool pan_connected;
    bool pairing;
    bool auto_reconnect;
} bt_pan_ctx_t;

static bt_pan_ctx_t s_bt_pan;
ALIGN(RT_ALIGN_SIZE) static rt_uint8_t s_bt_pan_stack[BT_PAN_THREAD_STACK_SIZE];

static void bt_pan_timer_cb(void *parameter);
static void bt_pan_dhcp_timer_cb(void *parameter);
static void bt_pan_acl_timer_cb(void *parameter);
static void bt_pan_grace_timer_cb(void *parameter);
static int bt_pan_event_handler(uint16_t type, uint16_t event_id, uint8_t *data, uint16_t data_len);
static void bt_pan_thread_entry(void *parameter);

static void bt_pan_stop_timer(bt_pan_ctx_t *ctx)
{
    rt_timer_stop(&ctx->connect_timer);
}

static void bt_pan_start_timer(bt_pan_ctx_t *ctx)
{
    rt_timer_stop(&ctx->connect_timer);
    rt_timer_start(&ctx->connect_timer);
}

static void bt_pan_save_remote(bt_pan_ctx_t *ctx, const bt_notify_device_mac_t *mac)
{
    if (mac == RT_NULL)
    {
        return;
    }

    ctx->remote = *mac;
    ctx->remote_valid = true;
}

static void bt_pan_clear_remote(bt_pan_ctx_t *ctx)
{
    memset(&ctx->remote, 0, sizeof(ctx->remote));
    ctx->remote_valid = false;
}

static void bt_pan_acl_timer_cb(void *parameter)
{
    bt_pan_ctx_t *ctx = (bt_pan_ctx_t *)parameter;

    if (ctx != RT_NULL)
    {
        rt_mb_send(&ctx->mailbox, BT_PAN_MSG_ACL_CONNECT);
    }
}

static void bt_pan_grace_timer_cb(void *parameter)
{
    bt_pan_ctx_t *ctx = (bt_pan_ctx_t *)parameter;

    if (ctx != RT_NULL)
    {
        rt_mb_send(&ctx->mailbox, BT_PAN_MSG_PAN_FALLBACK);
    }
}

/*
 * Establish the ACL directly, without asking the PAN profile to start an
 * SDP search first.  A PAN/SDP request issued before the ACL exists can
 * leave the BT stack's gn_sdp_pending flag set after a failed page, which
 * makes every later PAN attempt return "SDP is in progress" until reboot.
 */
static int bt_pan_acl_connect(bt_pan_ctx_t *ctx)
{
    bt_cm_bonded_dev_t *bonded;
    bt_cm_dev_info_t *dev;
    bt_cm_dev_acl_info_t *conn;
    BTS2S_BD_ADDR bd;
    uint8_t idx;

    if (ctx->acl_connected || ctx->pan_connected)
    {
        return 0;
    }

    bonded = bt_cm_get_bonded_dev();
    if (bonded == RT_NULL)
    {
        return -1;
    }

    idx = bonded->last_bond_idx;
    if ((idx >= BT_CM_MAX_BOND) ||
        bd_is_empty(&bonded->info[idx].bd_addr))
    {
        for (idx = 0; idx < BT_CM_MAX_BOND; idx++)
        {
            if (!bd_is_empty(&bonded->info[idx].bd_addr))
            {
                break;
            }
        }
    }

    if (idx >= BT_CM_MAX_BOND)
    {
        BT_PAN_WARN("ACL auto-connect: no bonded device");
        return -1;
    }

    dev = &bonded->info[idx];
    bd = dev->bd_addr;

    if (bt_cm_get_conn_by_addr(bt_cm_get_env(), &bd) != RT_NULL)
    {
        return 1;
    }

    conn = bt_cm_conn_alloc(bt_cm_get_env(), &bd, BT_LINK_PHONE);
    if (conn == RT_NULL)
    {
        BT_PAN_WARN("ACL auto-connect: BT CM slots full");
        return -1;
    }

    bt_addr_convert(&bd, ctx->remote.addr);
    ctx->remote_valid = true;

    BT_PAN_INFO("ACL auto-connect to %02x:%02x:%02x:%02x:%02x:%02x",
                ctx->remote.addr[5], ctx->remote.addr[4],
                ctx->remote.addr[3], ctx->remote.addr[2],
                ctx->remote.addr[1], ctx->remote.addr[0]);

    hcia_acl_conn_req(&bd, DEFAULT_ACL_PKT_TYPE);
    return 1;
}

static void bt_pan_try_connect(bt_pan_ctx_t *ctx)
{
    if (ctx->pan_connected)
    {
        return;
    }

    if (!ctx->acl_connected || !ctx->remote_valid)
    {
        BT_PAN_WARN("PAN connect skipped: acl=%d remote=%d", ctx->acl_connected, ctx->remote_valid);
        return;
    }

    if (ctx->auto_reconnect && (ctx->retry_count >= BT_PAN_RETRY_MAX))
    {
        BT_PAN_WARN("PAN retry limit reached");
        return;
    }

    if (ctx->auto_reconnect)
    {
        ctx->retry_count++;
        BT_PAN_INFO("PAN connect request (%u/%u)", ctx->retry_count, BT_PAN_RETRY_MAX);
    }
    else
    {
        BT_PAN_INFO("PAN connect request");
    }

    bt_err_t err = bt_interface_conn_ext((unsigned char *)ctx->remote.addr, BT_PROFILE_PAN);

    if (err != BT_EOK)
    {
        BT_PAN_WARN("PAN connect request failed: 0x%x", err);
    }

    /* Keep retrying until connected, ACL drops, or the retry limit is hit. */
    if (ctx->auto_reconnect)
    {
        bt_pan_start_timer(ctx);
    }
}

static void bt_pan_timer_cb(void *parameter)
{
    bt_pan_ctx_t *ctx = (bt_pan_ctx_t *)parameter;

    if (ctx != RT_NULL)
    {
        rt_mb_send(&ctx->mailbox, BT_PAN_MSG_CONNECT);
    }
}

static void bt_pan_dhcp_restart(struct netdev *netdev)
{
    if (netdev == RT_NULL)
    {
        return;
    }

    /*
     * Stop and restart the DHCP client instead of just enabling it.
     * After a PAN re-connect the lwIP DHCP client can still be attached
     * to the old BNEP link; dhcp_start() then returns ERR_USE and no new
     * DISCOVER is sent.  A clean stop/start always creates a new lease
     * attempt on the current link.
     */
    netdev_dhcp_enabled(netdev, RT_FALSE);
    netdev_dhcp_enabled(netdev, RT_TRUE);
}

static void bt_pan_dhcp_check(bt_pan_ctx_t *ctx)
{
    struct netdev *netdev;

    if (!ctx->pan_connected)
    {
        rt_timer_stop(&ctx->dhcp_timer);
        return;
    }

    netdev = netdev_get_by_name(BT_PAN_NETIF_NAME);
    if (netdev == RT_NULL)
    {
        return;
    }

    if (netdev->ip_addr.addr != 0)
    {
        BT_PAN_INFO("DHCP ready: %s", ipaddr_ntoa(&netdev->ip_addr));
        ctx->dhcp_retry = 0;
        rt_timer_stop(&ctx->dhcp_timer);
        return;
    }

    if (ctx->dhcp_retry >= BT_PAN_DHCP_RETRY_MAX)
    {
        BT_PAN_WARN("DHCP retry limit reached");
        rt_timer_stop(&ctx->dhcp_timer);
        return;
    }

    ctx->dhcp_retry++;
    BT_PAN_WARN("DHCP no lease, restart (%u/%u)",
                ctx->dhcp_retry, BT_PAN_DHCP_RETRY_MAX);
    bt_pan_dhcp_restart(netdev);
}

static void bt_pan_dhcp_timer_cb(void *parameter)
{
    bt_pan_ctx_t *ctx = (bt_pan_ctx_t *)parameter;

    if (ctx != RT_NULL)
    {
        rt_mb_send(&ctx->mailbox, BT_PAN_MSG_DHCP_CHECK);
    }
}

static void bt_pan_netif_up(void)
{
    struct netdev *netdev = RT_NULL;
    int retry;

    /* The PAN netif is normally registered before the profile event, but
       keep a short bounded wait for the stack ordering to settle. */
    for (retry = 0; retry < 10; retry++)
    {
        netdev = netdev_get_by_name(BT_PAN_NETIF_NAME);
        if (netdev != RT_NULL)
        {
            break;
        }
        rt_thread_mdelay(20);
    }

    if (netdev == RT_NULL)
    {
        BT_PAN_WARN("netif %s not found", BT_PAN_NETIF_NAME);
        return;
    }

    netdev_set_up(netdev);
    netdev_set_default(netdev);
    BT_PAN_INFO("netif %s is default", BT_PAN_NETIF_NAME);
}

static int bt_pan_event_handler(uint16_t type, uint16_t event_id, uint8_t *data, uint16_t data_len)
{
    bt_pan_ctx_t *ctx = &s_bt_pan;

    if (!ctx->initialized)
    {
        return 0;
    }

    if (type == BT_NOTIFY_COMMON)
    {
        switch (event_id)
        {
        case BT_NOTIFY_COMMON_BT_STACK_READY:
            rt_mb_send(&ctx->mailbox, BT_PAN_MSG_STACK_READY);
            break;

        case BT_NOTIFY_COMMON_ACL_CONNECTED:
        {
            bt_notify_device_acl_conn_info_t *info =
                (bt_notify_device_acl_conn_info_t *)data;

            if (data_len < sizeof(*info))
            {
                break;
            }

            if (info->res != BTS2_SUCC)
            {
                /*
                 * Failed outgoing pages are reported through the same
                 * event.  Treating them as "ACL up" used to start an SDP
                 * search on a dead link; the BT stack then leaves its
                 * gn_sdp_pending flag set and every later PAN attempt says
                 * "SDP is in progress" until reboot.
                 */
                BT_PAN_WARN("ACL connect failed: res=0x%x", info->res);
                break;
            }

            bt_pan_save_remote(ctx, &info->mac);
            ctx->acl_connected = true;
            ctx->retry_count = 0;
            ctx->acl_retry = 0;
            rt_timer_stop(&ctx->acl_timer);
            BT_PAN_INFO("ACL connected (hdl=%u)", info->handle);
            /*
             * Do not start PAN/SDP yet.  The phone usually starts
             * authentication/pairing right after the ACL is up, and an
             * SDP search on an unauthenticated link can stall the whole
             * pairing.  The grace timer is the fallback for links that
             * are already authenticated and produce no pairing event.
             */
            rt_timer_start(&ctx->pan_grace_timer);
            break;
        }

        case BT_NOTIFY_COMMON_ENCRYPTION:
        {
            bt_notify_device_mac_t *mac = (bt_notify_device_mac_t *)data;

            if (data_len >= sizeof(*mac))
            {
                bt_pan_save_remote(ctx, mac);
                ctx->acl_connected = true;
                ctx->pairing = false;
                ctx->retry_count = 0;
                rt_timer_stop(&ctx->pan_grace_timer);
                rt_mb_send(&ctx->mailbox, BT_PAN_MSG_SCHEDULE_CONNECT);
            }
            break;
        }

        case BT_NOTIFY_COMMON_PAIR_STATE:
            ctx->pairing = true;
            bt_pan_stop_timer(ctx);
            rt_timer_stop(&ctx->pan_grace_timer);
            BT_PAN_INFO("pairing in progress; confirm on phone if prompted");
            break;

        case BT_NOTIFY_COMMON_PAIR_IND:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;

            if (data_len < sizeof(*info))
            {
                break;
            }

            ctx->pairing = false;
            rt_timer_stop(&ctx->pan_grace_timer);

            if (info->res == BTS2_SUCC)
            {
                bt_pan_save_remote(ctx, &info->mac);
                ctx->acl_connected = true;
                ctx->retry_count = 0;
                BT_PAN_INFO("pair success");
                rt_mb_send(&ctx->mailbox, BT_PAN_MSG_SCHEDULE_CONNECT);
            }
            else
            {
                BT_PAN_WARN("pair failed: 0x%x (confirm on phone, or delete bond and re-pair)",
                            info->res);
            }
            break;
        }

        case BT_NOTIFY_COMMON_ACL_DISCONNECTED:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;
            uint8_t reason = (data_len >= sizeof(*info)) ? info->res : 0xff;

            ctx->acl_connected = false;
            ctx->pan_connected = false;
            ctx->pairing = false;
            ctx->retry_count = 0;
            ctx->dhcp_retry = 0;
            bt_pan_clear_remote(ctx);
            bt_pan_stop_timer(ctx);
            rt_timer_stop(&ctx->dhcp_timer);
            rt_timer_stop(&ctx->acl_timer);
            rt_timer_stop(&ctx->pan_grace_timer);
            BT_PAN_INFO("ACL disconnected (reason=0x%x)", reason);

            if ((reason == HCI_ERR_AUTH_FAIL) ||
                (reason == HCI_ERR_KEY_MISSING) ||
                (reason == HCI_ERR_CONN_TIMEOUT))
            {
                BT_PAN_WARN("BT authentication/link timeout; if this repeats, "
                            "run 'bt_pan del_bond' and re-pair on the phone");
            }

            if (ctx->auto_reconnect)
            {
                ctx->acl_retry = 0;
                rt_timer_start(&ctx->acl_timer);
            }
            break;
        }

        case BT_NOTIFY_COMMON_KEY_MISSING:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;

            BT_PAN_WARN("BT link key missing: %d", info ? info->res : -1);
            ctx->acl_connected = false;
            ctx->pan_connected = false;
            ctx->pairing = false;
            ctx->retry_count = 0;
            ctx->dhcp_retry = 0;
            bt_pan_clear_remote(ctx);
            bt_pan_stop_timer(ctx);
            rt_timer_stop(&ctx->dhcp_timer);
            rt_timer_stop(&ctx->acl_timer);
            rt_timer_stop(&ctx->pan_grace_timer);
#if defined(BSP_BT_CONNECTION_MANAGER)
            if ((info != RT_NULL) && (data_len >= sizeof(*info)))
            {
                bt_cm_delete_bonded_devs_and_linkkey(info->mac.addr);
            }
#endif
            break;
        }

        default:
            break;
        }
    }
    else if (type == BT_NOTIFY_PAN)
    {
        switch (event_id)
        {
        case BT_NOTIFY_PAN_PROFILE_CONNECTED:
            ctx->pan_connected = true;
            ctx->retry_count = 0;
            ctx->dhcp_retry = 0;
            ctx->acl_retry = 0;
            ctx->pairing = false;
            bt_pan_stop_timer(ctx);
            rt_timer_stop(&ctx->dhcp_timer);
            rt_timer_stop(&ctx->acl_timer);
            rt_timer_stop(&ctx->pan_grace_timer);
            BT_PAN_INFO("pan connect successed");
            rt_mb_send(&ctx->mailbox, BT_PAN_MSG_NETIF_UP);
            break;

        case BT_NOTIFY_PAN_PROFILE_DISCONNECTED:
            ctx->pan_connected = false;
            ctx->retry_count = 0;
            ctx->dhcp_retry = 0;
            rt_timer_stop(&ctx->dhcp_timer);
            BT_PAN_INFO("pan disconnected");
            if (ctx->auto_reconnect && ctx->acl_connected)
            {
                rt_mb_send(&ctx->mailbox, BT_PAN_MSG_RETRY);
            }
            break;

        default:
            break;
        }
    }

    return 0;
}

static void bt_pan_thread_entry(void *parameter)
{
    bt_pan_ctx_t *ctx = (bt_pan_ctx_t *)parameter;
    rt_uint32_t message;

    bt_interface_status_t status = bt_interface_register_bt_event_notify_callback(bt_pan_event_handler);

    if ((status != BT_INTERFACE_STATUS_OK) && (status != BT_INTERFACE_STATUS_ALREADY_EXIST))
    {
        BT_PAN_ERR("register BT callback failed: %d", status);
    }

    /*
     * Clear the BT-CM profile target.  The default target is HFP/A2DP,
     * neither of which is compiled in this project; BT-CM would call a
     * no-op profile connect and then disable scan/page, so the watch
     * would neither page the phone nor be reachable.  PAN is driven by
     * this module after a direct ACL connection instead.
     */
#if defined(BSP_BT_CONNECTION_MANAGER)
    bt_cm_set_profile_target(0, BT_LINK_PHONE, 0);
#endif

    sifli_ble_enable();

    for (;;)
    {
        if (rt_mb_recv(&ctx->mailbox, &message, RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }

        switch ((bt_pan_msg_t)message)
        {
        case BT_PAN_MSG_STACK_READY:
            BT_PAN_INFO("BT stack ready");
            bt_interface_set_local_name((int)strlen(BT_PAN_LOCAL_NAME), (void *)BT_PAN_LOCAL_NAME);
            /*
             * Give the phone a few seconds to page us first; simultaneous
             * pages are the usual cause of the res=0x0f failures.  If it
             * does not, the timer starts an ACL connection to the bonded
             * device.
             */
            ctx->acl_retry = 0;
            rt_timer_start(&ctx->acl_timer);
            break;

        case BT_PAN_MSG_SCHEDULE_CONNECT:
            if (ctx->auto_reconnect)
            {
                bt_pan_start_timer(ctx);
            }
            else
            {
                bt_pan_try_connect(ctx);
            }
            break;

        case BT_PAN_MSG_CONNECT:
            bt_pan_try_connect(ctx);
            break;

        case BT_PAN_MSG_RETRY:
            if (ctx->auto_reconnect)
            {
                bt_pan_start_timer(ctx);
            }
            break;

        case BT_PAN_MSG_NETIF_UP:
            bt_pan_netif_up();
            ctx->dhcp_retry = 0;
            rt_timer_start(&ctx->dhcp_timer);
            break;

        case BT_PAN_MSG_DHCP_CHECK:
            bt_pan_dhcp_check(ctx);
            break;

        case BT_PAN_MSG_ACL_CONNECT:
            if (ctx->acl_connected || ctx->pan_connected)
            {
                ctx->acl_retry = 0;
                rt_timer_stop(&ctx->acl_timer);
                break;
            }

            if (ctx->acl_retry >= BT_PAN_ACL_RETRY_MAX)
            {
                BT_PAN_WARN("ACL auto-connect retry limit reached");
                rt_timer_stop(&ctx->acl_timer);
                break;
            }

            ctx->acl_retry++;
            (void)bt_pan_acl_connect(ctx);

            if (ctx->acl_retry < BT_PAN_ACL_RETRY_MAX)
            {
                rt_timer_start(&ctx->acl_timer);
            }
            break;

        case BT_PAN_MSG_PAN_FALLBACK:
            if (!ctx->acl_connected || ctx->pan_connected)
            {
                break;
            }
            if (ctx->pairing)
            {
                BT_PAN_INFO("pairing still in progress; PAN start deferred");
                break;
            }
            BT_PAN_INFO("starting PAN after ACL grace period");
            rt_mb_send(&ctx->mailbox, BT_PAN_MSG_SCHEDULE_CONNECT);
            break;

        default:
            break;
        }
    }
}

int bt_pan_service_init(void)
{
    bt_pan_ctx_t *ctx = &s_bt_pan;

    if (ctx->initialized)
    {
        return 0;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->auto_reconnect = true;

    if (rt_mb_init(&ctx->mailbox, "bt_pan", ctx->mailbox_pool,
                   BT_PAN_MAILBOX_SIZE, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ENOMEM;
    }

    rt_timer_init(&ctx->connect_timer, "bt_pan", bt_pan_timer_cb, ctx,
                  rt_tick_from_millisecond(BT_PAN_CONNECT_DELAY_MS),
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);

    rt_timer_init(&ctx->dhcp_timer, "bt_dhcp", bt_pan_dhcp_timer_cb, ctx,
                  rt_tick_from_millisecond(BT_PAN_DHCP_CHECK_MS),
                  RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);

    rt_timer_init(&ctx->acl_timer, "bt_acl", bt_pan_acl_timer_cb, ctx,
                  rt_tick_from_millisecond(BT_PAN_ACL_DELAY_MS),
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);

    rt_timer_init(&ctx->pan_grace_timer, "bt_grace", bt_pan_grace_timer_cb, ctx,
                  rt_tick_from_millisecond(BT_PAN_PAIR_GRACE_MS),
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);

    if (rt_thread_init(&ctx->thread, "bt_pan", bt_pan_thread_entry, ctx,
                       s_bt_pan_stack, sizeof(s_bt_pan_stack),
                       RT_THREAD_PRIORITY_MIDDLE, RT_THREAD_TICK_DEFAULT) != RT_EOK)
    {
        rt_mb_detach(&ctx->mailbox);
        rt_timer_detach(&ctx->connect_timer);
        rt_timer_detach(&ctx->dhcp_timer);
        rt_timer_detach(&ctx->acl_timer);
        rt_timer_detach(&ctx->pan_grace_timer);
        return -RT_ENOMEM;
    }

    ctx->initialized = true;
    rt_thread_startup(&ctx->thread);
    return 0;
}

int bt_pan_connect(void)
{
    if (!s_bt_pan.initialized)
    {
        return -RT_ERROR;
    }

    /* A manual request starts a fresh retry window. */
    s_bt_pan.retry_count = 0;

    if (!s_bt_pan.acl_connected)
    {
        s_bt_pan.acl_retry = 0;
        rt_timer_start(&s_bt_pan.acl_timer);
    }

    return rt_mb_send(&s_bt_pan.mailbox, BT_PAN_MSG_CONNECT) == RT_EOK ? 0 : -RT_EFULL;
}

bool bt_pan_is_connected(void)
{
    return s_bt_pan.pan_connected;
}

int bt_pan_request_dhcp(void)
{
    struct netdev *netdev;

    if (!s_bt_pan.initialized || !s_bt_pan.pan_connected)
    {
        return -RT_ERROR;
    }

    netdev = netdev_get_by_name(BT_PAN_NETIF_NAME);
    if (netdev == RT_NULL)
    {
        return -RT_ENOSYS;
    }

    s_bt_pan.dhcp_retry = 0;
    bt_pan_dhcp_restart(netdev);
    rt_timer_start(&s_bt_pan.dhcp_timer);
    return 0;
}

void bt_pan_set_auto_reconnect(bool enable)
{
    if (!s_bt_pan.initialized)
    {
        return;
    }

    s_bt_pan.auto_reconnect = enable;

    if (!enable)
    {
        bt_pan_stop_timer(&s_bt_pan);
        rt_timer_stop(&s_bt_pan.acl_timer);
        rt_timer_stop(&s_bt_pan.pan_grace_timer);
    }
    else if (!s_bt_pan.acl_connected && !s_bt_pan.pan_connected)
    {
        s_bt_pan.acl_retry = 0;
        rt_timer_start(&s_bt_pan.acl_timer);
    }
}

uint32_t bt_get_class_of_device(void)
{
    return (uint32_t)BT_SRVCLS_NETWORK | BT_DEVCLS_LAP | BT_LAP_FULLY;
}

static void bt_pan_print_status(void)
{
    bt_pan_ctx_t *ctx = &s_bt_pan;

    rt_kprintf("bt_pan: acl=%d pan=%d auto=%d retry=%u acl_retry=%u dhcp_retry=%u\n",
               ctx->acl_connected, ctx->pan_connected,
               ctx->auto_reconnect, ctx->retry_count,
               ctx->acl_retry, ctx->dhcp_retry);

    if (ctx->remote_valid)
    {
        rt_kprintf("remote: %02x:%02x:%02x:%02x:%02x:%02x\n",
                   ctx->remote.addr[5], ctx->remote.addr[4], ctx->remote.addr[3],
                   ctx->remote.addr[2], ctx->remote.addr[1], ctx->remote.addr[0]);
    }
    else
    {
        rt_kprintf("remote: none\n");
    }

    {
        struct netdev *netdev = netdev_get_by_name(BT_PAN_NETIF_NAME);

        if (netdev != RT_NULL)
        {
            rt_kprintf("netif: %s mac=%02x:%02x:%02x:%02x:%02x:%02x ip=%s\n",
                       BT_PAN_NETIF_NAME,
                       netdev->hwaddr[0], netdev->hwaddr[1], netdev->hwaddr[2],
                       netdev->hwaddr[3], netdev->hwaddr[4], netdev->hwaddr[5],
                       ipaddr_ntoa(&netdev->ip_addr));
        }
        else
        {
            rt_kprintf("netif: %s not found\n", BT_PAN_NETIF_NAME);
        }
    }
}

static int bt_pan_cmd(int argc, char **argv)
{
    bt_pan_ctx_t *ctx = &s_bt_pan;

    if (argc < 2)
    {
        rt_kprintf("usage: bt_pan <status|connect|dhcp|del_bond|auto 0|1>\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0)
    {
        bt_pan_print_status();
    }
    else if (strcmp(argv[1], "connect") == 0)
    {
        if (bt_pan_connect() != 0)
        {
            rt_kprintf("bt_pan: connect request failed\n");
        }
    }
    else if (strcmp(argv[1], "dhcp") == 0)
    {
        struct netdev *netdev = netdev_get_by_name(BT_PAN_NETIF_NAME);

        if (netdev == RT_NULL)
        {
            rt_kprintf("bt_pan: netif %s not found\n", BT_PAN_NETIF_NAME);
        }
        else
        {
            ctx->dhcp_retry = 0;
            bt_pan_dhcp_restart(netdev);
            if (ctx->pan_connected)
            {
                rt_timer_start(&ctx->dhcp_timer);
            }
            rt_kprintf("bt_pan: DHCP restarted on %s\n", BT_PAN_NETIF_NAME);
        }
    }
    else if (strcmp(argv[1], "del_bond") == 0)
    {
#if defined(BSP_BT_CONNECTION_MANAGER)
        bt_cm_delete_bonded_devs();
        rt_kprintf("bt_pan: bond information deleted\n");
#else
        rt_kprintf("bt_pan: connection manager disabled\n");
#endif
    }
    else if (strcmp(argv[1], "auto") == 0)
    {
        if (argc >= 3)
        {
            bt_pan_set_auto_reconnect((strcmp(argv[2], "0") != 0) && (strcmp(argv[2], "off") != 0));
            rt_kprintf("bt_pan: auto reconnect %s\n", ctx->auto_reconnect ? "on" : "off");
        }
        else
        {
            rt_kprintf("usage: bt_pan auto <0|1>\n");
        }
    }
    else
    {
        rt_kprintf("usage: bt_pan <status|connect|dhcp|del_bond|auto 0|1>\n");
    }

    return 0;
}

MSH_CMD_EXPORT_ALIAS(bt_pan_cmd, bt_pan, BT PAN service control);

INIT_APP_EXPORT(bt_pan_service_init);
