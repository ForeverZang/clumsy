// 60-second byte-rate ring plus its self-drawn IupCanvas sparkline.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "traffic.h"

#define NO_MARKER ((UINT64)-1)

#define TRAFFIC_USERDATA "__TRAFFIC_DATA"
#define COLOR_DOWN  RGB(0x2b, 0x7d, 0xe9)
#define COLOR_UP    RGB(0x6d, 0xaa, 0x2c)
#define COLOR_MARK  RGB(0xd9, 0x2b, 0x2b)
#define COLOR_GRID  RGB(0xdc, 0xdc, 0xdc)
#define COLOR_AXIS  RGB(0x80, 0x80, 0x80)
#define AXIS_LEFT   48   // rate labels + unit band
#define AXIS_BOTTOM 18   // timestamp band
#define PLOT_TOP    18   // room for the unit above the ceiling label
#define PLOT_RIGHT  8

typedef struct {
    UINT64 down;
    UINT64 up;
} TrafficBucket;

struct Traffic {
    TrafficBucket ring[TRAFFIC_BUCKETS];
    int      head;      // live bucket index
    UINT64   ticks;     // total tick count (marker age reference)
    UINT64   markTick;  // tick at which Start was pressed, NO_MARKER if none
};

// ---------------------------------------------------------------------------
// Ring statistics
// ---------------------------------------------------------------------------

Traffic *trafficNew(void) {
    Traffic *t = calloc(1, sizeof(Traffic));
    if (t == NULL) {
        return NULL;
    }
    t->markTick = NO_MARKER;
    return t;
}

void trafficDestroy(Traffic *t) {
    free(t);
}

void trafficReset(Traffic *t) {
    if (t == NULL) {
        return;
    }
    memset(t->ring, 0, sizeof(t->ring));
    t->head = 0;
    t->ticks = 0;
    t->markTick = NO_MARKER;
}

void trafficRecord(Traffic *t, UINT bytes, BOOL outbound) {
    if (t == NULL) {
        return;
    }
    if (outbound) {
        t->ring[t->head].up += bytes;
    } else {
        t->ring[t->head].down += bytes;
    }
}

void trafficTick(Traffic *t) {
    if (t == NULL) {
        return;
    }
    t->head = (t->head + 1) % TRAFFIC_BUCKETS;
    t->ring[t->head].down = 0;
    t->ring[t->head].up = 0;
    t->ticks++;
}

void trafficSeries(const Traffic *t, UINT64 down[TRAFFIC_BUCKETS],
                   UINT64 up[TRAFFIC_BUCKETS]) {
    if (t == NULL || down == NULL || up == NULL) {
        return;
    }
    for (int i = 0; i < TRAFFIC_BUCKETS; i++) {
        // Oldest retained bucket is the one right after the live bucket.
        int slot = (t->head + 1 + i) % TRAFFIC_BUCKETS;
        down[i] = t->ring[slot].down;
        up[i] = t->ring[slot].up;
    }
}

void trafficMarkStart(Traffic *t) {
    if (t != NULL) {
        t->markTick = t->ticks;
    }
}

BOOL trafficStartColumn(const Traffic *t, int *outColumn) {
    if (t == NULL || outColumn == NULL || t->markTick == NO_MARKER
        || t->markTick > t->ticks) {
        return FALSE;
    }
    UINT64 age = t->ticks - t->markTick;
    if (age >= TRAFFIC_BUCKETS) {
        return FALSE;
    }
    *outColumn = TRAFFIC_BUCKETS - 1 - (int)age;
    return TRUE;
}

void trafficFormatRate(UINT64 bytesPerSec, char *out, size_t cap) {
    static const UINT64 KILO = 1024;
    static const UINT64 MEGA = 1024 * 1024;
    if (out == NULL || cap == 0) {
        return;
    }
    if (bytesPerSec < KILO) {
        snprintf(out, cap, "%llu B/s", (unsigned long long)bytesPerSec);
    } else if (bytesPerSec < MEGA) {
        snprintf(out, cap, "%.1f KB/s", (double)bytesPerSec / KILO);
    } else {
        snprintf(out, cap, "%.1f MB/s", (double)bytesPerSec / MEGA);
    }
}

// Round up to the smallest 1/2/5 x 10^n that reaches v (v >= 1).
static UINT64 niceCeiling(UINT64 v) {
    UINT64 p = 1;
    if (v <= 1) {
        return 1;
    }
    while (p <= v / 10) {
        p *= 10;
    }
    if (v <= p) {
        return p;
    }
    if (v <= 2 * p) {
        return 2 * p;
    }
    if (v <= 5 * p) {
        return 5 * p;
    }
    return 10 * p;
}

