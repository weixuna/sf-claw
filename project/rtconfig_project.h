#ifndef RTCONFIG_PROJECT_H__
#define RTCONFIG_PROJECT_H__

/*
 * RT-Thread 3.1.3 exposes RTT_VERSION/RTT_SUBVERSION/RTT_REVISION.
 * The SDK SAL source expects the newer RT_VERSION_* names.
 */
#ifndef RT_VERSION_MAJOR
#define RT_VERSION_MAJOR RTT_VERSION
#endif
#ifndef RT_VERSION_MINOR
#define RT_VERSION_MINOR RTT_SUBVERSION
#endif
#ifndef RT_VERSION_PATCH
#define RT_VERSION_PATCH RTT_REVISION
#endif

/*
 * SNTP fallback servers.  netutils/ntp.c sends to all configured hosts
 * when ntp_sync_to_rtc(NULL) is used; the Kconfig only exposes the first
 * one, so provide two more here.
 */
#ifndef NETUTILS_NTP_HOSTNAME2
#define NETUTILS_NTP_HOSTNAME2 "ntp.aliyun.com"
#endif
#ifndef NETUTILS_NTP_HOSTNAME3
#define NETUTILS_NTP_HOSTNAME3 "time1.cloud.tencent.com"
#endif

#if defined(_MSC_VER)
    #define RT_HEAP_SIZE   (680000)
    #define NORESOURCE  //RT_VESRION in winuser.h
    #define _CRT_ERRNO_DEFINED  //errno macro redefinition
    #define _INC_WTIME_INL//dfs_elm.c time.h conflicts with wtime.inl
    #define _INC_TIME_INL //dfs_elm.c time.h conflicts with wtime.inl

    /* disable some warning in MSC */
    #pragma warning(disable:4273)   /* to ignore: warning C4273: inconsistent dll linkage */
    #pragma warning(disable:4312)   /* to ignore: warning C4312: 'type cast' : conversion from 'rt_uint32_t' to 'rt_uint32_t *' */
    #pragma warning(disable:4311)   /* to ignore: warning C4311: 'type cast' : pointer truncation from 'short *__w64 ' to 'long' */
    #pragma warning(disable:4996)   /* to ignore: warning C4996: The POSIX name for this item is deprecated. */
    #pragma warning(disable:4267)   /* to ignore: warning C4267: conversion from 'size_t' to 'rt_size_t', possible loss of data */
    #pragma warning(disable:4244)   /* to ignore: warning C4244: '=' : conversion from '__w64 int' to 'rt_size_t', possible loss of data */

#endif /* end of _MSC_VER */

#endif
