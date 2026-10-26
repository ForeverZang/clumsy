// Local (non-admin) visual harness for the beginner window: mirrors the
// dialog assembly from main.c (language row + basic/advanced tab headers
// + holder-mounted active page) but never touches WinDivert, and exits
// after a fixed delay so screenshots can be taken. Delete after acceptance.
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <winsock2.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"
#include "i18n.h"
#include "ui_simple.h"
#include "ui_advanced.h"
#if defined(CLUMSY_OCR)
#include "ui_ocr.h"
#include "ui_picker.h"
#endif

Module* modules[MODULE_CNT] = {
    &lagModule, &dropModule, &throttleModule, &dupModule,
    &oodModule, &tamperModule, &resetModule, &bandwidthModule,
};
BOOL parameterized = FALSE;
volatile short sendState = SEND_STATUS_NONE;

static Ihandle *dialog, *vbox, *holderBox, *headerBox;
static Ihandle *simpleBox, *advancedBox, *activePage;
static Ihandle *basicTabBtn, *advTabBtn;
static Ihandle *zhBtn, *enBtn;
static int g_lang = LANG_ZH;
static const char *g_mode = "idle";
static const char *g_shotPath = NULL;
static HDESK g_privateDesk = NULL;
static BOOL g_mapped = FALSE;

// Grab a window client into a 24-bit BMP. Used on a private desktop,
// whose GDI surface renders even while the interactive desktop is
// locked. redraw=FALSE skips the forced repaint (desktop grabs while a
// layered overlay is up must not invalidate the whole screen).
static void saveBmpFlags(HWND hwnd, const char *path, BOOL redraw) {
    RECT rc;
    int w, h, rowSize, imgSize, y;
    HDC win, mem;
    HBITMAP bmp;
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bi;
    FILE *fp;
    unsigned char *pixels;

    GetClientRect(hwnd, &rc);
    w = rc.right; h = rc.bottom;
    if (redraw) {
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    win = GetDC(hwnd);
    mem = CreateCompatibleDC(win);
    bmp = CreateCompatibleBitmap(win, w, h);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, win, 0, 0, SRCCOPY);

    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    rowSize = ((w * 3 + 3) / 4) * 4;
    imgSize = rowSize * h;
    bi.biSizeImage = imgSize;
    pixels = (unsigned char *)malloc(imgSize);
    if (!pixels) { ReleaseDC(hwnd, win); return; }
    GetDIBits(mem, bmp, 0, h, pixels, (BITMAPINFO *)&bi, DIB_RGB_COLORS);

    fp = fopen(path, "wb");
    if (fp) {
        memset(&bf, 0, sizeof(bf));
        bf.bfType = 0x4D42;
        bf.bfOffBits = sizeof(bf) + sizeof(bi);
        bf.bfSize = bf.bfOffBits + imgSize;
        fwrite(&bf, sizeof(bf), 1, fp);
        fwrite(&bi, sizeof(bi), 1, fp);
        for (y = 0; y < h; y++)  /* GetDIBits already gave bottom-up rows */
            fwrite(pixels + y * rowSize, 1, rowSize, fp);
        fclose(fp);
        printf("shot saved: %s (%dx%d)\n", path, w, h);
    }
    free(pixels);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, win);
}

static void saveBmp(HWND hwnd, const char *path) {
    saveBmpFlags(hwnd, path, TRUE);
}

static int noop(Ihandle *ih) { (void)ih; return IUP_DEFAULT; }
static void watchNoop(const char *exe) { (void)exe; }

#define LANG_BTN_ACTIVE_BG  "43 125 233"
#define LANG_BTN_ACTIVE_FG  "255 255 255"
#define LANG_BTN_IDLE_BG    "238 238 242"
#define LANG_BTN_IDLE_FG    "70 70 74"
#define LANG_BTN_IDLE_EDGE  "200 200 205"

