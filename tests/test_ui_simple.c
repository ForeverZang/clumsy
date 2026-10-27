// Unit tests for the beginner window's non-widget logic: percent clamp,
// countdown math, app-list line formatting, and the chart accounting
// routing (SNIFF raw bytes while idle, reinjected bytes while affecting,
// ownership-gated in both modes).
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "common.h"
#include "targeting.h"
#include "i18n.h"
#include "procmap.h"
#include "traffic.h"
#include "ui_simple.h"
#include "ui_ocr.h"
#include "ui_ocr_groups.h"
#include "ui_picker.h"
#include "ocr_app.h"
#include "iup.h"
#include "iupcontrols.h"

// Marker callback so pointer identity can be checked on the widget.
static int g_fakeFired = 0;
static int fakeActionCb(Ihandle *ih) { (void)ih; g_fakeFired = 1;
                                       return IUP_DEFAULT; }

// Post-lock HWND-recycling driver: a one-shot timer that runs inside the
// picker's IupPopup modal loop and emulates the locked handle being
// recycled onto another process mid-pick.
static HWND g_recycleHwnd;
static int g_recyclePhase;

// Production invariant: the picker only opens while clumsy itself is the
// foreground app (the user clicked its button), so the popup bar owns the
// foreground outright. A test exe launched from a console does not, and
// Windows then keeps granting/revoking pending foreground requests
// asynchronously, which moves z-order under our assertions. Recreate the
// production precondition explicitly.
static void forceBarForegroundForTest(HWND bar) {
    DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD me = GetCurrentThreadId();
    if (fgTid != me && fgTid != 0) AttachThreadInput(me, fgTid, TRUE);
    SetForegroundWindow(bar);
    BringWindowToTop(bar);
    if (fgTid != me && fgTid != 0) AttachThreadInput(me, fgTid, FALSE);
}

