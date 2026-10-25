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

#if defined(CLUMSY_OCR)

#define PICKER_MAX_WINDOWS 32
#define PICKER_MIN_PX      6
#define PICKER_ALPHA       110

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

// ---------------------------------------------------------------------------
// modal window chooser
// ---------------------------------------------------------------------------

// The chooser is a UI-thread modal, so a file-static state is safe and
// also avoids storing a pointer inside an IUP string attribute (IUP
// copies attribute values, it does not adopt pointers).
typedef struct {
    Ihandle *dlg;
    Ihandle *list;
    int      picked;
} ListState;

static ListState g_ls;

static int listOkCb(Ihandle *ih) {
    (void)ih;
    g_ls.picked = IupGetInt(g_ls.list, "VALUE") >= 1;
    IupHide(g_ls.dlg);
    return IUP_IGNORE;
}

static int listCancelCb(Ihandle *ih) {
    (void)ih;
    g_ls.picked = 0;
    IupHide(g_ls.dlg);
    return IUP_IGNORE;
}

static int listDblCb(Ihandle *ih, char *item, int itemIndex, int state) {
    (void)ih;
    (void)item;
    (void)state;
    if (itemIndex >= 1) {
        g_ls.picked = 1;
        IupHide(g_ls.dlg);
    }
    return IUP_IGNORE;
}

static int listKeyCb(Ihandle *ih, int key) {
    if (key == K_ESC) return listCancelCb(ih);
    if (key == K_CR || key == K_sCR) return listOkCb(ih);
    return IUP_DEFAULT;
}

BOOL uiPickerPickWindow(const char *exeName, CaptureWindow *out) {
    CaptureWindow wins[PICKER_MAX_WINDOWS];
    Ihandle *lbl, *okBtn, *cancelBtn, *btnRow, *box;
    int n, i, chosen;
    char item[320];
    char itemKey[16];

    if (exeName == NULL || out == NULL) return FALSE;
    n = captureEnumWindowsForApp(exeName, wins, PICKER_MAX_WINDOWS);
    if (n <= 0) return FALSE;

    memset(&g_ls, 0, sizeof(g_ls));
    g_ls.list = IupList(NULL);
    IupSetAttribute(g_ls.list, "VISIBLEITEMS", "8");
    IupSetAttribute(g_ls.list, "EXPAND", "HORIZONTAL");
    for (i = 0; i < n; i++) {
        snprintf(item, sizeof(item), "%s  [%dx%d]",
                 wins[i].title[0] != '\0' ? wins[i].title
                                          : tr("ocr.picker.untitled"),
                 wins[i].width, wins[i].height);
        snprintf(itemKey, sizeof(itemKey), "%d", i + 1);
        IupStoreAttribute(g_ls.list, itemKey, item);
    }
    IupSetAttribute(g_ls.list, "VALUE", "1");

    lbl = IupLabel(tr("ocr.picker.prompt"));
    okBtn = IupButton(tr("ocr.picker.ok"), NULL);
    cancelBtn = IupButton(tr("ocr.picker.cancel"), NULL);
    IupSetCallback(okBtn, "ACTION", (Icallback)listOkCb);
    IupSetCallback(cancelBtn, "ACTION", (Icallback)listCancelCb);
    IupSetCallback(g_ls.list, "DBLCLICK_CB", (Icallback)listDblCb);

    btnRow = IupHbox(IupFill(), okBtn, cancelBtn, NULL);
    IupSetAttribute(btnRow, "GAP", "8");
    IupSetAttribute(okBtn, "RASTERSIZE", "72x26");
    IupSetAttribute(cancelBtn, "RASTERSIZE", "72x26");
    box = IupVbox(lbl, g_ls.list, btnRow, NULL);
    IupSetAttribute(box, "MARGIN", "10x10");
    IupSetAttribute(box, "GAP", "8");
    g_ls.dlg = IupDialog(box);
    IupSetAttribute(g_ls.dlg, "TITLE", tr("ocr.picker.title"));
    IupSetAttribute(g_ls.dlg, "RASTERSIZE", "420x280");
    IupSetCallback(g_ls.dlg, "K_ANY", (Icallback)listKeyCb);

    IupPopup(g_ls.dlg, IUP_CENTER, IUP_CENTER);
    chosen = IupGetInt(g_ls.list, "VALUE");
    {
        int picked = g_ls.picked;
        IupDestroy(g_ls.dlg);
        memset(&g_ls, 0, sizeof(g_ls));
        if (!picked || chosen < 1 || chosen > n) return FALSE;
    }
    *out = wins[chosen - 1];
    return TRUE;
}