static void styleLangButton(Ihandle *btn, BOOL active) {
    const char *bg = active ? LANG_BTN_ACTIVE_BG : LANG_BTN_IDLE_BG;
    const char *fg = active ? LANG_BTN_ACTIVE_FG : LANG_BTN_IDLE_FG;
    IupSetAttribute(btn, "BGCOLOR", bg);
    IupSetAttribute(btn, "HLCOLOR", bg);
    IupSetAttribute(btn, "PSCOLOR", bg);
    IupSetAttribute(btn, "FGCOLOR", fg);
    IupSetAttribute(btn, "TEXTHLCOLOR", fg);
    IupSetAttribute(btn, "TEXTPSCOLOR", fg);
    IupSetAttribute(btn, "BORDERCOLOR",
                    active ? LANG_BTN_ACTIVE_BG : LANG_BTN_IDLE_EDGE);
}

static void syncLangButtons(void) {
    styleLangButton(zhBtn, g_lang == LANG_ZH);
    styleLangButton(enBtn, g_lang == LANG_EN);
}

// Tab headers are two flat buttons. Active = white card merging into the
// page below; idle = gray.
#define TAB_ACTIVE_BG  "255 255 255"
#define TAB_ACTIVE_FG  "32 32 36"
#define TAB_IDLE_BG    "232 232 236"
#define TAB_IDLE_FG    "90 90 96"
#define TAB_EDGE       "180 180 186"

static void styleTabButton(Ihandle *btn, BOOL active) {
    const char *bg = active ? TAB_ACTIVE_BG : TAB_IDLE_BG;
    const char *fg = active ? TAB_ACTIVE_FG : TAB_IDLE_FG;
    IupSetAttribute(btn, "BGCOLOR", bg);
    IupSetAttribute(btn, "HLCOLOR", bg);
    IupSetAttribute(btn, "PSCOLOR", bg);
    IupSetAttribute(btn, "FGCOLOR", fg);
    IupSetAttribute(btn, "TEXTHLCOLOR", fg);
    IupSetAttribute(btn, "TEXTPSCOLOR", fg);
    IupSetAttribute(btn, "BORDERCOLOR", TAB_EDGE);
}

// Extra pixels around the vbox natural size, tuned headlessly so neither
// page clips. NCMARGIN/NCGAP are already inside the vbox natural.
#define VBOX_SLACK_W 12
#define VBOX_SLACK_H 12
// Title bar + borders measured on this Win11 box at RESIZE=NO
// (RASTERSIZE 500x600 -> client 494x571). The post-show pass replaces
// these with the live native chrome; they only set the opening size.
#define CHROME_W_INIT 6
#define CHROME_H_INIT 29

static BOOL parseNatural(Ihandle *ih, int *w, int *h) {
    const char *s = IupGetAttribute(ih, "NATURALSIZE");
    return s != NULL && sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *w < 10000
        && *h > 0 && *h < 10000;
}

// The dialog wraps exactly one page at a time inside holderBox, so the
// vbox natural size is the tight content size (no max-page overflow).
static BOOL computeContentSize(int *outW, int *outH) {
    int vw, vh;
    IupRefresh(dialog);
    if (!parseNatural(vbox, &vw, &vh))
        return FALSE;
    *outW = vw + VBOX_SLACK_W;
    *outH = vh + VBOX_SLACK_H;
    return TRUE;
}

static void fitPreMap(void) {
    int contentW, contentH;
    char raster[32];
    if (!computeContentSize(&contentW, &contentH))
        return;
    snprintf(raster, sizeof(raster), "%dx%d",
             contentW + CHROME_W_INIT, contentH + CHROME_H_INIT);
    IupStoreAttribute(dialog, "RASTERSIZE", raster);
    IupRefresh(dialog);
}

// Post-map fit: in IUP 3.30 storing a new dialog RASTERSIZE after map is
// a no-op, but a native SetWindowPos flows back through WM_SIZE and IUP
// re-lays the children.
static void fitDialogLive(void) {
    int contentW, contentH;
    int chromeW = CHROME_W_INIT, chromeH = CHROME_H_INIT;
    HWND dh;
    RECT wr, cr;
    MONITORINFO mi;
    int outerW, outerH, x, y;
    if (!computeContentSize(&contentW, &contentH))
        return;
    dh = (HWND)(INT_PTR)IupGetAttribute(dialog, "HWND");
    if (dh == NULL || !GetWindowRect(dh, &wr) || !GetClientRect(dh, &cr))
        return;
    chromeW = (wr.right - wr.left) - cr.right;
    chromeH = (wr.bottom - wr.top) - cr.bottom;
    outerW = contentW + chromeW;
    outerH = contentH + chromeH;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(dh, MONITOR_DEFAULTTONEAREST),
                        &mi)) {
        x = mi.rcWork.left
            + (mi.rcWork.right - mi.rcWork.left - outerW) / 2;
        y = mi.rcWork.top
            + (mi.rcWork.bottom - mi.rcWork.top - outerH) / 2;
    } else {
        x = wr.left;
        y = wr.top;
    }
    SetWindowPos(dh, NULL, x, y, outerW, outerH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    IupRefresh(dialog);
}

