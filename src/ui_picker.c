#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "i18n.h"
#include "iup.h"
#include "ocr_capture.h"
#include "ui_picker.h"
#include "ui_picker_state.h"

#if defined(CLUMSY_OCR)

#define PICKER_MAX_WINDOWS 32
#define PICKER_MIN_PX      6
#define PICKER_TIMER_MS    300
#define PICKER_BAR_W       600
#define PICKER_BAR_H       46

#define PICKER_DIM_A       120
// The locked window interior is visually clear but MUST stay clickable.
// A layered window hit-tests alpha-0 pixels as transparent (clicks fall
// through to the app underneath), so use the smallest non-zero alpha:
// 1/255 darkening is invisible, yet the pixel still hits the overlay.
#define PICKER_HOLE_A      1
#define PICKER_BLUE        RGB(64, 158, 255)
#define PICKER_ORANGE      RGB(255, 170, 60)
#define PICKER_DIMRGB      RGB(10, 12, 16)
#define PICKER_PILLBG      RGB(24, 26, 32)
#define PICKER_BARBG       "24 26 32"
#define PICKER_BTN_BG      "42 46 56"
#define PICKER_BTN_FG      "235 238 245"

// ---------------------------------------------------------------------------
// shared pure geometry
// ---------------------------------------------------------------------------

BOOL uiPickerDragToNorm(HWND target, POINT screenA, POINT screenB,
                        int minPixels, NormRect *out) {
    RECT cr;
    int left, top, right, bottom, w, h;
    if (target == NULL || out == NULL) return FALSE;
    if (!GetClientRect(target, &cr)) return FALSE;
    if (!ScreenToClient(target, &screenA) || !ScreenToClient(target, &screenB))
        return FALSE;

    left   = screenA.x < screenB.x ? screenA.x : screenB.x;
    top    = screenA.y < screenB.y ? screenA.y : screenB.y;
    right  = screenA.x > screenB.x ? screenA.x : screenB.x;
    bottom = screenA.y > screenB.y ? screenA.y : screenB.y;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > cr.right) right = cr.right;
    if (bottom > cr.bottom) bottom = cr.bottom;

    w = right - left;
    h = bottom - top;
    if (w < minPixels || h < minPixels) return FALSE;
    if (cr.right <= 0 || cr.bottom <= 0) return FALSE;

    out->x = (double)left / cr.right;
    out->y = (double)top / cr.bottom;
    out->w = (double)w / cr.right;
    out->h = (double)h / cr.bottom;
    return TRUE;
}

// FRAME click guard (Review Focus #1): the HWND enumerated moments ago
// may have been destroyed and its handle recycled onto another process.
BOOL uiPickerWindowStillOwned(HWND hwnd, DWORD pid) {
    DWORD owner = 0;
    if (hwnd == NULL || pid == 0) return FALSE;
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return FALSE;
    GetWindowThreadProcessId(hwnd, &owner);
    return owner == pid;
}

// ---------------------------------------------------------------------------
// modal overlay state
// ---------------------------------------------------------------------------

typedef struct {
    PickerMachine m;
    CaptureWindow wins[PICKER_MAX_WINDOWS];
    char exe[128];

    HWND         overlay;
    HDC          memdc;
    HBITMAP      dib;
    HGDIOBJ      oldBmp, oldFont;
    unsigned int *bits;
    int          bw, bh;
    POINT        origin; // monitor work-area origin (screen coords)
    RECT         work;   // monitor work area (toolbar clamp target)
    HFONT        font;

    Ihandle     *bar;
    int          barW, barH; // measured toolbar outer size
    Ihandle     *undoBtn, *repickBtn, *doneBtn;

    POINT        anchor, cur;
    BOOL         banding, haveBand;

    UiPickerResult result;
    BOOL         finished;
} Overlay;

static Overlay g_ov;

// ---------------------------------------------------------------------------
// per-pixel-alpha composition (premultiplied BGRA)
// ---------------------------------------------------------------------------

static unsigned int premul(COLORREF c, int a) {
    int pb = (GetBValue(c) * a) / 255;
    int pg = (GetGValue(c) * a) / 255;
    int pr = (GetRValue(c) * a) / 255;
    return (unsigned int)((a << 24) | (pr << 16) | (pg << 8) | pb);
}

static void clipRect(RECT *r) {
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > g_ov.bw) r->right = g_ov.bw;
    if (r->bottom > g_ov.bh) r->bottom = g_ov.bh;
}

