// Unit tests for capture-side pure logic: normalized rect mapping,
// saved-title match levels and unique resolution, grayscale thumbnail
// averaging and mean-absolute-difference change detection.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "ocr_capture.h"

static CaptureWindow makeCw(HWND hw, const char *title) {
    CaptureWindow c;
    memset(&c, 0, sizeof(c));
    c.hwnd = hw;
    c.pid = (DWORD)(uintptr_t)hw;
    strncpy(c.title, title, sizeof(c.title) - 1);
    c.width = 800;
    c.height = 600;
    return c;
}

// A real top-level window owned by a dedicated UI thread. It paints flat
// gray. While g_hangPrint is set, a posted parking message blocks the
// thread inside its window procedure -- exactly the state of an impaired
// target application whose UI thread stopped pumping messages. Probe-
// verified: PrintWindow then blocks indefinitely cross-thread.
static HANDLE g_releasePrint;
static HANDLE g_parkedEvent;
static volatile LONG g_hangPrint;
static volatile LONG g_timedParkMs;
#define UI_PARK_MSG (WM_APP + 1)

struct targetThreadArg {
    HANDLE ready;
    HWND hwnd;
};

static void paintGray(HDC dc, HWND hwnd) {
    RECT rc;
    HBRUSH brush = CreateSolidBrush(RGB(200, 200, 200));
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

static LRESULT CALLBACK hungTargetWndProc(HWND hwnd, UINT msg,
                                          WPARAM wp, LPARAM lp) {
    switch (msg) {
    case UI_PARK_MSG:
        SetEvent(g_parkedEvent);
        if (g_hangPrint) {
            // Park until released: a fully non-pumping UI thread.
            WaitForSingleObject(g_releasePrint, INFINITE);
        } else if (g_timedParkMs > 0) {
            // Timed park: the thread comes back just inside PrintWindow's
            // 1000 ms bounded wait, stressing the handshake boundary.
            Sleep((DWORD)g_timedParkMs);
        }
        return 0;
    case WM_PRINTCLIENT:
        paintGray((HDC)wp, hwnd);
        return 0;
    case WM_PRINT:
        paintGray((HDC)wp, hwnd);
        return 0;
    case WM_ERASEBKGND:
        paintGray((HDC)wp, hwnd);
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paintGray(ps.hdc, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// Stop the target's message pump: the queued parking message is
// dispatched once and then blocks the UI thread until g_releasePrint.
// Waits on the wndproc's own "parked" signal instead of a blind sleep so
// the capture call is guaranteed to meet a non-pumping thread.
static void parkTargetThread(HWND hwnd) {
    ResetEvent(g_parkedEvent);
    PostMessageW(hwnd, UI_PARK_MSG, 0, 0);
    assert(WaitForSingleObject(g_parkedEvent, 5000) == WAIT_OBJECT_0);
    Sleep(20); // let the parked thread settle fully inside its wait
}

static DWORD WINAPI targetUiThreadMain(LPVOID arg) {
    struct targetThreadArg *a = (struct targetThreadArg *)arg;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = hungTargetWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ClumsyCaptureHungTarget";
    RegisterClassW(&wc);
    // Ordinary DWM-composited top-level window: the probe proved
    // PrintWindow(PW_RENDERFULLCONTENT) still blocks forever once this
    // thread stops pumping, even though the DWM thumbnail stays current.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"target",
                                WS_OVERLAPPEDWINDOW,
                                40, 40, 240, 240,
                                NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd); // force the first real paint before DWM composes
    a->hwnd = hwnd;
    SetEvent(a->ready);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

int main(void) {
    // --- captureMapNorm ----------------------------------------------

    // 1. Proportions map to pixels within tolerance 1.
    NormRect r1 = { 0.6, 0.85, 0.35, 0.12 };
    RECT p1 = captureMapNorm(1000, 800, &r1);
    assert(abs(p1.left - 600) <= 1);
    assert(abs(p1.top - 680) <= 1);
    assert(abs(p1.right - 950) <= 1);
    assert(abs(p1.bottom - 776) <= 1);

    // 2. Same normalized rect on 150% physical pixels keeps proportions.
    RECT p1d = captureMapNorm(1500, 1200, &r1);
    assert(abs(p1d.left - 900) <= 1);
    assert(abs(p1d.top - 1020) <= 1);
    assert(abs(p1d.right - 1425) <= 1);
    assert(abs(p1d.bottom - 1164) <= 1);

    // 3. Out-of-range proportions clamp inside the client area.
    NormRect r2 = { -0.5, -0.25, 1.8, 2.0 };
    RECT p2 = captureMapNorm(1000, 800, &r2);
    assert(p2.left == 0 && p2.top == 0);
    assert(p2.right == 1000 && p2.bottom == 800);

    // 4. A rect starting in-range but overflowing the edge clamps.
    NormRect r3 = { 0.9, 0.9, 0.5, 0.5 };
    RECT p3 = captureMapNorm(1000, 800, &r3);
    assert(abs(p3.left - 900) <= 1 && abs(p3.top - 720) <= 1);
    assert(p3.right == 1000 && p3.bottom == 800);

    // --- captureTitleMatchLevel --------------------------------------

    // 5. Substring, exact, miss, empty/NULL.
    assert(captureTitleMatchLevel("记事本", "无标题 - 记事本") == 1);
    assert(captureTitleMatchLevel("记事本", "记事本") == 2);
    assert(captureTitleMatchLevel("记事本", "chrome") == 0);
    assert(captureTitleMatchLevel("", "记事本") == 0);
    assert(captureTitleMatchLevel("记事本", "") == 0);
    assert(captureTitleMatchLevel(NULL, "记事本") == 0);
    assert(captureTitleMatchLevel("记事本", NULL) == 0);

    // 6. Exact is trim + case insensitive; substring is too.
    assert(captureTitleMatchLevel("  notepad ", "NOTEPAD") == 2);
    assert(captureTitleMatchLevel("notepad", "x - NOTEPAD") == 1);

    // --- captureResolveWindow ----------------------------------------

    CaptureWindow out;
    int count = -1;

    // 7. No candidates.
    assert(!captureResolveWindow(NULL, 0, "记事本", &out, &count));
    assert(count == 0);

    // 8. Exact wins over any number of substring matches.
    CaptureWindow many[3];
    many[0] = makeCw((HWND)(uintptr_t)1, "无标题 - 记事本");
    many[1] = makeCw((HWND)(uintptr_t)2, "记事本");
    many[2] = makeCw((HWND)(uintptr_t)3, "记事本 - 另一个");
    memset(&out, 0, sizeof(out));
    assert(captureResolveWindow(many, 3, "记事本", &out, &count));
    assert(count == 1);
    assert((uintptr_t)out.hwnd == 2);

    // 9. Two substring-only matches are ambiguous; out is not written.
    CaptureWindow subs[2];
    subs[0] = makeCw((HWND)(uintptr_t)10, "A - 记事本");
    subs[1] = makeCw((HWND)(uintptr_t)11, "B - 记事本");
    out.hwnd = (HWND)(uintptr_t)0xDEAD;
    assert(!captureResolveWindow(subs, 2, "记事本", &out, &count));
    assert(count == 2);
    assert((uintptr_t)out.hwnd == 0xDEAD);

    // 10. Single substring resolves.
    assert(captureResolveWindow(subs, 1, "记事本", &out, &count));
    assert(count == 1);
    assert((uintptr_t)out.hwnd == 10);

    // 11. Two exact matches (two same-titled windows) are ambiguous.
    CaptureWindow dupes[2];
    dupes[0] = makeCw((HWND)(uintptr_t)20, "记事本");
    dupes[1] = makeCw((HWND)(uintptr_t)21, "记事本");
    assert(!captureResolveWindow(dupes, 2, "记事本", &out, &count));
    assert(count == 2);

    // 12. Nothing matches.
    CaptureWindow nomatch[2];
    nomatch[0] = makeCw((HWND)(uintptr_t)30, "chrome");
    nomatch[1] = makeCw((HWND)(uintptr_t)31, "explorer");
    assert(!captureResolveWindow(nomatch, 2, "记事本", &out, &count));
    assert(count == 0);

    // --- captureMakeThumbnail ----------------------------------------

    // 13. 4x4 BGRA with four gray quadrants -> 2x2 block-average thumb.
    unsigned char bgra[4 * 4 * 4];
    static const unsigned char quad[4] = { 10, 20, 30, 40 };
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int qi = (y / 2) * 2 + (x / 2);
            unsigned char g = quad[qi];
            unsigned char *px = &bgra[(y * 4 + x) * 4];
            px[0] = g; px[1] = g; px[2] = g; px[3] = 255;
        }
    }
    unsigned char gray[64];
    int th = 0, thumbH = 0;
    captureMakeThumbnail(bgra, 4, 4, 2, 32, gray, &th, &thumbH);
    assert(th == 2 && thumbH == 2);
    assert(gray[0] == 10 && gray[1] == 20);
    assert(gray[2] == 30 && gray[3] == 40);

    // 14. Non-square source scales height proportionally.
    unsigned char bgra2[4 * 8 * 4];
    memset(bgra2, 0, sizeof(bgra2));
    captureMakeThumbnail(bgra2, 8, 4, 4, 32, gray, &th, &thumbH);
    assert(th == 4 && thumbH == 2);

    // 19. Rounding can report one row over the proportional target; the
    // caller's maxThumbH cap must hold or the 1024-byte thumb buffers
    // overflow (32x65 with thumbW 16 would report th=33 uncapped).
    unsigned char *bgra3 = malloc((size_t)32 * 65 * 4);
    assert(bgra3);
    memset(bgra3, 40, (size_t)32 * 65 * 4);
    int tw3 = 0, th3 = 0;
    captureMakeThumbnail(bgra3, 32, 65, 16, 32, gray, &tw3, &th3);
    assert(tw3 == 16);
    assert(th3 >= 1 && th3 <= 32);
    free(bgra3);

    // 20. Extreme aspect ratio from a hand-edited saved config
    // (norm 0,0,0.001,1 on a narrow client -> 1x1920 crop): the reported
    // height stays inside the fixed 32x32 thumbnail buffer.
    unsigned char *bgraTall = malloc((size_t)1 * 1920 * 4);
    assert(bgraTall);
    memset(bgraTall, 0, (size_t)1 * 1920 * 4);
    unsigned char grayCap[32 * 32];
    int twT = 0, thT = 0;
    captureMakeThumbnail(bgraTall, 1, 1920, 1, 32, grayCap, &twT, &thT);
    assert(twT == 1);
    assert(thT >= 1 && thT <= 32);
    free(bgraTall);

    // --- captureChanged ----------------------------------------------

    unsigned char a[4] = { 100, 100, 100, 100 };
    unsigned char bSame[4] = { 100, 100, 100, 100 };

    // 15. Identical frames never change.
    assert(!captureChanged(a, bSame, 4, 0));
    assert(!captureChanged(a, bSame, 4, 5));

    // 16. Mean abs difference must EXCEED the threshold.
    unsigned char bEdge[4] = { 105, 100, 100, 100 }; // MAD 1.25
    assert(!captureChanged(a, bEdge, 4, 2));  // below
    assert(captureChanged(a, bEdge, 4, 1));   // above

    // 17. Every pixel at exactly the threshold is still "not changed".
    unsigned char bAll[4] = { 105, 105, 105, 105 }; // MAD exactly 5
    assert(!captureChanged(a, bAll, 4, 5));
    assert(captureChanged(a, bAll, 4, 4));

    // 18. One large change against 3 steady pixels trips detection.
    unsigned char bBig[4] = { 200, 100, 100, 100 }; // MAD 25
    assert(captureChanged(a, bBig, 4, 5));

    // --- captureFrameBlank -------------------------------------------

    // 21. All-zero frame (PrintWindow on a locked/exclusive-fullscreen
    // window) carries no picture and must be treated as unavailable.
    size_t px64 = 8u * 8u;
    unsigned char *black = malloc(px64 * 4u);
    assert(black);
    memset(black, 0, px64 * 4u);
    assert(captureFrameBlank(black, 8, 8));

    // 22. A single bright pixel breaks uniformity: real content present.
    black[3 * 8 * 4 + 2] = 255;
    assert(!captureFrameBlank(black, 8, 8));
    free(black);

    // 23. Uniform near-black below the gray floor is blank; a uniform
    // dark-theme canvas (#1e1e1e ~ gray 30) is a real picture.
    unsigned char *nearBlack = malloc(px64 * 4u);
    assert(nearBlack);
    memset(nearBlack, 8, px64 * 4u);
    assert(captureFrameBlank(nearBlack, 8, 8));
    unsigned char *darkTheme = malloc(px64 * 4u);
    assert(darkTheme);
    memset(darkTheme, 30, px64 * 4u);
    assert(!captureFrameBlank(darkTheme, 8, 8));
    free(nearBlack);
    free(darkTheme);

    // 24. NULL/empty input is blank (fail closed), a normal gradient is
    // not.
    assert(captureFrameBlank(NULL, 8, 8));
    unsigned char *grad = malloc(px64 * 4u);
    assert(grad);
    for (size_t i = 0; i < px64; i++) memset(grad + i * 4u, (int)(i % 64u), 4u);
    assert(!captureFrameBlank(grad, 8, 8));
    free(grad);

    // --- captureTargetAlive ------------------------------------------

    // 25. The identity gate fails closed for NULL, an unpinned window,
    // or a dead/recycled sentinel handle.
    assert(!captureTargetAlive(NULL));
    CaptureWindow unpinned;
    memset(&unpinned, 0, sizeof(unpinned));
    unpinned.hwnd = (HWND)(uintptr_t)1;
    assert(!captureTargetAlive(&unpinned)); // pid 0: cannot prove identity
    CaptureWindow dead;
    memset(&dead, 0, sizeof(dead));
    dead.hwnd = (HWND)(uintptr_t)1;
    dead.pid = 1234;
    assert(!captureTargetAlive(&dead)); // IsWindow((HWND)1) is false

    // --- bounded PrintWindow against a hung target -------------------
    // Root cause of "Stop never re-enables the config widgets":
    // PrintWindow cross-thread-sends into the target UI thread; while
    // clumsy's own impairment has that thread blocked, the worker sat in
    // PrintWindow forever and Stop joined it (INFINITE) on the GUI thread.
    // captureClientRegion must give up bounded, on every tick.

    // 26. Positive control: a responsive window still captures a real
    // (non-blank) frame through the bounded helper path.
    g_releasePrint = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_parkedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(g_parkedEvent != NULL);
    struct targetThreadArg targ;
    memset(&targ, 0, sizeof(targ));
    targ.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE uiThread = CreateThread(NULL, 0, targetUiThreadMain, &targ,
                                   0, NULL);
    assert(uiThread != NULL);
    assert(WaitForSingleObject(targ.ready, 5000) == WAIT_OBJECT_0);
    assert(targ.hwnd != NULL);
    Sleep(400); // let the DWM composite the gray client area (probe-verified)

    DWORD targPid = 0;
    GetWindowThreadProcessId(targ.hwnd, &targPid);
    assert(targPid != 0);

    RECT sub = { 10, 10, 110, 110 };
    unsigned char *frame = NULL;
    int fw = 0, fh = 0;
    char capErr[MSG_BUFSIZE];
    InterlockedExchange(&g_hangPrint, 0);
    DWORD t0 = GetTickCount64();
    BOOL capOk = captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh,
                                     capErr);
    DWORD fastMs = (DWORD)(GetTickCount64() - t0);
    assert(capOk);
    assert(frame != NULL && fw == 100 && fh == 100);
    assert(!captureFrameBlank(frame, fw, fh));
    captureFreeBitmap(frame);
    assert(fastMs < 5000);

    // 27. A target whose WM_PRINTCLIENT never returns must not block the
    // caller past the bounded wait (leaves ample slack over the 1s cap).
    InterlockedExchange(&g_hangPrint, 1);
    parkTargetThread(targ.hwnd);
    frame = NULL;
    t0 = GetTickCount64();
    capOk = captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh, capErr);
    DWORD hungMs = (DWORD)(GetTickCount64() - t0);
    assert(!capOk);
    assert(frame == NULL);
    assert(hungMs < 5000);

    // 28. A second stuck capture fills the small pending allowance; a
    // third fails fast instead of stacking threads on a dead target.
    frame = NULL;
    t0 = GetTickCount64();
    capOk = captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh, capErr);
    DWORD secondMs = (DWORD)(GetTickCount64() - t0);
    assert(!capOk && frame == NULL);
    assert(secondMs < 5000);

    frame = NULL;
    t0 = GetTickCount64();
    capOk = captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh, capErr);
    DWORD thirdMs = (DWORD)(GetTickCount64() - t0);
    assert(!capOk && frame == NULL);
    assert(thirdMs < 500); // gate rejects before another timed wait

    // 29. Once the target thread pumps again, the abandoned helpers drain,
    // pending slots return and captures succeed (no late GDI/stack touch).
    InterlockedExchange(&g_hangPrint, 0);
    SetEvent(g_releasePrint);
    for (int i = 0; i < 50; i++) {
        Sleep(100);
        frame = NULL;
        if (captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh, capErr))
            break;
    }
    assert(frame != NULL);
    assert(!captureFrameBlank(frame, fw, fh));
    captureFreeBitmap(frame);

    // 30. Handshake stress: hammer rapid captures on a responsive target.
    // A double-owner handshake races every iteration (helper signals done
    // before it learns who owns the GDI objects); the single-owner
    // handshake must survive this without a double free, leaked slot or
    // corrupted frame.
    InterlockedExchange(&g_hangPrint, 0);
    for (int i = 0; i < 100; i++) {
        frame = NULL;
        BOOL ok = captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh,
                                      capErr);
        assert(ok);
        assert(frame != NULL && fw == 100 && fh == 100);
        assert(!captureFrameBlank(frame, fw, fh));
        captureFreeBitmap(frame);
        SwitchToThread();
    }

    // 31a. Near-boundary completion on the success side: with a 2000 ms
    // deadline, a 900 ms UI-thread park means PrintWindow completes with
    // the helper parked right at the wait boundary. The capture must
    // succeed with the pixels intact (no racy GDI ownership handoff).
    // Deadline is injected via the test seam: a 900/1000 wall-clock race
    // would flake under scheduler/AV jitter and prove nothing.
    captureSetPrintTimeoutForTest(2000);
    InterlockedExchange(&g_timedParkMs, 900);
    parkTargetThread(targ.hwnd);
    frame = NULL;
    t0 = GetTickCount64();
    capOk = captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh, capErr);
    DWORD nearMs = (DWORD)(GetTickCount64() - t0);
    InterlockedExchange(&g_timedParkMs, 0);
    assert(capOk);
    assert(frame != NULL && !captureFrameBlank(frame, fw, fh));
    assert(nearMs >= 800 && nearMs < 2000);
    captureFreeBitmap(frame);

    // 31b. Timeout side of the same boundary: a 300 ms deadline against a
    // 700 ms park is a guaranteed timeout. The abandoned helper must drain
    // on its own once PrintWindow lands, hand back the pending slot, and a
    // later capture must succeed -- no stranded GDI objects or stuck slots.
    captureSetPrintTimeoutForTest(300);
    InterlockedExchange(&g_timedParkMs, 700);
    parkTargetThread(targ.hwnd);
    frame = NULL;
    t0 = GetTickCount64();
    capOk = captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh, capErr);
    DWORD overMs = (DWORD)(GetTickCount64() - t0);
    InterlockedExchange(&g_timedParkMs, 0);
    assert(!capOk && frame == NULL);
    assert(overMs < 1500); // bounded wait, never the 700 ms park itself
    frame = NULL;
    for (int i = 0; i < 50; i++) {
        Sleep(100);
        if (captureClientRegion(targ.hwnd, targPid, &sub, &frame, &fw, &fh, capErr))
            break;
        frame = NULL;
    }
    assert(frame != NULL);
    assert(!captureFrameBlank(frame, fw, fh));
    captureFreeBitmap(frame);
    captureSetPrintTimeoutForTest(0); // restore production default

    // 32. Owner pinning at the capture boundary: even a perfectly
    // responsive window must be refused when its live owner PID is not
    // the pinned one (the HWND died and its handle was recycled onto
    // another process between the per-tick gate and PrintWindow). The
    // refusal must complete normally, never strand a pending slot (100
    // rapid refusals followed by a good capture), and expectedPid 0
    // remains the explicit "caller does not pin" opt-out.
    {
        DWORD wrongPid = targPid ^ 0x55AA55AAu; // never a valid PID (not
                                               // divisible by 4, != 0)
        assert(wrongPid != 0 && wrongPid != targPid);
        for (int i = 0; i < 100; i++) {
            frame = NULL;
            BOOL refused = captureClientRegion(targ.hwnd, wrongPid, &sub,
                                               &frame, &fw, &fh, capErr);
            assert(!refused && frame == NULL);
        }
        frame = NULL;
        assert(captureClientRegion(targ.hwnd, targPid, &sub,
                                   &frame, &fw, &fh, capErr));
        assert(frame != NULL && !captureFrameBlank(frame, fw, fh));
        captureFreeBitmap(frame);

        frame = NULL;
        assert(captureClientRegion(targ.hwnd, 0, &sub,
                                   &frame, &fw, &fh, capErr));
        assert(frame != NULL && !captureFrameBlank(frame, fw, fh));
        captureFreeBitmap(frame);
    }

    PostThreadMessageW(GetThreadId(uiThread), WM_QUIT, 0, 0);
    assert(WaitForSingleObject(uiThread, 5000) == WAIT_OBJECT_0);
    CloseHandle(uiThread);
    CloseHandle(targ.ready);
    CloseHandle(g_releasePrint);
    CloseHandle(g_parkedEvent);

    printf("OK: ocr_capture\n");
    return 0;
}