// IupTabs reports the widest page as its natural size and IUP never
// allocates children below natural width, so a real IupTabs always made
// the basic window reserve the advanced page's 620px and clipped. Mount
// only the active page under holderBox instead; headers are flat buttons.
static void syncTabHeaders(void) {
    styleTabButton(basicTabBtn, activePage == simpleBox);
    styleTabButton(advTabBtn, activePage == advancedBox);
}

// Both pages stay mounted for the dialog's whole life. An
// IupDetach/IupMap round trip on IUP 3.30 mingw6 drops native interactive
// state (the list/radios/val/text come back blank even after IupUnmap),
// so switching never detaches: the inactive page is hidden AND taken out
// of flow with FLOATING=YES, so the holder natural size is just the
// active page and the dialog fit stays per-page.
static void selectTab(Ihandle *page) {
    Ihandle *other;
    if (page == activePage) {
        syncTabHeaders();
        return;
    }
    other = (page == simpleBox) ? advancedBox : simpleBox;
    IupSetAttribute(page, "FLOATING", "NO");
    IupSetAttribute(page, "VISIBLE", "YES");
    IupSetAttribute(other, "FLOATING", "YES");
    IupSetAttribute(other, "VISIBLE", "NO");
    activePage = page;
    syncTabHeaders();
    if (g_mapped) {
        fitDialogLive();
    }
}

static int basicTabCb(Ihandle *ih) {
    (void)ih;
    selectTab(simpleBox);
    return IUP_DEFAULT;
}

static int advTabCb(Ihandle *ih) {
    (void)ih;
    selectTab(advancedBox);
    return IUP_DEFAULT;
}

static int phaseCb(Ihandle *ih) {
    (void)ih;
    IupSetAttribute(ih, "RUN", "NO");
    uiSimpleSetRunning(TRUE, TRUE);
    uiSimpleArmRecovery(30, (Icallback)noop);  // countdown label only
    return IUP_DEFAULT;
}

// "switch" mode: exercise the live basic->advanced tab change.
static int switchTabCb(Ihandle *ih) {
    IupSetAttribute(ih, "RUN", "NO");
    selectTab(advancedBox);
    return IUP_DEFAULT;
}

// "switchback" second beat: advanced -> basic, proving the tab switch
// survives a full round trip without control state loss.
static int switchBackCb(Ihandle *ih) {
    IupSetAttribute(ih, "RUN", "NO");
    selectTab(simpleBox);
    return IUP_DEFAULT;
}

// "data"/"datamb"/"datamax" modes: fill the ring with 55 synthetic seconds
// so the KB/s or MB/s axis scale, gridline labels, polylines, start marker
// and rate-readout reservation render with real magnitudes (SNIFF off).
static int feedDataCb(Ihandle *ih) {
    Traffic *t = uiSimpleTraffic();
    BOOL mb = strcmp(g_mode, "datamb") == 0;
    BOOL max = strcmp(g_mode, "datamax") == 0;
    const UINT64 K = 1024;
    const UINT64 M = 1024 * 1024;
    int i;
    IupSetAttribute(ih, "RUN", "NO");
    uiSimpleSetRunning(TRUE, TRUE);
    for (i = 0; i < 55; i++) {
        double w = fabs(sin(i / 6.5));
        double v = fabs(cos(i / 9.0));
        UINT64 down = max
            ? (UINT64)((900.0 + 99.0 * w) * (double)M)
            : mb
                ? (UINT64)((1.2 + 3.0 * w) * (double)M)
                : (UINT64)((120.0 + 240.0 * w) * (double)K);
        UINT64 up = max
            ? (UINT64)((300.0 + 40.0 * v) * (double)M)
            : mb
                ? (UINT64)((0.3 + 0.5 * v) * (double)M)
                : (UINT64)((35.0 + 55.0 * v) * (double)K);
        if (i == 50 && !max) down = mb ? 7 * M : 460 * K; // spike
        trafficRecord(t, down, FALSE);
        trafficRecord(t, up, TRUE);
        if (i == 39) {
            trafficMarkStart(t); // dashed red column ~15s back
        }
        trafficTick(t);
    }
    IupUpdate(dialog);
    return IUP_DEFAULT;
}

