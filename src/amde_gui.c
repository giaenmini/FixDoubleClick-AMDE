/*
 * amde_gui.c — AMDE v3.0 GUI
 *
 * Implements:
 *   - Tray icon with live tooltip
 *   - Config dialog: per-button override slider, enable/disable per button,
 *     threshold floor control, reset button
 *   - Real-time GDI bounce chart (last 256 events, animated)
 *   - About dialog
 */

#include "../include/amde_core.h"
#include "../include/amde_engine.h"
#include "../include/amde_gui.h"

/* ── Chart ring buffer ─────────────────────────────────── */
CHART_BUFFER g_chart;

typedef struct {
    uint32_t deltaUs;
    uint32_t thresholdUs;
    BOOL     wasBounce;
} CHART_ENTRY;

static CHART_ENTRY g_chartEntries[CHART_HISTORY];

void Chart_Push(uint32_t deltaUs, uint32_t thresholdUs, BOOL wasBounce) {
    AcquireSRWLockExclusive(&g_chart.Lock);
    g_chartEntries[g_chart.head].deltaUs     = deltaUs;
    g_chartEntries[g_chart.head].thresholdUs = thresholdUs;
    g_chartEntries[g_chart.head].wasBounce   = wasBounce;
    g_chart.head = (g_chart.head + 1) % CHART_HISTORY;
    if (g_chart.count < CHART_HISTORY) g_chart.count++;
    ReleaseSRWLockExclusive(&g_chart.Lock);

    /* Notify config window to repaint (non-blocking) */
    if (g_hMsgWnd) PostMessageW(g_hMsgWnd, WM_AMDE_REFRESH_CHART, 0, 0);
}

/* ── Tray ──────────────────────────────────────────────── */
void Tray_Add(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = ID_TRAY_ICON;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_AMDE_TRAYICON;
    g_nid.hIcon            = LoadIconW(NULL, IDI_SHIELD);
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), AMDE_VERSION_STR L" — Running");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void Tray_Remove(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void Tray_UpdateTooltip(void) {
    uint32_t totalBounce = 0, maxThr = 0;
    int      devCount = 0, priorCount = 0;
    double   avgProb  = 0.0;

    AcquireSRWLockShared(&g_RegistryLock);
    for (int i = 0; i < g_profileCount; i++) {
        if (!g_profiles[i].valid) continue;
        devCount++;
        for (int b = 0; b < BUTTON_COUNT; b++) {
            BUTTON_PROFILE* bp = &g_profiles[i].buttons[b];
            AcquireSRWLockShared(&bp->Lock);
            totalBounce += bp->persist.total_bounces;
            if (bp->persist.threshold_exit_us > maxThr)
                maxThr = bp->persist.threshold_exit_us;
            avgProb += Bayes_BounceProb(bp->persist.bayes_alpha, bp->persist.bayes_beta);
            priorCount++;
            ReleaseSRWLockShared(&bp->Lock);
        }
    }
    ReleaseSRWLockShared(&g_RegistryLock);
    if (priorCount > 0) avgProb /= priorCount;

    LONG session = InterlockedCompareExchange(&g_totalBounceSession, 0, 0);
    WCHAR tip[128];
    StringCchPrintfW(tip, ARRAYSIZE(tip),
        L"%s\nDevices: %d  Session: %ld\nLifetime bounces: %u\nMax thr: %.1f ms  P(bounce): %.2f%%",
        AMDE_VERSION_STR, devCount, session, totalBounce,
        maxThr / 1000.0, avgProb * 100.0);
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), tip);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void Tray_ShowContextMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0,              AMDE_VERSION_STR);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CONFIG,   L"⚙ Configure…");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESETALL, L"↺ Reset all profiles");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_ABOUT,    L"ℹ About");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT,     L"✕ Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

/* ══════════════════════════════════════════════════════════
   CONFIG DIALOG
   Layout:
     ┌─ Summary ────────────────────────────────────────────┐
     │  Devices: N   Session bounces: N   P(bounce): N%     │
     ├─ Settings ───────────────────────────────────────────┤
     │  Min threshold floor:  [slider 20–80 ms]  [value]   │
     │  Hysteresis band:      [slider 1–10 ms]   [value]   │
     ├─ Bounce chart (live) ────────────────────────────────┤
     │  [GDI chart — 256 events, green=clean, red=bounce]   │
     │  [threshold line in yellow]                          │
     ├──────────────────────────────────────────────────────┤
     │       [Reset profiles]           [Close]             │
     └──────────────────────────────────────────────────────┘
   ══════════════════════════════════════════════════════════ */

