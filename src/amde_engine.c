/*
 * amde_engine.c — AMDE v3.0 Debounce Engine
 *
 * Implements:
 *   - QuickSelect P95 (iterative, median-of-three pivot)
 *   - Beta(α,β) conjugate Bayesian prior (real posterior, not EMA)
 *   - Dual-threshold hysteresis
 *   - LRU device profile registry (SRWLock, double-check)
 *   - Registry persistence with versioned binary blob
 *   - XBUTTON1 + XBUTTON2 support
 *   - Drag detection per-button
 */

#include "../include/amde_core.h"
#include "../include/amde_engine.h"
#include "../include/amde_gui.h"

/* Live config knobs written by the GUI sliders.
   Declared volatile in amde_gui.c — import here for the engine. */
extern volatile uint32_t g_cfgFloorUs;
extern volatile uint32_t g_cfgHysteresisUs;

/* ── Global state ─────────────────────────────────────── */
DEVICE_PROFILE  g_profiles[MAX_DEVICE_PROFILES];
int             g_profileCount        = 0;
SRWLOCK         g_RegistryLock        = SRWLOCK_INIT;
LARGE_INTEGER   g_QpcFrequency;
volatile LONG   g_totalBounceSession  = 0;
PVOID volatile  g_hLastActiveRawDevice = NULL;
HWND            g_hMsgWnd             = NULL;
HWND            g_hRawWnd             = NULL;
HHOOK           g_hHook               = NULL;
NOTIFYICONDATAW g_nid;

/* ══════════════════════════════════════════════════════
   QuickSelect — iterative, median-of-three pivot
   O(n) average, O(n²) worst-case (rare with MoT pivot)
   ══════════════════════════════════════════════════════ */
#define SWAP32(a,b) do { uint32_t _t=(a);(a)=(b);(b)=_t; } while(0)

static uint32_t QuickSelect(uint32_t* arr, int n, int k) {
    int left = 0, right = n - 1;
    while (left <= right) {
        if (left == right) return arr[left];
        int mid = left + (right - left) / 2;
        /* Median-of-three: sort left/mid/right, use mid as pivot */
        if (arr[left]  > arr[mid])   SWAP32(arr[left],  arr[mid]);
        if (arr[left]  > arr[right]) SWAP32(arr[left],  arr[right]);
        if (arr[mid]   > arr[right]) SWAP32(arr[mid],   arr[right]);
        uint32_t pivot = arr[mid];
        SWAP32(arr[mid], arr[right]);
        int store = left;
        for (int i = left; i < right; i++)
            if (arr[i] < pivot) { SWAP32(arr[i], arr[store]); store++; }
        SWAP32(arr[store], arr[right]);
        if      (store == k) return arr[store];
        else if (store <  k) left  = store + 1;
        else                 right = store - 1;
    }
    return arr[left];
}
#undef SWAP32

/* ══════════════════════════════════════════════════════
   Threshold recalculation
   Called after every Bayesian update.
   ══════════════════════════════════════════════════════ */
static void RecalcThresholds(BUTTON_PERSISTENT* p) {
    /*
     * Bayesian extension:
     *   bounce_prob = alpha / (alpha+beta)  in [0,1]
     *   extension   = bounce_prob x MAX_BAYES_EXTENSION_US
     *
     * This is the correct use of the posterior mean — the higher
     * the estimated bounce probability, the more we extend the
     * debounce window.
     *
     * Bug 1 fix: use the live GUI config values (g_cfgFloorUs,
     * g_cfgHysteresisUs) instead of the compile-time constants so
     * slider changes actually take effect in the engine.
     */
    uint32_t floorUs   = g_cfgFloorUs;       /* volatile read — GUI slider */
    uint32_t hysterUs  = g_cfgHysteresisUs;  /* volatile read — GUI slider */

    double   bounce_prob = Bayes_BounceProb(p->bayes_alpha, p->bayes_beta);
    uint32_t bayes_ext   = (uint32_t)(bounce_prob * MAX_BAYES_EXTENSION_US);

    /* Enforce the user-chosen floor before adding the Bayesian extension. */
    if (p->p95_threshold_us < floorUs)
        p->p95_threshold_us = floorUs;

    p->threshold_exit_us  = p->p95_threshold_us + bayes_ext;
    p->threshold_enter_us = p->threshold_exit_us + hysterUs;
}