// 60 ring buckets across the three beats so the chart fills edge to edge:
// 24s healthy pre-start, 14s collapsed while affecting (start marker at
// the boundary), 22s recovered after stop. Tick first, then accumulate
// into the fresh live bucket, mirroring the product's 1s timer.
static void feedHealthy(int count, int phase) {
    Traffic *t = uiSimpleTraffic();
    const UINT64 K = 1024;
    int i;
    for (i = 0; i < count; i++) {
        int n = phase + i;
        trafficTick(t);
        trafficRecord(t, (UINT64)((150.0 + 120.0 * fabs(sin(n / 4.0))) * K),
                      FALSE);
        trafficRecord(t, (UINT64)((35.0 + 45.0 * fabs(cos(n / 5.0))) * K),
                      TRUE);
    }
}

static void feedCollapsed(int count) {
    Traffic *t = uiSimpleTraffic();
    const UINT64 K = 1024;
    int i;
    for (i = 0; i < count; i++) {
        trafficTick(t);
        trafficRecord(t, (UINT64)((3.0 + 6.0 * fabs(sin(i / 2.0))) * K), FALSE);
        trafficRecord(t, (UINT64)((0.5 + 1.5 * fabs(cos(i / 3.0))) * K), TRUE);
    }
}

// "lifecycle" mode: visual proxy for spec 7.5 — healthy pre-start line,
// collapse right after the start marker, recovery after stop.
static int lifePreCb(Ihandle *ih) {
    IupSetAttribute(ih, "RUN", "NO");
    feedHealthy(24, 0);
    IupUpdate(dialog);
    return IUP_DEFAULT;
}

static int lifeRunCb(Ihandle *ih) {
    IupSetAttribute(ih, "RUN", "NO");
    uiSimpleSetRunning(TRUE, TRUE);
    trafficMarkStart(uiSimpleTraffic());
    feedCollapsed(14);
    IupUpdate(dialog);
    return IUP_DEFAULT;
}

static int lifeRecoverCb(Ihandle *ih) {
    IupSetAttribute(ih, "RUN", "NO");
    uiSimpleSetRunning(FALSE, FALSE);
    feedHealthy(22, 24);
    IupUpdate(dialog);
    return IUP_DEFAULT;
}

// "langswitch" mode: visual proxy for spec 7.6 — flip language live while
// the running presentation (countdown included) is on screen.
static int langFlipCb(Ihandle *ih) {
    IupSetAttribute(ih, "RUN", "NO");
    g_lang = (g_lang == LANG_ZH) ? LANG_EN : LANG_ZH;
    i18nSetLang(g_lang);
    i18nRetranslate();
    uiSimpleRetranslate();
#if defined(CLUMSY_OCR)
    uiOcrRetranslate();
#endif
    syncLangButtons();
    syncTabHeaders();
    fitDialogLive();
    IupUpdate(dialog);
    return IUP_DEFAULT;
}

static int quitCb(Ihandle *ih) {
    (void)ih;
    if (g_shotPath) {
        HWND hwnd = (HWND)(INT_PTR)IupGetAttribute(dialog, "HWND");
        saveBmp(hwnd, g_shotPath);
    }
    IupExitLoop();
    return IUP_CLOSE;
}

static int showCb(Ihandle *dlg, int state) {
    if (state != IUP_SHOW) return IUP_DEFAULT;
    g_mapped = TRUE;
    fitDialogLive();
    printf("at SHOW: RASTERSIZE=%s CLIENTSIZE=%s vbox nat=%s\n",
           IupGetAttribute(dlg, "RASTERSIZE"),
           IupGetAttribute(dlg, "CLIENTSIZE"),
           IupGetAttribute(vbox, "NATURALSIZE"));
    return IUP_DEFAULT;
}