#define IDC_SLIDER_FLOOR    2001
#define IDC_SLIDER_HYSTER   2002
#define IDC_LABEL_FLOOR     2003
#define IDC_LABEL_HYSTER    2004
#define IDC_CHART           2005
#define IDC_BTN_RESET       2006
#define IDC_STATIC_SUMMARY  2007
#define IDC_BTN_CLOSE       2008
#define IDT_CFG_REFRESH     2009

/* Shared config — written by dialog, read by engine on next event.
   Using volatile uint32_t; writes are 32-bit atomic on x86/x64. */
volatile uint32_t g_cfgFloorUs      = DEFAULT_MIN_THRESHOLD_US;
volatile uint32_t g_cfgHysteresisUs = HYSTERESIS_BAND_US;

static HINSTANCE g_hInst = NULL;
HWND             g_hCfgDlg = NULL;   /* NULL when dialog is closed */

/* ── Chart painting ──────────────────────────────────────
   Draws inside the IDC_CHART static control's client rect.
   Uses double-buffered GDI to avoid flicker.
*/
static void PaintChart(HWND hChart) {
    RECT rc;
    GetClientRect(hChart, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    HDC hdcScr = GetDC(hChart);
    HDC hdc    = CreateCompatibleDC(hdcScr);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScr, W, H);
    SelectObject(hdc, hBmp);

    /* Background */
    HBRUSH bgBrush = CreateSolidBrush(RGB(18, 18, 24));
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    /* Axis labels */
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(120, 120, 140));
    HFONT hFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
    SelectObject(hdc, hFont);
    TextOutW(hdc, 4, 2, L"ms", 2);

    AcquireSRWLockShared(&g_chart.Lock);
    int count = g_chart.count;
    if (count == 0) {
        ReleaseSRWLockShared(&g_chart.Lock);
        BitBlt(hdcScr, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
        DeleteObject(hFont); DeleteObject(hBmp);
        DeleteDC(hdc); ReleaseDC(hChart, hdcScr);
        return;
    }

    /* Find max delta for Y-scale (min 60 ms for readability) */
    uint32_t maxDelta = 60000;
    for (int i = 0; i < count; i++) {
        int idx = (g_chart.head - count + i + CHART_HISTORY) % CHART_HISTORY;
        if (g_chartEntries[idx].deltaUs > maxDelta)
            maxDelta = g_chartEntries[idx].deltaUs;
    }

    /* Draw horizontal grid lines at 10 ms, 20 ms, 30 ms, 40 ms, 50 ms */
    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(40, 40, 55));
    SelectObject(hdc, gridPen);
    for (int ms = 10; ms <= (int)(maxDelta / 1000); ms += 10) {
        int y = H - (int)((double)ms * 1000 / maxDelta * (H - 16)) - 4;
        if (y < 4) break;
        MoveToEx(hdc, 0, y, NULL);
        LineTo(hdc, W, y);
        WCHAR label[8];
        StringCchPrintfW(label, 8, L"%d", ms);
        TextOutW(hdc, 2, y - 10, label, (int)wcslen(label));
    }
    DeleteObject(gridPen);

    /* Draw bars */
    int barW  = (count > 0) ? max(1, W / count) : 1;
    int slots = W / max(1, barW);
    int start = (count > slots) ? count - slots : 0;

    for (int i = start; i < count; i++) {
        int idx = (g_chart.head - count + i + CHART_HISTORY) % CHART_HISTORY;
        CHART_ENTRY* e = &g_chartEntries[idx];

        double ratio = (maxDelta > 0) ? (double)e->deltaUs / maxDelta : 0.0;
        int barH = (int)(ratio * (H - 16));
        if (barH < 1) barH = 1;

        int x  = (i - start) * barW;
        int y  = H - barH - 4;

        /* Bar colour: red=bounce, green=clean */
        COLORREF col = e->wasBounce ? RGB(220, 60, 60) : RGB(60, 185, 100);
        HBRUSH barBrush = CreateSolidBrush(col);
        RECT barRc = { x, y, x + barW - 1, H - 4 };
        FillRect(hdc, &barRc, barBrush);
        DeleteObject(barBrush);

        /* Threshold tick line (yellow) — drawn over bar */
        if (e->thresholdUs > 0) {
            double thrRatio = (double)e->thresholdUs / maxDelta;
            int thrY = H - (int)(thrRatio * (H - 16)) - 4;
            if (thrY >= 0 && thrY < H) {
                HPEN thrPen = CreatePen(PS_SOLID, 1, RGB(255, 210, 0));
                SelectObject(hdc, thrPen);
                MoveToEx(hdc, x, thrY, NULL);
                LineTo(hdc, x + barW, thrY);
                DeleteObject(thrPen);
            }
        }
    }
    ReleaseSRWLockShared(&g_chart.Lock);

    /* Legend */
    SetTextColor(hdc, RGB(220, 60, 60));
    TextOutW(hdc, W - 100, 2, L"■ Bounce", 8);
    SetTextColor(hdc, RGB(60, 185, 100));
    TextOutW(hdc, W - 100, 14, L"■ Clean", 7);
    SetTextColor(hdc, RGB(255, 210, 0));
    TextOutW(hdc, W - 100, 26, L"— Threshold", 11);

    BitBlt(hdcScr, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
    DeleteObject(hFont);
    DeleteObject(hBmp);
    DeleteDC(hdc);
    ReleaseDC(hChart, hdcScr);
}