// ---------------------------------------------------------------------------
// region rubber-band overlay
// ---------------------------------------------------------------------------

typedef struct {
    Ihandle *dlg;
    Ihandle *canvas;
    HWND     target;
    POINT    origin; // monitor work-area origin in screen coords
    POINT    anchor; // screen coords
    POINT    cur;
    BOOL     dragging;
    BOOL     haveRect;
    BOOL     accepted;
    NormRect norm;
    HFONT    font;
} Overlay;

static Overlay g_ov;

static void finishOverlay(BOOL accepted) {
    g_ov.accepted = accepted;
    IupHide(g_ov.dlg);
}

static POINT canvasToScreen(int x, int y) {
    POINT p;
    p.x = x + g_ov.origin.x;
    p.y = y + g_ov.origin.y;
    return p;
}

static int overlayPaintCb(Ihandle *ih) {
    HWND hwnd = (HWND)(UINT_PTR)IupGetAttribute(ih, "HWND");
    RECT rc;
    HDC dc, mem;
    HBITMAP bmp, oldBmp;
    HFONT oldFont;
    HBRUSH dim;
    HBRUSH border;
    RECT sel;
    int x0, y0, x1, y1;
    const char *hint = tr("ocr.picker.dragHint");
    // tr() returns UTF-8 and raw GDI speaks the ANSI codepage; draw the
    // hint as wide text or Chinese renders as mojibake.
    wchar_t hintW[96];
    int hintChars = MultiByteToWideChar(CP_UTF8, 0, hint, -1,
                                        hintW, 96);

    GetClientRect(hwnd, &rc);
    dc = GetDC(hwnd);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    oldBmp = (HBITMAP)SelectObject(mem, bmp);

    dim = CreateSolidBrush(RGB(30, 30, 34));
    FillRect(mem, &rc, dim);
    DeleteObject(dim);

    if (g_ov.haveRect) {
        x0 = (g_ov.anchor.x < g_ov.cur.x ? g_ov.anchor.x : g_ov.cur.x)
             - g_ov.origin.x;
        y0 = (g_ov.anchor.y < g_ov.cur.y ? g_ov.anchor.y : g_ov.cur.y)
             - g_ov.origin.y;
        x1 = (g_ov.anchor.x > g_ov.cur.x ? g_ov.anchor.x : g_ov.cur.x)
             - g_ov.origin.x;
        y1 = (g_ov.anchor.y > g_ov.cur.y ? g_ov.anchor.y : g_ov.cur.y)
             - g_ov.origin.y;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > rc.right) x1 = rc.right;
        if (y1 > rc.bottom) y1 = rc.bottom;
        SetRect(&sel, x0, y0, x1, y1);
        FrameRect(mem, &sel, (HBRUSH)GetStockObject(WHITE_BRUSH));
        border = CreateSolidBrush(RGB(64, 158, 255));
        InflateRect(&sel, -2, -2);
        FrameRect(mem, &sel, border);
        DeleteObject(border);
    }

    // Hint last so the band never hides it.
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(235, 238, 245));
    oldFont = NULL;
    if (g_ov.font != NULL)
        oldFont = (HFONT)SelectObject(mem, g_ov.font);
    SetTextAlign(mem, TA_CENTER);
    if (hintChars > 0)
        TextOutW(mem, rc.right / 2, 10, hintW, hintChars - 1);
    if (oldFont != NULL) SelectObject(mem, oldFont);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    return IUP_DEFAULT;
}