#if defined(CLUMSY_OCR)
// "ocr" mode: section expanded with two fake groups, sample timeline
// mirror and a CSV path; any other mode mounts it folded and disabled.
static void setupOcr(BOOL expanded) {
    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.exeName, "qa-fixture.exe");
    strcpy(cfg.windowTitle, "QA Fixture 窗口");
    cfg.enabled = expanded;
    cfg.changeDetect = TRUE;
    cfg.intervalMs = 500;
    cfg.regionCount = 2;
    cfg.groupCount = 2;

    strcpy(cfg.regions[0].name, "idle-bar");
    cfg.regions[0].norm.x = 0.10;
    cfg.regions[0].norm.y = 0.90;
    cfg.regions[0].norm.w = 0.50;
    cfg.regions[0].norm.h = 0.08;

    strcpy(cfg.regions[1].name, "reconnect");
    cfg.regions[1].norm.x = 0.35;
    cfg.regions[1].norm.y = 0.40;
    cfg.regions[1].norm.w = 0.30;
    cfg.regions[1].norm.h = 0.20;

    cfg.groups[0].edge = OCR_EDGE_DISAPPEAR;
    cfg.groups[0].confirmFrames = 2;
    cfg.groups[0].lossPercent = 95;
    cfg.groups[0].memberCount = 1;
    cfg.groups[0].members[0].regionIndex = 0;
    strcpy(cfg.groups[0].members[0].keywords, "仓库");

    cfg.groups[1].edge = OCR_EDGE_APPEAR;
    cfg.groups[1].confirmFrames = 1;
    cfg.groups[1].lossPercent = 50;
    cfg.groups[1].memberCount = 1;
    cfg.groups[1].members[0].regionIndex = 1;
    strcpy(cfg.groups[1].members[0].keywords, "重连,reconnect");

    uiOcrSetAppContext("qa-fixture.exe");
    uiOcrPopulate(&cfg);
    if (expanded) {
        char line[256];
        // Same tr() format strings as ocr_glue.c, so shots show the real
        // bilingual log wording.
        uiOcrSetCsvPath("C:\\Users\\qa\\AppData\\Local\\clumsy\\ocr-runs\\ocr-run-20261026-103000.csv");
        snprintf(line, sizeof(line), tr("ocr.log.start"), 0);
        uiOcrAppendLog(line);
        snprintf(line, sizeof(line), tr("ocr.log.trigger"),
                 "group 1", "仓库", 95);
        uiOcrAppendLog(line);
        snprintf(line, sizeof(line), tr("ocr.log.loss"), 95);
        uiOcrAppendLog(line);
        snprintf(line, sizeof(line), tr("ocr.log.unavailable"),
                 "group 2");
        uiOcrAppendLog(line);
        snprintf(line, sizeof(line), tr("ocr.log.trigger"),
                 "group 2", "重连", 50);
        uiOcrAppendLog(line);
        snprintf(line, sizeof(line), tr("ocr.log.loss"), 50);
        uiOcrAppendLog(line);
    }
}

// --- pickwindow / picklist / pickrect headless flows -------------------
//
// clumsy only enumerates windows of processes that own network
// endpoints (procmap), so these modes bind a throwaway UDP socket and
// pick THIS harness process: its main dialog is the unique top-level
// window. picklist adds a second visible window to force the modal
// IupList; pickrect then drives the rubber band through test seams.
static char g_pickExe[128] = "";
static const char *g_overlayShot = NULL;
static SOCKET g_udpSocket = INVALID_SOCKET;
static HWND g_extraWin = NULL;

static void ownExeName(char *buf, size_t cap) {
    char path[MAX_PATH];
    char *base;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    (void)n;
    base = strrchr(path, '\\');
    strncpy(buf, base != NULL ? base + 1 : path, cap - 1);
    buf[cap - 1] = '\0';
}

static void bindUdpEndpoint(void) {
    WSADATA wsa;
    struct sockaddr_in addr;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    g_udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udpSocket == INVALID_SOCKET) return;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(g_udpSocket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(g_udpSocket);
        g_udpSocket = INVALID_SOCKET;
    }
}