/* ── Dialog summary update ─────────────────────────────── */
static void UpdateSummary(HWND hDlg) {
    uint32_t totalBounce = 0, totalClean = 0;
    int      devCount = 0;
    double   avgProb  = 0.0;
    int      priorCnt = 0;

    AcquireSRWLockShared(&g_RegistryLock);
    for (int i = 0; i < g_profileCount; i++) {
        if (!g_profiles[i].valid) continue;
        devCount++;
        for (int b = 0; b < BUTTON_COUNT; b++) {
            BUTTON_PROFILE* bp = &g_profiles[i].buttons[b];
            AcquireSRWLockShared(&bp->Lock);
            totalBounce += bp->persist.total_bounces;
            totalClean  += bp->persist.total_cleans;
            avgProb     += Bayes_BounceProb(bp->persist.bayes_alpha, bp->persist.bayes_beta);
            priorCnt++;
            ReleaseSRWLockShared(&bp->Lock);
        }
    }
    ReleaseSRWLockShared(&g_RegistryLock);
    if (priorCnt > 0) avgProb /= priorCnt;

    LONG session = InterlockedCompareExchange(&g_totalBounceSession, 0, 0);
    WCHAR buf[256];
    StringCchPrintfW(buf, ARRAYSIZE(buf),
        L"Devices tracked: %d    Session bounces: %ld    "
        L"Lifetime: %u bounces / %u clean    P(bounce): %.1f%%",
        devCount, session, totalBounce, totalClean, avgProb * 100.0);
    SetDlgItemTextW(hDlg, IDC_STATIC_SUMMARY, buf);
}

/* ── Slider label update ───────────────────────────────── */
static void UpdateSliderLabels(HWND hDlg) {
    int floorMs   = (int)(g_cfgFloorUs / 1000);
    int hysterMs  = (int)(g_cfgHysteresisUs / 1000);
    WCHAR buf[32];
    StringCchPrintfW(buf, 32, L"%d ms", floorMs);
    SetDlgItemTextW(hDlg, IDC_LABEL_FLOOR, buf);
    StringCchPrintfW(buf, 32, L"%d ms", hysterMs);
    SetDlgItemTextW(hDlg, IDC_LABEL_HYSTER, buf);
}

/* ── Window procedure ──────────────────────────────────────
 * Bug 3 fix: this window was created with CreateWindowExW and its class
 * registered with lpfnWndProc, so the callback MUST be WNDPROC
 * (returns LRESULT).  The original code declared it as INT_PTR CALLBACK
 * (DLGPROC) and called DefWindowProcW for WM_PAINT — which is correct
 * for a WNDPROC but the mismatched return type caused silent UB on
 * 64-bit Windows where INT_PTR and LRESULT differ in sign convention
 * and the calling convention for unhandled messages differs entirely.
 * Renamed to ConfigWndProc to make the role explicit.
 */
