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
    captureMakeThumbnail(bgra, 4, 4, 2, gray, &th, &thumbH);
    assert(th == 2 && thumbH == 2);
    assert(gray[0] == 10 && gray[1] == 20);
    assert(gray[2] == 30 && gray[3] == 40);

    // 14. Non-square source scales height proportionally.
    unsigned char bgra2[4 * 8 * 4];
    memset(bgra2, 0, sizeof(bgra2));
    captureMakeThumbnail(bgra2, 8, 4, 4, gray, &th, &thumbH);
    assert(th == 4 && thumbH == 2);

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

    printf("OK: ocr_capture\n");
    return 0;
}