static void repaintOverlay(void) {
    HWND hwnd = (HWND)(UINT_PTR)IupGetAttribute(g_ov.canvas, "HWND");
    if (hwnd != NULL) InvalidateRect(hwnd, NULL, FALSE);
}

// Mouse-release (or simulated release) path: accept a big-enough drag,
// otherwise erase the band and let the user try again.
static void completeDrag(void) {
    NormRect norm;
    if (uiPickerDragToNorm(g_ov.target, g_ov.anchor, g_ov.cur,
                           PICKER_MIN_PX, &norm)) {
        g_ov.norm = norm;
        finishOverlay(TRUE);
    } else {
        g_ov.haveRect = FALSE;
        repaintOverlay();
    }
}

static int overlayButtonCb(Ihandle *ih, int button, int pressed,
                           int x, int y, char *status) {
    (void)ih;
    (void)status;
    if (button == IUP_BUTTON3 && pressed) {
        finishOverlay(FALSE);
        return IUP_DEFAULT;
    }
    if (button != IUP_BUTTON1) return IUP_DEFAULT;
    if (pressed) {
        g_ov.anchor = canvasToScreen(x, y);
        g_ov.cur = g_ov.anchor;
        g_ov.dragging = TRUE;
        g_ov.haveRect = FALSE;
    } else if (g_ov.dragging) {
        g_ov.dragging = FALSE;
        g_ov.cur = canvasToScreen(x, y);
        completeDrag();
    }
    return IUP_DEFAULT;
}

static int overlayMotionCb(Ihandle *ih, int x, int y, char *status) {
    (void)ih;
    if (!g_ov.dragging || status == NULL || status[0] != 'd')
        return IUP_DEFAULT;
    g_ov.cur = canvasToScreen(x, y);
    g_ov.haveRect = TRUE;
    repaintOverlay();
    return IUP_DEFAULT;
}

static int overlayKeyCb(Ihandle *ih, int key) {
    (void)ih;
    if (key == K_ESC) finishOverlay(FALSE);
    return IUP_DEFAULT;
}

static int overlayMapCb(Ihandle *ih) {
    HWND hwnd = (HWND)(UINT_PTR)IupGetAttribute(ih, "HWND");
    HWND parent = GetParent(hwnd); // borderless dialog hosting the canvas
    LONG ex = GetWindowLongW(parent, GWL_EXSTYLE);
    SetWindowLongW(parent, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(parent, 0, PICKER_ALPHA, LWA_ALPHA);
    // Keep receiving motion/button events when the drag leaves the
    // monitor work area momentarily; release happens on unmap.
    SetCapture(hwnd);
    g_ov.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Microsoft YaHei UI");
    return IUP_DEFAULT;
}

static int overlayUnmapCb(Ihandle *ih) {
    (void)ih;
    ReleaseCapture();
    if (g_ov.font != NULL) {
        DeleteObject(g_ov.font);
        g_ov.font = NULL;
    }
    return IUP_DEFAULT;
}

BOOL uiPickerPickRect(HWND target, NormRect *out) {
    MONITORINFO mi;
    int w, h;
    char raster[32], pos[48];
    BOOL ok;

    if (target == NULL || out == NULL) return FALSE;
    if (!captureIsCapturable(target)) return FALSE;

    memset(&g_ov, 0, sizeof(g_ov));
    g_ov.target = target;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST),
                         &mi))
        return FALSE;
    g_ov.origin.x = mi.rcWork.left;
    g_ov.origin.y = mi.rcWork.top;
    w = mi.rcWork.right - mi.rcWork.left;
    h = mi.rcWork.bottom - mi.rcWork.top;

    g_ov.canvas = IupCanvas(NULL);
    IupSetAttribute(g_ov.canvas, "CANFOCUS", "YES");
    IupSetCallback(g_ov.canvas, "ACTION", (Icallback)overlayPaintCb);
    IupSetCallback(g_ov.canvas, "BUTTON_CB", (Icallback)overlayButtonCb);
    IupSetCallback(g_ov.canvas, "MOTION_CB", (Icallback)overlayMotionCb);
    IupSetCallback(g_ov.canvas, "MAP_CB", (Icallback)overlayMapCb);
    IupSetCallback(g_ov.canvas, "UNMAP_CB", (Icallback)overlayUnmapCb);

    g_ov.dlg = IupDialog(g_ov.canvas);
    IupSetAttribute(g_ov.dlg, "BORDER", "NO");
    IupSetAttribute(g_ov.dlg, "TITLEBAR", "NO");
    IupSetAttribute(g_ov.dlg, "MENUBOX", "NO");
    IupSetAttribute(g_ov.dlg, "RESIZE", "NO");
    IupSetAttribute(g_ov.dlg, "TOPMOST", "YES");
    IupSetCallback(g_ov.dlg, "K_ANY", (Icallback)overlayKeyCb);

    snprintf(raster, sizeof(raster), "%dx%d", w, h);
    snprintf(pos, sizeof(pos), "%ld,%ld",
             (long)mi.rcWork.left, (long)mi.rcWork.top);
    IupSetAttribute(g_ov.dlg, "RASTERSIZE", raster);
    IupSetAttribute(g_ov.dlg, "SCREENPOSITION", pos);

    IupPopup(g_ov.dlg, IUP_DEFAULT, IUP_DEFAULT);
    ok = g_ov.accepted;
    if (ok) *out = g_ov.norm;
    IupDestroy(g_ov.dlg);
    memset(&g_ov, 0, sizeof(g_ov));
    return ok;
}

