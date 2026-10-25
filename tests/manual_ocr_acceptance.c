// Task 12 headless acceptance (not in run_tests.sh: needs the fixture,
// the real RapidOCR runtime and model files). Throwaway after acceptance.
//
//   manual_ocr_acceptance fail  [modelsDir]  engine/0-match/ambiguous/
//                                             unique-resolve cases
//   manual_ocr_acceptance min   [modelsDir]  minimized -> unavailable,
//                                             no gear change
//   manual_ocr_acceptance soak  [modelsDir]  200 ms for 180 s, then five
//                                             Start/Stop cycles at 500 ms
//
// Launches build/tests/manual_ocr_fixture.exe (manual window, persistent).
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocr_app.h"
#include "ocr_config.h"

#define FIXTURE_CLASS "ClumsyOcrFixtureClass"
#define FIXTURE_TITLE_UTF8 "OCR QA Fixture 夹具"
#define FIXTURE_EXE "build\\tests\\manual_ocr_fixture.exe"
#define RUNS_DIR "build/tests/accept-runs"
#define EMPTY_MODELS "build/tests/empty-models"

struct Fixture {
    PROCESS_INFORMATION pi;
    HWND hwnd;
};

static int g_lossCount;
static int g_unavailable;
static int g_triggers;

static void onLoss(int percent, void *user) {
    (void)percent; (void)user;
    g_lossCount++;
}

static void onEvent(const OcrSessionEvent *ev, void *user) {
    (void)user;
    if (strcmp(ev->event, "capture_unavailable") == 0) g_unavailable++;
    if (strcmp(ev->event, "rule_trigger") == 0) g_triggers++;
}

static BOOL launchFixture(struct Fixture *f) {
    STARTUPINFOA si;
    char cmd[] = FIXTURE_EXE;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&f->pi, 0, sizeof(f->pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &f->pi)) {
        printf("FAIL: launch fixture err %lu\n", GetLastError());
        return FALSE;
    }
    return TRUE;
}

static void killFixture(struct Fixture *f) {
    if (f->pi.hProcess) {
        TerminateProcess(f->pi.hProcess, 1);
        WaitForSingleObject(f->pi.hProcess, 3000);
        CloseHandle(f->pi.hThread);
        CloseHandle(f->pi.hProcess);
        f->pi.hProcess = NULL;
    }
}

struct FindCtx { HWND *out; int cap; int n; };

static BOOL CALLBACK collectCb(HWND h, LPARAM lp) {
    struct FindCtx *c = (struct FindCtx *)lp;
    char cls[64];
    if (IsWindowVisible(h)
        && GetClassNameA(h, cls, sizeof(cls))
        && strcmp(cls, FIXTURE_CLASS) == 0 && c->n < c->cap) {
        c->out[c->n++] = h;
    }
    return TRUE;
}

static int fixtureWindows(HWND *out, int cap) {
    struct FindCtx c = { out, cap, 0 };
    EnumWindows(collectCb, (LPARAM)&c);
    return c.n;
}

static HWND waitFixtures(int want, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    HWND wins[8];
    for (;;) {
        if (fixtureWindows(wins, 8) == want)
            return want == 1 ? wins[0] : (HWND)(INT_PTR)1;
        if (GetTickCount() - start > timeoutMs) return NULL;
        Sleep(50);
    }
}

static int countRunCsv(void) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(RUNS_DIR "\\ocr-run-*.csv", &fd);
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do { n++; } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return n;
}

static void pumpMs(DWORD ms) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        ocrAppPump();
        Sleep(50);
    }
}

static OcrConfig makeCfg(int intervalMs) {
    OcrConfig c;
    memset(&c, 0, sizeof(c));
    strcpy(c.exeName, "manual_ocr_fixture.exe");
    strcpy(c.windowTitle, FIXTURE_TITLE_UTF8);
    c.intervalMs = intervalMs;
    c.changeDetect = TRUE;
    c.enabled = TRUE;
    c.ruleCount = 2;
    strcpy(c.rules[0].name, "status-bar");
    strcpy(c.rules[0].keywords, "仓库");
    c.rules[0].edge = OCR_EDGE_DISAPPEAR;
    c.rules[0].confirmFrames = 2;
    c.rules[0].lossPercent = 95;
    c.rules[0].norm = (NormRect){ 0.58f, 0.78f, 0.40f, 0.20f };
    strcpy(c.rules[1].name, "reconnect");
    strcpy(c.rules[1].keywords, "重连");
    c.rules[1].edge = OCR_EDGE_APPEAR;
    c.rules[1].confirmFrames = 2;
    c.rules[1].lossPercent = 50;
    c.rules[1].norm = (NormRect){ 0.26f, 0.34f, 0.50f, 0.30f };
    return c;
}