/* ══════════════════════════════════════════════════════
   Registry Persistence
   Blob layout:
     [u32 magic] [u32 version] [u32 count]
     count × {
       WCHAR[MAX_DEVICE_ID_LEN]       szId
       BUTTON_PERSISTENT[BUTTON_COUNT]
     }
   ══════════════════════════════════════════════════════ */
#define AMDE_REG_SUBKEY     L"Software\\AMDE"
#define AMDE_PROFILE_VALUE  L"DeviceProfilesV3"

void Engine_PersistSave(void) {
    AcquireSRWLockShared(&g_RegistryLock);

    int validCount = 0;
    for (int i = 0; i < g_profileCount; i++)
        if (g_profiles[i].valid) validCount++;

    size_t recSz    = MAX_DEVICE_ID_LEN * sizeof(WCHAR)
                    + BUTTON_COUNT * sizeof(BUTTON_PERSISTENT);
    size_t totalSz  = sizeof(uint32_t) * 3          /* magic+version+count */
                    + (size_t)validCount * recSz;

    BYTE* blob = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, totalSz);
    if (!blob) { ReleaseSRWLockShared(&g_RegistryLock); return; }

    BYTE* p = blob;
    *(uint32_t*)p = PERSIST_MAGIC;    p += 4;
    *(uint32_t*)p = PERSIST_VERSION;  p += 4;
    *(uint32_t*)p = (uint32_t)validCount; p += 4;

    for (int i = 0; i < g_profileCount; i++) {
        if (!g_profiles[i].valid) continue;
        memcpy(p, g_profiles[i].szDeviceInstanceId, MAX_DEVICE_ID_LEN * sizeof(WCHAR));
        p += MAX_DEVICE_ID_LEN * sizeof(WCHAR);
        for (int b = 0; b < BUTTON_COUNT; b++) {
            AcquireSRWLockShared(&g_profiles[i].buttons[b].Lock);
            memcpy(p, &g_profiles[i].buttons[b].persist, sizeof(BUTTON_PERSISTENT));
            ReleaseSRWLockShared(&g_profiles[i].buttons[b].Lock);
            p += sizeof(BUTTON_PERSISTENT);
        }
    }
    ReleaseSRWLockShared(&g_RegistryLock);

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, AMDE_REG_SUBKEY,
                        0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, AMDE_PROFILE_VALUE, 0, REG_BINARY, blob, (DWORD)totalSz);
        RegCloseKey(hKey);
        AmdeLog(L"[AMDE] Persist saved: %d devices\n", validCount);
    }
    HeapFree(GetProcessHeap(), 0, blob);
}

void Engine_PersistLoad(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AMDE_REG_SUBKEY,
                      0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return;

    DWORD dataSz = 0;
    RegQueryValueExW(hKey, AMDE_PROFILE_VALUE, NULL, NULL, NULL, &dataSz);
    if (dataSz < sizeof(uint32_t) * 3) { RegCloseKey(hKey); return; }

    BYTE* blob = (BYTE*)HeapAlloc(GetProcessHeap(), 0, dataSz);
    if (!blob) { RegCloseKey(hKey); return; }

    if (RegQueryValueExW(hKey, AMDE_PROFILE_VALUE, NULL, NULL, blob, &dataSz) == ERROR_SUCCESS) {
        BYTE* p = blob;
        uint32_t magic   = *(uint32_t*)p; p += 4;
        uint32_t version = *(uint32_t*)p; p += 4;
        uint32_t count   = *(uint32_t*)p; p += 4;

        if (magic != PERSIST_MAGIC || version != PERSIST_VERSION) {
            AmdeLog(L"[AMDE] Persist: stale/incompatible blob (magic=%08X ver=%u), skipping\n",
                    magic, version);
            HeapFree(GetProcessHeap(), 0, blob);
            RegCloseKey(hKey);
            return;
        }

        if (count > MAX_DEVICE_PROFILES) count = MAX_DEVICE_PROFILES;

        AcquireSRWLockExclusive(&g_RegistryLock);
        for (uint32_t i = 0; i < count; i++) {
            int idx = (g_profileCount < MAX_DEVICE_PROFILES) ? g_profileCount++ : MAX_DEVICE_PROFILES - 1;
            memcpy(g_profiles[idx].szDeviceInstanceId, p, MAX_DEVICE_ID_LEN * sizeof(WCHAR));
            p += MAX_DEVICE_ID_LEN * sizeof(WCHAR);
            for (int b = 0; b < BUTTON_COUNT; b++) {
                memcpy(&g_profiles[idx].buttons[b].persist, p, sizeof(BUTTON_PERSISTENT));
                p += sizeof(BUTTON_PERSISTENT);
                g_profiles[idx].buttons[b].is_in_filter_zone = FALSE;
                g_profiles[idx].buttons[b].state             = STATE_IDLE;
                g_profiles[idx].buttons[b].llLastDownTimeUs  = 0;
                g_profiles[idx].buttons[b].llLastUpTimeUs    = 0;
                g_profiles[idx].buttons[b].op_counter        = 0;
                InitializeSRWLock(&g_profiles[idx].buttons[b].Lock);
            }
            g_profiles[idx].hRawDevice         = NULL;
            g_profiles[idx].llLastAccessTimeMs = 0;
            g_profiles[idx].valid              = TRUE;
        }
        ReleaseSRWLockExclusive(&g_RegistryLock);
        AmdeLog(L"[AMDE] Persist loaded: %u devices\n", count);
    }
    HeapFree(GetProcessHeap(), 0, blob);
    RegCloseKey(hKey);
}