static RECT screenToOverlay(RECT r) {
    r.left -= g_ov.origin.x;
    r.top -= g_ov.origin.y;
    r.right -= g_ov.origin.x;
    r.bottom -= g_ov.origin.y;
    clipRect(&r);
    return r;
}

static void compDim(void) {
    size_t i, n = (size_t)g_ov.bw * (size_t)g_ov.bh;
    unsigned int dim = premul(PICKER_DIMRGB, PICKER_DIM_A);
    for (i = 0; i < n; i++) g_ov.bits[i] = dim;
}

static void compHole(RECT r) {
    int x, y;
    unsigned int clear = premul(RGB(0, 0, 0), PICKER_HOLE_A);
    clipRect(&r);
    for (y = r.top; y < r.bottom; y++)
        for (x = r.left; x < r.right; x++)
            g_ov.bits[(size_t)y * (size_t)g_ov.bw + x] = clear;
}

static void compFrame(RECT r, COLORREF c) {
    int x, y, t;
    unsigned int v = premul(c, 255);
    clipRect(&r);
    if (r.right <= r.left || r.bottom <= r.top) return;
    for (t = 0; t < 2; t++) {
        int l = r.left + t, rr = r.right - 1 - t;
        int tp = r.top + t, b = r.bottom - 1 - t;
        if (l > rr || tp > b) break;
        for (x = l; x <= rr; x++) {
            g_ov.bits[(size_t)tp * g_ov.bw + x] = v;
            g_ov.bits[(size_t)b * g_ov.bw + x] = v;
        }
        for (y = tp; y <= b; y++) {
            g_ov.bits[(size_t)y * g_ov.bw + l] = v;
            g_ov.bits[(size_t)y * g_ov.bw + rr] = v;
        }
    }
}

static void compDashed(RECT r, COLORREF c) {
    int x, y;
    unsigned int v = premul(c, 255);
    clipRect(&r);
    if (r.right <= r.left || r.bottom <= r.top) return;
    for (x = r.left; x < r.right; x += 3) {
        g_ov.bits[(size_t)r.top * g_ov.bw + x] = v;
        g_ov.bits[(size_t)(r.bottom - 1) * g_ov.bw + x] = v;
    }
    for (y = r.top; y < r.bottom; y += 3) {
        g_ov.bits[(size_t)y * g_ov.bw + r.left] = v;
        g_ov.bits[(size_t)y * g_ov.bw + (r.right - 1)] = v;
    }
}

// Solid opaque label pill. GDI ignores the alpha channel, so paint the
// whole rectangle opaque first and force alpha 255 afterwards.
static void compPill(int x, int y, const wchar_t *s, COLORREF bg) {
    SIZE ext;
    int w, h, i, j;
    RECT r;
    if (s == NULL || s[0] == L'\0') return;
    if (!GetTextExtentPoint32W(g_ov.memdc, s, (int)wcslen(s), &ext))
        return;
    w = ext.cx + 12;
    h = ext.cy + 6;
    if (x < 0) x = 0;
    if (x + w > g_ov.bw) x = g_ov.bw - w;
    if (x < 0) { w = g_ov.bw; x = 0; }
    if (y < 0) y = 0;
    if (y + h > g_ov.bh) y = g_ov.bh - h;
    SetRect(&r, x, y, x + w, y + h);
    // GDI fill + text (RGB is final at alpha 255).
    {
        HBRUSH brush = CreateSolidBrush(bg);
        RECT fr = r;
        FillRect(g_ov.memdc, &fr, brush);
        DeleteObject(brush);
    }
    SetBkMode(g_ov.memdc, TRANSPARENT);
    SetTextColor(g_ov.memdc, RGB(255, 255, 255));
    TextOutW(g_ov.memdc, x + 6, y + 3, s, (int)wcslen(s));
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            g_ov.bits[(size_t)j * g_ov.bw + i] |= 0xFF000000u;
}

static void compHint(const char *utf8) {
    wchar_t w[192];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 192);
    SIZE ext;
    if (n <= 0) return;
    if (!GetTextExtentPoint32W(g_ov.memdc, w, n - 1, &ext)) return;
    compPill((g_ov.bw - ext.cx - 12) / 2, 12, w, PICKER_PILLBG);
}

// ---------------------------------------------------------------------------
// geometry helpers
// ---------------------------------------------------------------------------

static RECT clientScreen(HWND h) {
    RECT cr;
    POINT tl = {0, 0};
    GetClientRect(h, &cr);
    ClientToScreen(h, &tl);
    SetRect(&cr, tl.x, tl.y,
            tl.x + cr.right, tl.y + cr.bottom);
    return cr;
}

