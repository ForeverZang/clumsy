// Scripted end-to-end check for Task 10 Step 3 (not in run_tests.sh: it
// needs the fixture exe, the RapidOCR runtime and real model files).
//
// Launches tests/manual_ocr_fixture.exe --auto, starts a real
// ocrAppStart run against its window (PrintWindow capture + RapidOCR
// engine + real worker thread), pumps events on this thread while the
// fixture plays the script, then verifies the timeline CSV:
//   one  0 -> 95 change (rule:status-bar, 仓库 disappears)
//   one 95 -> 50 change (rule:reconnect, 重连 appears)
//   complete lifecycle rows and OCR text columns.
//
// Usage: manual_ocr_e2e [fixture.exe] [modelsDir]
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "ocr_app.h"
#include "ocr_config.h"

#define FIXTURE_CLASS "ClumsyOcrFixtureClass"
#define WAIT_LAUNCH_MS  5000
#define WAIT_EXIT_MS    20000

static int g_losses[32];
static int g_lossCount;
static void recordLoss(int percent, void *user) {
    (void)user;
    if (g_lossCount < 32) g_losses[g_lossCount++] = percent;
}

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

static char *readFile(const char *path, long *outLen) {
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    rewind(f);
    buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    *outLen = len;
    return buf;
}

static int countSubstr(const char *hay, const char *needle) {
    int n = 0;
    const char *p = hay;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
    return n;
}

static OcrConfig makeCfg(void) {
    OcrConfig c;
    memset(&c, 0, sizeof(c));
    strcpy(c.exeName, "manual_ocr_fixture.exe");
    strcpy(c.windowTitle, "OCR QA Fixture");
    c.intervalMs = 500;
    c.changeDetect = TRUE;
    c.enabled = TRUE;
    c.ruleCount = 2;

    strcpy(c.rules[0].name, "status-bar");
    strcpy(c.rules[0].keywords, "仓库");
    c.rules[0].edge = OCR_EDGE_DISAPPEAR;
    c.rules[0].confirmFrames = 2;
    c.rules[0].lossPercent = 95;
    c.rules[0].norm.x = 0.58;
    c.rules[0].norm.y = 0.78;
    c.rules[0].norm.w = 0.40;
    c.rules[0].norm.h = 0.20;

    strcpy(c.rules[1].name, "reconnect");
    strcpy(c.rules[1].keywords, "重连");
    c.rules[1].edge = OCR_EDGE_APPEAR;
    c.rules[1].confirmFrames = 2;
    c.rules[1].lossPercent = 50;
    c.rules[1].norm.x = 0.26;
    c.rules[1].norm.y = 0.34;
    c.rules[1].norm.w = 0.50;
    c.rules[1].norm.h = 0.30;
    return c;
}

int main(int argc, char **argv) {
    const char *fixtureExe = argc > 1 ? argv[1]
                                      : "build/tests/manual_ocr_fixture.exe";
    const char *modelsDir = argc > 2 ? argv[2]
                                    : "build/mingw64-debug/models";
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char err[512];
    OcrConfig cfg;
    OcrAppHooks hooks;
    HWND hwnd;
    long len;
    char *csv;
    DWORD waited;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    snprintf(cmd, sizeof(cmd), "\"%s\" --auto", fixtureExe);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &pi)) {
        printf("FAIL: cannot launch %s (err %lu)\n", fixtureExe,
               GetLastError());
        return 2;
    }

    hwnd = waitForWindow(WAIT_LAUNCH_MS);
    if (!hwnd) {
        printf("FAIL: fixture window never appeared\n");
        TerminateProcess(pi.hProcess, 1);
        return 2;
    }

    cfg = makeCfg();
    memset(&hooks, 0, sizeof(hooks));
    hooks.onLoss = recordLoss;
    {
        CaptureWindow picked;
        memset(&picked, 0, sizeof(picked));
        picked.hwnd = hwnd;
        if (!ocrAppStart(&cfg, &picked, "build/tests/e2e-runs",
                         modelsDir, 0, &hooks, err)) {
            printf("FAIL: ocrAppStart: %s\n", err);
            TerminateProcess(pi.hProcess, 1);
            return 2;
        }
    }

    // Pump queued events every 100 ms until the scripted fixture exits.
    waited = 0;
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        ocrAppPump();
        waited += 100;
        if (waited > WAIT_EXIT_MS) break;
    }
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ocrAppStop(); // final drain + session_stop + close CSV

    csv = readFile(ocrAppCsvPath(), &len);
    if (!csv) {
        printf("FAIL: timeline CSV missing or empty\n");
        return 1;
    }

    // Applier sequence: exactly one 95 then one 50, in that order.
    if (g_lossCount != 2 || g_losses[0] != 95 || g_losses[1] != 50) {
        printf("FAIL: loss sequence =");
        for (int i = 0; i < g_lossCount; i++) printf(" %d", g_losses[i]);
        printf(" (want 95 50)\n");
        return 1;
    }

    // Exact lifecycle/source columns (event,region,text,keyword,edge,
    // loss,source,note):
    // Lifecycle rows change no gear: the loss_percent column is empty
    // (timeline serializes the -1 sentinel as an empty field).
    const char *startRow = strstr(csv, "session_start,,,,,,system,");
    const char *stopRow = strstr(csv, "session_stop,,,,,,manual_stop,");
    const char *initRow = strstr(csv,
        "loss_change,,,,,0,initial_slider,");
    const char *r95 = strstr(csv, "loss_change,status-bar,,,disappear,95,rule:status-bar,");
    const char *r50 = strstr(csv, "loss_change,reconnect,,,appear,50,rule:reconnect,");
    if (!startRow || !stopRow || !initRow || !r95 || !r50) {
        printf("FAIL: lifecycle/rule rows incomplete:\n%s\n", csv);
        return 1;
    }
    // Each rule fires once: one trigger row and one gear row per region.
    // (Both rows carry loss+source, so anchor on the event name.)
    if (countSubstr(csv, "rule_trigger,status-bar,") != 1
        || countSubstr(csv, "loss_change,status-bar,") != 1
        || countSubstr(csv, "rule_trigger,reconnect,") != 1
        || countSubstr(csv, "loss_change,reconnect,") != 1) {
        printf("FAIL: rule rows not fired exactly once\n%s\n", csv);
        return 1;
    }
    // The appear trigger carries the recognized text "重连" (UTF-8).
    if (!strstr(csv, "rule_trigger,reconnect,\xE9\x87\x8D\xE8\xBF\x9E")) {
        printf("FAIL: reconnect trigger lacks OCR text\n%s\n", csv);
        return 1;
    }

    printf("OK: manual_ocr_e2e (%ld byte CSV, path %s)\n",
           len, ocrAppCsvPath());
    free(csv);
    return 0;
}