void Engine_ResetAllProfiles(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AMDE_REG_SUBKEY,
                      0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, AMDE_PROFILE_VALUE);
        RegCloseKey(hKey);
    }
    AcquireSRWLockExclusive(&g_RegistryLock);
    g_profileCount = 0;
    memset(g_profiles, 0, sizeof(g_profiles));
    ReleaseSRWLockExclusive(&g_RegistryLock);
    AmdeLog(L"[AMDE] All profiles reset\n");
}

/* ══════════════════════════════════════════════════════
   Device ID
   ══════════════════════════════════════════════════════ */
static void ParseDeviceId(HANDLE hDevice, WCHAR* szOutId) {
    UINT size = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, NULL, &size) == 0 && size > 0) {
        WCHAR* name = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR));
        if (name) {
            if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, name, &size) != (UINT)-1)
                StringCchCopyW(szOutId, MAX_DEVICE_ID_LEN, name);
            HeapFree(GetProcessHeap(), 0, name);
        }
    }
}

/* ══════════════════════════════════════════════════════
   LRU Profile Registry
   ══════════════════════════════════════════════════════ */
static void InitButtonProfile(BUTTON_PROFILE* bp) {
    bp->persist.bayes_alpha        = BAYES_ALPHA_INIT;
    bp->persist.bayes_beta         = BAYES_BETA_INIT;
    bp->persist.p95_threshold_us   = DEFAULT_MIN_THRESHOLD_US;
    bp->persist.count              = 0;
    bp->persist.sample_index       = 0;
    bp->persist.total_bounces      = 0;
    bp->persist.total_cleans       = 0;
    bp->is_in_filter_zone          = FALSE;
    bp->state                      = STATE_IDLE;
    bp->llLastDownTimeUs           = 0;
    bp->llLastUpTimeUs             = 0;
    bp->op_counter                 = 0;
    RecalcThresholds(&bp->persist);
    InitializeSRWLock(&bp->Lock);
}

