#include "../include/amde_core.h"
#include "../include/amde_engine.h"
#include "../include/amde_gui.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' \
  name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
  processorArchitecture='*' publicKeyToken='6595b64144ccf1df' \
  language='*'\"")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev,
                    LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\AMDE_v3_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"AMDE is already running.",
                    AMDE_VERSION_STR, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    AmdeLog(L"[AMDE] v3.0 starting\n");

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    InitializeSRWLock(&g_chart.Lock);
    g_chart.head  = 0;
    g_chart.count = 0;

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