static RECT normToScreen(HWND h, const NormRect *n) {
    RECT cr;
    POINT tl = {0, 0};
    GetClientRect(h, &cr);
    ClientToScreen(h, &tl);
    return (RECT){
        tl.x + (int)(n->x * cr.right),
        tl.y + (int)(n->y * cr.bottom),
        tl.x + (int)((n->x + n->w) * cr.right),
        tl.y + (int)((n->y + n->h) * cr.bottom)
    };
}

static POINT clampToClient(HWND h, POINT p) {
    RECT cr = clientScreen(h);
    if (p.x < cr.left) p.x = cr.left;
    if (p.x > cr.right) p.x = cr.right;
    if (p.y < cr.top) p.y = cr.top;
    if (p.y > cr.bottom) p.y = cr.bottom;
    return p;
}

// What is under this screen point when our own overlay is ignored?
static long windowIndexAt(POINT screen) {
    LONG ex;
    HWND h;
    size_t i;
    ex = GetWindowLongW(g_ov.overlay, GWL_EXSTYLE);
    SetWindowLongW(g_ov.overlay, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    h = WindowFromPoint(screen);
    SetWindowLongW(g_ov.overlay, GWL_EXSTYLE, ex);
    if (h == NULL) return -1;
    h = GetAncestor(h, GA_ROOT);
    for (i = 0; i < g_ov.m.winCount; i++)
        if (g_ov.wins[i].hwnd == h) return (long)i;
    return -1;
}

static void frameLabel(const CaptureWindow *w, wchar_t *out,
                       size_t cap) {
    wchar_t titleW[256];
    wchar_t exeW[128];
    int tn, en;
    tn = MultiByteToWideChar(CP_UTF8, 0, w->title, -1, titleW, 256);
    en = MultiByteToWideChar(CP_UTF8, 0, g_ov.exe, -1, exeW, 128);
    if (tn <= 0) {
        const char *fallback = tr("ocr.picker.untitled");
        MultiByteToWideChar(CP_UTF8, 0, fallback, -1, titleW, 256);
    }
    if (en <= 0) exeW[0] = L'\0';
    _snwprintf(out, cap, L"%ls — %ls", exeW, titleW);
    out[cap - 1] = L'\0';
}

// ---------------------------------------------------------------------------
// frame composition per phase
// ---------------------------------------------------------------------------

static void composeFrame(void) {
    wchar_t label[400];
    RECT wr;
    compDim();
    if (g_ov.m.hover >= 0) {
        const CaptureWindow *w = &g_ov.wins[g_ov.m.hover];
        if (GetWindowRect(w->hwnd, &wr)) {
            RECT r = screenToOverlay(wr);
            compHole(r);
            compFrame(r, PICKER_BLUE);
            frameLabel(w, label, 400);
            compPill(r.left + 4, r.top + 4, label, PICKER_BLUE);
        }
    }
    compHint(tr("ocr.picker.frameHint"));
}

static void composeDrag(void) {
    HWND h = g_ov.wins[g_ov.m.locked].hwnd;
    RECT cr, br;
    size_t i;
    wchar_t tag[16], sizeW[32];
    char sizeUtf8[32];
    compDim();
    cr = screenToOverlay(clientScreen(h));
    compHole(cr);
    compFrame(cr, PICKER_BLUE);
    for (i = 0; i < g_ov.m.rectCount; i++) {
        RECT r = screenToOverlay(normToScreen(h, &g_ov.m.rects[i]));
        if (i < g_ov.m.keepCount) compDashed(r, PICKER_ORANGE);
        else compFrame(r, PICKER_BLUE);
        _snwprintf(tag, 16, L"R%u", (unsigned)(i + 1));
        tag[15] = L'\0';
        compPill(r.left + 2, r.top + 2, tag,
                 i < g_ov.m.keepCount ? PICKER_ORANGE : PICKER_BLUE);
    }
    if (g_ov.haveBand) {
        br = (RECT){
            g_ov.anchor.x < g_ov.cur.x ? g_ov.anchor.x : g_ov.cur.x,
            g_ov.anchor.y < g_ov.cur.y ? g_ov.anchor.y : g_ov.cur.y,
            g_ov.anchor.x > g_ov.cur.x ? g_ov.anchor.x : g_ov.cur.x,
            g_ov.anchor.y > g_ov.cur.y ? g_ov.anchor.y : g_ov.cur.y
        };
        br = screenToOverlay(br);
        compFrame(br, PICKER_BLUE);
        snprintf(sizeUtf8, sizeof(sizeUtf8), tr("ocr.picker.sizeTip"),
                 br.right - br.left, br.bottom - br.top);
        MultiByteToWideChar(CP_UTF8, 0, sizeUtf8, -1, sizeW, 32);
        sizeW[31] = L'\0';
        compPill(br.left + 4, br.bottom + 4, sizeW, PICKER_PILLBG);
    }
    compHint(tr("ocr.picker.dragHint"));
}

static void present(void) {
    HDC screenDc;
    POINT dst, src = {0, 0};
    SIZE size;
    BLENDFUNCTION bf;
    if (g_ov.overlay == NULL || g_ov.bits == NULL) return;
    if (g_ov.m.phase == PICKER_PHASE_FRAME) composeFrame();
    else composeDrag();
    screenDc = GetDC(NULL);
    dst = g_ov.origin;
    size.cx = g_ov.bw;
    size.cy = g_ov.bh;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_ov.overlay, screenDc, &dst, &size,
                        g_ov.memdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screenDc);
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------

static void finishPick(UiPickerResult r) {
    if (g_ov.finished) return;
    g_ov.finished = TRUE;
    g_ov.result = r;
    if (g_ov.bar != NULL) IupHide(g_ov.bar);
}

// Every DRAG action re-validates the locked window's OWNERSHIP, not just
// its liveness: the HWND may have been destroyed and its value recycled
// onto another process. The 300ms timer is the input-free backstop.
static BOOL lockedAlive(void) {
    long locked = g_ov.m.locked;
    HWND h;
    if (locked < 0) return FALSE;
    h = g_ov.wins[locked].hwnd;
    if (uiPickerWindowStillOwned(h, g_ov.wins[locked].pid))
        return TRUE;
    finishPick(UI_PICKER_WINDOW_DIED);
    return FALSE;
}

static void syncToolbar(void) {
    char title[64];
    BOOL drag = g_ov.m.phase == PICKER_PHASE_DRAG;
    if (g_ov.bar == NULL) return;
    IupSetAttribute(g_ov.undoBtn, "ACTIVE",
                    (drag && uiPickerStateSessionCount(&g_ov.m) > 0)
                        ? "YES" : "NO");
    IupSetAttribute(g_ov.repickBtn, "ACTIVE", drag ? "YES" : "NO");
    snprintf(title, sizeof(title), tr("ocr.picker.done"),
             (unsigned)g_ov.m.rectCount);
    IupStoreAttribute(g_ov.doneBtn, "TITLE", title);
    IupSetAttribute(g_ov.doneBtn, "ACTIVE",
                    uiPickerStateCanFinish(&g_ov.m) ? "YES" : "NO");
}

static void completeBand(void) {
    NormRect nr;
    HWND h;
    if (!lockedAlive()) return;
    h = g_ov.wins[g_ov.m.locked].hwnd;
    if (uiPickerDragToNorm(h, g_ov.anchor, g_ov.cur,
                           PICKER_MIN_PX, &nr))
        uiPickerStateAddRect(&g_ov.m, &nr);
    g_ov.banding = FALSE;
    g_ov.haveBand = FALSE;
    syncToolbar();
    present();
}

static int undoCb(Ihandle *ih) {
    (void)ih;
    if (!lockedAlive()) return IUP_DEFAULT;
    uiPickerStateUndo(&g_ov.m);
    syncToolbar();
    present();
    return IUP_DEFAULT;
}

static int repickCb(Ihandle *ih) {
    (void)ih;
    if (!lockedAlive()) return IUP_DEFAULT;
    uiPickerStateReframe(&g_ov.m);
    syncToolbar();
    present();
    return IUP_DEFAULT;
}

static int cancelPickCb(Ihandle *ih) {
    (void)ih;
    finishPick(UI_PICKER_CANCEL);
    return IUP_DEFAULT;
}

static int doneCb(Ihandle *ih) {
    (void)ih;
    if (!lockedAlive()) return IUP_DEFAULT;
    if (uiPickerStateCanFinish(&g_ov.m)) finishPick(UI_PICKER_OK);
    return IUP_DEFAULT;
}

static int barKeyCb(Ihandle *ih, int key) {
    (void)ih;
    if (key == K_ESC) finishPick(UI_PICKER_CANCEL);
    return IUP_DEFAULT;
}

// ---------------------------------------------------------------------------
// raw layered overlay window
// ---------------------------------------------------------------------------

#define PICKER_TIMER_ID 1

static POINT clientToScreenPoint(LPARAM lp) {
    POINT p;
    p.x = (short)(int)(LOWORD(lp));
    p.y = (short)(int)(HIWORD(lp));
    p.x += g_ov.origin.x;
    p.y += g_ov.origin.y;
    return p;
}

static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // Take the click without becoming active: MA_NOACTIVATE still
        // delivers WM_LBUTTONDOWN, but the window is neither activated
        // nor raised, so the topmost toolbar stays above the overlay.
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        POINT p = clientToScreenPoint(lp);
        // No SetCapture: the overlay covers the whole work area, and a
        // capture would swallow clicks meant for the bottom toolbar.
        // No SetFocus either: focusing a top-level window activates and
        // raises it even when WM_MOUSEACTIVATE refused activation. Escape
        // stays bound to the toolbar dialog (barKeyCb, K_ANY).
        if (g_ov.m.phase == PICKER_PHASE_FRAME) {
            long idx = windowIndexAt(p);
            if (idx >= 0
                && uiPickerWindowStillOwned(g_ov.wins[idx].hwnd,
                                            g_ov.wins[idx].pid)
                && uiPickerStateLock(&g_ov.m, idx)) {
                syncToolbar();
                present();
            }
        } else if (lockedAlive()) {
            RECT cr = clientScreen(g_ov.wins[g_ov.m.locked].hwnd);
            if (PtInRect(&cr, p)) {
                g_ov.anchor = g_ov.cur = clampToClient(
                    g_ov.wins[g_ov.m.locked].hwnd, p);
                g_ov.banding = TRUE;
                g_ov.haveBand = FALSE;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = clientToScreenPoint(lp);
        if (g_ov.memdc == NULL) return 0;
        if (g_ov.m.phase == PICKER_PHASE_DRAG && g_ov.banding) {
            g_ov.cur = clampToClient(g_ov.wins[g_ov.m.locked].hwnd, p);
            g_ov.haveBand = TRUE;
            present();
        } else if (g_ov.m.phase == PICKER_PHASE_FRAME) {
            long idx = windowIndexAt(p);
            if (idx != g_ov.m.hover) {
                uiPickerStateSetHover(&g_ov.m, idx);
                present();
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_ov.m.phase == PICKER_PHASE_DRAG && g_ov.banding)
            completeBand();
        return 0;
    case WM_RBUTTONDOWN:
        if (g_ov.m.phase == PICKER_PHASE_FRAME)
            finishPick(UI_PICKER_CANCEL);
        else if (g_ov.banding) {
            g_ov.banding = FALSE;
            g_ov.haveBand = FALSE;
            present();
        }
        return 0;
    case WM_KEYDOWN:
        // Defense in depth: with WS_EX_NOACTIVATE and no SetFocus the
        // overlay can never get keyboard focus in production, so Esc is
        // normally handled by barKeyCb on the toolbar dialog. Keep this
        // for the case where focus somehow lands here anyway.
        if (wp == VK_ESCAPE) finishPick(UI_PICKER_CANCEL);
        return 0;
    case WM_TIMER:
        if (wp == PICKER_TIMER_ID && g_ov.m.locked >= 0
            && !uiPickerWindowStillOwned(g_ov.wins[g_ov.m.locked].hwnd,
                                         g_ov.wins[g_ov.m.locked].pid))
            finishPick(UI_PICKER_WINDOW_DIED);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static BOOL createOverlay(void) {
    static BOOL registered = FALSE;
    WNDCLASSW wc;
    MONITORINFO mi;
    POINT cursor;
    HMONITOR mon;
    BITMAPINFO bi;
    int w, h;

    if (!registered) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = overlayWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ClumsyOcrPickerOverlay";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        if (!RegisterClassW(&wc)) return FALSE;
        registered = TRUE;
    }
    GetCursorPos(&cursor);
    mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return FALSE;
    g_ov.work = mi.rcWork;
    g_ov.origin.x = mi.rcWork.left;
    g_ov.origin.y = mi.rcWork.top;
    g_ov.bw = w = mi.rcWork.right - mi.rcWork.left;
    g_ov.bh = h = mi.rcWork.bottom - mi.rcWork.top;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_ov.memdc = CreateCompatibleDC(NULL);
    g_ov.dib = CreateDIBSection(g_ov.memdc, &bi, DIB_RGB_COLORS,
                                (void **)&g_ov.bits, NULL, 0);
    if (g_ov.dib == NULL) { DeleteDC(g_ov.memdc); g_ov.memdc = NULL; return FALSE; }
    g_ov.oldBmp = SelectObject(g_ov.memdc, g_ov.dib);
    g_ov.font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Microsoft YaHei UI");
    g_ov.oldFont = SelectObject(g_ov.memdc, g_ov.font);

    // WS_EX_NOACTIVATE: clicking the overlay must never activate it.
    // Activation raises this full-work-area window to the top of the
    // topmost band, above the toolbar popup, after which clicks on the
    // toolbar hit the overlay and start stray bands. WM_MOUSEACTIVATE in
    // the wndproc says the same thing on the input path.
    g_ov.overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW
        | WS_EX_NOACTIVATE,
        L"ClumsyOcrPickerOverlay", L"", WS_POPUP,
        mi.rcWork.left, mi.rcWork.top, w, h,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (g_ov.overlay == NULL) return FALSE;
    SetTimer(g_ov.overlay, PICKER_TIMER_ID, PICKER_TIMER_MS, NULL);
    present();
    ShowWindow(g_ov.overlay, SW_SHOWNOACTIVATE);
    return TRUE;
}

static void destroyOverlay(void) {
    if (g_ov.overlay != NULL) {
        KillTimer(g_ov.overlay, PICKER_TIMER_ID);
        DestroyWindow(g_ov.overlay);
        g_ov.overlay = NULL;
    }
    if (g_ov.memdc != NULL && g_ov.oldFont != NULL)
        SelectObject(g_ov.memdc, g_ov.oldFont);
    if (g_ov.memdc != NULL && g_ov.oldBmp != NULL)
        SelectObject(g_ov.memdc, g_ov.oldBmp);
    if (g_ov.font != NULL) { DeleteObject(g_ov.font); g_ov.font = NULL; }
    if (g_ov.dib != NULL) { DeleteObject(g_ov.dib); g_ov.dib = NULL; }
    if (g_ov.memdc != NULL) { DeleteDC(g_ov.memdc); g_ov.memdc = NULL; }
    g_ov.bits = NULL;
}

// ---------------------------------------------------------------------------
// bottom toolbar (IUP popup runs the modal loop)
// ---------------------------------------------------------------------------

static Ihandle *darkButton(const char *title, Icallback cb) {
    Ihandle *b = IupFlatButton(title);
    IupSetCallback(b, "FLAT_ACTION", cb);
    IupSetAttribute(b, "BGCOLOR", PICKER_BTN_BG);
    IupSetAttribute(b, "FGCOLOR", PICKER_BTN_FG);
    return b;
}

// Zero native chrome for the bottom toolbar. MINBOX/MAXBOX=NO are load
// bearing: BORDER/TITLEBAR/MENUBOX/RESIZE=NO alone leave WS_CAPTION on
// (framework finding, IUP 3.30 Win64 probed on Win11), producing a ~29px
// white caption strip that hides the client.
static void setToolbarChrome(Ihandle *dlg) {
    IupSetAttribute(dlg, "BORDER", "NO");
    IupSetAttribute(dlg, "TITLEBAR", "NO");
    IupSetAttribute(dlg, "MENUBOX", "NO");
    IupSetAttribute(dlg, "RESIZE", "NO");
    IupSetAttribute(dlg, "MINBOX", "NO");
    IupSetAttribute(dlg, "MAXBOX", "NO");
}

static Ihandle *createToolbar(void) {
    Ihandle *row, *dlg;
    char raster[32];
    const char *natural;
    g_ov.undoBtn = darkButton(tr("ocr.picker.undo"), (Icallback)undoCb);
    g_ov.repickBtn = darkButton(tr("ocr.picker.repick"),
                                (Icallback)repickCb);
    g_ov.doneBtn = darkButton(tr("ocr.picker.done"), (Icallback)doneCb);
    row = IupHbox(IupFill(), g_ov.undoBtn, g_ov.repickBtn,
                  darkButton(tr("ocr.picker.cancel2"),
                             (Icallback)cancelPickCb),
                  g_ov.doneBtn, IupFill(), NULL);
    IupSetAttribute(row, "GAP", "8");
    IupSetAttribute(row, "MARGIN", "8x6");
    IupSetAttribute(row, "BGCOLOR", PICKER_BARBG);
    dlg = IupDialog(row);
    setToolbarChrome(dlg);
    IupSetAttribute(dlg, "TOPMOST", "YES");
    IupSetAttribute(dlg, "BGCOLOR", PICKER_BARBG);
    IupSetCallback(dlg, "K_ANY", (Icallback)barKeyCb);
    // Pin the dialog (outer size; it is zero-chrome now) to its measured
    // natural size rather than a fixed 600x46, so the Chinese button row
    // fits at any display scaling. The constants are only the fallback
    // when IUP cannot report a natural size.
    g_ov.barW = PICKER_BAR_W;
    g_ov.barH = PICKER_BAR_H;
    IupRefresh(dlg);
    natural = IupGetAttribute(dlg, "NATURALSIZE");
    if (natural != NULL
        && sscanf(natural, "%dx%d", &g_ov.barW, &g_ov.barH) == 2
        && g_ov.barW > 0 && g_ov.barH > 0) {
        // measured outer size (outer == client with caption stripped)
    } else {
        g_ov.barW = PICKER_BAR_W;
        g_ov.barH = PICKER_BAR_H;
    }
    snprintf(raster, sizeof(raster), "%dx%d", g_ov.barW, g_ov.barH);
    IupSetAttribute(dlg, "RASTERSIZE", raster);
    g_ov.bar = dlg;
    return dlg;
}

// Bottom-center of the overlay monitor, 12px above the work-area bottom,
// pulled inside the work rect for narrow monitors or an edge taskbar.
static BOOL placementBar(int barW, int barH, POINT *out) {
    int workW, desiredX, desiredY;
    if (g_ov.overlay == NULL || out == NULL) return FALSE;
    if (barW <= 0) barW = PICKER_BAR_W;
    if (barH <= 0) barH = PICKER_BAR_H;
    workW = g_ov.work.right - g_ov.work.left;
    desiredX = g_ov.work.left + (workW - barW) / 2;
    desiredY = g_ov.work.bottom - barH - 12;
    clampIntoWorkRect(desiredX, desiredY, barW, barH, &g_ov.work, out);
    return TRUE;
}

// ---------------------------------------------------------------------------
// modal entry
// ---------------------------------------------------------------------------

static long findWindowIndex(HWND hwnd) {
    size_t i;
    for (i = 0; i < g_ov.m.winCount; i++)
        if (g_ov.wins[i].hwnd == hwnd) return (long)i;
    return -1;
}

UiPickerResult uiPickerPick(const char *exeName,
                            BOOL startFrame,
                            BOOL hadWindow,
                            const CaptureWindow *initialWindow,
                            const NormRect *existing, size_t existingCount,
                            CaptureWindow *outWindow,
                            NormRect *outRects, size_t *outRectCount) {
    Ihandle *dlg;
    int n;
    long initial = -1;
    int barX, barY;
    UiPickerResult result;

    if (exeName == NULL || outWindow == NULL) return UI_PICKER_CANCEL;
    if (!startFrame && initialWindow == NULL) return UI_PICKER_CANCEL;
    memset(&g_ov, 0, sizeof(g_ov));
    {
        size_t len = strnlen(exeName, sizeof(g_ov.exe) - 1);
        memcpy(g_ov.exe, exeName, len);
        g_ov.exe[len] = '\0';
    }

    n = captureEnumWindowsForApp(exeName, g_ov.wins, PICKER_MAX_WINDOWS);
    if (n <= 0) { memset(&g_ov, 0, sizeof(g_ov)); return UI_PICKER_CANCEL; }

    uiPickerStateInit(&g_ov.m, hadWindow, (size_t)n, existing, existingCount);
    if (startFrame) {
        // The single window may have died or been recycled onto another
        // process between enumeration and lock; ownership-validate first.
        if (n == 1) {
            if (!uiPickerWindowStillOwned(g_ov.wins[0].hwnd, g_ov.wins[0].pid))
                return UI_PICKER_WINDOW_DIED;
            uiPickerStateAutoLock(&g_ov.m);
        }
    } else {
        initial = findWindowIndex(initialWindow->hwnd);
        if (initial < 0
            || !uiPickerWindowStillOwned(g_ov.wins[initial].hwnd,
                                         g_ov.wins[initial].pid))
            return UI_PICKER_WINDOW_DIED;
        uiPickerStateLock(&g_ov.m, initial);
    }

    if (!createOverlay()) {
        destroyOverlay();
        memset(&g_ov, 0, sizeof(g_ov));
        return UI_PICKER_CANCEL;
    }
    dlg = createToolbar();
    syncToolbar();
    {
        POINT barPos;
        if (placementBar(g_ov.barW, g_ov.barH, &barPos)) {
            barX = barPos.x;
            barY = barPos.y;
        } else {
            barX = g_ov.origin.x + (g_ov.bw - PICKER_BAR_W) / 2;
            barY = g_ov.origin.y + g_ov.bh - PICKER_BAR_H - 12;
        }
    }
    IupPopup(dlg, barX, barY);

    result = g_ov.result;
    if (result == UI_PICKER_OK && g_ov.m.locked >= 0) {
        *outWindow = g_ov.wins[g_ov.m.locked];
        if (outRectCount != NULL) *outRectCount = g_ov.m.rectCount;
        if (outRects != NULL && g_ov.m.rectCount > 0)
            memcpy(outRects, g_ov.m.rects,
                   g_ov.m.rectCount * sizeof(outRects[0]));
    } else if (outRectCount != NULL) {
        *outRectCount = 0;
    }
    g_ov.bar = NULL;
    IupDestroy(dlg);
    destroyOverlay();
    memset(&g_ov, 0, sizeof(g_ov));
    return result;
}

// ---------------------------------------------------------------------------
// test seams
// ---------------------------------------------------------------------------

void uiPickerSetDragForTest(POINT screenA, POINT screenB) {
    HWND h;
    if (g_ov.overlay == NULL || g_ov.m.locked < 0) return;
    h = g_ov.wins[g_ov.m.locked].hwnd;
    g_ov.anchor = clampToClient(h, screenA);
    g_ov.cur = clampToClient(h, screenB);
    g_ov.banding = FALSE;
    g_ov.haveBand = TRUE;
    present();
}

BOOL uiPickerCompleteDragForTest(void) {
    size_t before;
    if (g_ov.overlay == NULL || g_ov.m.locked < 0) return FALSE;
    before = g_ov.m.rectCount;
    completeBand();
    return g_ov.m.rectCount > before;
}

UiPickerResult uiPickerFinishForTest(void) {
    if (g_ov.bar == NULL) return UI_PICKER_CANCEL;
    doneCb(NULL);
    return g_ov.result;
}

HWND uiPickerOverlayHwndForTest(void) { return g_ov.overlay; }

BOOL uiPickerBarPlacementForTest(int barW, int barH, POINT *out) {
    return placementBar(barW, barH, out);
}

HWND uiPickerBarHwndForTest(void) {
    if (g_ov.bar == NULL) return NULL;
    return (HWND)(intptr_t)IupGetAttribute(g_ov.bar, "HWND");
}

void uiPickerSetLockedPidForTest(DWORD pid) {
    if (g_ov.m.locked < 0) return;
    g_ov.wins[g_ov.m.locked].pid = pid;
}

BOOL uiPickerTimerTickForTest(void) {
    if (g_ov.overlay == NULL || g_ov.m.locked < 0) return FALSE;
    SendMessageW(g_ov.overlay, WM_TIMER, (WPARAM)PICKER_TIMER_ID, 0);
    return g_ov.finished;
}

#else // !CLUMSY_OCR — no-op stubs for builds without OCR

#include <stddef.h>

BOOL uiPickerDragToNorm(HWND target, POINT screenA, POINT screenB,
                        int minPixels, NormRect *out) {
    (void)target; (void)screenA; (void)screenB;
    (void)minPixels; (void)out;
    return FALSE;
}
BOOL uiPickerWindowStillOwned(HWND hwnd, DWORD pid) {
    (void)hwnd; (void)pid;
    return FALSE;
}
UiPickerResult uiPickerPick(const char *exeName, BOOL startFrame,
                            BOOL hadWindow, const CaptureWindow *initialWindow,
                            const NormRect *existing, size_t existingCount,
                            CaptureWindow *outWindow,
                            NormRect *outRects, size_t *outRectCount) {
    (void)exeName; (void)startFrame; (void)hadWindow;
    (void)initialWindow; (void)existing; (void)existingCount;
    (void)outWindow; (void)outRects; (void)outRectCount;
    return UI_PICKER_CANCEL;
}
void uiPickerSetDragForTest(POINT screenA, POINT screenB) {
    (void)screenA; (void)screenB;
}
BOOL uiPickerCompleteDragForTest(void) { return FALSE; }
UiPickerResult uiPickerFinishForTest(void) { return UI_PICKER_CANCEL; }
HWND uiPickerOverlayHwndForTest(void) { return NULL; }
BOOL uiPickerBarPlacementForTest(int barW, int barH, POINT *out) {
    (void)barW; (void)barH; (void)out;
    return FALSE;
}
HWND uiPickerBarHwndForTest(void) { return NULL; }
void uiPickerSetLockedPidForTest(DWORD pid) { (void)pid; }
BOOL uiPickerTimerTickForTest(void) { return FALSE; }

#endif // CLUMSY_OCR