DEVICE_PROFILE* Engine_GetOrCreateProfile(HANDLE hDevice) {
    uint64_t tick = GetTickCount64();

    /* Phase 1: shared read — fast path (most calls land here) */
    AcquireSRWLockShared(&g_RegistryLock);
    for (int i = 0; i < g_profileCount; i++) {
        if (g_profiles[i].valid && g_profiles[i].hRawDevice == hDevice) {
            DEVICE_PROFILE* ptr = &g_profiles[i];
            ReleaseSRWLockShared(&g_RegistryLock);
            InterlockedExchange64((LONGLONG*)&ptr->llLastAccessTimeMs, (LONGLONG)tick);
            return ptr;
        }
    }
    ReleaseSRWLockShared(&g_RegistryLock);

    /* Phase 2: exclusive write — double-check pattern */
    AcquireSRWLockExclusive(&g_RegistryLock);
    for (int i = 0; i < g_profileCount; i++) {
        if (g_profiles[i].valid && g_profiles[i].hRawDevice == hDevice) {
            g_profiles[i].llLastAccessTimeMs = tick;
            DEVICE_PROFILE* ptr = &g_profiles[i];
            ReleaseSRWLockExclusive(&g_RegistryLock);
            return ptr;
        }
    }

    /* Re-map persisted profile by device string */
    WCHAR szId[MAX_DEVICE_ID_LEN] = {0};
    ParseDeviceId(hDevice, szId);
    for (int i = 0; i < g_profileCount; i++) {
        if (g_profiles[i].valid && !g_profiles[i].hRawDevice &&
            wcsncmp(g_profiles[i].szDeviceInstanceId, szId, MAX_DEVICE_ID_LEN) == 0) {
            g_profiles[i].hRawDevice         = hDevice;
            g_profiles[i].llLastAccessTimeMs = tick;
            DEVICE_PROFILE* ptr = &g_profiles[i];
            ReleaseSRWLockExclusive(&g_RegistryLock);
            AmdeLog(L"[AMDE] Re-mapped: %s\n", szId);
            return ptr;
        }
    }

    /* Allocate new slot or evict LRU */
    int targetIdx = -1;
    if (g_profileCount < MAX_DEVICE_PROFILES) {
        targetIdx = g_profileCount++;
    } else {
        uint64_t oldest = UINT64_MAX;
        int oldestIdx = 0;
        for (int i = 0; i < MAX_DEVICE_PROFILES; i++) {
            if ((uint64_t)g_profiles[i].llLastAccessTimeMs < oldest) {
                oldest    = (uint64_t)g_profiles[i].llLastAccessTimeMs;
                oldestIdx = i;
            }
        }
        targetIdx = oldestIdx;
        AmdeLog(L"[AMDE] LRU evict slot %d (%s)\n",
                targetIdx, g_profiles[targetIdx].szDeviceInstanceId);
    }

    memset(&g_profiles[targetIdx], 0, sizeof(DEVICE_PROFILE));
    g_profiles[targetIdx].hRawDevice         = hDevice;
    g_profiles[targetIdx].llLastAccessTimeMs = tick;
    g_profiles[targetIdx].valid              = TRUE;
    StringCchCopyW(g_profiles[targetIdx].szDeviceInstanceId, MAX_DEVICE_ID_LEN, szId);
    for (int b = 0; b < BUTTON_COUNT; b++)
        InitButtonProfile(&g_profiles[targetIdx].buttons[b]);

    DEVICE_PROFILE* ptr = &g_profiles[targetIdx];
    ReleaseSRWLockExclusive(&g_RegistryLock);
    AmdeLog(L"[AMDE] New profile slot %d: %s\n", targetIdx, szId);
    return ptr;
}

/* ══════════════════════════════════════════════════════
   Raw Input Window — records last active device handle
   ══════════════════════════════════════════════════════ */
LRESULT CALLBACK RawInputWndProc(HWND hwnd, UINT uMsg, WPARAM wp, LPARAM lp) {
    if (uMsg == WM_INPUT) {
        UINT dwSize = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
        if (dwSize > 0) {
            BYTE* lpb = (BYTE*)HeapAlloc(GetProcessHeap(), 0, dwSize);
            if (lpb) {
                if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, lpb, &dwSize,
                                    sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                    RAWINPUT* raw = (RAWINPUT*)lpb;
                    if (raw->header.dwType == RIM_TYPEMOUSE)
                        InterlockedExchangePointer(&g_hLastActiveRawDevice, raw->header.hDevice);
                }
                HeapFree(GetProcessHeap(), 0, lpb);
            }
        }
    }
    return DefWindowProcW(hwnd, uMsg, wp, lp);
}

/* ══════════════════════════════════════════════════════
   Core Hook Engine
   ══════════════════════════════════════════════════════ */
