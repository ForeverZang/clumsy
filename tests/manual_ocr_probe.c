// Diagnostic probe (manual): capture + REAL OCR of the two fixture
// regions every 500 ms and print the recognized text verbatim.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ocr_capture.h"
#include "ocr_engine.h"

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

static void readRegion(OcrEngine *eng, HWND hwnd, int w, int h,
                       double nx, double ny, double nw, double nh,
                       const char *tag) {
    RECT rc;
    unsigned char *bgra = NULL;
    int cw = 0, ch = 0;
    char text[512], err[512];
    NormRect n = { nx, ny, nw, nh };
    rc = captureMapNorm(w, h, &n);
    if (!captureClientRegion(hwnd, &rc, &bgra, &cw, &ch, err)) {
        printf("  %s: capture fail %s\n", tag, err);
        return;
    }
    text[0] = '\0';
    if (!ocrEngineRead(eng, bgra, cw, ch, text, sizeof(text), err)) {
        printf("  %s: read fail %s\n", tag, err);
    } else {
        printf("  %s: [%s] (%dx%d)\n", tag, text, cw, ch);
    }
    captureFreeBitmap(bgra);
}

int main(int argc, char **argv) {
    const char *fixtureExe = argc > 1 ? argv[1]
                                      : "build/tests/manual_ocr_fixture.exe";
    const char *modelsDir = argc > 2 ? argv[2]
                                    : "build/mingw64-debug/models";
    char cmd[1024], err[512];
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

    OcrEngine *eng = ocrEngineCreate(modelsDir, err);
    if (!eng) { printf("engine: %s\n", err); return 2; }

    RECT cr;
    GetClientRect(hwnd, &cr);
    for (int tick = 0; tick < 22; tick++) {
        printf("[%4.1fs]\n", tick * 0.5);
        readRegion(eng, hwnd, cr.right, cr.bottom,
                   0.58, 0.78, 0.40, 0.20, "status");
        readRegion(eng, hwnd, cr.right, cr.bottom,
                   0.26, 0.34, 0.50, 0.30, "reconnect");
        if (WaitForSingleObject(pi.hProcess, 500) != WAIT_TIMEOUT) break;
    }
    ocrEngineDestroy(eng);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
