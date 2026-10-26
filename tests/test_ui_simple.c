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
#include "ui_picker.h"
#include "ocr_app.h"
#include "iup.h"

// Marker callback so pointer identity can be checked on the widget.
static int g_fakeFired = 0;
static int fakeActionCb(Ihandle *ih) { (void)ih; g_fakeFired = 1;
                                       return IUP_DEFAULT; }

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
            Ihandle *ocr = uiOcrCreate((Icallback)fakeActionCb);
            Ihandle *enToggle = uiOcrEnableToggleForTest();
            Ihandle *pick = uiOcrChooseButtonForTest();
            Ihandle *addBtn = uiOcrAddButtonForTest();
            Ihandle *startBtn = uiOcrStartButtonForTest();
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

            assert(ocr != NULL);
            assert(enToggle != NULL && ocrBody != NULL);
            // Action button in the OCR block dispatches to main's
            // Start/Stop callback (wired on FlatButton's FLAT_ACTION).
            assert(startBtn != NULL);
            assert(IupGetCallback(startBtn, "FLAT_ACTION")
                   == (Icallback)fakeActionCb);
            // Window/region buttons are Task 9 placeholders in Task 8.
            assert(pick != NULL && addBtn != NULL);
            assert(IupGetAttribute(pick, "ACTIVE") == NULL
                   || strcmp(IupGetAttribute(pick, "ACTIVE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(addBtn, "ACTIVE"), "NO") == 0);
            // Defaults: not enabled, body folded, 500ms interval, change
            // detection on.
            assert(uiOcrEnabled() == FALSE);
            assert(strcmp(IupGetAttribute(ocrBody, "VISIBLE"), "NO") == 0);
            // The in-block Start must not fire while OCR is disabled:
            // it shares main's Start callback and would otherwise begin
            // an ordinary affect session from inside section ⑤.
            assert(strcmp(IupGetAttribute(startBtn, "ACTIVE"), "NO") == 0);
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
            assert(strcmp(IupGetAttribute(startBtn, "ACTIVE"), "NO") == 0);

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
            // A real enable hides the gate warning and arms the block's
            // own Start/Stop button.
            assert(strcmp(IupGetAttribute(gateErr, "VISIBLE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(startBtn, "ACTIVE"), "YES") == 0);

            // Disabling again folds the block, hides any gate text and
            // disarms the in-block Start.
            IupSetAttribute(enToggle, "VALUE", "OFF");
            ((ToggleActionFn)enCb)(enToggle, 0);
            assert(uiOcrEnabled() == FALSE);
            assert(strcmp(IupGetAttribute(ocrBody, "VISIBLE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(gateErr, "VISIBLE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(startBtn, "ACTIVE"), "NO") == 0);

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

            // Two parallel region + single-member-group rows round-trip
            // through the row widgets (zh keywords, norm preservation,
            // dropdown edge mapping) under the Task 2 interim adapter.
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
            g0->memberCount = 1;
            g0->members[0].regionIndex = 0;
            strcpy(g0->members[0].keywords, "仓库");
            g1 = &cfg.groups[1];
            g1->edge = OCR_EDGE_APPEAR;
            g1->confirmFrames = 1;
            g1->lossPercent = 50;
            g1->memberCount = 1;
            g1->members[0].regionIndex = 1;
            strcpy(g1->members[0].keywords, "重连,reconnect");
            assert(uiOcrPopulate(&cfg));
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
            assert(got.groups[0].memberCount == 1);
            assert(got.groups[0].members[0].regionIndex == 0);
            assert(strcmp(got.groups[0].members[0].keywords,
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

            // Invalid rows make collection fail (Start must refuse):
            // loss out of 0..100, 0 confirm frames, empty keywords.
            cfg.groups[0].lossPercent = 120;
            assert(uiOcrPopulate(&cfg));
            memset(&got, 0, sizeof(got));
            assert(!uiOcrGetConfig(&got));
            cfg.groups[0].lossPercent = 95;

            cfg.groups[1].confirmFrames = 0;
            assert(uiOcrPopulate(&cfg));
            assert(!uiOcrGetConfig(&got));
            cfg.groups[1].confirmFrames = 1;

            cfg.groups[0].members[0].keywords[0] = '\0';
            assert(uiOcrPopulate(&cfg));
            assert(!uiOcrGetConfig(&got));

            // Restore the field the negative case damaged: the Task 9
            // positive cases below collect a config that must validate.
            strcpy(cfg.groups[0].members[0].keywords, "仓库");
            assert(uiOcrPopulate(&cfg));

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

            // Running freezes the editors; stop re-enables them. The
            // in-block Start stays clickable while running (it is the
            // Stop button by then), titled with the dedicated OCR labels.
            uiOcrSetRunning(TRUE);
            assert(strcmp(IupGetAttribute(enToggle, "ACTIVE"), "NO") == 0);
            assert(strcmp(IupGetAttribute(startBtn, "ACTIVE"), "YES") == 0);
            assert(strcmp(IupGetAttribute(startBtn, "TITLE"),
                          tr("ocr.stop")) == 0);
            uiOcrSetRunning(FALSE);
            assert(strcmp(IupGetAttribute(enToggle, "ACTIVE"), "YES") == 0);
            assert(strcmp(IupGetAttribute(startBtn, "TITLE"),
                          tr("ocr.start")) == 0);

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

                // A picked region appends a placeholder region + group
                // the user then fills in: default DISAPPEAR / 2 frames /
                // 95%, member pointing at the parallel region. Strict
                // collection now refuses empty keywords itself; on
                // failure *out is still fully written, so the defaults
                // are asserted on the rejected config.
                nr.x = 0.10; nr.y = 0.90; nr.w = 0.50; nr.h = 0.08;
                assert(uiOcrAddRegionForTest(&nr));
                memset(&got, 0, sizeof(got));
                assert(!uiOcrGetConfig(&got));
                assert(got.regionCount == 3);
                assert(got.groupCount == 3);
                assert(strcmp(got.regions[2].name, "Region 1") == 0);
                assert(got.groups[2].members[0].regionIndex == 2);
                assert(got.groups[2].members[0].keywords[0] == '\0');
                assert(got.groups[2].edge == OCR_EDGE_DISAPPEAR);
                assert(got.groups[2].confirmFrames == 2);
                assert(got.groups[2].lossPercent == 95);
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

                uiOcrSetAppContext("no_such_app_xyz.exe");
                assert(!uiOcrChooseWindow(&cw));

                DestroyWindow(unique);
                closesocket(udp);
                WSACleanup();
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
                // Rows present before show map wholesale with dialog.
                assert(isMapped(IupGetChild(uiOcrRowBoxForTest(0), 0)));

                // "Add region" after show must map the new row subtree
                // immediately; an unmapped child takes layout space but
                // paints blank (2 populated rows -> new row is index 2).
                // The hbox row itself is a composition node (sentinel
                // WID); its first field (the name text) proves the
                // subtree was mapped.
                {
                    NormRect postRect = { 0.05, 0.05, 0.20, 0.20 };
                    Ihandle *newRow;
                    assert(uiOcrAddRegionForTest(&postRect));
                    newRow = uiOcrRowBoxForTest(2);
                    assert(newRow != NULL);
                    assert(isMapped(IupGetChild(newRow, 0)));
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