LRESULT CALLBACK BalancedMouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode < 0) return CallNextHookEx(NULL, nCode, wp, lp);

    MSLLHOOKSTRUCT* hs = (MSLLHOOKSTRUCT*)lp;

    /* Skip injected (software-generated) events */
    if (hs->flags & LLMHF_INJECTED)
        return CallNextHookEx(NULL, nCode, wp, lp);

    HANDLE hActiveDev = (HANDLE)InterlockedCompareExchangePointer(
                            &g_hLastActiveRawDevice, NULL, NULL);

    /* ── Drag detection — updates state, never filters ── */
    if (wp == WM_MOUSEMOVE) {
        if (hActiveDev) {
            DEVICE_PROFILE* dp = Engine_GetOrCreateProfile(hActiveDev);
            for (int b = 0; b < BUTTON_COUNT; b++) {
                BUTTON_PROFILE* bp = &dp->buttons[b];
                AcquireSRWLockExclusive(&bp->Lock);
                if (bp->state == STATE_PRESSED) {
                    int dx = hs->pt.x - bp->ptLastDownPos.x;
                    int dy = hs->pt.y - bp->ptLastDownPos.y;
                    if ((dx * dx + dy * dy) > DRAG_THRESHOLD_SQ)
                        bp->state = STATE_DRAGGING;
                }
                ReleaseSRWLockExclusive(&bp->Lock);
            }
        }
        return CallNextHookEx(NULL, nCode, wp, lp);
    }

    /* ── Classify button and direction ── */
    MOUSE_BUTTON btn    = BUTTON_COUNT;
    BOOL         isDown = FALSE;

    switch (wp) {
        case WM_LBUTTONDOWN: btn = BUTTON_LEFT;   isDown = TRUE;  break;
        case WM_LBUTTONUP:   btn = BUTTON_LEFT;   isDown = FALSE; break;
        case WM_RBUTTONDOWN: btn = BUTTON_RIGHT;  isDown = TRUE;  break;
        case WM_RBUTTONUP:   btn = BUTTON_RIGHT;  isDown = FALSE; break;
        case WM_MBUTTONDOWN: btn = BUTTON_MIDDLE; isDown = TRUE;  break;
        case WM_MBUTTONUP:   btn = BUTTON_MIDDLE; isDown = FALSE; break;
        case WM_XBUTTONDOWN:
            btn    = (HIWORD(hs->mouseData) == XBUTTON1) ? BUTTON_X1 : BUTTON_X2;
            isDown = TRUE;  break;
        case WM_XBUTTONUP:
            btn    = (HIWORD(hs->mouseData) == XBUTTON1) ? BUTTON_X1 : BUTTON_X2;
            isDown = FALSE; break;
    }

    if (btn == BUTTON_COUNT || !hActiveDev)
        return CallNextHookEx(NULL, nCode, wp, lp);

    /* High-resolution timestamp */
    LARGE_INTEGER liPerf;
    QueryPerformanceCounter(&liPerf);
    uint64_t nowUs = (liPerf.QuadPart * 1000000ULL) / g_QpcFrequency.QuadPart;

    DEVICE_PROFILE* dp = Engine_GetOrCreateProfile(hActiveDev);
    BUTTON_PROFILE* bp = &dp->buttons[btn];

    AcquireSRWLockExclusive(&bp->Lock);

    if (isDown) {
        uint64_t deltaUs = (bp->llLastUpTimeUs > 0 && nowUs >= bp->llLastUpTimeUs)
                           ? (nowUs - bp->llLastUpTimeUs)
                           : UINT64_MAX;

        /*
         * Hysteresis:
         *   In filter zone  → use exit threshold (lower)  — need larger gap to exit
         *   Outside zone    → use enter threshold (higher) — only enter on very short delta
         */
        uint32_t evalThresh = bp->is_in_filter_zone
                              ? bp->persist.threshold_exit_us
                              : bp->persist.threshold_enter_us;

        if (bp->state != STATE_DRAGGING && deltaUs < (uint64_t)evalThresh) {
            /* ── BOUNCE DETECTED ── */
            bp->is_in_filter_zone = TRUE;

            /* Record sample */
            bp->persist.samples[bp->persist.sample_index] = (uint32_t)deltaUs;
            bp->persist.sample_index = (bp->persist.sample_index + 1) % BOUNCE_HISTORY_SIZE;
            if (bp->persist.count < BOUNCE_HISTORY_SIZE) bp->persist.count++;

            bp->persist.total_bounces++;
            InterlockedIncrement(&g_totalBounceSession);

            /* Real Bayesian update — Beta conjugate posterior */
            Bayes_Update(&bp->persist, TRUE);

            /* Recalculate P95 every QUANTILE_UPDATE_PERIOD bounces */
            if (++bp->op_counter >= QUANTILE_UPDATE_PERIOD && bp->persist.count > 1) {
                bp->op_counter = 0;
                uint32_t temp[BOUNCE_HISTORY_SIZE];
                int cnt = bp->persist.count;
                for (int i = 0; i < cnt; i++) temp[i] = bp->persist.samples[i];
                int k = (int)(cnt * 0.95f);
                if (k >= cnt) k = cnt - 1;
                uint32_t p95 = QuickSelect(temp, cnt, k);
                bp->persist.p95_threshold_us = (p95 < DEFAULT_MIN_THRESHOLD_US)
                                               ? DEFAULT_MIN_THRESHOLD_US : p95;
            }

            RecalcThresholds(&bp->persist);

            AmdeLog(L"[AMDE] Bounce btn=%d delta=%.2fms thr=%.2fms α=%.2f β=%.2f p=%.3f\n",
                    btn, deltaUs/1000.0, evalThresh/1000.0,
                    bp->persist.bayes_alpha, bp->persist.bayes_beta,
                    Bayes_BounceProb(bp->persist.bayes_alpha, bp->persist.bayes_beta));

            /* Feed chart — do NOT hold the button lock across Chart_Push */
            uint32_t snapDelta = (uint32_t)deltaUs;
            uint32_t snapThr   = evalThresh;
            ReleaseSRWLockExclusive(&bp->Lock);
            Chart_Push(snapDelta, snapThr, TRUE);
            return 1; /* swallow event */
        }

        /* ── CLEAN CLICK ── */
        bp->is_in_filter_zone = FALSE;
        bp->state             = STATE_PRESSED;
        bp->llLastDownTimeUs  = nowUs;
        bp->ptLastDownPos     = hs->pt;
        bp->persist.total_cleans++;

        Bayes_Update(&bp->persist, FALSE);
        RecalcThresholds(&bp->persist);

        uint32_t snapDelta = (deltaUs == UINT64_MAX) ? 0 : (uint32_t)deltaUs;
        uint32_t snapThr   = evalThresh;
        ReleaseSRWLockExclusive(&bp->Lock);
        Chart_Push(snapDelta, snapThr, FALSE);

    } else { /* isUp */
        bp->state          = STATE_IDLE;
        bp->llLastUpTimeUs = nowUs;
        ReleaseSRWLockExclusive(&bp->Lock);
    }

    return CallNextHookEx(NULL, nCode, wp, lp);
}