static LRESULT CALLBACK ConfigWndProc(HWND hDlg, UINT uMsg, WPARAM wp, LPARAM lp) {
    switch (uMsg) {
    case WM_INITDIALOG: {
        /* Initialise common controls (for TrackBar) */
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);

        /* Floor slider: 20–80 ms */
        HWND hFloor = GetDlgItem(hDlg, IDC_SLIDER_FLOOR);
        SendMessageW(hFloor, TBM_SETRANGE, TRUE, MAKELONG(20, 80));
        SendMessageW(hFloor, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hFloor, TBM_SETPOS, TRUE, (LPARAM)(g_cfgFloorUs / 1000));

        /* Hysteresis slider: 1–10 ms */
        HWND hHyster = GetDlgItem(hDlg, IDC_SLIDER_HYSTER);
        SendMessageW(hHyster, TBM_SETRANGE, TRUE, MAKELONG(1, 10));
        SendMessageW(hHyster, TBM_SETPOS, TRUE, (LPARAM)(g_cfgHysteresisUs / 1000));

        UpdateSliderLabels(hDlg);
        UpdateSummary(hDlg);

        /* Refresh timer — repaints chart + updates summary every 500 ms */
        SetTimer(hDlg, IDT_CFG_REFRESH, 500, NULL);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_CFG_REFRESH) {
            UpdateSummary(hDlg);
            HWND hChart = GetDlgItem(hDlg, IDC_CHART);
            InvalidateRect(hChart, NULL, FALSE);
        }
        break;

    case WM_HSCROLL: {
        HWND hCtrl = (HWND)lp;
        HWND hFloor  = GetDlgItem(hDlg, IDC_SLIDER_FLOOR);
        HWND hHyster = GetDlgItem(hDlg, IDC_SLIDER_HYSTER);

        if (hCtrl == hFloor) {
            int val = (int)SendMessageW(hFloor, TBM_GETPOS, 0, 0);
            g_cfgFloorUs = (uint32_t)(val * 1000);
        } else if (hCtrl == hHyster) {
            int val = (int)SendMessageW(hHyster, TBM_GETPOS, 0, 0);
            g_cfgHysteresisUs = (uint32_t)(val * 1000);
        }
        UpdateSliderLabels(hDlg);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_RESET:
            if (MessageBoxW(hDlg,
                L"Reset all device learning data?\n"
                L"The engine will re-learn thresholds from scratch.",
                L"Reset profiles", MB_YESNO | MB_ICONWARNING) == IDYES) {
                Engine_ResetAllProfiles();
                UpdateSummary(hDlg);
            }
            break;
        case IDC_BTN_CLOSE:
        case IDCANCEL:
            KillTimer(hDlg, IDT_CFG_REFRESH);
            DestroyWindow(hDlg);
            g_hCfgDlg = NULL;
            break;
        }
        break;

    /* Bug 4 fix: the chart is painted exclusively by ChartSubclassProc
     * via WM_PAINT on the child HWND.  Remove the redundant PaintChart()
     * call here that was causing every repaint cycle to draw the chart
     * twice (once here via the parent WM_PAINT, once via the subclass).
     * Just forward to DefWindowProcW so the dialog chrome paints normally. */
    case WM_PAINT:
        return DefWindowProcW(hDlg, uMsg, wp, lp);

    case WM_AMDE_REFRESH_CHART: {
        HWND hChart = GetDlgItem(hDlg, IDC_CHART);
        if (hChart) InvalidateRect(hChart, NULL, FALSE);
        break;
    }

    case WM_DESTROY:
        KillTimer(hDlg, IDT_CFG_REFRESH);
        g_hCfgDlg = NULL;
        break;
    }
    return DefWindowProcW(hDlg, uMsg, wp, lp);
}

/*
 * Chart subclass — intercepts WM_PAINT on the static child so we can
 * draw directly into it without needing a resource-compiled dialog template.
 */
