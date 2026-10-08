/* rtl_bss.h - scan result entry shared by driver.c (C) and wifi.cpp (C++) */
#pragma once
#include <ntddk.h>

#define BSS_BODY_MAX 640            /* beacon / probe-response body (after the 24-byte MAC header), IE-aligned */

typedef struct _BSS_ENTRY {
    UCHAR  Bssid[6];
    UCHAR  Ssid[33];
    UCHAR  Ch;                      /* from DS Parameter IE (0 if absent) */
    UCHAR  RxCh;                    /* channel the radio was on when heard */
    UCHAR  BodyIsResp;              /* 1 = body is from a probe response, 0 = beacon */
    UCHAR  Lq;                      /* link quality 0..100 */
    USHORT BodyLen;
    LONG   Rssi;                    /* dBm */
    ULONG  Hits;
    ULONG  Resp;                    /* probe responses seen */
    UCHAR  Body[BSS_BODY_MAX];
} BSS_ENTRY;