static OcrAppHooks makeHooks(void) {
    OcrAppHooks h;
    memset(&h, 0, sizeof(h));
    h.onLoss = onLoss;
    h.onSessionEvent = onEvent;
    return h;
}

static BOOL startPicked(HWND hwnd, const char *models, int intervalMs,
                        char *err) {
    OcrConfig cfg = makeCfg(intervalMs);
    CaptureWindow picked;
    OcrAppHooks hooks = makeHooks();
    memset(&picked, 0, sizeof(picked));
    picked.hwnd = hwnd;
    return ocrAppStart(&cfg, &picked, RUNS_DIR, models, 0, &hooks, err);
}

static BOOL startResolved(const char *models, char *err) {
    OcrConfig cfg = makeCfg(500);
    OcrAppHooks hooks = makeHooks();
    return ocrAppStart(&cfg, NULL, RUNS_DIR, models, 0, &hooks, err);
}

static int caseEngineFail(HWND hwnd, const char *models) {
    char err[512];
    int before = countRunCsv();
    CreateDirectoryA("build/tests/empty-models", NULL);
    if (startPicked(hwnd, EMPTY_MODELS, 500, err)) {
        printf("FAIL: engine-missing start succeeded\n");
        return 1;
    }
    if (strncmp(err, "engine:", 7) != 0) {
        printf("FAIL: engine-missing err='%s' (want engine:)\n", err);
        return 1;
    }
    if (countRunCsv() != before) {
        printf("FAIL: header-only CSV left after engine failure\n");
        return 1;
    }
    (void)models;
    printf("PASS: missing models -> inline engine error, no CSV left\n");
    return 0;
}

static int caseResolve(const char *models) {
    char err[512];
    int rc = 0;
    int before;

    before = countRunCsv();
    if (startResolved(models, err)
        || strncmp(err, "window:", 7) != 0
        || countRunCsv() != before) {
        printf("FAIL: 0-match start (err='%s')\n", err);
        return 1;
    }
    printf("PASS: saved title 0 matches -> rejected, no CSV\n");

    struct Fixture a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    if (!launchFixture(&a)) return 1;
    if (!launchFixture(&b)) { killFixture(&a); return 1; }
    if (waitFixtures(2, 5000) == NULL) {
        printf("FAIL: two fixtures never appeared\n");
        rc = 1; goto done2;
    }
    before = countRunCsv();
    if (startResolved(models, err)
        || strncmp(err, "window:", 7) != 0
        || countRunCsv() != before) {
        printf("FAIL: ambiguous start (err='%s')\n", err);
        rc = 1; goto done2;
    }
    printf("PASS: ambiguous title (2 windows) -> rejected, no CSV\n");
    killFixture(&b);
    if (waitFixtures(1, 5000) == NULL) {
        printf("FAIL: unique fixture never reappeared\n");
        rc = 1; goto done1;
    }
    if (!startResolved(models, err)) {
        printf("FAIL: unique-title re-resolve: %s\n", err);
        rc = 1; goto done1;
    }
    pumpMs(1000);
    ocrAppStop();
    // initial_slider is written straight to the timeline, so the gear
    // hook stays silent on a steady fixture; the new CSV is the proof.
    if (countRunCsv() != before + 1) {
        printf("FAIL: unique resolve run (csv delta=%d)\n",
               countRunCsv() - before);
        rc = 1;
    } else {
        printf("PASS: unique saved title -> re-resolved and run recorded\n");
    }
done1:
    killFixture(&a);
done2:
    if (b.pi.hProcess) killFixture(&b);
    return rc;
}

static int caseMinimized(const char *models) {
    struct Fixture f;
    char err[512];
    int rc = 1;
    if (!launchFixture(&f)) return 1;
    f.hwnd = waitFixtures(1, 5000);
    if (!f.hwnd) { printf("FAIL: fixture missing\n"); goto done; }

    if (!startPicked(f.hwnd, models, 500, err)) {
        printf("FAIL: start: %s\n", err);
        goto done;
    }
    pumpMs(1500);
    ShowWindow(f.hwnd, SW_MINIMIZE);
    pumpMs(7000); // first unavailable immediate, 5 s rate-limit allows 2
    ShowWindow(f.hwnd, SW_RESTORE);
    pumpMs(1000);
    ocrAppStop();
    if (g_lossCount != 0 || g_triggers != 0 || g_unavailable < 1) {
        printf("FAIL: minimized losses=%d triggers=%d unavailable=%d\n",
               g_lossCount, g_triggers, g_unavailable);
        goto done;
    }
    printf("PASS: minimized -> %d unavailable event(s), no gear change\n",
           g_unavailable);
    rc = 0;
done:
    killFixture(&f);
    return rc;
}