static LRESULT CALLBACK ChartSubclassProc(HWND hwnd, UINT uMsg,
                                           WPARAM wp, LPARAM lp,
                                           UINT_PTR, DWORD_PTR) {
    if (uMsg == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        PaintChart(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (uMsg == WM_ERASEBKGND) return 1; /* suppress flicker */
    return DefWindowProcW(hwnd, uMsg, wp, lp);
}

/* Bug 2 fix: proper WNDENUMPROC for font propagation. */
static BOOL CALLBACK SetFontOnChild(HWND hChild, LPARAM lFont) {
    SendMessageW(hChild, WM_SETFONT, (WPARAM)lFont, TRUE);
    return TRUE; /* continue enumeration */
}

/* ── Build dialog at runtime (no .rc file needed) ──────── */
void GUI_ShowConfigDialog(HINSTANCE hInst) {
    if (g_hCfgDlg) {
        SetForegroundWindow(g_hCfgDlg);
        return;
    }
    g_hInst = hInst;

    /*
     * Build dialog template in memory.
     * We create a modeless dialog with a fixed layout.
     * Units are dialog units (DLU). Dialog base: 4×2 DLU/pixel typical.
     *
     * Items:
     *   Summary text     (STATIC)
     *   Floor label      (STATIC)
     *   Floor slider     (TRACKBAR)
     *   Floor value      (STATIC)
     *   Hyster label     (STATIC)
     *   Hyster slider    (TRACKBAR)
     *   Hyster value     (STATIC)
     *   Chart frame      (STATIC)
     *   Reset button     (BUTTON)
     *   Close button     (BUTTON)
     */

    /* Easier: use CreateWindowExW directly for a real window (not dialog) */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ConfigWndProc;  /* Bug 3 fix: proper WNDPROC, not a DLGPROC */
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AMDE_ConfigWnd";
    RegisterClassExW(&wc); /* ignore error on re-register */

    /* We'll use CreateDialog with an in-memory template. */
    /* For simplicity, build as a regular overlapped window with manual controls. */

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME,
        L"AMDE_ConfigWnd", AMDE_VERSION_STR L" — Configuration",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 460,
        NULL, NULL, hInst, NULL);

    if (!hWnd) return;
    g_hCfgDlg = hWnd;

    /* ── Create child controls ── */

    /* Summary */
    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 10, 590, 18, hWnd, (HMENU)IDC_STATIC_SUMMARY, hInst, NULL);

    /* Floor label + slider + value */
    CreateWindowExW(0, L"STATIC", L"Min threshold floor:",
        WS_CHILD | WS_VISIBLE,
        10, 40, 140, 18, hWnd, NULL, hInst, NULL);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    HWND hFloor = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        155, 36, 380, 28, hWnd, (HMENU)IDC_SLIDER_FLOOR, hInst, NULL);
    SendMessageW(hFloor, TBM_SETRANGE, TRUE, MAKELONG(20, 80));
    SendMessageW(hFloor, TBM_SETTICFREQ, 10, 0);
    SendMessageW(hFloor, TBM_SETPOS, TRUE, (LPARAM)(g_cfgFloorUs / 1000));

    CreateWindowExW(0, L"STATIC", L"35 ms",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        540, 40, 60, 18, hWnd, (HMENU)IDC_LABEL_FLOOR, hInst, NULL);

    /* Hysteresis label + slider + value */
    CreateWindowExW(0, L"STATIC", L"Hysteresis band:",
        WS_CHILD | WS_VISIBLE,
        10, 80, 140, 18, hWnd, NULL, hInst, NULL);

    HWND hHyster = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        155, 76, 380, 28, hWnd, (HMENU)IDC_SLIDER_HYSTER, hInst, NULL);
    SendMessageW(hHyster, TBM_SETRANGE, TRUE, MAKELONG(1, 10));
    SendMessageW(hHyster, TBM_SETPOS, TRUE, (LPARAM)(g_cfgHysteresisUs / 1000));

    CreateWindowExW(0, L"STATIC", L"2 ms",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        540, 80, 60, 18, hWnd, (HMENU)IDC_LABEL_HYSTER, hInst, NULL);

    /* Separator label */
    CreateWindowExW(0, L"STATIC", L"Live bounce chart  (red = bounce filtered, green = clean click, yellow line = threshold)",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 115, 590, 16, hWnd, NULL, hInst, NULL);

    /* Chart area */
    HWND hChart = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        10, 132, 590, 240, hWnd, (HMENU)IDC_CHART, hInst, NULL);

    /* Subclass the chart static to intercept WM_PAINT */
    SetWindowSubclass(hChart, ChartSubclassProc, 0, 0);

    /* Buttons */
    CreateWindowExW(0, L"BUTTON", L"Reset all profiles",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 386, 150, 28, hWnd, (HMENU)IDC_BTN_RESET, hInst, NULL);

    CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
        470, 386, 130, 28, hWnd, (HMENU)IDC_BTN_CLOSE, hInst, NULL);

    /* Apply font to all children.
     * Bug 2 fix: EnumChildWindows requires a WNDENUMPROC (BOOL CALLBACK(HWND,LPARAM)).
     * Passing SendMessageW directly is the wrong signature and will crash or
     * corrupt the stack.  Use a proper inline callback instead. */
    HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");

    /* Nested callback — valid in C99/C11 via a file-scope helper. */
    EnumChildWindows(hWnd, SetFontOnChild, (LPARAM)hFont);
    SendMessageW(hWnd, WM_SETFONT, (WPARAM)hFont, TRUE);

    /* Start refresh timer */
    SetTimer(hWnd, IDT_CFG_REFRESH, 500, NULL);

    /* Initial data */
    UpdateSummary(hWnd);
    UpdateSliderLabels(hWnd);

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
}

