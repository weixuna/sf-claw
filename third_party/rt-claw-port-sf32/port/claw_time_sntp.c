/**
 * @file claw_time_sntp.c
 * @brief Time sync once the board netdev gets an IP address.
 *
 * Certificate validation requires a valid RTC.  SNTP uses UDP/123, which
 * some phone tethering APNs block.  This thread therefore tries:
 *   1. SNTP (multiple hosts, see rtconfig_project.h)
 *   2. an HTTP "Date:" header (plain HTTP, no certificate/time needed)
 *   3. a compile-time fallback so TLS can still work
 * and keeps retrying SNTP in the background to correct the time.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>

#include "ntp.h"
#include "osal/claw_os.h"
#include "claw_port_net.h"
#include "claw_port_time.h"

#if defined(PKG_USING_WEBCLIENT)
#include <webclient.h>
#endif

#define TAG "claw_ntp"
#define CLAW_NTP_RETRIES   60
#define CLAW_NTP_RETRY_MS  5000
#define CLAW_NTP_MIN_YEAR  2024

#ifndef CLAW_PORT_NTP_STACK
#define CLAW_PORT_NTP_STACK 6144
#endif
#ifndef CLAW_PORT_NTP_PRIO
#define CLAW_PORT_NTP_PRIO 26
#endif

static bool time_is_valid(void)
{
    time_t now = time(RT_NULL);
    struct tm tm_now;

    if (now <= 0) {
        return false;
    }
    if (!localtime_r(&now, &tm_now)) {
        return false;
    }

    return (tm_now.tm_year + 1900) >= CLAW_NTP_MIN_YEAR;
}

static int set_rtc_time(time_t t)
{
#ifdef RT_USING_RTC
    rt_device_t rtc = rt_device_find("rtc");

    if (!rtc) {
        return -1;
    }
    if (rt_device_control(rtc, RT_DEVICE_CTRL_RTC_SET_TIME, &t) != RT_EOK) {
        return -1;
    }
    return 0;
#else
    (void)t;
    return -1;
#endif
}

static int month_number(const char *mon)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int i;

    for (i = 0; i < 12; i++) {
        if (strncasecmp(mon, names + i * 3, 3) == 0) {
            return i + 1;
        }
    }
    return 0;
}

/* Howard Hinnant's civil-date algorithm; returns days since 1970-01-01. */
static int64_t days_from_civil(int year, int month, int day)
{
    int era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;
    int y = year - (month <= 2);

    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +
                     day - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static time_t make_time(int year, int month, int day,
                        int hour, int minute, int second)
{
    int64_t days;

    if (year < CLAW_NTP_MIN_YEAR || month < 1 || month > 12 ||
        day < 1 || day > 31) {
        return 0;
    }
    days = days_from_civil(year, month, day);
    return (time_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

#if defined(PKG_USING_WEBCLIENT)
/*
 * HTTP Date is enough for TLS certificate validation and does not depend
 * on UDP/123.  The request is deliberately hand-built: this runs before
 * the system time is valid, so HTTPS is not usable here.
 */
static int http_date_sync(void)
{
    static const char req[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.baidu.com\r\n"
        "User-Agent: sf-claw-watch\r\n"
        "Connection: close\r\n\r\n";
    struct webclient_session *session;
    char header[1024];
    char wd[8];
    char mon[4];
    int day, year, hour, minute, second;
    int n = 0;
    int len;

    session = webclient_session_create(2048);
    if (!session) {
        return -1;
    }

    if (webclient_connect(session, "http://www.baidu.com/") != WEBCLIENT_OK) {
        webclient_close(session);
        return -1;
    }
    webclient_set_timeout(session, 5000);

    len = (int)strlen(req);
    if (webclient_write(session, req, len) != len) {
        webclient_close(session);
        return -1;
    }

    while (n < (int)sizeof(header) - 1) {
        int rc = webclient_read(session, &header[n], 1);

        if (rc != 1) {
            break;
        }
        n++;
        if (n >= 4 && memcmp(header + n - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    webclient_close(session);
    header[n] = '\0';

    {
        char *p = strstr(header, "Date:");

        if (!p) {
            return -1;
        }
        if (sscanf(p, "Date: %7s %d %3s %d %d:%d:%d",
                   wd, &day, mon, &year, &hour, &minute, &second) != 7) {
            return -1;
        }
    }

    {
        int month = month_number(mon);
        time_t t = make_time(year, month, day, hour, minute, second);

        if (t <= 0) {
            return -1;
        }
        if (set_rtc_time(t) != 0) {
            return -1;
        }
        CLAW_LOGI(TAG, "HTTP Date sync done: %s", ctime(&t));
    }
    return 0;
}
#endif /* PKG_USING_WEBCLIENT */

static time_t build_time(void)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int month;
    int i;

    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return 0;
    }

    month = 0;
    for (i = 0; i < 12; i++) {
        if (strncasecmp(mon, names + i * 3, 3) == 0) {
            month = i + 1;
            break;
        }
    }
    return make_time(year, month, day, hour, minute, second);
}

static void claw_ntp_thread(void *arg)
{
    bool fallback = false;
    bool http_tried = false;

    (void)arg;

    for (int i = 0; i < CLAW_NTP_RETRIES; i++) {
        if (time_is_valid() && !fallback) {
            CLAW_LOGI(TAG, "system time already valid");
            return;
        }

        if (claw_port_net_is_online()) {
            if (!fallback) {
                CLAW_LOGI(TAG, "syncing time via SNTP ...");
            }

            if (ntp_sync_to_rtc(RT_NULL) > 0) {
                CLAW_LOGI(TAG, "SNTP sync done");
                return;
            }

            if (!fallback) {
                CLAW_LOGW(TAG, "SNTP sync failed");
            }

#if defined(PKG_USING_WEBCLIENT)
            if (!fallback && !http_tried) {
                http_tried = true;
                if (http_date_sync() == 0) {
                    return;
                }
                CLAW_LOGW(TAG, "HTTP Date sync failed");
            }
#endif

            if (!fallback) {
                time_t t = build_time();

                if (t > 0 && set_rtc_time(t) == 0) {
                    fallback = true;
                    CLAW_LOGW(TAG,
                              "using firmware build time as fallback: %s",
                              ctime(&t));
                    /* Keep retrying SNTP below to correct the clock. */
                }
            }
        }

        rt_thread_mdelay(CLAW_NTP_RETRY_MS);
    }

    CLAW_LOGW(TAG, "SNTP gave up; set time manually or run ntp_sync");
}

void claw_port_time_start(void)
{
    static bool started;
    rt_thread_t tid;

    if (started) {
        return;
    }
    started = true;

    tid = rt_thread_create("claw_ntp", claw_ntp_thread, RT_NULL,
                           CLAW_PORT_NTP_STACK, CLAW_PORT_NTP_PRIO, 10);
    if (!tid) {
        CLAW_LOGE(TAG, "thread create failed");
        return;
    }
    rt_thread_startup(tid);
}