void trafficAxisScale(UINT64 maxBytesPerSec, UINT64 *outDivisor,
                      const char **outUnit, UINT64 *outCeiling) {
    static const UINT64 KILO = 1024;
    static const UINT64 MEGA = 1024ull * 1024ull;
    UINT64 divisor;
    const char *unit;

    if (maxBytesPerSec < KILO) {
        divisor = 1;
        unit = "B/s";
    } else if (maxBytesPerSec < MEGA) {
        divisor = KILO;
        unit = "KB/s";
    } else {
        divisor = MEGA;
        unit = "MB/s";
    }
    if (outDivisor != NULL) {
        *outDivisor = divisor;
    }
    if (outUnit != NULL) {
        *outUnit = unit;
    }
    if (outCeiling != NULL) {
        *outCeiling = niceCeiling(maxBytesPerSec / divisor);
    }
}

void trafficFormatAxisValue(double value, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return;
    }
    if (value == 0.0) {
        snprintf(out, cap, "0");
    } else if (value < 10.0) {
        snprintf(out, cap, "%.1f", value);
    } else {
        snprintf(out, cap, "%.0f", value);
    }
}

void trafficTimeLabel(time_t when, char *out, size_t cap) {
    struct tm local;
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (localtime_s(&local, &when) == 0) {
        strftime(out, cap, "%H:%M:%S", &local);
    }
}

