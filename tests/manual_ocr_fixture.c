// Manual QA fixture for OCR auto-loss (Task 10 Step 3 / Task 12 §7).
//
// A small owned "target application": it binds a loopback UDP socket so
// procmap enumeration lists it, shows a bottom-right status-bar static
// reading "仓库" (idle text that disappears while the user "types"),
// and a center control reading "重连" that appears on demand.
//
// Manual mode: buttons toggle the two texts and run the full script.
//   build/tests/manual_ocr_fixture.exe
// Auto mode (driven by tests/manual_ocr_e2e.c): fixed timed script that
// exits by itself:
//   0-4 s   仓库 visible, 重连 hidden (OCR seed/baseline)
//   4 s     仓库 hidden   -> DISAPPEAR rule should switch loss to 95%
//   8 s     重连 shown    -> APPEAR rule should switch loss to 50%
//   12.5 s  process exits
#include <winsock2.h>
#include <windows.h>
#include <string.h>

#define FIXTURE_CLASS L"ClumsyOcrFixtureClass"
#define FIXTURE_TITLE L"OCR QA Fixture 夹具"
#define IDC_STATUS   1001
#define IDC_RECONNECT 1002
#define IDC_HIDE_STATUS 1003
#define IDC_SHOW_RECONNECT 1004
#define IDC_RUN_SCRIPT 1005

static HWND g_status, g_reconnect, g_hideBtn, g_showBtn, g_runBtn;
static SOCKET g_udp = INVALID_SOCKET;

// Script timeline in milliseconds (WM_TIMER driven).
static const struct { UINT at; int action; } SCRIPT[] = {
    { 4000,  IDC_HIDE_STATUS },
    { 8000,  IDC_SHOW_RECONNECT },
    { 12500, 0 }, // exit
};
static int g_scriptStep = 0;
static DWORD g_scriptStartedAt = 0;
static BOOL g_auto = FALSE;

static void bindEndpoint(void) {
    WSADATA wsa;
    struct sockaddr_in sa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp == INVALID_SOCKET) return;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    bind(g_udp, (struct sockaddr *)&sa, sizeof(sa));
}

static void doAction(int action) {
    if (action == IDC_HIDE_STATUS) {
        ShowWindow(g_status, SW_HIDE);
    } else if (action == IDC_SHOW_RECONNECT) {
        ShowWindow(g_reconnect, SW_SHOW);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_status = CreateWindowExW(WS_EX_STATICEDGE, L"Static", L"仓库",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_CENTER,
            300, 245, 140, 28, hwnd, (HMENU)(INT_PTR)IDC_STATUS,
            GetModuleHandleW(NULL), NULL);
        g_reconnect = CreateWindowExW(0, L"Static", L"重连",
            WS_CHILD | SS_CENTERIMAGE | SS_CENTER,
            150, 120, 160, 40, hwnd, (HMENU)(INT_PTR)IDC_RECONNECT,
            GetModuleHandleW(NULL), NULL);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(g_reconnect, WM_SETFONT, (WPARAM)font, TRUE);

        if (!g_auto) {
            g_hideBtn = CreateWindowExW(0, L"Button", L"隐藏仓库",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                12, 12, 120, 30, hwnd,
                (HMENU)(INT_PTR)IDC_HIDE_STATUS,
                GetModuleHandleW(NULL), NULL);
            g_showBtn = CreateWindowExW(0, L"Button", L"显示重连",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                140, 12, 120, 30, hwnd,
                (HMENU)(INT_PTR)IDC_SHOW_RECONNECT,
                GetModuleHandleW(NULL), NULL);
            g_runBtn = CreateWindowExW(0, L"Button", L"运行完整剧本",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                268, 12, 160, 30, hwnd,
                (HMENU)(INT_PTR)IDC_RUN_SCRIPT,
                GetModuleHandleW(NULL), NULL);
        } else {
            (void)g_hideBtn; (void)g_showBtn; (void)g_runBtn;
        }
        return 0;
    }
    case WM_COMMAND:
        if (g_auto) break;
        if (LOWORD(wp) == IDC_HIDE_STATUS
            || LOWORD(wp) == IDC_SHOW_RECONNECT) {
            doAction(LOWORD(wp));
        } else if (LOWORD(wp) == IDC_RUN_SCRIPT) {
            g_scriptStep = 0;
            g_scriptStartedAt = GetTickCount();
            SetTimer(hwnd, 1, 100, NULL);
            EnableWindow(g_runBtn, FALSE);
        }
        return 0;
    case WM_TIMER: {
        // 100 ms ticks; fire each script action whose time has come.
        DWORD elapsed = GetTickCount() - g_scriptStartedAt;
        while (g_scriptStep < (int)(sizeof(SCRIPT) / sizeof(SCRIPT[0]))
               && elapsed >= SCRIPT[g_scriptStep].at) {
            if (SCRIPT[g_scriptStep].action == 0) {
                KillTimer(hwnd, 1);
                PostQuitMessage(0);
                return 0;
            }
            doAction(SCRIPT[g_scriptStep].action);
            g_scriptStep++;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    // GetCommandLineA (not WinMain's lpCmdLine) so --auto is detected
    // regardless of how the C runtime strips the program name.
    if (strstr(GetCommandLineA(), "--auto")) g_auto = TRUE;
    bindEndpoint();

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = FIXTURE_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, FIXTURE_CLASS,
        FIXTURE_TITLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        200, 200, 480, 320, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);

    if (g_auto) {
        g_scriptStep = 0;
        g_scriptStartedAt = GetTickCount();
        SetTimer(hwnd, 1, 100, NULL);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_udp != INVALID_SOCKET) {
        closesocket(g_udp);
        WSACleanup();
    }
    return 0;
}
