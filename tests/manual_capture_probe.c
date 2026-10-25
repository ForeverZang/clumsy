// Diagnostic probe (manual, not in run_tests.sh): launches the fixture
// --auto and PrintWindow-captures its client every 500 ms, printing frame
// size, overall non-white pixel count and brightness samples inside the
// two OCR regions, so PrintWindow visibility/change behavior is visible.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ocr_capture.h"

#define FIXTURE_CLASS "ClumsyOcrFixtureClass"

static HWND waitForWindow(DWORD timeoutMs) {
    DWORD start = GetTickCount();
    HWND hwnd = NULL;
    while (GetTickCount() - start < timeoutMs) {
        hwnd = FindWindowA(FIXTURE_CLASS, NULL);
        if (hwnd && IsWindowVisible(hwnd)) return hwnd;
        Sleep(50);
    }
    return hwnd;
}

int main(int argc, char **argv) {
    const char *fixtureExe = argc > 1 ? argv[1]
                                      : "build/tests/manual_ocr_fixture.exe";
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    snprintf(cmd, sizeof(cmd), "\"%s\" --auto", fixtureExe);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &pi)) {
        printf("launch failed %lu\n", GetLastError());
        return 2;
    }
    HWND hwnd = waitForWindow(5000);
    if (!hwnd) { printf("no window\n"); return 2; }

    for (int tick = 0; tick < 25; tick++) {
        unsigned char *bgra = NULL;
        int w = 0, h = 0;
        char err[512];
        BOOL ok = captureClientRegion(hwnd, NULL, &bgra, &w, &h, err);
        if (!ok) {
            printf("[%4.1fs] capture fail: %s\n", tick * 0.5, err);
        } else {
            long nonWhite = 0;
            long sumB = 0;
            for (int i = 0; i < w * h; i++) {
                unsigned char b = bgra[i * 4], g = bgra[i * 4 + 1],
                              r = bgra[i * 4 + 2];
                sumB += b;
                if (b < 240 || g < 240 || r < 240) nonWhite++;
            }
            // Sample pixels at the centers of the configured regions.
            int sx0 = (int)(0.78 * w), sy0 = (int)(0.88 * h);
            int sx1 = (int)(0.51 * w), sy1 = (int)(0.48 * h);
            int i0 = (sy0 * w + sx0) * 4;
            int i1 = (sy1 * w + sx1) * 4;
            printf("[%4.1fs] %dx%d nonWhite=%ld avgB=%ld "
                   "status=(%d,%d,%d) reconnect=(%d,%d,%d)\n",
                   tick * 0.5, w, h, nonWhite, sumB / ((long)w * h),
                   bgra[i0 + 2], bgra[i0 + 1], bgra[i0],
                   bgra[i1 + 2], bgra[i1 + 1], bgra[i1]);
            captureFreeBitmap(bgra);
        }
        WaitForSingleObject(pi.hProcess, 500);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