static DWORD handleCount(void) {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

static DWORD threadCount(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    DWORD n = 0;
    if (s == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof(te);
    if (Thread32First(s, &te)) {
        do { if (te.th32OwnerProcessID == pid) n++; }
        while (Thread32Next(s, &te));
    }
    CloseHandle(s);
    return n;
}

static ULONGLONG ftToU64(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return u.QuadPart;
}

static int caseSoak(const char *models) {
    struct Fixture f;
    char err[512];
    DWORD baseHandles, baseThreads, t0;
    FILETIME ct0, et0, kt0, ut0, cte, ete, kte, ute;
    int i;
    if (!launchFixture(&f)) return 1;
    f.hwnd = waitFixtures(1, 5000);
    if (!f.hwnd) { printf("FAIL: fixture missing\n"); killFixture(&f); return 1; }

    if (!startPicked(f.hwnd, models, 200, err)) {
        printf("FAIL: soak start: %s\n", err);
        killFixture(&f);
        return 1;
    }
    baseThreads = threadCount();
    baseHandles = handleCount();
    GetProcessTimes(GetCurrentProcess(), &ct0, &et0, &kt0, &ut0);
    printf("soak baseline handles=%lu threads=%lu\n",
           baseHandles, baseThreads);
    t0 = GetTickCount();
    for (i = 1; i <= 6; i++) {
        pumpMs(30000);
        printf("  %3lus handles=%lu (d%ld) threads=%lu (d%ld)\n",
               (GetTickCount() - t0) / 1000, handleCount(),
               (long)(handleCount() - baseHandles), threadCount(),
               (long)(threadCount() - baseThreads));
    }
    ocrAppStop();
    GetProcessTimes(GetCurrentProcess(), &cte, &ete, &kte, &ute);
    {
        ULONGLONG cpu100ns = (ftToU64(&kte) + ftToU64(&ute))
                           - (ftToU64(&kt0) + ftToU64(&ut0));
        printf("soak CPU time over 180 s: %.1f s (%.1f%% of one core)\n",
               cpu100ns / 1e7, cpu100ns / 1e7 / 180.0 * 100.0);
    }

    // Five Start/Stop cycles at 500 ms against the same persistent window.
    for (i = 0; i < 5; i++) {
        if (!startPicked(f.hwnd, models, 500, err)) {
            printf("FAIL: cycle %d start: %s\n", i + 1, err);
            killFixture(&f);
            return 1;
        }
        pumpMs(3000);
        ocrAppStop();
    }
    Sleep(500);
    {
        long dh = (long)(handleCount() - baseHandles);
        long dt = (long)(threadCount() - baseThreads);
        printf("post-cycles handle delta=%ld thread delta=%ld\n", dh, dt);
        if (dh > 2 || dt > 2) {
            printf("FAIL: handles/threads accumulated\n");
            killFixture(&f);
            return 1;
        }
    }
    printf("PASS: soak 200 ms x180 s + 5 cycles, no accumulation\n");
    killFixture(&f);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "fail";
    const char *models = argc > 2 ? argv[2]
                                  : "build/mingw64-debug/models";
    int rc;
    CreateDirectoryA("build/tests", NULL);
    CreateDirectoryA(RUNS_DIR, NULL);
    struct Fixture f;

    if (strcmp(mode, "min") == 0) return caseMinimized(models);
    if (strcmp(mode, "soak") == 0) return caseSoak(models);

    if (!launchFixture(&f)) return 1;
    f.hwnd = waitFixtures(1, 5000);
    if (!f.hwnd) { printf("FAIL: fixture missing\n"); return 1; }
    rc = caseEngineFail(f.hwnd, models);
    killFixture(&f);
    if (waitFixtures(0, 5000) == NULL) {
        printf("FAIL: fixture did not exit\n");
        return 1;
    }
    if (rc == 0) rc = caseResolve(models);
    printf(rc == 0 ? "OK: manual_ocr_acceptance fail\n"
                   : "FAIL: manual_ocr_acceptance fail\n");
    return rc;
}