void trafficRecentAverage(const Traffic *t, int seconds,
                          UINT64 *outDown, UINT64 *outUp) {
    int available;
    int count;
    int slot;
    int i;
    UINT64 sumDown = 0;
    UINT64 sumUp = 0;

    if (outDown != NULL) {
        *outDown = 0;
    }
    if (outUp != NULL) {
        *outUp = 0;
    }
    if (t == NULL || outDown == NULL || outUp == NULL || seconds <= 0) {
        return;
    }
    // Only completed buckets that have actually lived count: the window
    // is clipped by both the requested seconds and the elapsed tick count,
    // and the live bucket at head is excluded.
    available = (int)(t->ticks < TRAFFIC_BUCKETS - 1
                          ? t->ticks
                          : TRAFFIC_BUCKETS - 1);
    count = seconds < available ? seconds : available;
    if (count == 0) {
        return;
    }
    slot = t->head;
    for (i = 0; i < count; i++) {
        slot = (slot - 1 + TRAFFIC_BUCKETS) % TRAFFIC_BUCKETS;
        sumDown += t->ring[slot].down;
        sumUp += t->ring[slot].up;
    }
    *outDown = sumDown / (UINT64)count;
    *outUp = sumUp / (UINT64)count;
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

// This IUP build reports 0 for WIDTH/HEIGHT on canvas controls; take the
// real client rectangle from the native window.
static BOOL canvasClientRect(Ihandle *self, RECT *rc) {
    HWND hwnd = (HWND)(UINT_PTR)IupGetAttribute(self, "HWND");
    if (hwnd == NULL || !GetClientRect(hwnd, rc)
        || rc->right <= 0 || rc->bottom <= 0) {
        return FALSE;
    }
    return TRUE;
}

static int pointX(int col, const RECT *plot) {
    return plot->left
           + col * (plot->right - plot->left) / (TRAFFIC_BUCKETS - 1);
}

static int pointY(UINT64 value, UINT64 maxValue, const RECT *plot) {
    int span = plot->bottom - plot->top;
    return plot->bottom - (int)(value * (UINT64)span / maxValue);
}

static void drawPolyline(HDC dc, const UINT64 *values, UINT64 maxValue,
                         const RECT *plot, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = SelectObject(dc, pen);
    MoveToEx(dc, pointX(0, plot),
             pointY(values[0], maxValue, plot), NULL);
    for (int i = 1; i < TRAFFIC_BUCKETS; i++) {
        LineTo(dc, pointX(i, plot), pointY(values[i], maxValue, plot));
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static void drawStartMarker(HDC dc, int column, const RECT *plot) {
    HPEN pen = CreatePen(PS_DASH, 1, COLOR_MARK);
    HPEN oldPen = SelectObject(dc, pen);
    int x = pointX(column, plot);
    MoveToEx(dc, x, plot->top, NULL);
    LineTo(dc, x, plot->bottom);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Tick columns whose wall-clock time is labelled below the axis:
// oldest, two interior, and live (right edge).
static const int AXIS_TICK_COLS[] = {0, 19, 39, 59};

static HFONT makeAxisFont(void) {
    return CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

static void drawAxisLabels(HDC dc, UINT64 ceiling, UINT64 divisor,
                           const char *unit, const RECT *plot) {
    // Three gridlines: 0, half ceiling, ceiling. A ceiling of 1 skips the
    // duplicate mid line (its half rounds to the same zero line).
    double levels[3] = {0.0, (double)ceiling / 2.0, (double)ceiling};
    static const int ALL_LEVELS[3] = {0, 1, 2};
    static const int TWO_LEVELS[2] = {0, 2};
    const int *levelIx = (ceiling < 2) ? TWO_LEVELS : ALL_LEVELS;
    int levelCount = (ceiling < 2) ? 2 : 3;
    int i;
    SetTextAlign(dc, TA_RIGHT | TA_BASELINE);
    for (i = 0; i < levelCount; i++) {
        double level = levels[levelIx[i]];
        UINT64 bytes = (UINT64)(level * (double)divisor + 0.5);
        int y = pointY(bytes, ceiling * divisor, plot);
        char num[24];
        HPEN pen = CreatePen(PS_SOLID, 1, COLOR_GRID);
        HFONT font = makeAxisFont();
        HFONT oldFont;
        HPEN oldPen;
        trafficFormatAxisValue(level, num, sizeof(num));
        oldPen = SelectObject(dc, pen);
        MoveToEx(dc, plot->left, y, NULL);
        LineTo(dc, plot->right, y);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        oldFont = SelectObject(dc, font);
        TextOutA(dc, plot->left - 5, y - 1, num, (int)strlen(num));
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    // The single shared unit, as an axis title above the plot's left
    // edge. It used to sit at the bottom of the left band and overprinted
    // the "0" gridline label.
    {
        HFONT font = makeAxisFont();
        HFONT oldFont = SelectObject(dc, font);
        SetTextAlign(dc, TA_LEFT | TA_TOP);
        TextOutA(dc, plot->left, 1, unit, (int)strlen(unit));
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
}

static void drawTimeLabels(HDC dc, time_t now, const RECT *plot) {
    HFONT font = makeAxisFont();
    HFONT oldFont = SelectObject(dc, font);
    size_t i;
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < sizeof(AXIS_TICK_COLS) / sizeof(AXIS_TICK_COLS[0]);
         i++) {
        int col = AXIS_TICK_COLS[i];
        int ageSec = TRAFFIC_BUCKETS - 1 - col;
        char stamp[16];
        int x = pointX(col, plot);
        UINT align;
        trafficTimeLabel(now - ageSec, stamp, sizeof(stamp));
        if (i == 0) {
            align = TA_LEFT;
        } else if (i == 3) {
            align = TA_RIGHT;
        } else {
            align = TA_CENTER;
        }
        SetTextAlign(dc, align | TA_TOP);
        TextOutA(dc, x, plot->bottom + 3, stamp, (int)strlen(stamp));
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

static void paintTraffic(const Traffic *t, HDC hdc, const RECT *client) {
    int width = client->right;
    int height = client->bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldBmp = SelectObject(mem, bmp);

    RECT full = {0, 0, width, height};
    FillRect(mem, &full, (HBRUSH)GetStockObject(WHITE_BRUSH));

    RECT plot = {AXIS_LEFT, PLOT_TOP, width - PLOT_RIGHT,
                 height - AXIS_BOTTOM};

    UINT64 down[TRAFFIC_BUCKETS], up[TRAFFIC_BUCKETS];
    trafficSeries(t, down, up);
    UINT64 rawMax = 1;
    for (int i = 0; i < TRAFFIC_BUCKETS; i++) {
        if (down[i] > rawMax) rawMax = down[i];
        if (up[i] > rawMax) rawMax = up[i];
    }
    UINT64 divisor = 1, ceiling = 1;
    const char *unit = "B/s";
    trafficAxisScale(rawMax, &divisor, &unit, &ceiling);
    UINT64 maxValue = ceiling * divisor;

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, COLOR_AXIS);
    drawAxisLabels(mem, ceiling, divisor, unit, &plot);

    if (rawMax > 1) {
        drawPolyline(mem, down, maxValue, &plot, COLOR_DOWN);
        drawPolyline(mem, up, maxValue, &plot, COLOR_UP);
    }

    int markerCol;
    if (trafficStartColumn(t, &markerCol)) {
        drawStartMarker(mem, markerCol, &plot);
    }

    drawTimeLabels(mem, time(NULL), &plot);

    BitBlt(hdc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static int trafficCanvasAction(Ihandle *self) {
    const Traffic *t =
        (const Traffic *)(UINT_PTR)IupGetAttribute(self, TRAFFIC_USERDATA);
    RECT client;
    if (!canvasClientRect(self, &client)) {
        return IUP_DEFAULT; // pre-map callback: nothing drawable yet
    }
    Traffic empty; // empty-state chart if the canvas has no ring attached
    if (t == NULL) {
        memset(&empty, 0, sizeof(empty));
        empty.markTick = NO_MARKER;
        t = &empty;
    }
    // IUP 3.30 does not expose a paint DC via PSHDC; take one ourselves.
    // IUP validates the WM_PAINT region, so this never re-triggers paint.
    HWND hwnd = (HWND)(UINT_PTR)IupGetAttribute(self, "HWND");
    HDC hdc = GetDC(hwnd);
    if (hdc == NULL) {
        return IUP_DEFAULT;
    }
    paintTraffic(t, hdc, &client);
    ReleaseDC(hwnd, hdc);
    return IUP_DEFAULT;
}

Ihandle *trafficCreateCanvas(Traffic *t) {
    Ihandle *canvas = IupCanvas(NULL);
    if (canvas != NULL) {
        IupSetAttribute(canvas, TRAFFIC_USERDATA, (char *)(UINT_PTR)t);
        IupSetCallback(canvas, "ACTION", (Icallback)trafficCanvasAction);
    }
    return canvas;
}