// ---------------------------------------------------------------------------
// test seams for the modal flows
// ---------------------------------------------------------------------------

void uiPickerAcceptChooserForTest(void) {
    if (g_ls.dlg != NULL) listOkCb(NULL);
}

void uiPickerSetDragForTest(POINT screenA, POINT screenB) {
    HWND hwnd;
    if (g_ov.dlg == NULL) return;
    g_ov.anchor = screenA;
    g_ov.cur = screenB;
    g_ov.dragging = FALSE;
    g_ov.haveRect = TRUE;
    hwnd = (HWND)(UINT_PTR)IupGetAttribute(g_ov.canvas, "HWND");
    if (hwnd != NULL) {
        InvalidateRect(hwnd, NULL, FALSE);
        // Synchronous WM_PAINT so a headless grab right after this seam
        // already shows the band (no nested-loop message dispatch needed).
        UpdateWindow(hwnd);
    }
}

HWND uiPickerOverlayHwndForTest(void) {
    if (g_ov.canvas == NULL) return NULL;
    return (HWND)(UINT_PTR)IupGetAttribute(g_ov.canvas, "HWND");
}

HWND uiPickerChooserHwndForTest(void) {
    if (g_ls.dlg == NULL) return NULL;
    return (HWND)(UINT_PTR)IupGetAttribute(g_ls.dlg, "HWND");
}

BOOL uiPickerCompleteDragForTest(void) {
    if (g_ov.dlg == NULL) return FALSE;
    completeDrag();
    return g_ov.accepted;
}

#else // !CLUMSY_OCR — no-op stubs for builds without OCR

void uiPickerAcceptChooserForTest(void) {}
void uiPickerSetDragForTest(POINT screenA, POINT screenB) {
    (void)screenA;
    (void)screenB;
}
BOOL uiPickerCompleteDragForTest(void) { return FALSE; }
HWND uiPickerOverlayHwndForTest(void) { return NULL; }
HWND uiPickerChooserHwndForTest(void) { return NULL; }

BOOL uiPickerPickWindow(const char *exeName, CaptureWindow *out) {
    (void)exeName;
    (void)out;
    return FALSE;
}
BOOL uiPickerPickRect(HWND target, NormRect *out) {
    (void)target;
    (void)out;
    return FALSE;
}
BOOL uiPickerDragToNorm(HWND target, POINT screenA, POINT screenB,
                        int minPixels, NormRect *out) {
    (void)target;
    (void)screenA;
    (void)screenB;
    (void)minPixels;
    (void)out;
    return FALSE;
}

#endif // CLUMSY_OCR
