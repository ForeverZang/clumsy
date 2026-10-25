#include <winsock2.h>
#include <windows.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ocr_capture.h"
#include "procmap.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// --- normalized rect -----------------------------------------------------

static double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static int mapAxis(double start, int clientSpan) {
    return (int)(clamp01(start) * clientSpan + 0.5);
}

RECT captureMapNorm(int clientW, int clientH, const NormRect *r) {
    RECT rc;
    rc.left   = mapAxis(r->x, clientW);
    rc.top    = mapAxis(r->y, clientH);
    rc.right  = (int)(clamp01(r->x + r->w) * clientW + 0.5);
    rc.bottom = (int)(clamp01(r->y + r->h) * clientH + 0.5);
    if (rc.right < rc.left) rc.right = rc.left;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}

// --- title matching ------------------------------------------------------

static char *trimCopy(const char *in, char *out, size_t cap) {
    if (!in || cap == 0) {
        if (cap) out[0] = '\0';
        return out;
    }
    while (*in && isspace((unsigned char)*in)) in++;
    strncpy(out, in, cap - 1);
    out[cap - 1] = '\0';
    size_t len = strlen(out);
    while (len && isspace((unsigned char)out[len - 1])) {
        out[--len] = '\0';
    }
    return out;
}

static void asciiLower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

int captureTitleMatchLevel(const char *saved, const char *candidate) {
    if (!saved || !candidate) return 0;
    char a[256];
    char b[256];
    trimCopy(saved, a, sizeof(a));
    trimCopy(candidate, b, sizeof(b));
    if (a[0] == '\0' || b[0] == '\0') return 0;
    asciiLower(a);
    asciiLower(b);
    if (strcmp(a, b) == 0) return 2;
    return strstr(b, a) ? 1 : 0;
}

BOOL captureResolveWindow(const CaptureWindow *cands, int candCount,
                          const char *savedTitle, CaptureWindow *out,
                          int *matchCount) {
    if (matchCount) *matchCount = 0;
    if (!cands || candCount <= 0 || !savedTitle) return FALSE;

    int bestLevel = 0;
    int bestIdx = -1;
    int bestCount = 0;

    for (int i = 0; i < candCount; i++) {
        int level = captureTitleMatchLevel(savedTitle, cands[i].title);
        if (level > bestLevel) {
            bestLevel = level;
            bestIdx = i;
            bestCount = 1;
        } else if (level == bestLevel && level > 0) {
            bestCount++;
        }
    }

    if (bestLevel == 0 || bestCount != 1) {
        if (matchCount) *matchCount = bestCount;
        return FALSE;
    }
    if (matchCount) *matchCount = 1;
    if (out) *out = cands[bestIdx];
    return TRUE;
}

// --- thumbnail + change detection ---------------------------------------

void captureMakeThumbnail(const unsigned char *bgra, int w, int h,
                          int thumbW, int maxThumbH,
                          unsigned char *grayOut, int *outTh,
                          int *outThumbH) {
    int tw = thumbW > 0 ? thumbW : 1;
    int maxTh = maxThumbH > 0 ? maxThumbH : 1;
    int th = (int)((double)tw * h / w + 0.5);
    if (th < 1) th = 1;
    // Rounding and extreme aspect ratios (1xN crops from a degenerate
    // saved norm) can report a taller thumb than the caller's buffer;
    // clamp before any write.
    if (th > maxTh) th = maxTh;
    if (outTh) *outTh = tw;
    if (outThumbH) *outThumbH = th;

    for (int ty = 0; ty < th; ty++) {
        int y0 = (int)((double)ty * h / th);
        int y1 = (int)((double)(ty + 1) * h / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int tx = 0; tx < tw; tx++) {
            int x0 = (int)((double)tx * w / tw);
            int x1 = (int)((double)(tx + 1) * w / tw);
            if (x1 <= x0) x1 = x0 + 1;

            unsigned int sum = 0;
            int n = 0;
            for (int y = y0; y < y1 && y < h; y++) {
                const unsigned char *row = bgra + ((size_t)y * w + x0) * 4;
                int xEnd = x1 < w ? x1 : w;
                for (int x = x0; x < xEnd; x++) {
                    sum += (row[0] + row[1] + row[2]) / 3;
                    row += 4;
                }
                n += xEnd - x0;
            }
            grayOut[ty * tw + tx] = (unsigned char)(n ? sum / n : 0);
        }
    }
}

BOOL captureChanged(const unsigned char *grayA, const unsigned char *grayB,
                    int pixels, int madThreshold) {
    if (pixels <= 0) return FALSE;
    unsigned long long diffSum = 0;
    for (int i = 0; i < pixels; i++) {
        diffSum += (unsigned int)abs((int)grayA[i] - (int)grayB[i]);
    }
    return diffSum > (unsigned long long)madThreshold * (unsigned)pixels;
}

// A frame counts as blank only when its darkest-to-brightest span is a
// few gray levels AND the brightest pixel is still nearly black. Pure
// PrintWindow black is 0/0; lock-screen and some exclusive-fullscreen
// captures come back a flat very dark gray. A dark-theme canvas
// (#1e1e1e ~ 30) already fails the brightness floor.
#define OCR_BLANK_MAX_GRAY  16
#define OCR_BLANK_MAX_RANGE 4