static HWND createExtraWindow(void) {
    return CreateWindowExW(WS_EX_APPWINDOW, L"Static",
        L"QA Fixture 二号窗口 (stand-in)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        120, 120, 360, 220, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void setupPickerOcr(void) {
    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.exeName, g_pickExe);
    cfg.enabled = TRUE;
    cfg.changeDetect = TRUE;
    cfg.intervalMs = 500;
    uiOcrSetAppContext(g_pickExe);
    uiOcrPopulate(&cfg);
}

// Fires while uiPickerPickRect's nested loop is open: paint a band in
// the lower dialog area, optionally grab the screen, then accept.
static int dragSimCb(Ihandle *ih) {
    HWND dh = (HWND)(INT_PTR)IupGetAttribute(dialog, "HWND");
    RECT rc;
    POINT a, b;
    IupSetAttribute(ih, "RUN", "NO");
    if (dh == NULL || !GetClientRect(dh, &rc)) return IUP_DEFAULT;
    a.x = rc.right * 10 / 100;  a.y = rc.bottom * 78 / 100;
    b.x = rc.right * 62 / 100;  b.y = rc.bottom * 90 / 100;
    ClientToScreen(dh, &a);
    ClientToScreen(dh, &b);
    uiPickerSetDragForTest(a, b);
    // Grab the overlay canvas itself: on a non-active private desktop
    // the desktop DC does not composite, but per-window DCs render.
    if (g_overlayShot != NULL) {
        HWND ov = uiPickerOverlayHwndForTest();
        if (ov != NULL) saveBmpFlags(ov, g_overlayShot, FALSE);
    }
    if (!uiPickerCompleteDragForTest())
        printf("pickrect: simulated drag rejected\n");
    return IUP_DEFAULT;
}

static int pickerFlowCb(Ihandle *ih) {
    CaptureWindow win;
    IupSetAttribute(ih, "RUN", "NO");
    // Same production entry point as the "Choose window" button:
    // unique window picks directly, two windows open the modal chooser
    // (dismissed by pickerAcceptCb in picklist mode).
    if (!uiOcrChooseWindow(&win)) {
        printf("pickwindow: cancelled or no windows for %s\n", g_pickExe);
        return IUP_DEFAULT;
    }
    printf("pickwindow: picked hwnd=%p title=%s\n",
           (void *)win.hwnd, win.title);
    uiOcrSetPickedWindow(&win);
    fitDialogLive();
    IupUpdate(dialog);

    if (strcmp(g_mode, "pickrect") != 0) return IUP_DEFAULT;
    {
        NormRect norm;
        Ihandle *dragTimer = IupTimer();
        IupSetAttribute(dragTimer, "TIME", "700");
        IupSetCallback(dragTimer, "ACTION_CB", (Icallback)dragSimCb);
        IupSetAttribute(dragTimer, "RUN", "YES");
        if (!uiPickerPickRect(win.hwnd, &norm)) {
            printf("pickrect: cancelled\n");
            return IUP_DEFAULT;
        }
        printf("pickrect: norm=%.3f,%.3f,%.3f,%.3f\n",
               norm.x, norm.y, norm.w, norm.h);
        if (uiOcrAddRegionForTest(&norm)) {
            fitDialogLive();
            IupUpdate(dialog);
        }
    }
    return IUP_DEFAULT;
}

// picklist: grab the open chooser, then close it with OK.
static int pickerAcceptCb(Ihandle *ih) {
    IupSetAttribute(ih, "RUN", "NO");
    if (g_overlayShot != NULL) {
        HWND ch = uiPickerChooserHwndForTest();
        if (ch != NULL) saveBmpFlags(ch, g_overlayShot, FALSE);
    }
    uiPickerAcceptChooserForTest();
    return IUP_DEFAULT;
}

static BOOL isPickerMode(const char *m) {
    return strcmp(m, "pickwindow") == 0
        || strcmp(m, "picklist") == 0
        || strcmp(m, "pickrect") == 0;
}

static void startPickerTimers(void) {
    Ihandle *flow;
    if (strcmp(g_mode, "picklist") == 0) {
        Ihandle *accept = IupTimer();
        IupSetAttribute(accept, "TIME", "1100");
        IupSetCallback(accept, "ACTION_CB", (Icallback)pickerAcceptCb);
        IupSetAttribute(accept, "RUN", "YES");
    }
    flow = IupTimer();
    IupSetAttribute(flow, "TIME", "500");
    IupSetCallback(flow, "ACTION_CB", (Icallback)pickerFlowCb);
    IupSetAttribute(flow, "RUN", "YES");
}
#endif

