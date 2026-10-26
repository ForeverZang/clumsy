// Throwaway diagnostic (Phase 1 evidence): does a REAL native click on
// the section-⑤ toggle and its in-block Start button reach the callbacks
// after the folded body is expanded? Mirrors main.c dialog assembly but
// records actions instead of attaching WinDivert.
#include <stdlib.h>
#include <stdio.h>
#include <winsock2.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"
#include "i18n.h"
#include "ui_simple.h"
#include "ui_advanced.h"
#include "ui_ocr.h"
#include "ocr_glue.h"

Module* modules[MODULE_CNT] = {
    &lagModule, &dropModule, &throttleModule, &dupModule,
    &oodModule, &tamperModule, &resetModule, &bandwidthModule,
};
BOOL parameterized = FALSE;
volatile short sendState = SEND_STATUS_NONE;

static int g_fired = 0;
static int recorder(Ihandle *ih) { (void)ih; g_fired++; printf("[recorder] action cb fired (%d)\n", g_fired); return IUP_DEFAULT; }

static void pump(int n) {
    for (int i = 0; i < n; i++) IupLoopStep();
    Sleep(30);
    for (int i = 0; i < n; i++) IupLoopStep();
}

// Hardware-level click: IUP flat widgets ignore synthetic PostMessage
// button messages (observed: focus moves but FLAT_ACTION never fires), so
// drive the real input queue over the widget's screen center.
static void realClick(Ihandle *ih, const char *tag) {
    HWND h = (HWND)(intptr_t)IupGetAttribute(ih, "HWND");
    RECT rc;
    POINT pt;
    INPUT in[2];
    printf("[%s] HWND=%p\n", tag, (void*)h);
    if (h == NULL || !GetClientRect(h, &rc)) { printf("[%s] NO NATIVE HANDLE\n", tag); return; }
    pt.x = (rc.right - rc.left) / 2;
    pt.y = (rc.bottom - rc.top) / 2;
    ClientToScreen(h, &pt);
    printf("[%s] real click at screen %ld,%ld (client %dx%d)\n",
           tag, (long)pt.x, (long)pt.y, rc.right, rc.bottom);
    SetCursorPos(pt.x, pt.y);
    Sleep(150);
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
    pump(10);
    SendInput(1, &in[1], sizeof(INPUT));
    pump(15);
}

static void shot(HWND hwnd, const char *path) {
    RECT rc;
    HDC win, mem;
    HBITMAP bmp;
    BITMAPINFO bi;
    BITMAPFILEHEADER bf;
    FILE *fp;
    int w, h, rowSize, imgSize;
    unsigned char *px;
    GetClientRect(hwnd, &rc);
    w = rc.right; h = rc.bottom;
    win = GetDC(hwnd);
    mem = CreateCompatibleDC(win);
    bmp = CreateCompatibleBitmap(win, w, h);
    SelectObject(mem, bmp);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (!BitBlt(mem, 0, 0, w, h, win, 0, 0, SRCCOPY)) { printf("bitblt fail %lu\n", GetLastError()); }
    rowSize = ((w * 3 + 3) / 4) * 4;
    imgSize = rowSize * h;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    px = malloc(imgSize);
    GetDIBits(mem, bmp, 0, h, px, &bi, DIB_RGB_COLORS);
    memset(&bf, 0, sizeof(bf));
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bf.bfSize = bf.bfOffBits + imgSize;
    fp = fopen(path, "wb");
    if (fp) {
        fwrite(&bf, sizeof(bf), 1, fp);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, fp);
        fwrite(px, 1, imgSize, fp);
        fclose(fp);
        printf("[shot] %s (%dx%d)\n", path, w, h);
    }
    free(px);
    DeleteObject(bmp);
    ReleaseDC(hwnd, win);
    DeleteDC(mem);
}

static Ihandle *g_dlg, *g_topVbox;