BOOL captureFrameBlank(const unsigned char *bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return TRUE;
    unsigned int minG = 255;
    unsigned int maxG = 0;
    size_t pixels = (size_t)w * (size_t)h;
    for (size_t i = 0; i < pixels; i++) {
        unsigned int g = ((unsigned int)bgra[i * 4]
                          + bgra[i * 4 + 1]
                          + bgra[i * 4 + 2]) / 3u;
        if (g < minG) minG = g;
        if (g > maxG) maxG = g;
    }
    return maxG < OCR_BLANK_MAX_GRAY
        && (maxG - minG) <= OCR_BLANK_MAX_RANGE;
}

// --- live window enumeration --------------------------------------------

struct EnumCtx {
    const UINT32 *pids;
    UINT          pidCount;
    CaptureWindow *out;
    int           cap;
    int           n;
};

static BOOL pidOwned(const struct EnumCtx *ctx, DWORD pid) {
    for (UINT i = 0; i < ctx->pidCount; i++) {
        if (ctx->pids[i] == pid) return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK enumCb(HWND hwnd, LPARAM lp) {
    struct EnumCtx *ctx = (struct EnumCtx *)lp;
    if (ctx->n >= ctx->cap) return FALSE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pidOwned(ctx, pid)) return TRUE;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    wchar_t titleW[256];
    int titleLen = GetWindowTextW(hwnd, titleW, 256);
    if (titleLen <= 0 && !(cw >= 100 && ch >= 100)) return TRUE;

    CaptureWindow *c = &ctx->out[ctx->n++];
    memset(c, 0, sizeof(*c));
    c->hwnd = hwnd;
    c->pid = pid;
    c->width = cw;
    c->height = ch;
    WideCharToMultiByte(CP_UTF8, 0, titleW, titleLen,
                        c->title, sizeof(c->title), NULL, NULL);
    return ctx->n < ctx->cap ? TRUE : FALSE;
}

int captureEnumWindowsForApp(const char *exeName, CaptureWindow *out,
                             int cap) {
    if (!exeName || !out || cap <= 0) return 0;

    AppGroup groups[64];
    size_t groupCount = 0;
    if (!procmapEnumApps(groups, 64, &groupCount)) return 0;

    const UINT32 *pids = NULL;
    UINT pidCount = 0;
    for (size_t i = 0; i < groupCount; i++) {
        if (_stricmp(groups[i].exeName, exeName) == 0) {
            pids = groups[i].pids;
            pidCount = groups[i].pidCount;
            break;
        }
    }
    if (!pids || pidCount == 0) return 0;

    struct EnumCtx ctx = { pids, pidCount, out, cap, 0 };
    EnumWindows(enumCb, (LPARAM)&ctx);
    return ctx.n;
}

// --- PrintWindow frame capture ------------------------------------------

BOOL captureIsCapturable(HWND hwnd) {
    return hwnd
        && IsWindow(hwnd)
        && IsWindowVisible(hwnd)
        && !IsIconic(hwnd);
}

void captureFreeBitmap(unsigned char *bgra) {
    free(bgra);
}

BOOL captureClientRegion(HWND hwnd, const RECT *sub,
                         unsigned char **outBgra, int *outW, int *outH,
                         char err[MSG_BUFSIZE]) {
    if (!hwnd || !outBgra || !outW || !outH) return FALSE;
    *outBgra = NULL;
    *outW = 0;
    *outH = 0;

    RECT client;
    if (!GetClientRect(hwnd, &client)) {
        if (err) strncpy(err, "window has no client area",
                         MSG_BUFSIZE - 1);
        return FALSE;
    }
    int cw = client.right - client.left;
    int ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) {
        if (err) strncpy(err, "window client area is empty",
                         MSG_BUFSIZE - 1);
        return FALSE;
    }

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    unsigned char *fullBits = NULL;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                                   (void **)&fullBits, NULL, 0);
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    BOOL pw = PrintWindow(hwnd, mem,
                          PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    SelectObject(mem, oldBmp);

    if (!pw) {
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        if (err) {
            snprintf(err, MSG_BUFSIZE,
                     "PrintWindow failed (win32 error %lu)",
                     GetLastError());
        }
        return FALSE;
    }

    RECT rc = sub ? *sub : client;
    if (rc.left < 0) rc.left = 0;
    if (rc.top < 0) rc.top = 0;
    if (rc.right > cw) rc.right = cw;
    if (rc.bottom > ch) rc.bottom = ch;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        if (err) strncpy(err, "capture region is empty",
                         MSG_BUFSIZE - 1);
        return FALSE;
    }

    unsigned char *buf = malloc((size_t)w * h * 4);
    if (!buf) {
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        if (err) strncpy(err, "out of memory", MSG_BUFSIZE - 1);
        return FALSE;
    }
    for (int y = 0; y < h; y++) {
        memcpy(buf + (size_t)y * w * 4,
               fullBits + ((size_t)(rc.top + y) * cw + rc.left) * 4,
               (size_t)w * 4);
    }

    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    *outBgra = buf;
    *outW = w;
    *outH = h;
    if (err) err[0] = '\0';
    return TRUE;
}
