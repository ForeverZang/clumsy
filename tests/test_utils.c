// Pure geometry helper: keep a rectangle at its current top-left but pull
// it back inside the monitor work area when a dialog refit or the picker
// toolbar placement would overflow it.
#include <windows.h>
#include <assert.h>
#include "common.h"

static RECT rc(long l, long t, long r, long b) {
    RECT x;
    x.left = l; x.top = t; x.right = r; x.bottom = b;
    return x;
}

static void expect(int line, POINT got, long x, long y) {
    if (got.x != x || got.y != y) {
        printf("FAIL line %d: got (%ld,%ld), want (%ld,%ld)\n",
               line, got.x, got.y, x, y);
        assert(0);
    }
}

#define EXPECT(p, x, y) expect(__LINE__, p, x, y)

int main(void) {
    RECT work = rc(0, 0, 1000, 800);
    POINT p;

    // Fits and already inside: the whole point of the helper is that the
    // current position is preserved (no recenter on dialog refit).
    clampIntoWorkRect(100, 100, 400, 300, &work, &p);
    EXPECT(p, 100, 100);

    // Overflow right/bottom: pull back just enough to fit.
    clampIntoWorkRect(900, 700, 400, 300, &work, &p);
    EXPECT(p, 600, 500);

    // Overflow left/top: clamp to the work area origin.
    clampIntoWorkRect(-50, -80, 400, 300, &work, &p);
    EXPECT(p, 0, 0);

    // Larger than the work area on an axis: align at the leading edge.
    clampIntoWorkRect(300, 200, 1200, 900, &work, &p);
    EXPECT(p, 0, 0);

    // Non-zero work-area origin (multi-monitor): right overflow pulls
    // back minimally inside the negative-origin coordinate space (the
    // bar keeps as much of its desired position as possible; pinning it
    // to -1920 would be exactly the forced recenter users must not get).
    work = rc(-1920, 0, 0, 1040);
    clampIntoWorkRect(-20, 900, 600, 200, &work, &p);
    EXPECT(p, -600, 840);

    // Picker toolbar at 150% DPI: 720x70, horizontally centered, 12px off
    // the bottom edge. Positioned by the caller, helper must leave it
    // fully inside the work area (the 46px-tall forced size used to
    // overflow under the taskbar).
    work = rc(0, 0, 1920, 1040);
    clampIntoWorkRect((1920 - 720) / 2, 1040 - 70 - 12, 720, 70, &work, &p);
    EXPECT(p, 600, 958);
    assert(p.x + 720 <= 1920);
    assert(p.y + 70 <= 1040);

    // Bar wider than the work area (very narrow monitor): align leading
    // edges instead of producing a negative origin.
    clampIntoWorkRect(-10, 0, 2000, 46, &work, &p);
    EXPECT(p, 0, 0);

    printf("OK: utils\n");
    return 0;
}