// Same host responsibility main.c carries: grow/recenter the RESIZE=NO
// dialog when section ⑤ folds/expands.
static void fitLive(void) {
    int vw = 0, vh = 0;
    const char *ns;
    HWND h;
    RECT wr, cr;
    int outerW, outerH;
    IupRefresh(g_dlg);
    ns = IupGetAttribute(g_topVbox, "NATURALSIZE");
    if (ns == NULL || sscanf(ns, "%dx%d", &vw, &vh) != 2) return;
    h = (HWND)(intptr_t)IupGetAttribute(g_dlg, "HWND");
    if (h == NULL || !GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return;
    outerW = vw + ((wr.right - wr.left) - cr.right) + 12;
    outerH = vh + ((wr.bottom - wr.top) - cr.bottom) + 12;
    SetWindowPos(h, NULL, 80, 80, outerW, outerH, SWP_NOZORDER | SWP_NOACTIVATE);
    IupRefresh(g_dlg);
}

int main(void) {
    Ihandle *toggle, *start, *body;
    HWND dh;
    int targc = 1;
    char *targv[] = { (char*)"diag", NULL };
    char **targvp = targv;

    printf("NOTE: harness moves the real mouse cursor for ~6s\n");
    IupOpen(&targc, &targvp);
    IupSetGlobal("UTF8MODE", "YES");
    i18nSetLang(LANG_ZH);

    Ihandle *simpleBox = uiSimpleCreate((Icallback)recorder, NULL);
    Ihandle *ocr = uiOcrCreate((Icallback)recorder);
    uiSimpleAppendSection(ocr);
    uiOcrGlueInit();

    toggle = uiOcrEnableToggleForTest();
    start = uiOcrStartButtonForTest();
    body = uiOcrBodyForTest();

    g_topVbox = IupVbox(simpleBox, NULL);
    g_dlg = IupDialog(g_topVbox);
    IupSetAttribute(g_dlg, "RESIZE", "NO");
    IupSetAttribute(g_dlg, "TITLE", "diag");
    IupSetAttribute(g_topVbox, "NCMARGIN", "6x6");
    uiOcrSetLayoutChangedCallback(fitLive);
    IupShowXY(g_dlg, 80, 80);
    pump(20);
    fitLive();
    pump(10);
    dh = (HWND)(intptr_t)IupGetAttribute(g_dlg, "HWND");
    printf("== folded: body VISIBLE=%s FLOATING=%s startHWND=%p enabled=%d\n",
           IupGetAttribute(body, "VISIBLE"), IupGetAttribute(body, "FLOATING"),
           (void*)IupGetAttribute(start, "HWND"), (int)uiOcrEnabled());
    shot(dh, "build/diag1-folded.bmp");

    printf("\n== stage A: real click, NO app context (expect refusal)\n");
    realClick(toggle, "toggle-A");
    pump(10);
    printf("after A: VALUE=%s enabled=%d body VISIBLE=%s\n",
           IupGetAttribute(toggle, "VALUE"), (int)uiOcrEnabled(),
           IupGetAttribute(body, "VISIBLE"));
    shot(dh, "build/diag2-blockedApp.bmp");

    printf("\n== stage B: app context set, real click enables\n");
    uiOcrSetAppContext("diag.exe");
    realClick(toggle, "toggle-B");
    pump(20);
    printf("after B: VALUE=%s enabled=%d body VISIBLE=%s FLOATING=%s startHWND=%p\n",
           IupGetAttribute(toggle, "VALUE"), (int)uiOcrEnabled(),
           IupGetAttribute(body, "VISIBLE"), IupGetAttribute(body, "FLOATING"),
           (void*)IupGetAttribute(start, "HWND"));
    shot(dh, "build/diag3-expanded.bmp");

    printf("\n== stage C: real click on in-block Start (0 rules)\n");
    g_fired = 0;
    realClick(start, "start-C");
    pump(20);
    printf("after C: recorder fired %d time(s)\n", g_fired);
    shot(dh, "build/diag4-start0rules.bmp");

    printf("\n== stage D: direct glue start with 0 rules (preflight evidence)\n");
    {
        BOOL ok = uiOcrGlueStart(0);
        printf("uiOcrGlueStart(0) = %d (FALSE means refused, TRUE means passed)\n", (int)ok);
    }
    pump(10);
    shot(dh, "build/diag5-glue0rules.bmp");

    uiOcrGlueCleanup();
    IupDestroy(g_dlg);
    IupClose();
    printf("== diag done\n");
    return 0;
}