int main(int argc, char **argv) {
    Ihandle *t;
    BOOL advMode;
    g_lang = (argc > 1 && argv[1][0] == 'e') ? LANG_EN : LANG_ZH;
    int holdMs = (argc > 2) ? atoi(argv[2]) : 3000;

    // argv[3]=="desk" runs on a private desktop (renders while locked);
    // argv[4] saves a client-area BMP on quit; argv[5] selects the mode.
    if (argc > 3 && strcmp(argv[3], "desk") == 0) {
        g_privateDesk = CreateDesktopW(L"IupShotDesktop", NULL, NULL, 0,
                                       GENERIC_ALL, NULL);
        if (g_privateDesk && SetThreadDesktop(g_privateDesk))
            printf("private desktop IupShotDesktop\n");
        else {
            printf("private desktop failed: %lu\n",
                   (unsigned long)GetLastError());
            g_privateDesk = NULL;
        }
    }
    if (argc > 4) g_shotPath = argv[4];
    if (argc > 5) g_mode = argv[5];
    advMode = strcmp(g_mode, "adv") == 0 || strcmp(g_mode, "runadv") == 0;
#if defined(CLUMSY_OCR)
    if (isPickerMode(g_mode)) {
        ownExeName(g_pickExe, sizeof(g_pickExe));
        if (argc > 6 && argv[6][0] != '\0')
            strncpy(g_pickExe, argv[6], sizeof(g_pickExe) - 1);
        if (argc > 7) g_overlayShot = argv[7];
        bindUdpEndpoint();
    }
#endif

    uiAdvancedLoadConfig();
    IupOpen(&argc, &argv);
    IupSetGlobal("UTF8MODE", "YES");
    i18nSetLang(g_lang);

    simpleBox = uiSimpleCreate((Icallback)noop, watchNoop);
    advancedBox = uiAdvancedCreate();
#if defined(CLUMSY_OCR)
    {
        Ihandle *ocrBox = uiOcrCreate((Icallback)noop);
        uiSimpleAppendSection(ocrBox);
        // Folding section ⑤ changes its natural size post-map; the OCR
        // block asks the host to re-fit the RESIZE=NO dialog.
        uiOcrSetLayoutChangedCallback(fitDialogLive);
        if (isPickerMode(g_mode))
            setupPickerOcr();
        else
            setupOcr(strcmp(g_mode, "ocr") == 0);
    }
#endif

    // One header row (mirrors main.c): tabs flush-left, language flush-right,
    // all 26px tall so edges and baseline align.
    basicTabBtn = IupFlatButton(NULL);
    advTabBtn = IupFlatButton(NULL);
    i18nBind(basicTabBtn, "TITLE", "tab.basic");
    i18nBind(advTabBtn, "TITLE", "advanced.toggle");
    IupSetCallback(basicTabBtn, "FLAT_ACTION", (Icallback)basicTabCb);
    IupSetCallback(advTabBtn, "FLAT_ACTION", (Icallback)advTabCb);
    IupSetAttribute(basicTabBtn, "RASTERSIZE", "96x26");
    IupSetAttribute(advTabBtn, "RASTERSIZE", "96x26");

    zhBtn = IupFlatButton("中");
    enBtn = IupFlatButton("EN");
    IupSetCallback(zhBtn, "FLAT_ACTION", (Icallback)noop);
    IupSetCallback(enBtn, "FLAT_ACTION", (Icallback)noop);
    IupSetAttribute(zhBtn, "RASTERSIZE", "36x26");
    IupSetAttribute(enBtn, "RASTERSIZE", "40x26");
    syncLangButtons();

    headerBox = IupHbox(basicTabBtn, advTabBtn, IupFill(), zhBtn, enBtn, NULL);
    IupSetAttribute(headerBox, "GAP", "4");

    // Both pages are mounted permanently; selectTab only flips VISIBLE
    // and pins the holder to the active page's natural size.
    holderBox = IupVbox(simpleBox, advancedBox, NULL);
    vbox = IupVbox(headerBox, holderBox, NULL);

    dialog = IupDialog(vbox);
    IupSetCallback(dialog, "SHOW_CB", (Icallback)showCb);
    IupSetAttribute(dialog, "TITLE", "游戏流量控制器 v0.3.1");
    IupSetAttribute(dialog, "RASTERSIZE", "500x600");
    IupSetAttribute(dialog, "RESIZE", "NO");
    IupSetAttribute(vbox, "ALIGNMENT", "ALEFT");
    IupSetAttribute(vbox, "NCMARGIN", "6x6");
    IupSetAttribute(vbox, "NCGAP", "6x4");
    IupSetAttribute(holderBox, "EXPAND", "YES");

    // Initial selection pre-map but with the dialog tree assembled, so
    // the holder pin can measure the page's natural size.
    g_mapped = FALSE;
    activePage = NULL;
    selectTab(advMode ? advancedBox : simpleBox);

    fitPreMap();
    IupShowXY(dialog, IUP_CENTER, IUP_CENTER);
    g_mapped = TRUE;
    fitDialogLive();

#if defined(CLUMSY_OCR)
    if (isPickerMode(g_mode)) {
        if (strcmp(g_mode, "picklist") == 0) g_extraWin = createExtraWindow();
        startPickerTimers();
    }
#endif

    if (strcmp(g_mode, "run") == 0 || strcmp(g_mode, "runadv") == 0) {
        Ihandle *phase = IupTimer();
        IupSetAttribute(phase, "TIME", "300");
        IupSetCallback(phase, "ACTION_CB", (Icallback)phaseCb);
        IupSetAttribute(phase, "RUN", "YES");
    }
    if (strcmp(g_mode, "switch") == 0 || strcmp(g_mode, "switchback") == 0) {
        Ihandle *phase = IupTimer();
        IupSetAttribute(phase, "TIME", "450");
        IupSetCallback(phase, "ACTION_CB", (Icallback)switchTabCb);
        IupSetAttribute(phase, "RUN", "YES");
    }
    if (strcmp(g_mode, "switchback") == 0) {
        Ihandle *phase2 = IupTimer();
        IupSetAttribute(phase2, "TIME", "1000");
        IupSetCallback(phase2, "ACTION_CB", (Icallback)switchBackCb);
        IupSetAttribute(phase2, "RUN", "YES");
    }
    if (strcmp(g_mode, "data") == 0 || strcmp(g_mode, "datamb") == 0
        || strcmp(g_mode, "datamax") == 0) {
        Ihandle *phase = IupTimer();
        IupSetAttribute(phase, "TIME", "400");
        IupSetCallback(phase, "ACTION_CB", (Icallback)feedDataCb);
        IupSetAttribute(phase, "RUN", "YES");
    }
    if (strcmp(g_mode, "lifecycle") == 0) {
        struct { const char *time; Icallback cb; } beats[] = {
            { "250", (Icallback)lifePreCb },
            { "650", (Icallback)lifeRunCb },
            { "1050", (Icallback)lifeRecoverCb },
        };
        size_t i;
        for (i = 0; i < sizeof(beats) / sizeof(beats[0]); i++) {
            Ihandle *phase = IupTimer();
            IupStoreAttribute(phase, "TIME", beats[i].time);
            IupSetCallback(phase, "ACTION_CB", beats[i].cb);
            IupSetAttribute(phase, "RUN", "YES");
        }
    }
    if (strcmp(g_mode, "langswitch") == 0) {
        Ihandle *phase = IupTimer();
        IupSetAttribute(phase, "TIME", "300");
        IupSetCallback(phase, "ACTION_CB", (Icallback)phaseCb);
        IupSetAttribute(phase, "RUN", "YES");
        phase = IupTimer();
        IupSetAttribute(phase, "TIME", "800");
        IupSetCallback(phase, "ACTION_CB", (Icallback)langFlipCb);
        IupSetAttribute(phase, "RUN", "YES");
    }

    t = IupTimer();
    {
        char hold[16];
        snprintf(hold, sizeof(hold), "%d", holdMs);
        IupStoreAttribute(t, "TIME", hold);
    }
    IupSetCallback(t, "ACTION_CB", (Icallback)quitCb);
    IupSetAttribute(t, "RUN", "YES");

    IupMainLoop();
    IupClose();
    if (g_privateDesk) CloseDesktop(g_privateDesk);
    return 0;
}