/* ══════════════════════════════════════════════════════════
   Main message window WndProc
   ══════════════════════════════════════════════════════════ */
LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT uMsg, WPARAM wp, LPARAM lp) {
    switch (uMsg) {
    case WM_CREATE:
        SetTimer(hwnd, IDT_TOOLTIP_REFRESH, TOOLTIP_REFRESH_MS, NULL);
        break;

    case WM_TIMER:
        if (wp == IDT_TOOLTIP_REFRESH) Tray_UpdateTooltip();
        break;

    case WM_AMDE_REFRESH_CHART:
        /* Forward to config dialog if open */
        if (g_hCfgDlg) PostMessageW(g_hCfgDlg, WM_AMDE_REFRESH_CHART, 0, 0);
        break;

    case WM_AMDE_TRAYICON:
        switch (lp) {
        case WM_LBUTTONDBLCLK:
            GUI_ShowConfigDialog(g_hInst);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            Tray_ShowContextMenu(hwnd);
            break;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TRAY_EXIT:
            Engine_Shutdown();
            Tray_Remove();
            PostQuitMessage(0);
            break;
        case IDM_TRAY_RESETALL:
            Engine_ResetAllProfiles();
            MessageBoxW(NULL,
                L"All device profiles reset.\n"
                L"AMDE will re-learn thresholds from scratch.",
                AMDE_VERSION_STR, MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_TRAY_CONFIG:
            GUI_ShowConfigDialog(g_hInst);
            break;
        case IDM_TRAY_ABOUT:
            MessageBoxW(NULL,
                L"Adaptive Mechanical Debounce Engine v3.0\n\n"
                L"Algorithm:\n"
                L"  • Per-device P95 Quantile threshold (QuickSelect)\n"
                L"  • Beta(α,β) conjugate Bayesian posterior\n"
                L"    (real posterior mean, not EMA)\n"
                L"  • Dual-threshold Hysteresis (enter > exit)\n"
                L"  • Drag detection — suppresses false positives\n"
                L"  • LRU device registry (SRWLock, double-check)\n"
                L"  • Registry persistence with version check\n"
                L"  • XBUTTON1 + XBUTTON2 support\n\n"
                L"Profile store: HKCU\\Software\\AMDE\n"
                L"Double-click tray icon to open config.",
                AMDE_VERSION_STR, MB_OK | MB_ICONINFORMATION);
            break;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_TOOLTIP_REFRESH);
        Tray_Remove();
        break;
    }

    /* Store hInstance for GUI_ShowConfigDialog */
    static HINSTANCE s_hInst = NULL;
    if (uMsg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        s_hInst  = cs->hInstance;
        g_hInst  = cs->hInstance;
    }

    return DefWindowProcW(hwnd, uMsg, wp, lp);
}