static int recyclePickTimerCb(Ihandle *t) {
    IupSetAttribute(t, "RUN", "NO");
    // While the overlay is up, the bottom toolbar placement seam must
    // keep a DPI-grown bar (720x70 at 150%) fully inside the overlay
    // monitor's work area: horizontally centered, 12px above its bottom.
    // The old hard-coded 600x46 placement both clipped the buttons and
    // could run the bar under the taskbar.
    {
        HWND ov = uiPickerOverlayHwndForTest();
        HWND bar;
        MONITORINFO mi;
        POINT bp;
        RECT br;
        long ww, wh;
        LONG_PTR style;
        assert(ov != NULL);
        mi.cbSize = sizeof(mi);
        assert(GetMonitorInfoW(MonitorFromWindow(ov,
                            MONITOR_DEFAULTTONEAREST), &mi));
        assert(uiPickerBarPlacementForTest(720, 70, &bp));
        assert(bp.x >= mi.rcWork.left);
        assert(bp.y >= mi.rcWork.top);
        assert(bp.x + 720 <= mi.rcWork.right);
        assert(bp.y + 70 <= mi.rcWork.bottom);
        ww = mi.rcWork.right - mi.rcWork.left;
        wh = mi.rcWork.bottom - mi.rcWork.top;
        if (ww >= 720) {
            assert(bp.x == mi.rcWork.left + (ww - 720) / 2);
        }
        if (wh >= 70 + 12) {
            assert(bp.y == mi.rcWork.bottom - 70 - 12);
        }
        // Framework finding (probed on IUP 3.30 Win64, Win11): a dialog
        // with BORDER/TITLEBAR/MENUBOX/RESIZE=NO still keeps WS_CAPTION
        // while MINBOX/MAXBOX stay at their YES defaults, leaving a ~29px
        // white caption strip and a 6px-tall client — the "toolbar shows
        // as an empty white pill" bug. MINBOX/MAXBOX=NO strip it, giving
        // a zero-chrome popup whose outer rect equals its client rect.
        bar = uiPickerBarHwndForTest();
        assert(bar != NULL);
        style = GetWindowLongPtrW(bar, GWL_STYLE);
        assert((style & WS_CAPTION) == 0);
        assert(GetWindowRect(bar, &br));
        {
            RECT bc;
            assert(GetClientRect(bar, &bc));
            assert((br.right - br.left) == bc.right);
            assert((br.bottom - br.top) == bc.bottom);
        }
        // The real popup (measured natural size, not the 720x70 seam
        // probe above) must sit fully inside the overlay work area.
        assert(br.left >= mi.rcWork.left);
        assert(br.top >= mi.rcWork.top);
        assert(br.right <= mi.rcWork.right);
        assert(br.bottom <= mi.rcWork.bottom);
        // Recreate the production precondition (clumsy itself is the
        // foreground app when the picker opens) so the activation pins
        // below are deterministic in a console-launched test exe.
        forceBarForegroundForTest(bar);
    }
    assert(uiPickerOverlayHwndForTest() != NULL);
    // Layered windows hit-test only pixels with non-zero alpha: alpha-0
    // pixels are mouse-transparent. The locked window interior was fully
    // cleared, so button-downs and mouse-moves there fell through to the
    // app underneath (its text got drag-selected) and a band could only
    // start from a painted pixel such as the frame edge. The interior
    // must carry the smallest non-zero alpha instead: WindowFromPoint at
    // the locked client center has to hit OUR overlay, not the fixture.
    {
        POINT c0 = {0, 0}, c;
        RECT cr;
        HWND hit;
        assert(GetClientRect(g_recycleHwnd, &cr));
        c = c0;
        ClientToScreen(g_recycleHwnd, &c);
        c.x += cr.right / 2;
        c.y += cr.bottom / 2;
        hit = WindowFromPoint(c);
        assert(hit != NULL);
        assert(GetAncestor(hit, GA_ROOT)
               == uiPickerOverlayHwndForTest());
    }
    // Regression pins for "toolbar buttons dead after drawing a band":
    // a click on the overlay must never activate or focus this
    // full-work-area topmost window, otherwise it rises above the
    // toolbar popup and every later bar click hits the overlay and
    // starts a stray band (the phantom band at the bar edge seen on the
    // shipped build). Proven end-to-end with injected real input:
    // NOACTIVATE + MA_NOACTIVATE alone were insufficient, because the
    // WM_LBUTTONDOWN handler's SetFocus activated and raised the window
    // anyway.
    //
    // Pin three runs while the lock is ALIVE: overlay-client (10,10) is
    // the work-area origin +10, well outside the locked fixture rect, so
    // the click neither locks a window nor starts a band -- it isolates
    // the activation/focus behavior without tearing down the popup.
    {
        HWND ov2 = uiPickerOverlayHwndForTest();
        HWND bar2 = uiPickerBarHwndForTest();
        LONG_PTR ex2;
        LRESULT ma;
        assert(ov2 != NULL && bar2 != NULL);
        (void)bar2;
        // Pin one: WS_EX_NOACTIVATE on the window style.
        ex2 = GetWindowLongPtrW(ov2, GWL_EXSTYLE);
        assert(ex2 & WS_EX_NOACTIVATE);
        // Pin two: WM_MOUSEACTIVATE answers MA_NOACTIVATE — no activation,
        // but the click message is still delivered (not ...ANDEAT).
        ma = SendMessageW(ov2, WM_MOUSEACTIVATE, (WPARAM)ov2,
                          MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        assert(ma == MA_NOACTIVATE);
        // Pin three: the button-down handler must not SetFocus the
        // overlay — focusing a top-level window activates and raises it
        // even when WM_MOUSEACTIVATE refused activation. The lock is
        // alive and the point is outside the locked client rect, so the
        // click changes no picker state; it isolates focus.
        SendMessageW(ov2, WM_LBUTTONDOWN, (WPARAM)MK_LBUTTON,
                     MAKELPARAM(10, 10));
        assert(GetActiveWindow() != ov2);
        assert(GetForegroundWindow() != ov2);
        assert(GetFocus() != ov2);
    }
    // Same HWND value, different current owner: the pinned pid no longer
    // matches, so every gate must fail closed.
    uiPickerSetLockedPidForTest(0x0DEFACEDu);
    if (g_recyclePhase == 0) {
        RECT r;
        POINT a, b;
        GetWindowRect(g_recycleHwnd, &r);
        a.x = r.left + (r.right - r.left) / 8;
        a.y = r.top + (r.bottom - r.top) / 8;
        b.x = r.left + (r.right - r.left) * 6 / 8;
        b.y = r.top + (r.bottom - r.top) * 6 / 8;
        uiPickerSetDragForTest(a, b);
        // After a drag's present() cycle the toolbar must still be the
        // topmost clickable window over its own rect: a click on a
        // toolbar button must not land on the full-work-area overlay
        // (which would start another band instead of pressing the
        // button). WindowFromPoint here must root at the toolbar HWND.
        {
            HWND bar2 = uiPickerBarHwndForTest();
            RECT b2;
            POINT bc2;
            HWND hit2;
            assert(bar2 != NULL);
            assert(GetWindowRect(bar2, &b2));
            bc2.x = (b2.left + b2.right) / 2;
            bc2.y = (b2.top + b2.bottom) / 2;
            hit2 = WindowFromPoint(bc2);
            assert(hit2 != NULL);
            assert(GetAncestor(hit2, GA_ROOT) == bar2);
        }
        assert(!uiPickerCompleteDragForTest()); // band refused, no rect
        assert(uiPickerFinishForTest() == UI_PICKER_WINDOW_DIED);
    } else {
        // The 300 ms death backstop detects the recycled owner too.
        assert(uiPickerTimerTickForTest());
    }
    return IUP_DEFAULT;
}

// Minimal valid IPv4 TCP packet (20 IP + 20 TCP) for tuple parsing.
static unsigned char g_tcpPacket[40] = {
    0x45, 0x00, 0x00, 0x28, 0, 0, 0, 0, 0, 6, 0, 0,
    10, 0, 0, 1, 10, 0, 0, 2,
    0, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x50, 0, 0, 0, 0, 0, 0, 0
};
static unsigned char g_udpPacket[28] = {
    0x45, 0x00, 0x00, 0x1c, 0, 0, 0, 0, 0, 17, 0, 0,
    10, 0, 0, 1, 10, 0, 0, 2,
    0, 80, 0, 53, 0, 8, 0, 0
};

static BOOL fakeOwnerHit(const PacketNode *node, UINT32 *pid) {
    (void)node;
    *pid = 42;
    return TRUE;
}
static BOOL fakeOwnerMiss(const PacketNode *node, UINT32 *pid) {
    (void)node;
    (void)pid;
    return FALSE;
}
static BOOL fakeName(UINT32 pid, char *out, size_t cap) {
    (void)pid;
    strncpy(out, "a.exe", cap - 1);
    out[cap - 1] = '\0';
    return TRUE;
}

// Headless OCR app coordinator fakes (section 0c).
static int g_appLossCalls;
static int g_appLastLoss;
static int g_appFactoryCalls;
static void appCountingLoss(int percent, void *user) {
    (void)user;
    g_appLossCalls++;
    g_appLastLoss = percent;
}
static OcrEngine *appFailingFactory(const char *dir,
                                    char err[MSG_BUFSIZE]) {
    (void)dir;
    g_appFactoryCalls++;
    if (err) {
        strncpy(err, "fake: model files unavailable", MSG_BUFSIZE - 1);
    }
    return NULL;
}

static void liveRates(UINT64 *down, UINT64 *up) {
    UINT64 d[TRAFFIC_BUCKETS], u[TRAFFIC_BUCKETS];
    trafficSeries(uiSimpleTraffic(), d, u);
    *down = d[TRAFFIC_BUCKETS - 1];
    *up = u[TRAFFIC_BUCKETS - 1];
}

int main(void) {
    WINDIVERT_ADDRESS out, in;
    memset(&out, 0, sizeof(out));
    memset(&in, 0, sizeof(in));
    out.Outbound = 1;

    // 0. Main action button: IupFlatButton reserves ACTION as its internal
    // paint routine; the click must be wired through FLAT_ACTION. Binding
    // the user callback on ACTION both kills painting (white band) and
    // makes the button fire its callback on every repaint.
    {
        int targc = 1;
        char *argvStore[] = { (char *)"test_ui_simple", NULL };
        char **targv = argvStore;
        Ihandle *btn, *box;
        IupOpen(&targc, &targv);
        IupSetGlobal("UTF8MODE", "YES");
        // The region/group tables are IupMatrix controls from iupcontrols.
        assert(IupControlsOpen() == IUP_NOERROR);
        box = uiSimpleCreate((Icallback)fakeActionCb, NULL);
        btn = uiSimpleActionButton();
        assert(btn != NULL);
        {
            // Click is wired on FLAT_ACTION and reaches the impl.
            Icallback click = IupGetCallback(btn, "FLAT_ACTION");
            assert(click != NULL);
            g_fakeFired = 0;
            click(btn);
            assert(g_fakeFired == 1);
            // ACTION still belongs to FlatButton's own paint routine;
            // the click wrapper must not have hijacked it.
            assert(IupGetCallback(btn, "ACTION") != click);
            assert(IupGetCallback(btn, "ACTION") != NULL);

            // Countdown label reserves layout width up front: it starts
            // empty and is filled post-map once per second, and canvas
            // FlatLabels do not re-layout on TITLE changes, so without a
            // reserved size the running-state countdown clips to ~0 px.
            {
                Ihandle *cd = uiSimpleCountdownLabel();
                const char *rs = IupGetAttribute(cd, "RASTERSIZE");
                int rw = 0;
                assert(cd != NULL);
                assert(rs != NULL);
                sscanf(rs, "%d", &rw);
                assert(rw >= 90);
            }
            // Rate readouts grow from "0 B/s" to "999.9 MB/s" pairs after
            // map; canvas FlatLabels do not re-layout on TITLE changes, so
            // both must reserve the wide-text width up front or the tail
            // (the upload rate) clips past the frame's right edge.
            {
                Ihandle *beforeLbl = NULL;
                Ihandle *nowLbl = NULL;
                const char *rs1;
                const char *rs2;
                int w1 = 0;
                int w2 = 0;
                uiSimpleRateLabels(&beforeLbl, &nowLbl);
                assert(beforeLbl != NULL && nowLbl != NULL);
                rs1 = IupGetAttribute(beforeLbl, "RASTERSIZE");
                rs2 = IupGetAttribute(nowLbl, "RASTERSIZE");
                assert(rs1 != NULL && rs2 != NULL);
                sscanf(rs1, "%d", &w1);
                sscanf(rs2, "%d", &w2);
                assert(w1 >= 200);
                assert(w2 >= 200);
            }
        }
        // 0b. Section ⑤ OCR automation: create/defaults, app-context
        // gate, config roundtrip via row widgets, invalid-row rejection,
        // external loss seam. Mounts into the beginner vbox like main does.
        {
            Ihandle *ocr = uiOcrCreate();
            Ihandle *enToggle = uiOcrEnableToggleForTest();
            Ihandle *pick = uiOcrChooseButtonForTest();
            Ihandle *addBtn = uiOcrAddButtonForTest();
            Ihandle *ocrBody = uiOcrBodyForTest();
            Ihandle *gateErr = uiOcrGateErrorForTest();
            Icallback enCb;
            // FlatToggle FLAT_ACTION receives (Ihandle*, int state);
            // Icallback is the one-arg generic type, so invoke through
            // this cast.
            typedef int (*ToggleActionFn)(Ihandle *, int);
            OcrConfig cfg, got;
            OcrRegionDef *d0, *d1;
            OcrGroup *g0, *g1;
            // Dynamic matrix text comes from VALUE_CB (no stored cell
            // attribute); invoke the callback the way paint does.
            const char *matCell(Ihandle *m, int lin, int col) {
                typedef char *(*MatValueCb)(Ihandle *, int, int);
                MatValueCb cb = (MatValueCb)IupGetCallback(m, "VALUE_CB");
                return cb != NULL ? cb(m, lin, col) : NULL;
            }

            assert(ocr != NULL);
            assert(enToggle != NULL && ocrBody != NULL);
            // Window/region buttons are Task 9 placeholders in Task 8.
            assert(pick != NULL && addBtn != NULL);
            assert(IupGetAttribute(pick, "ACTIVE") == NULL
                   || strcmp(IupGetAttribute(pick, "ACTIVE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"), "NO") == 0);
            // Defaults: not enabled, body folded, 500ms interval, change
            // detection on.
            assert(uiOcrEnabled() == FALSE);
            assert(strcmp(IupGetAttribute(ocrBody, "VISIBLE"), "NO") == 0);
            // The gate error lives OUTSIDE the folded body, but starts
            // hidden and out of natural-size flow.
            assert(gateErr != NULL);
            assert(strcmp(IupGetAttribute(gateErr, "VISIBLE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(gateErr, "FLOATING"), "YES")
                   == 0);

            // Folding also takes the body out of natural-size flow
            // (FLOATING=YES), like the tab pages, so the dialog fit
            // shrinks with the section.
            assert(strcmp(IupGetAttribute(ocrBody, "FLOATING"), "YES") == 0);

            // Two fixed-height IupMatrix tables start empty and stay a
            // constant pixel height regardless of how many entries later
            // land in the model (internal scrolling). Framework finding
            // (probed on IUP 3.30 Win64): NUMLIN_VISIBLE is clamped to
            // NUMLIN and reads -1 before map, so an empty table cannot
            // advertise visible rows through that attribute. The fixed
            // height is pinned as a height-only RASTERSIZE "0xH"
            // (17px header + 25px/row -> 4 rows 117px, 3 rows 92px).
            {
                int pinH(const char *rs) {
                    const char *x = strchr(rs, 'x');
                    return x != NULL ? atoi(x + 1) : -1;
                }
                Ihandle *rm0 = uiOcrRegionsMatrixForTest();
                Ihandle *gm0 = uiOcrGroupsMatrixForTest();
                assert(rm0 != NULL && gm0 != NULL);
                assert(IupGetInt(rm0, "NUMLIN") == 0);
                assert(IupGetInt(gm0, "NUMLIN") == 0);
                assert(pinH(IupGetAttribute(rm0, "RASTERSIZE")) == 117);
                assert(pinH(IupGetAttribute(gm0, "RASTERSIZE")) == 92);
            }

            // The region and group tables sit side by side (hbox): the
            // enabled OCR block must grow horizontally, not stack into a
            // very tall window. Both frames stretch to share the width.
            {
                Ihandle *root = uiOcrGroupsRootForTest();
                Ihandle *rf, *gf;
                assert(root != NULL);
                assert(strcmp(IupGetClassName(root), "hbox") == 0);
                assert(IupGetChildCount(root) == 2);
                rf = IupGetChild(root, 0);
                gf = IupGetChild(root, 1);
                assert(strcmp(IupGetClassName(rf), "frame") == 0);
                assert(strcmp(IupGetClassName(gf), "frame") == 0);
                assert(strcmp(IupGetAttribute(rf, "EXPAND"),
                              "HORIZONTAL") == 0);
                assert(strcmp(IupGetAttribute(gf, "EXPAND"),
                              "HORIZONTAL") == 0);
            }

            // IupFlatToggle mirrors IupFlatButton: ACTION is reserved as
            // its internal canvas paint routine. The user click must be
            // wired through FLAT_ACTION; hijacking ACTION both blanks the
            // checkbox/title repaint and makes the widget fire the user
            // callback with a garbage second arg on every internal
            // map/paint pass.
            enCb = IupGetCallback(enToggle, "FLAT_ACTION");
            assert(enCb != NULL);
            assert(IupGetCallback(enToggle, "ACTION") != enCb);
            assert(IupGetCallback(enToggle, "ACTION") != NULL);

            // Before map the section must stay disarmed even if the
            // toggle's VALUE is ON (defense for any internal path that
            // reaches the callback pre-map).
            uiOcrSetAppContext(NULL);
            IupSetAttribute(enToggle, "VALUE", "ON");
            ((ToggleActionFn)enCb)(enToggle, 1);
            assert(uiOcrEnabled() == FALSE);
            assert(strcmp(IupGetAttribute(ocrBody, "VISIBLE"), "NO") == 0);

            // Post-map the callback is live. "All applications" context:
            // trying to enable is refused, toggle flips back off. The
            // explanation must render OUTSIDE the (still folded) body or
            // the click looks completely dead to the user.
            uiOcrSetMappedForTest(TRUE);
            ((ToggleActionFn)enCb)(enToggle, 1);
            assert(uiOcrEnabled() == FALSE);
            assert(strcmp(IupGetAttribute(enToggle, "VALUE"), "OFF") == 0);
            assert(strcmp(IupGetAttribute(ocrBody, "VISIBLE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(ocrBody, "FLOATING"), "YES") == 0);
            assert(strcmp(IupGetAttribute(gateErr, "VISIBLE"), "YES") == 0);
            assert(strcmp(IupGetAttribute(gateErr, "FLOATING"), "NO") == 0);
            assert(strcmp(IupGetAttribute(gateErr, "TITLE"),
                          tr("ocr.blockedApp")) == 0);

            // A concrete app: enable expands the body; collected config
            // carries the context exe with default parameters. The
            // callback trusts the widget VALUE, not its int argument
            // (the historical bug was the callback being bound to
            // ACTION, whose internal invocations passed a garbage
            // second arg, observed 0x5CB5C640, during map).
            uiOcrSetAppContext("qa-fixture.exe");
            IupSetAttribute(enToggle, "VALUE", "ON");
            ((ToggleActionFn)enCb)(enToggle, 0);
            assert(uiOcrEnabled() == TRUE);
            assert(strcmp(IupGetAttribute(ocrBody, "VISIBLE"), "YES") == 0);
            assert(strcmp(IupGetAttribute(ocrBody, "FLOATING"), "NO") == 0);
            // A real enable hides the gate warning; the green bar in
            // the main area is the sole Start (already wired in main).
            assert(strcmp(IupGetAttribute(gateErr, "VISIBLE"), "NO") == 0);

            // Disabling again folds the block and hides any gate text.
            IupSetAttribute(enToggle, "VALUE", "OFF");
            ((ToggleActionFn)enCb)(enToggle, 0);
            assert(uiOcrEnabled() == FALSE);
            assert(strcmp(IupGetAttribute(ocrBody, "VISIBLE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(gateErr, "VISIBLE"), "NO") == 0);

            // Re-enable for the config round-trip cases below.
            IupSetAttribute(enToggle, "VALUE", "ON");
            ((ToggleActionFn)enCb)(enToggle, 0);
            assert(uiOcrEnabled() == TRUE);
            memset(&got, 0, sizeof(got));
            assert(uiOcrGetConfig(&got));
            assert(got.enabled == TRUE);
            assert(strcmp(got.exeName, "qa-fixture.exe") == 0);
            assert(got.intervalMs == 500);
            assert(got.changeDetect == TRUE);
            assert(got.regionCount == 0);
            assert(got.groupCount == 0);

            // Two regions + two groups round-trip through the fixed
            // tables; group 0 is a real two-member AND group (R1 "就绪"
            // AND R2 "仓库"), group 1 stays single-member.
            memset(&cfg, 0, sizeof(cfg));
            strcpy(cfg.exeName, "qa-fixture.exe");
            strcpy(cfg.windowTitle, "QA Fixture 窗口");
            cfg.intervalMs = 500;
            cfg.changeDetect = TRUE;
            cfg.regionCount = 2;
            cfg.groupCount = 2;
            d0 = &cfg.regions[0];
            strcpy(d0->name, "idle-bar");
            d0->norm.x = 0.1; d0->norm.y = 0.9;
            d0->norm.w = 0.5; d0->norm.h = 0.08;
            d1 = &cfg.regions[1];
            strcpy(d1->name, "reconnect");
            d1->norm.x = 0.2; d1->norm.y = 0.3;
            d1->norm.w = 0.3; d1->norm.h = 0.2;
            g0 = &cfg.groups[0];
            g0->edge = OCR_EDGE_DISAPPEAR;
            g0->confirmFrames = 2;
            g0->lossPercent = 95;
            g0->memberCount = 2;
            g0->members[0].regionIndex = 0;
            strcpy(g0->members[0].keywords, "就绪");
            g0->members[1].regionIndex = 1;
            strcpy(g0->members[1].keywords, "仓库");
            g1 = &cfg.groups[1];
            g1->edge = OCR_EDGE_APPEAR;
            g1->confirmFrames = 1;
            g1->lossPercent = 50;
            g1->memberCount = 1;
            g1->members[0].regionIndex = 1;
            strcpy(g1->members[0].keywords, "重连,reconnect");
            assert(uiOcrPopulate(&cfg));
            {
                Ihandle *rmT = uiOcrRegionsMatrixForTest();
                Ihandle *gmT = uiOcrGroupsMatrixForTest();
                assert(IupGetInt(rmT, "NUMLIN") == 2);
                assert(IupGetInt(gmT, "NUMLIN") == 2);
            }

            // The group-table condition column summarizes members in
            // table language: zh "R1.. 且 R2.." (first keyword before a
            // comma), English after a live language switch (" and ").
            {
                char sum[128];
                const char *cell;
                Ihandle *rmT = uiOcrRegionsMatrixForTest();
                Ihandle *gmT = uiOcrGroupsMatrixForTest();
                // g_lang defaults to EN in a headless test (no i18n
                // detection); pin ZH explicitly for the zh assertions.
                i18nSetLang(LANG_ZH);
                uiOcrGroupsSummary(g0, sum, sizeof(sum));
                assert(strstr(sum, "R1") != NULL);
                assert(strstr(sum, "且") != NULL);
                assert(strstr(sum, "R2") != NULL);
                // First group row, summary column (col 0 is the row
                // header); the region-table position cell shows the
                // two-decimal "x, y w×h" format.
                cell = matCell(gmT, 1, 1);
                assert(cell != NULL);
                assert(strstr(cell, "R1") && strstr(cell, "且")
                       && strstr(cell, "R2"));
                cell = matCell(rmT, 1, 2);
                assert(cell != NULL);
                assert(strstr(cell, "0.10") && strstr(cell, "0.08"));
                i18nSetLang(LANG_EN);
                uiOcrGroupsRetranslate();
                cell = matCell(gmT, 1, 1);
                assert(cell != NULL && strstr(cell, " and ") != NULL);
                uiOcrGroupsSummary(g0, sum, sizeof(sum));
                assert(strstr(sum, " and ") != NULL);
                i18nSetLang(LANG_ZH);
                uiOcrGroupsRetranslate();
            }

            memset(&got, 0, sizeof(got));
            assert(uiOcrGetConfig(&got));
            assert(got.regionCount == 2);
            assert(got.groupCount == 2);
            assert(strcmp(got.regions[0].name, "idle-bar") == 0);
            assert(fabs(got.regions[0].norm.x - 0.1) < 1e-6);
            assert(fabs(got.regions[0].norm.y - 0.9) < 1e-6);
            assert(fabs(got.regions[0].norm.w - 0.5) < 1e-6);
            assert(fabs(got.regions[0].norm.h - 0.08) < 1e-6);
            assert(got.groups[0].edge == OCR_EDGE_DISAPPEAR);
            assert(got.groups[0].confirmFrames == 2);
            assert(got.groups[0].lossPercent == 95);
            assert(got.groups[0].memberCount == 2);
            assert(got.groups[0].members[0].regionIndex == 0);
            assert(strcmp(got.groups[0].members[0].keywords,
                          "就绪") == 0);
            assert(got.groups[0].members[1].regionIndex == 1);
            assert(strcmp(got.groups[0].members[1].keywords,
                          "仓库") == 0);
            assert(got.groups[1].memberCount == 1);
            assert(got.groups[1].members[0].regionIndex == 1);
            assert(strcmp(got.groups[1].members[0].keywords,
                          "重连,reconnect") == 0);
            assert(got.groups[1].edge == OCR_EDGE_APPEAR);
            assert(got.groups[1].confirmFrames == 1);
            assert(got.groups[1].lossPercent == 50);
            assert(strcmp(got.windowTitle, "QA Fixture 窗口") == 0);
            assert(strcmp(got.exeName, "qa-fixture.exe") == 0);

            // The same model comes back through the module-level getter.
            {
                OcrRegionDef mr[OCR_MAX_REGIONS];
                OcrGroup mg[OCR_MAX_GROUPS];
                size_t mrc = 99, mgc = 99;
                uiOcrGroupsGetModel(mr, &mrc, mg, &mgc);
                assert(mrc == 2 && mgc == 2);
                assert(strcmp(mr[1].name, "reconnect") == 0);
                assert(mg[0].memberCount == 2);
                assert(mg[0].members[0].regionIndex == 0);
                assert(mg[0].members[1].regionIndex == 1);
            }

            // Invalid group drafts are refused on the same red-label
            // channel and never enter the table: no members, empty
            // keywords, bad frames/loss, out-of-range region.
            {
                Ihandle *errLbl = uiOcrErrorLabelForTest();
                OcrGroup bad;
                const char *et;
                memset(&bad, 0, sizeof(bad));
                bad.edge = OCR_EDGE_DISAPPEAR;
                bad.confirmFrames = 2;
                bad.lossPercent = 95;
                assert(errLbl != NULL);
                uiOcrShowError("");
                assert(!uiOcrGroupsCommitGroupForTest(&bad, -1));
                et = IupGetAttribute(errLbl, "TITLE");
                assert(et != NULL && et[0] != '\0');
                bad.memberCount = 1;
                bad.members[0].regionIndex = 0;
                bad.members[0].keywords[0] = '\0';
                uiOcrShowError("");
                assert(!uiOcrGroupsCommitGroupForTest(&bad, -1));
                et = IupGetAttribute(errLbl, "TITLE");
                assert(et != NULL && et[0] != '\0');
                strcpy(bad.members[0].keywords, "就绪");
                bad.confirmFrames = 0;
                uiOcrShowError("");
                assert(!uiOcrGroupsCommitGroupForTest(&bad, -1));
                et = IupGetAttribute(errLbl, "TITLE");
                assert(et != NULL && et[0] != '\0');
                bad.confirmFrames = 2;
                bad.lossPercent = 101;
                uiOcrShowError("");
                assert(!uiOcrGroupsCommitGroupForTest(&bad, -1));
                et = IupGetAttribute(errLbl, "TITLE");
                assert(et != NULL && et[0] != '\0');
                bad.lossPercent = 95;
                bad.members[0].regionIndex = 7;
                uiOcrShowError("");
                assert(!uiOcrGroupsCommitGroupForTest(&bad, -1));
                et = IupGetAttribute(errLbl, "TITLE");
                assert(et != NULL && et[0] != '\0');
                // None of the refusals appended a row.
                assert(uiOcrGroupsGroupCountForTest() == 2);
                // A valid draft becomes the third group (same OK path
                // the modal dialog's OK button uses).
                bad.members[0].regionIndex = 0;
                assert(uiOcrGroupsCommitGroupForTest(&bad, -1));
                assert(uiOcrGroupsGroupCountForTest() == 3);
            }
            assert(uiOcrPopulate(&cfg)); // back to the 2/2 model

            // Region deletion remaps member indices by IDENTITY: remove
            // the middle region and the {0,2} group becomes {0,1} with
            // each keyword still attached to its original region
            // (Review Focus #4). Deleting every referenced region leaves
            // an empty red-state group, which strict validation rejects
            // naming the group.
            uiOcrGroupsSetConfirmSuppressedForTest(TRUE);
            {
                OcrRegionDef rs[3];
                OcrGroup gg;
                OcrRegionDef mr[OCR_MAX_REGIONS];
                OcrGroup mg[OCR_MAX_GROUPS];
                size_t mrc, mgc;
                char verr[MSG_BUFSIZE];
                memset(rs, 0, sizeof(rs));
                memset(&gg, 0, sizeof(gg));
                strcpy(rs[0].name, "r0");
                strcpy(rs[1].name, "r1");
                strcpy(rs[2].name, "r2");
                rs[0].norm = (NormRect){ 0.0f, 0.0f, 0.2f, 0.2f };
                rs[1].norm = (NormRect){ 0.2f, 0.2f, 0.2f, 0.2f };
                rs[2].norm = (NormRect){ 0.4f, 0.4f, 0.2f, 0.2f };
                gg.edge = OCR_EDGE_DISAPPEAR;
                gg.confirmFrames = 2;
                gg.lossPercent = 95;
                gg.memberCount = 2;
                gg.members[0].regionIndex = 0;
                strcpy(gg.members[0].keywords, "kw0");
                gg.members[1].regionIndex = 2;
                strcpy(gg.members[1].keywords, "kw2");

                assert(uiOcrGroupsSetModel(rs, 3, &gg, 1));
                uiOcrGroupsDeleteRegionForTest(1);
                uiOcrGroupsGetModel(mr, &mrc, mg, &mgc);
                assert(mrc == 2 && mgc == 1);
                assert(strcmp(mr[0].name, "r0") == 0);
                assert(strcmp(mr[1].name, "r2") == 0);
                assert(mg[0].memberCount == 2);
                assert(mg[0].members[0].regionIndex == 0);
                assert(strcmp(mg[0].members[0].keywords, "kw0") == 0);
                assert(mg[0].members[1].regionIndex == 1);
                assert(strcmp(mg[0].members[1].keywords, "kw2") == 0);

                // Fresh model, then remove both referenced regions:
                // the group survives as a 0-member red empty group.
                assert(uiOcrGroupsSetModel(rs, 3, &gg, 1));
                uiOcrGroupsDeleteRegionForTest(2);
                uiOcrGroupsDeleteRegionForTest(0);
                uiOcrGroupsGetModel(mr, &mrc, mg, &mgc);
                assert(mrc == 1 && strcmp(mr[0].name, "r1") == 0);
                assert(mgc == 1 && mg[0].memberCount == 0);
                // The emptied group renders the red empty-row text.
                assert(strcmp(matCell(uiOcrGroupsMatrixForTest(), 1, 1),
                              tr("ocr.group.empty")) == 0);
                memset(&got, 0, sizeof(got));
                assert(!uiOcrGetConfig(&got));
                assert(!ocrConfigValidate(&got, verr));
                assert(strstr(verr, "group 1") != NULL);
            }
            uiOcrGroupsSetConfirmSuppressedForTest(FALSE);

            // Rename seam lands in both the model and collected config.
            assert(uiOcrPopulate(&cfg));
            uiOcrGroupsRenameRegionForTest(0, "renamed");
            {
                OcrRegionDef mr[OCR_MAX_REGIONS];
                OcrGroup mg[OCR_MAX_GROUPS];
                size_t mrc, mgc;
                uiOcrGroupsGetModel(mr, &mrc, mg, &mgc);
                assert(strcmp(mr[0].name, "renamed") == 0);
                memset(&got, 0, sizeof(got));
                assert(uiOcrGetConfig(&got));
                assert(strcmp(got.regions[0].name, "renamed") == 0);
            }

            // Appended regions take default "Region N" names (sequence
            // continuing the current count) and preserve their rects.
            assert(uiOcrGroupsSetModel(NULL, 0, NULL, 0));
            {
                NormRect pair[2] = {
                    { 0.1f, 0.2f, 0.3f, 0.4f },
                    { 0.5f, 0.6f, 0.2f, 0.1f }
                };
                OcrRegionDef mr[OCR_MAX_REGIONS];
                OcrGroup mg[OCR_MAX_GROUPS];
                size_t mrc, mgc;
                assert(uiOcrGroupsAppendRegions(pair, 2));
                uiOcrGroupsGetModel(mr, &mrc, mg, &mgc);
                assert(mrc == 2 && mgc == 0);
                assert(strcmp(mr[0].name, "Region 1") == 0);
                assert(strcmp(mr[1].name, "Region 2") == 0);
                assert(fabs(mr[0].norm.x - 0.1) < 1e-6);
                assert(fabs(mr[1].norm.y - 0.6) < 1e-6);
                assert(fabs(mr[1].norm.h - 0.1) < 1e-6);
            }

            // 16/16 caps: both add controls disable with an explanatory
            // tip, and both overflow entry points return FALSE.
            {
                OcrRegionDef sixteen[OCR_MAX_REGIONS];
                OcrGroup g16;
                NormRect one = { 0.1f, 0.1f, 0.2f, 0.2f };
                Ihandle *newBtn = uiOcrGroupsNewGroupButtonForTest();
                size_t k;
                memset(sixteen, 0, sizeof(sixteen));
                memset(&g16, 0, sizeof(g16));
                for (k = 0; k < OCR_MAX_REGIONS; k++) {
                    snprintf(sixteen[k].name,
                             sizeof(sixteen[k].name), "r%u",
                             (unsigned)k);
                    sixteen[k].norm = one;
                }
                assert(uiOcrGroupsSetModel(sixteen, 16, NULL, 0));
                assert(!uiOcrGroupsAppendRegions(&one, 1));
                assert(uiOcrGroupsRegionCountForTest() == 16);
                assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"),
                              "NO") == 0);
                assert(strcmp(IupGetAttribute(addBtn, "TIP"),
                              tr("ocr.limitRegions")) == 0);
                g16.edge = OCR_EDGE_DISAPPEAR;
                g16.confirmFrames = 1;
                g16.lossPercent = 95;
                g16.memberCount = 1;
                g16.members[0].regionIndex = 0;
                strcpy(g16.members[0].keywords, "x");
                for (k = 0; k < OCR_MAX_GROUPS; k++) {
                    assert(uiOcrGroupsCommitGroupForTest(&g16, -1));
                }
                assert(uiOcrGroupsGroupCountForTest() == 16);
                assert(!uiOcrGroupsCommitGroupForTest(&g16, -1));
                assert(strcmp(IupGetAttribute(newBtn, "ACTIVE"),
                              "NO") == 0);
                assert(strcmp(IupGetAttribute(newBtn, "TIP"),
                              tr("ocr.limitGroups")) == 0);
            }

            // Running freezes both tables and both add buttons; stop
            // restores each to its prior state.
            {
                Ihandle *rmT = uiOcrRegionsMatrixForTest();
                Ihandle *gmT = uiOcrGroupsMatrixForTest();
                Ihandle *newBtn = uiOcrGroupsNewGroupButtonForTest();
                int addWas = IupGetInt(addBtn, "ACTIVE");
                int newWas = IupGetInt(newBtn, "ACTIVE");
                assert(uiOcrPopulate(&cfg));
                uiOcrSetRunning(TRUE);
                assert(IupGetInt(rmT, "ACTIVE") == 0);
                assert(IupGetInt(gmT, "ACTIVE") == 0);
                assert(IupGetInt(addBtn, "ACTIVE") == 0);
                assert(IupGetInt(newBtn, "ACTIVE") == 0);
                uiOcrSetRunning(FALSE);
                assert(IupGetInt(rmT, "ACTIVE") != 0);
                assert(IupGetInt(gmT, "ACTIVE") != 0);
                assert(IupGetInt(addBtn, "ACTIVE") == addWas);
                assert(IupGetInt(newBtn, "ACTIVE") == newWas);
            }

            // Log mirror: the FIRST append grows the buffer from NULL;
            // the fresh realloc block must be NUL-terminated before
            // strcat or it scans heap garbage and corrupts the heap
            // (observed c0000374 inside IupStoreAttribute). Poison freed
            // blocks across the small-size buckets so a zeroed fresh
            // page cannot hide the bug.
            {
                enum { SPRAY_ROUNDS = 128, SIZES = 8 };
                void *slop[SIZES][SPRAY_ROUNDS];
                static const size_t bucket[SIZES] =
                    { 40, 44, 48, 56, 64, 72, 80, 96 };
                const char *first =
                    "session_start loss=0 source=initial_slider";
                const char *second = "loss_change 0->95 source=ocr";
                int b, i;
                for (b = 0; b < SIZES; b++) {
                    for (i = 0; i < SPRAY_ROUNDS; i++) {
                        slop[b][i] = malloc(bucket[b]);
                        assert(slop[b][i] != NULL);
                        memset(slop[b][i], 'Q', bucket[b]);
                    }
                }
                for (b = 0; b < SIZES; b++)
                    for (i = 0; i < SPRAY_ROUNDS; i++)
                        free(slop[b][i]);

                uiOcrAppendLog(first);
                assert(strcmp(IupGetAttribute(uiOcrLogForTest(), "VALUE"),
                              first) == 0);
                uiOcrAppendLog(second);
                {
                    const char *v =
                        IupGetAttribute(uiOcrLogForTest(), "VALUE");
                    assert(strncmp(v, first, strlen(first)) == 0);
                    assert(strstr(v, second) != NULL);
                }
            }

            // External loss from OCR mirrors onto slider fraction 0.07,
            // permyriad 700 and 7% label.
            uiSimpleApplyExternalLoss(7);
            assert(uiSimpleLoss() == 700);
            assert(fabs(IupGetFloat(uiSimpleSlider(), "VALUE") - 0.07f)
                   < 1e-5f);

            // Running freezes the editors; stop re-enables them. There
            // is no start control in this block anymore: the bottom
            // green/red bar is the only Start/Stop surface.
            uiOcrSetRunning(TRUE);
            assert(strcmp(IupGetAttribute(enToggle, "ACTIVE"), "NO") == 0);
            uiOcrSetRunning(FALSE);
            assert(strcmp(IupGetAttribute(enToggle, "ACTIVE"), "YES") == 0);

            // Task 9: "Choose window" is active once an app context
            // exists; "Add region" waits for a window picked this
            // session.
            assert(strcmp(IupGetAttribute(pick, "ACTIVE"), "YES") == 0);
            assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"), "NO") == 0);
            {
                CaptureWindow pw;
                NormRect nr;
                memset(&pw, 0, sizeof(pw));
                pw.hwnd = (HWND)(INT_PTR)0x5151;
                strcpy(pw.title, "QA Fixture 窗口");
                pw.width = 400;
                pw.height = 300;
                uiOcrSetPickedWindow(&pw);
                assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"),
                              "YES") == 0);
                assert(strcmp(uiOcrSavedWindowTitle(),
                              "QA Fixture 窗口") == 0);

                // A picked region appends a REGION ONLY now (groups are
                // built separately through the New-group dialog): the
                // current model has 2 regions, so the default name is
                // "Region 3", groups stay at 2 and the existing groups
                // still validate.
                nr.x = 0.10; nr.y = 0.90; nr.w = 0.50; nr.h = 0.08;
                assert(uiOcrAddRegionForTest(&nr));
                memset(&got, 0, sizeof(got));
                assert(uiOcrGetConfig(&got));
                assert(got.regionCount == 3);
                assert(got.groupCount == 2);
                assert(strcmp(got.regions[2].name, "Region 3") == 0);
                assert(fabs(got.regions[2].norm.x - 0.10) < 1e-6);
                assert(fabs(got.regions[2].norm.y - 0.90) < 1e-6);
                assert(fabs(got.regions[2].norm.w - 0.50) < 1e-6);
                assert(fabs(got.regions[2].norm.h - 0.08) < 1e-6);

                uiOcrSetPickedWindow(NULL);
                assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"),
                              "NO") == 0);
            }

            // Running freezes both picker buttons; stop restores the
            // context-derived states (choose=YES, add=NO after unpick).
            uiOcrSetRunning(TRUE);
            assert(strcmp(IupGetAttribute(pick, "ACTIVE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"), "NO") == 0);
            uiOcrSetRunning(FALSE);
            assert(strcmp(IupGetAttribute(pick, "ACTIVE"), "YES") == 0);
            assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"), "NO") == 0);

            // Picker drag geometry on a real borderless Win32 window:
            // screen points -> client-relative, ordered, clamped norms.
            {
                NormRect nr2;
                POINT a, b;
                HWND target = CreateWindowExW(0, L"Static", L"drag",
                    WS_POPUP, 100, 100, 400, 300,
                    NULL, NULL, GetModuleHandleW(NULL), NULL);
                assert(target != NULL);
                // 200x110 drag from client (10,30) = screen (110,130).
                a.x = 110; a.y = 130; b.x = 310; b.y = 240;
                assert(uiPickerDragToNorm(target, a, b, 6, &nr2));
                assert(fabs(nr2.x - 0.025) < 1e-6);
                assert(fabs(nr2.y - 0.10) < 1e-6);
                assert(fabs(nr2.w - 0.50) < 1e-6);
                assert(fabs(nr2.h - (110.0 / 300.0)) < 1e-6);
                // Reversed drag order yields the same rect.
                assert(uiPickerDragToNorm(target, b, a, 6, &nr2));
                assert(fabs(nr2.x - 0.025) < 1e-6);
                assert(fabs(nr2.h - (110.0 / 300.0)) < 1e-6);
                // Outside-client drag is clamped to the full client.
                a.x = -50; a.y = -50; b.x = 900; b.y = 900;
                assert(uiPickerDragToNorm(target, a, b, 6, &nr2));
                assert(fabs(nr2.x) < 1e-9 && fabs(nr2.y) < 1e-9);
                assert(fabs(nr2.w - 1.0) < 1e-9);
                assert(fabs(nr2.h - 1.0) < 1e-9);
                // A 3x2 drag is below the 6px minimum: refused.
                a.x = 110; a.y = 130; b.x = 113; b.y = 132;
                assert(!uiPickerDragToNorm(target, a, b, 6, &nr2));
                DestroyWindow(target);
            }

            // Ownership re-check before FRAME lock (Review Focus #1):
            // a live visible window owned by the pinned pid passes;
            // pid 0 (impossible owner) fails; after DestroyWindow the
            // recycled-or-dead handle fails closed.
            {
                HWND own = CreateWindowExW(WS_EX_APPWINDOW, L"Static",
                    L"ownership fixture",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                    220, 220, 360, 240, NULL, NULL,
                    GetModuleHandleW(NULL), NULL);
                assert(own != NULL);
                assert(uiPickerWindowStillOwned(own,
                                                GetCurrentProcessId()));
                assert(!uiPickerWindowStillOwned(own, 0));
                DestroyWindow(own);
                assert(!uiPickerWindowStillOwned(own,
                                                 GetCurrentProcessId()));
            }

            // Overlay parameter gates must never open a window: a
            // direct-DRAG entry without an initial window is a caller
            // bug and returns CANCEL headless.
            {
                CaptureWindow pickWin;
                NormRect pickRects[OCR_MAX_REGIONS];
                size_t pickN = 0;
                assert(uiPickerPick("no_such_app_xyz.exe", FALSE, TRUE,
                                    NULL, NULL, 0, &pickWin,
                                    pickRects, &pickN)
                       == UI_PICKER_CANCEL);
            }

            // Choose policy without UI: one visible top-level window is
            // picked directly (no modal); an app with zero windows fails.
            // procmap only lists endpoint-owning processes, so bind a
            // throwaway loopback UDP socket to make this test exe visible.
            {
                WSADATA wsa;
                SOCKET udp;
                struct sockaddr_in sa;
                HWND unique;
                CaptureWindow cw;
                RECT ur;
                char exePath[MAX_PATH], *exeBase;

                WSAStartup(MAKEWORD(2, 2), &wsa);
                udp = socket(AF_INET, SOCK_DGRAM, 0);
                assert(udp != INVALID_SOCKET);
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                sa.sin_port = 0;
                assert(bind(udp, (struct sockaddr *)&sa,
                            sizeof(sa)) == 0);

                unique = CreateWindowExW(WS_EX_APPWINDOW, L"Static",
                    L"choose direct window",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                    200, 200, 400, 300, NULL, NULL,
                    GetModuleHandleW(NULL), NULL);
                assert(unique != NULL);
                GetClientRect(unique, &ur);
                GetModuleFileNameA(NULL, exePath, MAX_PATH);
                exeBase = strrchr(exePath, '\\');
                uiOcrSetAppContext(exeBase ? exeBase + 1 : exePath);
                assert(uiOcrChooseWindow(&cw));
                assert(cw.hwnd == unique);
                assert(cw.width == ur.right);
                assert(cw.height == ur.bottom);
                assert(strcmp(cw.title, "choose direct window") == 0);

                // n>1 policy: the modal list chooser is deleted; the
                // headless policy only resolves 0/1 windows and returns
                // FALSE for several (the button uses uiPickerPick).
                {
                    HWND second = CreateWindowExW(WS_EX_APPWINDOW,
                        L"Static", L"choose second window",
                        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        240, 240, 360, 240, NULL, NULL,
                        GetModuleHandleW(NULL), NULL);
                    assert(second != NULL);
                    assert(!uiOcrChooseWindow(&cw));
                    DestroyWindow(second);
                }

                uiOcrSetAppContext("no_such_app_xyz.exe");
                assert(!uiOcrChooseWindow(&cw));
                // Zero enumerated windows never enters the overlay.
                {
                    CaptureWindow pickWin;
                    NormRect pickRects[OCR_MAX_REGIONS];
                    size_t pickN = 0;
                    assert(uiPickerPick("no_such_app_xyz.exe", TRUE,
                                        FALSE, NULL, NULL, 0,
                                        &pickWin, pickRects, &pickN)
                           == UI_PICKER_CANCEL);
                }

                DestroyWindow(unique);
                closesocket(udp);
                WSACleanup();
            }

            // Post-lock HWND recycling (Review Focus #1): DRAG actions,
            // Done and the 300ms timer must compare the live owner pid
            // against the pinned one, not mere window liveness; a
            // recycled handle belongs to another process and must end
            // the pick with WINDOW_DIED.
            {
                WSADATA rwsa;
                SOCKET rudp;
                struct sockaddr_in rsa;
                char rExe[MAX_PATH], *rBase;
                CaptureWindow rWin;
                NormRect rRects[OCR_MAX_REGIONS];
                size_t rN;
                Ihandle *rt;

                WSAStartup(MAKEWORD(2, 1), &rwsa);
                rudp = socket(AF_INET, SOCK_DGRAM, 0);
                assert(rudp != INVALID_SOCKET);
                memset(&rsa, 0, sizeof(rsa));
                rsa.sin_family = AF_INET;
                rsa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                rsa.sin_port = 0;
                assert(bind(rudp, (struct sockaddr *)&rsa,
                            sizeof(rsa)) == 0);

                g_recycleHwnd = CreateWindowExW(WS_EX_APPWINDOW, L"Static",
                    L"recycle fixture",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                    300, 160, 380, 260, NULL, NULL,
                    GetModuleHandleW(NULL), NULL);
                assert(g_recycleHwnd != NULL);
                GetModuleFileNameA(NULL, rExe, MAX_PATH);
                rBase = strrchr(rExe, '\\');
                if (rBase) rBase++; else rBase = rExe;
                uiOcrSetAppContext(rBase);

                // Phase 0: the band-complete and Done gates fail closed.
                g_recyclePhase = 0;
                rN = 99;
                rt = IupTimer();
                IupSetInt(rt, "TIME", 200);
                IupSetCallback(rt, "ACTION_CB",
                               (Icallback)recyclePickTimerCb);
                IupSetAttribute(rt, "RUN", "YES");
                assert(uiPickerPick(rBase, TRUE, FALSE, NULL, NULL, 0,
                                    &rWin, rRects, &rN)
                       == UI_PICKER_WINDOW_DIED);
                IupDestroy(rt);
                assert(rN == 0);

                // Phase 1: the timer backstop fails closed on its own.
                g_recyclePhase = 1;
                rt = IupTimer();
                IupSetInt(rt, "TIME", 200);
                IupSetCallback(rt, "ACTION_CB",
                               (Icallback)recyclePickTimerCb);
                IupSetAttribute(rt, "RUN", "YES");
                assert(uiPickerPick(rBase, TRUE, FALSE, NULL, NULL, 0,
                                    &rWin, rRects, &rN)
                       == UI_PICKER_WINDOW_DIED);
                IupDestroy(rt);

                DestroyWindow(g_recycleHwnd);
                g_recycleHwnd = NULL;
                closesocket(rudp);
                WSACleanup();
                uiOcrSetAppContext("no_such_app_xyz.exe");
            }

            // Restore the field the negative cases damaged and populate
            // an enabled config (the startup restore path from
            // ocr_config.txt).
            strcpy(cfg.groups[0].members[0].keywords, "仓库");
            cfg.enabled = TRUE;
            assert(uiOcrPopulate(&cfg));

            uiSimpleAppendSection(ocr);

            // Really map the tree: IupFlatToggle's own map path drops a
            // VALUE set before map (observed ON -> NULL, the checkbox
            // then paints empty even though the section is expanded).
            // The frame MAP_CB (children map first) must re-assert the
            // populated check state.
            {
                const char *mappedValue;
                Ihandle *dlg = IupDialog(box);
                // On this Win32 IUP build the native handle of a mapped
                // control lives in "WID": "HWND" resolves only for
                // dialog/canvas/flat-* classes, and vbox/hbox/frame/fill
                // composition nodes carry the (char*)-1 invalid-id
                // sentinel. A real show is also required -- a bare
                // IupMap on a hidden dialog creates no classic controls.
                BOOL isMapped(Ihandle *e) {
                    const char *wid = IupGetAttribute(e, "WID");
                    return wid != NULL
                        && wid != (const char *)(UINT_PTR)-1;
                }
                IupShowXY(dlg, IUP_CENTER, IUP_CENTER);
                mappedValue = IupGetAttribute(enToggle, "VALUE");
                assert(mappedValue != NULL
                       && strcmp(mappedValue, "ON") == 0);
                // Both tables map wholesale with the dialog (the old
                // row-container blank-paint bug, now owned by IupMatrix
                // internals, is still worth pinning at the table level).
                assert(isMapped(uiOcrRegionsMatrixForTest()));
                assert(isMapped(uiOcrGroupsMatrixForTest()));

                // "Add region" after show must grow the live table
                // immediately (2 populated regions -> row 3) instead of
                // only updating the model behind a stale widget.
                {
                    NormRect postRect = { 0.05, 0.05, 0.20, 0.20 };
                    assert(uiOcrAddRegionForTest(&postRect));
                    assert(IupGetInt(uiOcrRegionsMatrixForTest(),
                                     "NUMLIN") == 3);
                    assert(uiOcrGroupsRegionCountForTest() == 3);
                    assert(strcmp(matCell(uiOcrRegionsMatrixForTest(),
                                          3, 1), "Region 3") == 0);
                    assert(isMapped(uiOcrRegionsMatrixForTest()));
                }
                // Fixed-height contract on the live widgets. Framework
                // quirk (probed on IUP 3.30 Win64): while every row
                // fits without a scrollbar, NUMLIN_VISIBLE reports
                // NUMLIN-1 (the bottom row is counted as a partial
                // row); once content overflows it reports the true
                // fixed window. A 5th region is therefore what opens
                // the 4-row scroll window, and both tables keep their
                // pinned pixel heights the whole time.
                {
                    NormRect fourthRect = { 0.06, 0.06, 0.21, 0.21 };
                    NormRect fifthRect = { 0.07, 0.07, 0.22, 0.22 };
                    const char *rh;
                    const char *gh;
                    assert(uiOcrAddRegionForTest(&fourthRect));
                    assert(uiOcrAddRegionForTest(&fifthRect));
                    assert(IupGetInt(uiOcrRegionsMatrixForTest(),
                                     "NUMLIN") == 5);
                    assert(IupGetInt(uiOcrRegionsMatrixForTest(),
                                     "NUMLIN_VISIBLE") == 4);
                    rh = IupGetAttribute(uiOcrRegionsMatrixForTest(),
                                         "RASTERSIZE");
                    gh = IupGetAttribute(uiOcrGroupsMatrixForTest(),
                                         "RASTERSIZE");
                    assert(atoi(strchr(rh, 'x') + 1) == 117);
                    assert(atoi(strchr(gh, 'x') + 1) == 92);
                }
                IupDestroy(dlg); // owns box
            }
        }
        IupClose();
    }

    // 0c. Headless OCR app coordinator: strict validation and the
    // engine-failure preflight path. No IUP is involved below.
    {
        OcrConfig ac;
        char aerr[MSG_BUFSIZE];
        OcrAppHooks hooks;
        memset(&hooks, 0, sizeof(hooks));
        hooks.onLoss = appCountingLoss;

        // A strict-valid config needs non-empty keywords on every group
        // member; an empty-keywords single-member group must be refused
        // at the app layer too (defense in depth behind the widget gate).
        memset(&ac, 0, sizeof(ac));
        strcpy(ac.exeName, "fixture.exe");
        strcpy(ac.windowTitle, "Fixture");
        ac.intervalMs = 500;
        ac.changeDetect = TRUE;
        ac.enabled = TRUE;
        ac.regionCount = 1;
        ac.groupCount = 1;
        strcpy(ac.regions[0].name, "status");
        // keywords intentionally left empty
        ac.regions[0].norm.x = 0.0;
        ac.regions[0].norm.y = 0.9;
        ac.regions[0].norm.w = 0.5;
        ac.regions[0].norm.h = 0.08;
        ac.groups[0].edge = OCR_EDGE_DISAPPEAR;
        ac.groups[0].confirmFrames = 2;
        ac.groups[0].lossPercent = 95;
        ac.groups[0].memberCount = 1;
        ac.groups[0].members[0].regionIndex = 0;

        g_appLossCalls = 0;
        g_appFactoryCalls = 0;
        ocrAppSetEngineFactoryForTest(appFailingFactory);
        assert(!ocrAppStart(&ac, NULL, "build/tests/app-badcfg",
                            "build/tests/no-models", 0, &hooks, aerr));
        assert(g_appLossCalls == 0);
        assert(g_appFactoryCalls == 0); // refused before engine creation

        // Valid config + a live picked window, but engine creation
        // fails: start fails closed, the loss applier never fires and
        // the header-only timeline of a run that never started is
        // removed rather than left behind.
        strcpy(ac.groups[0].members[0].keywords, "仓库");
        HWND fw = CreateWindowExW(WS_EX_APPWINDOW, L"Static",
            L"app engine fail fixture",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            200, 200, 400, 300, NULL, NULL,
            GetModuleHandleW(NULL), NULL);
        assert(fw != NULL);
        CaptureWindow picked;
        memset(&picked, 0, sizeof(picked));
        picked.hwnd = fw;

        g_appLossCalls = 0;
        g_appFactoryCalls = 0;
        assert(!ocrAppStart(&ac, &picked, "build/tests/app-enginefail",
                            "build/tests/no-models", 0, &hooks, aerr));
        assert(g_appLossCalls == 0);
        assert(g_appFactoryCalls == 1);
        assert(!ocrAppRunning());
        {
            const char *left = ocrAppCsvPath();
            assert(left != NULL);
            assert(GetFileAttributesA(left)
                   == INVALID_FILE_ATTRIBUTES);
        }
        // HWND recycling between OK and Start (Review Focus #1): the
        // live HWND's current owner differs from the pid pinned at pick
        // time. Start must fail closed ("window:") before timeline or
        // engine work, never retarget the foreign process.
        picked.pid = 0x0DEFACEDu;
        g_appFactoryCalls = 0;
        assert(!ocrAppStart(&ac, &picked, "build/tests/app-recycled",
                            "build/tests/no-models", 0, &hooks, aerr));
        assert(g_appFactoryCalls == 0);
        assert(!ocrAppRunning());
        assert(strncmp(aerr, "window:", 7) == 0);

        ocrAppSetEngineFactoryForTest(NULL); // restore real factory
        DestroyWindow(fw);
    }

    // 1. Slider percent -> permyriad, clamped.
    assert(uiSimpleLossPermyriad(0) == 0);
    assert(uiSimpleLossPermyriad(50) == 5000);
    assert(uiSimpleLossPermyriad(95) == 9500);
    assert(uiSimpleLossPermyriad(100) == 10000);
    assert(uiSimpleLossPermyriad(-5) == 0);
    assert(uiSimpleLossPermyriad(120) == 10000);

    // 2. Countdown seconds: ceil of remaining ms, 0 once past the deadline.
    assert(uiSimpleRemainingSeconds(0, 5000) == 5);
    assert(uiSimpleRemainingSeconds(4900, 5000) == 1);
    assert(uiSimpleRemainingSeconds(5000, 5000) == 0);
    assert(uiSimpleRemainingSeconds(9000, 5000) == 0);

    // 3. App-list line: product description present -> two-part line,
    // otherwise the exe-only fallback.
    AppGroup app;
    char line[256], expect[256];
    memset(&app, 0, sizeof(app));
    strcpy(app.exeName, "chrome.exe");
    app.pidCount = 12;
    uiSimpleFormatAppLine(&app, "Google Chrome", line, sizeof(line));
    snprintf(expect, sizeof(expect), tr("app.lineProduct"),
             "Google Chrome", "chrome.exe", 12u);
    assert(strcmp(line, expect) == 0);
    uiSimpleFormatAppLine(&app, "", line, sizeof(line));
    snprintf(expect, sizeof(expect), tr("app.line"), "chrome.exe", 12u);
    assert(strcmp(line, expect) == 0);
    uiSimpleFormatAppLine(&app, NULL, line, sizeof(line));
    snprintf(expect, sizeof(expect), tr("app.line"), "chrome.exe", 12u);
    assert(strcmp(line, expect) == 0);

    // 4. Chart routing.
    Traffic *t = uiSimpleTraffic();
    assert(t != NULL);
    targetingSetOwnerLookup(fakeOwnerHit);
    targetingSetPidNameResolver(fakeName);
    targetingClear();
    uiSimpleSetAffecting(FALSE);

    // Idle: SNIFF observer counts raw bytes, direction split by addr.
    uiSimpleOnSniff((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleOnSniff((const char *)g_udpPacket, sizeof(g_udpPacket), &in);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == sizeof(g_udpPacket));
        assert(up == sizeof(g_tcpPacket));
    }
    // Reinject events are ignored while idle.
    trafficReset(t);
    uiSimpleOnReinject((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == 0 && up == 0);
    }

    // Affecting: reinjected survivor bytes count; SNIFF bytes do not.
    uiSimpleSetAffecting(TRUE);
    uiSimpleOnSniff((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleOnReinject((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleOnReinject((const char *)g_udpPacket, sizeof(g_udpPacket), &in);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == sizeof(g_udpPacket));
        assert(up == sizeof(g_tcpPacket));
    }

    // Both modes are ownership-gated: an unowned packet never charts.
    trafficReset(t);
    targetingSetOwnerLookup(fakeOwnerMiss);
    {
        const char *targets[] = { "a.exe" };
        targetingSetImages(targets, 1);
    }
    uiSimpleOnReinject((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleSetAffecting(FALSE);
    uiSimpleOnSniff((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == 0 && up == 0);
    }
    targetingClear();
    targetingSetOwnerLookup(fakeOwnerHit);
    uiSimpleSetAffecting(FALSE);

    printf("OK: ui_simple\n");
    return 0;
}
