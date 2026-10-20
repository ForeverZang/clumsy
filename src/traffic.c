// 60-second byte-rate ring plus its self-drawn IupCanvas sparkline.
#include <stdlib.h>
#include <string.h>

#include "traffic.h"

#define NO_MARKER ((UINT64)-1)

#define TRAFFIC_USERDATA "__TRAFFIC_DATA"
#define COLOR_DOWN  RGB(0x2b, 0x7d, 0xe9)
#define COLOR_UP    RGB(0x6d, 0xaa, 0x2c)
#define COLOR_MARK  RGB(0xd9, 0x2b, 0x2b)
#define COLOR_GRID  RGB(0xdc, 0xdc, 0xdc)
#define CHART_PAD   8

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

static void paintTraffic(const Traffic *t, HDC hdc, const RECT *client) {
    int width = client->right;
    int height = client->bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldBmp = SelectObject(mem, bmp);

    RECT full = {0, 0, width, height};
    FillRect(mem, &full, (HBRUSH)GetStockObject(WHITE_BRUSH));

    RECT plot = {CHART_PAD, CHART_PAD, width - CHART_PAD,
                 height - CHART_PAD};
    HPEN grid = CreatePen(PS_SOLID, 1, COLOR_GRID);
    HPEN oldPen = SelectObject(mem, grid);
    MoveToEx(mem, plot.left, plot.bottom, NULL);
    LineTo(mem, plot.right, plot.bottom);
    SelectObject(mem, oldPen);
    DeleteObject(grid);

    UINT64 down[TRAFFIC_BUCKETS], up[TRAFFIC_BUCKETS];
    trafficSeries(t, down, up);
    UINT64 maxValue = 1;
    for (int i = 0; i < TRAFFIC_BUCKETS; i++) {
        if (down[i] > maxValue) maxValue = down[i];
        if (up[i] > maxValue) maxValue = up[i];
    }
    if (maxValue > 1) {
        drawPolyline(mem, down, maxValue, &plot, COLOR_DOWN);
        drawPolyline(mem, up, maxValue, &plot, COLOR_UP);
    }

    int markerCol;
    if (trafficStartColumn(t, &markerCol)) {
        drawStartMarker(mem, markerCol, &plot);
    }

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
