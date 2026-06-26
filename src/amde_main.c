/*
 * amde_main.c — AMDE v3.0 Entry Point
 *
 * Build (MSVC):
 *   cl /O2 /W4 /DUNICODE /D_UNICODE /I include
 *      src\amde_main.c src\amde_engine.c src\amde_gui.c
 *      /link /SUBSYSTEM:WINDOWS
 *      user32.lib shell32.lib advapi32.lib comctl32.lib
 *
 * Build (MinGW):
 *   gcc -O2 -Wall -DUNICODE -D_UNICODE -mwindows -I include
 *       src/amde_main.c src/amde_engine.c src/amde_gui.c
 *       -luser32 -lshell32 -ladvapi32 -lcomctl32 -lcomdlg32
 *       -o amde.exe
 */

#include "../include/amde_core.h"
#include "../include/amde_engine.h"
#include "../include/amde_gui.h"

/* Enable visual styles (comctl32 v6) */
#pragma comment(linker, "\"/manifestdependency:type='win32' \
  name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
  processorArchitecture='*' publicKeyToken='6595b64144ccf1df' \
  language='*'\"")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev,
                    LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;

    /* Single-instance guard */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\AMDE_v3_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"AMDE is already running.",
                    AMDE_VERSION_STR, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    AmdeLog(L"[AMDE] v3.0 starting\n");

    /* Initialise common controls for TrackBar etc. */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    /* Initialise chart buffer */
    InitializeSRWLock(&g_chart.Lock);
    g_chart.head  = 0;
    g_chart.count = 0;

    /* Start engine (loads profiles, registers hook, creates tray) */
    if (!Engine_Init(hInstance)) {
        MessageBoxW(NULL,
            L"AMDE failed to initialise.\n"
            L"Please run as administrator if hook installation fails.",
            AMDE_VERSION_STR, MB_OK | MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    AmdeLog(L"[AMDE] Entering message loop\n");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Allow config dialog to get keyboard messages */
        if (g_hCfgDlg && IsDialogMessageW(g_hCfgDlg, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Engine_Shutdown();
    CloseHandle(hMutex);
    AmdeLog(L"[AMDE] Clean exit\n");
    return (int)msg.wParam;
}
