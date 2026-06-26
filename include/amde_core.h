/*
 * amde_core.h — AMDE v3.0 Core Types & Constants
 *
 * Shared across all translation units.
 * No function declarations here — each module has its own header.
 */
#pragma once
#ifndef AMDE_CORE_H
#define AMDE_CORE_H

#ifndef STRICT
#define STRICT
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602   /* Windows 8+ (SRWLock, GetTickCount64) */
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <strsafe.h>
#include <commctrl.h>

/* ── Version ───────────────────────────────────────────── */
#define AMDE_VERSION_STR        L"AMDE v3.0"
#define AMDE_VERSION_MAJOR      3
#define AMDE_VERSION_MINOR      0

/* ── Debounce engine constants ─────────────────────────── */
#define BOUNCE_HISTORY_SIZE         128     /* 2× v2 — đủ cho P95 tin cậy hơn */
#define QUANTILE_UPDATE_PERIOD      4       /* cập nhật nhanh hơn v2 (8→4)    */
#define MAX_DEVICE_PROFILES         32      /* tăng từ 16 để hỗ trợ KVM lớn   */
#define MAX_DEVICE_ID_LEN           128
#define DEFAULT_MIN_THRESHOLD_US    35000   /* 35 ms — hard floor              */
#define MAX_BAYES_EXTENSION_US      20000   /* tăng từ 15 ms → 20 ms           */
#define HYSTERESIS_BAND_US          2500    /* dải hysteresis 2.5 ms           */
#define DRAG_THRESHOLD_SQ           25      /* 5 px                            */
#define TOOLTIP_REFRESH_MS          1500

/*
 * Bayesian Beta prior: alpha_bounce / beta_clean
 * Dùng Beta(α,β) conjugate prior cho Bernoulli observation.
 * Initial: Beta(1,9) → prior 10% bounce probability.
 */
#define BAYES_ALPHA_INIT    1.0
#define BAYES_BETA_INIT     9.0

/* ── Button indices ────────────────────────────────────── */
typedef enum {
    BUTTON_LEFT   = 0,
    BUTTON_RIGHT  = 1,
    BUTTON_MIDDLE = 2,
    BUTTON_X1     = 3,
    BUTTON_X2     = 4,       /* v3: XBUTTON2 support */
    BUTTON_COUNT  = 5
} MOUSE_BUTTON;

typedef enum {
    STATE_IDLE     = 0,
    STATE_PRESSED  = 1,
    STATE_DRAGGING = 2
} BUTTON_STATE;

/* ── Persistence blob (POD, serialised to registry) ────── */
#define PERSIST_MAGIC    0x414D4433u   /* "AMD3" — v3 format */
#define PERSIST_VERSION  1u

typedef struct {
    /* Bayesian Beta(α,β) prior — v3 real posterior */
    double   bayes_alpha;           /* pseudo-count bounces   */
    double   bayes_beta;            /* pseudo-count cleans    */

    /* Quantile-derived threshold */
    uint32_t p95_threshold_us;
    uint32_t threshold_enter_us;
    uint32_t threshold_exit_us;

    /* Sample ring buffer */
    uint32_t samples[BOUNCE_HISTORY_SIZE];
    uint8_t  count;
    uint8_t  sample_index;

    /* Statistics */
    uint32_t total_bounces;
    uint32_t total_cleans;
} BUTTON_PERSISTENT;

/* ── Per-button runtime state ──────────────────────────── */
typedef struct {
    BUTTON_PERSISTENT  persist;

    BOOL         is_in_filter_zone;
    uint64_t     llLastDownTimeUs;
    uint64_t     llLastUpTimeUs;
    BUTTON_STATE state;
    POINT        ptLastDownPos;
    uint8_t      op_counter;

    SRWLOCK      Lock;
} BUTTON_PROFILE;

/* ── Per-device state ──────────────────────────────────── */
typedef struct {
    WCHAR          szDeviceInstanceId[MAX_DEVICE_ID_LEN];
    HANDLE         hRawDevice;
    BUTTON_PROFILE buttons[BUTTON_COUNT];
    uint64_t       llLastAccessTimeMs;
    BOOL           valid;
} DEVICE_PROFILE;

/* ── Global shared state (defined in amde_engine.c) ────── */
extern DEVICE_PROFILE   g_profiles[MAX_DEVICE_PROFILES];
extern int              g_profileCount;
extern SRWLOCK          g_RegistryLock;
extern LARGE_INTEGER    g_QpcFrequency;
extern volatile LONG    g_totalBounceSession;
extern PVOID volatile   g_hLastActiveRawDevice;
extern HWND             g_hMsgWnd;
extern HWND             g_hRawWnd;
extern HHOOK            g_hHook;
extern NOTIFYICONDATAW  g_nid;

/* ── Window messages ───────────────────────────────────── */
#define WM_AMDE_TRAYICON        (WM_USER + 1)
#define WM_AMDE_REFRESH_CHART   (WM_USER + 2)   /* posted by hook → chart wnd */

#define IDM_TRAY_EXIT           40001
#define IDM_TRAY_RESETALL       40002
#define IDM_TRAY_ABOUT          40003
#define IDM_TRAY_CONFIG         40004
#define ID_TRAY_ICON            1
#define IDT_TOOLTIP_REFRESH     1001
#define IDT_CHART_REFRESH       1002

/* ── Logging ───────────────────────────────────────────── */
static inline void AmdeLog(const WCHAR* fmt, ...) {
    WCHAR buf[512];
    va_list va;
    va_start(va, fmt);
    StringCchVPrintfW(buf, 512, fmt, va);
    va_end(va);
    OutputDebugStringW(buf);
}

#endif /* AMDE_CORE_H */