/* ══════════════════════════════════════════════════════
   Engine init / shutdown
   ══════════════════════════════════════════════════════ */
BOOL Engine_Init(HINSTANCE hInstance) {
    QueryPerformanceFrequency(&g_QpcFrequency);
    Engine_PersistLoad();

    /* Register window classes */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.hInstance     = hInstance;

    wc.lpfnWndProc   = RawInputWndProc;
    wc.lpszClassName = L"AMDE_RawInput";
    if (!RegisterClassExW(&wc)) return FALSE;

    wc.lpfnWndProc   = MsgWndProc;
    wc.lpszClassName = L"AMDE_MsgWnd";
    if (!RegisterClassExW(&wc)) return FALSE;

    /* Hidden window for Raw Input */
    g_hRawWnd = CreateWindowExW(0, L"AMDE_RawInput", L"", 0,
                                0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (!g_hRawWnd) return FALSE;

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = g_hRawWnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    /* Hidden window for tray + timers */
    g_hMsgWnd = CreateWindowExW(0, L"AMDE_MsgWnd", L"", 0,
                                0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (!g_hMsgWnd) return FALSE;

    Tray_Add(g_hMsgWnd);

    /* Install global hook */
    g_hHook = SetWindowsHookExW(WH_MOUSE_LL, BalancedMouseProc, hInstance, 0);
    if (!g_hHook) {
        AmdeLog(L"[AMDE] SetWindowsHookEx failed: %lu\n", GetLastError());
        return FALSE;
    }
    AmdeLog(L"[AMDE] Engine initialised. Hook installed.\n");
    return TRUE;
}

void Engine_Shutdown(void) {
    if (g_hHook) { UnhookWindowsHookEx(g_hHook); g_hHook = NULL; }
    Engine_PersistSave();
    AmdeLog(L"[AMDE] Engine shutdown.\n");
}
