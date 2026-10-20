// Beginner main window logic: app picker, direction, loss slider, timed
// recovery and the per-app traffic chart.
//
// Split in two parts: pure helpers + chart accounting (unit-tested, no
// live IUP state) and the widget layer (driven by main.c, covered by the
// Task 12 end-to-end scenarios).
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
// winsock2 (pulled by procmap) must precede windows.h from common.h
#include <winsock2.h>

#include "common.h"
#include "i18n.h"
#include "procmap.h"
#include "targeting.h"
#include "simple.h"
#include "ui_simple.h"

#define LOSS_FULL_PERCENT 100
#define LOSS_FULL_PERMYRIAD 10000
// "Before start" rate averages this many completed seconds.
#define RATE_WINDOW_SECONDS 10

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

int uiSimpleLossPermyriad(int percent) {
    if (percent < 0) {
        return 0;
    }
    if (percent > LOSS_FULL_PERCENT) {
        return LOSS_FULL_PERMYRIAD;
    }
    return percent * 100;
}

int uiSimpleRemainingSeconds(DWORD elapsedMs, DWORD totalMs) {
    DWORD remainingMs;
    if (totalMs == 0 || elapsedMs >= totalMs) {
        return 0;
    }
    remainingMs = totalMs - elapsedMs;
    return (int)((remainingMs + 999) / 1000);
}

void uiSimpleFormatAppLine(const AppGroup *app, const char *product,
                           char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return;
    }
    if (app == NULL) {
        out[0] = '\0';
        return;
    }
    if (product != NULL && product[0] != '\0') {
        snprintf(out, cap, tr("app.lineProduct"), product, app->exeName,
                 (unsigned)app->pidCount);
    } else {
        snprintf(out, cap, tr("app.line"), app->exeName,
                 (unsigned)app->pidCount);
    }
}

// ---------------------------------------------------------------------------
// Chart accounting
// ---------------------------------------------------------------------------

static Traffic *g_traffic = NULL;
static volatile short g_affecting = 0;
static UINT64 g_beforeDown = 0;
static UINT64 g_beforeUp = 0;

Traffic *uiSimpleTraffic(void) {
    if (g_traffic == NULL) {
        g_traffic = trafficNew();
    }
    return g_traffic;
}

void uiSimpleSetAffecting(BOOL affecting) {
    InterlockedExchange16(&g_affecting, (short)(affecting ? 1 : 0));
    if (affecting) {
        // Freeze the "before" snapshot at the moment Start is pressed.
        trafficRecentAverage(uiSimpleTraffic(), RATE_WINDOW_SECONDS,
                             &g_beforeDown, &g_beforeUp);
    }
}

// Record a packet that belongs on the chart: ownership gate only, split
// by the wire direction. Used by the SNIFF observer while idle and by the
// reinject observer while a session is affecting traffic.
static void chartPacket(const char *packet, UINT packetLen,
                        const WINDIVERT_ADDRESS *addr) {
    if (packet == NULL || addr == NULL) {
        return;
    }
    if (targetingOwnsPacket(packet, packetLen, addr)) {
        trafficRecord(uiSimpleTraffic(), packetLen,
                      addr->Outbound ? TRUE : FALSE);
    }
}

void uiSimpleOnSniff(const char *packet, UINT packetLen,
                     const WINDIVERT_ADDRESS *addr) {
    // Idle chart: the read-only SNIFF copy shows pre-Start traffic.
    if (!g_affecting) {
        chartPacket(packet, packetLen, addr);
    }
}

void uiSimpleOnReinject(const char *packet, UINT packetLen,
                        const WINDIVERT_ADDRESS *addr) {
    // Running chart: only survivors actually reinjected count.
    if (g_affecting) {
        chartPacket(packet, packetLen, addr);
    }
}

// ---------------------------------------------------------------------------
// Widget layer
// ---------------------------------------------------------------------------

#define UI_TICK_MS          1000
#define NOW_WINDOW_SECONDS  3
#define DEFAULT_TIMER_SECS  30
#define MAX_TIMER_SECS      86400
#define APP_ENUM_CAP        128
#define RATE_TEXT_LEN       96
#define COLOR_GREEN         "109 170 44"
#define COLOR_RED           "208 70 72"
#define COLOR_HINT          "110 110 110"

static Ihandle *g_appList, *g_refreshBtn, *g_appHint;
static Ihandle *g_dirAll, *g_dirIn, *g_dirOut;
static Ihandle *g_val, *g_percentLabel;
static Ihandle *g_timerToggle, *g_timerText, *g_secondsLabel, *g_countdownLabel;
static Ihandle *g_chartFrame, *g_canvas, *g_beforeLabel, *g_nowLabel;
static Ihandle *g_actionBtn, *g_statusLabel, *g_secondTimer, *g_recoveryTimer;
static Icallback g_actionCb = NULL;
static UiSimpleAppWatchFn g_watchApp = NULL;
static AppGroup *g_apps = NULL;
static size_t g_appCount = 0;
static int g_selectedRow = 1;   // 1 == "all applications"
static BOOL g_running = FALSE;
static DWORD g_recoveryStart = 0;
static DWORD g_recoveryMs = 0;
static int g_percent = 100;

static void updateChartTitle(const char *exe);
static void renderRates(void);

const char *uiSimpleSelectedApp(void) {
    if (g_selectedRow >= 2 && (size_t)(g_selectedRow - 2) < g_appCount) {
        return g_apps[g_selectedRow - 2].exeName;
    }
    return NULL;
}

void uiSimpleGetDirection(short *outIn, short *outOut) {
    short in = (short)(IupGetInt(g_dirIn, "VALUE") == 1);
    short out = (short)(IupGetInt(g_dirOut, "VALUE") == 1);
    if (IupGetInt(g_dirAll, "VALUE") == 1) {
        in = 1;
        out = 1;
    }
    if (outIn != NULL) *outIn = in;
    if (outOut != NULL) *outOut = out;
}

int uiSimpleLoss(void) {
    return uiSimpleLossPermyriad(g_percent);
}

BOOL uiSimpleTimerSeconds(int *outSeconds) {
    int seconds;
    if (IupGetInt(g_timerToggle, "VALUE") != 1) {
        return FALSE;
    }
    seconds = atoi(IupGetAttribute(g_timerText, "VALUE"));
    if (seconds < 1) seconds = 1;
    if (seconds > MAX_TIMER_SECS) seconds = MAX_TIMER_SECS;
    if (outSeconds != NULL) *outSeconds = seconds;
    return TRUE;
}

// Enumerate apps into g_apps, growing if the table needs more room.
static BOOL enumerateApps(void) {
    size_t cap = APP_ENUM_CAP;
    size_t count = 0;
    AppGroup *apps = (AppGroup *)calloc(cap, sizeof(AppGroup));
    if (apps == NULL) {
        return FALSE;
    }
    if (!procmapEnumApps(apps, cap, &count) && count > cap) {
        AppGroup *bigger = (AppGroup *)realloc(apps,
                                               count * sizeof(AppGroup));
        if (bigger == NULL) {
            free(apps);
            return FALSE;
        }
        apps = bigger;
        cap = count;
        if (!procmapEnumApps(apps, cap, &count)) {
            free(apps);
            return FALSE;
        }
    }
    free(g_apps);
    g_apps = apps;
    g_appCount = count;
    return TRUE;
}

// Rebuild dropdown items, keeping the row whose exe matches keepExe.
static void populateAppList(const char *keepExe) {
    size_t i;
    char idx[8];
    // IUP removes the first item on each REMOVEITEM, including the fixed
    // "all applications" row.
    while (IupGetInt(g_appList, "COUNT") > 0) {
        IupSetAttribute(g_appList, "REMOVEITEM", "1");
    }
    IupStoreAttribute(g_appList, "1", (char *)tr("app.all"));
    for (i = 0; i < g_appCount; i++) {
        char line[256];
        snprintf(idx, sizeof(idx), "%d", (int)(i + 2));
        uiSimpleFormatAppLine(&g_apps[i], NULL, line, sizeof(line));
        IupStoreAttribute(g_appList, idx, line);
    }
    g_selectedRow = 1;
    if (keepExe != NULL) {
        for (i = 0; i < g_appCount; i++) {
            if (_stricmp(g_apps[i].exeName, keepExe) == 0) {
                g_selectedRow = (int)i + 2;
                break;
            }
        }
    }
    snprintf(idx, sizeof(idx), "%d", g_selectedRow);
    IupSetAttribute(g_appList, "VALUE", idx);
    updateChartTitle(uiSimpleSelectedApp());
}

static int appListCb(Ihandle *ih, char *text, int item, int state) {
    const char *exe;
    UNREFERENCED_PARAMETER(ih);
    UNREFERENCED_PARAMETER(text);
    if (state != 1 || g_running) {
        return IUP_DEFAULT;
    }
    g_selectedRow = item;
    exe = uiSimpleSelectedApp();
    if (exe != NULL) {
        targetingSetImages(&exe, 1);
        trafficReset(uiSimpleTraffic());
        renderRates();
    } else {
        targetingClear();
    }
    if (g_watchApp != NULL) {
        g_watchApp(exe);
    }
    updateChartTitle(exe);
    return IUP_DEFAULT;
}

static int refreshCb(Ihandle *ih) {
    const char *keep = uiSimpleSelectedApp();
    UNREFERENCED_PARAMETER(ih);
    if (!g_running && enumerateApps()) {
        populateAppList(keep);
    }
    return IUP_DEFAULT;
}

static int directionCb(Ihandle *ih, int checked) {
    if (checked) {
        if (ih == g_dirAll) {
            targetingSetDirection(1, 1);
        } else if (ih == g_dirIn) {
            targetingSetDirection(1, 0);
        } else {
            targetingSetDirection(0, 1);
        }
    }
    return IUP_DEFAULT;
}

static void applyPercent(int percent) {
    char title[16];
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_percent = percent;
    simpleSetLoss((short)uiSimpleLossPermyriad(percent));
    snprintf(title, sizeof(title), "%d%%", percent);
    IupStoreAttribute(g_percentLabel, "TITLE", title);
}

static int sliderCb(Ihandle *ih, double value) {
    UNREFERENCED_PARAMETER(ih);
    applyPercent((int)(value * 100.0 + 0.5));
    return IUP_DEFAULT;
}

static int presetCb(Ihandle *ih) {
    int percent = IupGetInt(ih, "PCT");
    char value[16];
    snprintf(value, sizeof(value), "%.2f", percent / 100.0);
    IupSetAttribute(g_val, "VALUE", value);
    applyPercent(percent);
    return IUP_DEFAULT;
}

static void renderBeforeRates(void) {
    char down[32], up[32], line[RATE_TEXT_LEN];
    trafficFormatRate(g_beforeDown, down, sizeof(down));
    trafficFormatRate(g_beforeUp, up, sizeof(up));
    snprintf(line, sizeof(line), tr("chart.before"), down, up);
    IupStoreAttribute(g_beforeLabel, "TITLE", line);
}

static void renderRates(void) {
    char down[32], up[32], line[RATE_TEXT_LEN];
    UINT64 nowDown = 0, nowUp = 0;
    if (g_nowLabel == NULL) {
        return; // pre-create call from trafficReset paths
    }
    trafficRecentAverage(uiSimpleTraffic(), NOW_WINDOW_SECONDS,
                         &nowDown, &nowUp);
    trafficFormatRate(nowDown, down, sizeof(down));
    trafficFormatRate(nowUp, up, sizeof(up));
    snprintf(line, sizeof(line), tr("chart.now"), down, up);
    IupStoreAttribute(g_nowLabel, "TITLE", line);
    renderBeforeRates();
}

static int secondTimerCb(Ihandle *ih) {
    char line[64];
    int left;
    UNREFERENCED_PARAMETER(ih);
    trafficTick(uiSimpleTraffic());
    IupUpdate(g_canvas);
    renderRates();
    if (g_running && g_recoveryMs != 0) {
        left = uiSimpleRemainingSeconds(GetTickCount() - g_recoveryStart,
                                        g_recoveryMs);
        snprintf(line, sizeof(line), tr("timer.countdown"), (unsigned)left);
        IupStoreAttribute(g_countdownLabel, "TITLE", line);
    }
    return IUP_DEFAULT;
}

void uiSimpleArmRecovery(int seconds, Icallback stopCb) {
    char timeBuf[16];
    g_recoveryStart = GetTickCount();
    g_recoveryMs = (DWORD)seconds * 1000;
    IupSetCallback(g_recoveryTimer, "ACTION_CB", stopCb);
    snprintf(timeBuf, sizeof(timeBuf), "%d", seconds * 1000);
    IupStoreAttribute(g_recoveryTimer, "TIME", timeBuf);
    IupSetAttribute(g_recoveryTimer, "RUN", "YES");
    // Show the full duration immediately; the 1s timer counts it down.
    {
        char line[64];
        snprintf(line, sizeof(line), tr("timer.countdown"),
                 (unsigned)seconds);
        IupStoreAttribute(g_countdownLabel, "TITLE", line);
    }
}

void uiSimpleDisarmRecovery(void) {
    g_recoveryMs = 0;
    if (g_recoveryTimer != NULL) {
        IupSetAttribute(g_recoveryTimer, "RUN", "NO");
    }
    if (g_countdownLabel != NULL) {
        IupStoreAttribute(g_countdownLabel, "TITLE", "");
    }
}

static int actionCb(Ihandle *ih) {
    if (g_actionCb != NULL) {
        return g_actionCb(ih);
    }
    return IUP_DEFAULT;
}

static void updateChartTitle(const char *exe) {
    char title[128];
    if (g_chartFrame == NULL) {
        return;
    }
    if (exe != NULL) {
        snprintf(title, sizeof(title), tr("section.chart.app"), exe);
    } else {
        snprintf(title, sizeof(title), "%s", tr("section.chart"));
    }
    IupStoreAttribute(g_chartFrame, "TITLE", title);
}

void uiSimpleSetStatus(const char *text) {
    if (g_statusLabel != NULL) {
        IupStoreAttribute(g_statusLabel, "TITLE", text != NULL ? text : "");
    }
}

void uiSimpleSetRunning(BOOL running, BOOL enableLoss) {
    const char *activeState = running ? "NO" : "YES";
    g_running = running;
    uiSimpleSetAffecting(running);
    if (enableLoss) {
        simpleSetEnabled(running);
    }
    IupStoreAttribute(g_actionBtn, "TITLE",
                      tr(running ? "action.stop" : "action.start"));
    IupSetAttribute(g_actionBtn, "BGCOLOR", running ? COLOR_RED : COLOR_GREEN);
    IupSetAttribute(g_actionBtn, "FGCOLOR", "255 255 255");
    IupSetAttribute(g_appList, "ACTIVE", activeState);
    IupSetAttribute(g_refreshBtn, "ACTIVE", activeState);
    IupSetAttribute(g_dirAll, "ACTIVE", activeState);
    IupSetAttribute(g_dirIn, "ACTIVE", activeState);
    IupSetAttribute(g_dirOut, "ACTIVE", activeState);
    IupSetAttribute(g_timerToggle, "ACTIVE", activeState);
    IupSetAttribute(g_timerText, "ACTIVE", activeState);
    if (running) {
        trafficMarkStart(uiSimpleTraffic());
    } else {
        uiSimpleDisarmRecovery();
    }
    renderRates();
}

void uiSimpleRetranslate(void) {
    // Bound controls retranslate themselves; handle dynamic pieces.
    populateAppList(uiSimpleSelectedApp());
    applyPercent(g_percent);
    updateChartTitle(uiSimpleSelectedApp());
    renderRates();
    IupStoreAttribute(g_actionBtn, "TITLE",
                      tr(g_running ? "action.stop" : "action.start"));
}

static Ihandle *makePresetButton(const char *key, int percent) {
    Ihandle *btn = IupButton(tr(key), NULL);
    i18nBind(btn, "TITLE", key);
    IupSetInt(btn, "PCT", percent);
    IupSetCallback(btn, "ACTION", (Icallback)presetCb);
    return btn;
}

Ihandle *uiSimpleCreate(Icallback actionCbImpl, UiSimpleAppWatchFn watchApp) {
    Ihandle *appFrame, *dirFrame, *condFrame;
    Ihandle *appRow, *dirRadio, *sliderRow, *timerRow, *legendRow;
    Ihandle *downIcon, *upIcon, *downLabel, *upLabel;

    g_actionCb = actionCbImpl;
    g_watchApp = watchApp;
    g_traffic = trafficNew();

    // 1. application
    g_appList = IupList(NULL);
    IupSetAttribute(g_appList, "DROPDOWN", "YES");
    IupSetAttribute(g_appList, "VISIBLECOLUMNS", "26");
    IupSetAttribute(g_appList, "EXPAND", "HORIZONTAL");
    IupSetCallback(g_appList, "ACTION", (Icallback)appListCb);
    g_refreshBtn = IupButton(tr("app.refresh"), NULL);
    i18nBind(g_refreshBtn, "TITLE", "app.refresh");
    IupSetCallback(g_refreshBtn, "ACTION", (Icallback)refreshCb);
    g_appHint = IupFlatLabel(tr("app.hint"));
    i18nBind(g_appHint, "TITLE", "app.hint");
    IupSetAttribute(g_appHint, "WRAP", "YES");
    IupSetAttribute(g_appHint, "FGCOLOR", COLOR_HINT);
    IupSetAttribute(g_appHint, "EXPAND", "HORIZONTAL");
    appRow = IupHbox(g_appList, g_refreshBtn, NULL);
    appFrame = IupFrame(IupVbox(appRow, g_appHint, NULL));
    i18nBind(appFrame, "TITLE", "section.app");

    // 2. direction
    g_dirAll = IupToggle(tr("direction.all"), NULL);
    g_dirIn = IupToggle(tr("direction.in"), NULL);
    g_dirOut = IupToggle(tr("direction.out"), NULL);
    i18nBind(g_dirAll, "TITLE", "direction.all");
    i18nBind(g_dirIn, "TITLE", "direction.in");
    i18nBind(g_dirOut, "TITLE", "direction.out");
    IupSetAttribute(g_dirAll, "VALUE", "ON");
    IupSetCallback(g_dirAll, "ACTION", (Icallback)directionCb);
    IupSetCallback(g_dirIn, "ACTION", (Icallback)directionCb);
    IupSetCallback(g_dirOut, "ACTION", (Icallback)directionCb);
    dirRadio = IupRadio(IupHbox(g_dirAll, g_dirIn, g_dirOut, IupFill(), NULL));
    dirFrame = IupFrame(dirRadio);
    i18nBind(dirFrame, "TITLE", "section.direction");

    // 3. condition: percent, presets, slider, timed recovery, hint
    g_percentLabel = IupLabel("100%");
    IupSetAttribute(g_percentLabel, "FONT", "Segoe UI, 16");
    g_val = IupVal("HORIZONTAL");
    IupSetAttribute(g_val, "EXPAND", "HORIZONTAL");
    IupSetAttribute(g_val, "VALUE", "1.0");
    IupSetCallback(g_val, "VALUECHANGED_CB", (Icallback)sliderCb);
    sliderRow = IupHbox(g_percentLabel, IupFill(),
                        makePresetButton("preset.normal", 0),
                        makePresetButton("preset.severe", 95),
                        makePresetButton("preset.offline", 100), NULL);
    g_timerToggle = IupToggle(tr("timer.enable"), NULL);
    i18nBind(g_timerToggle, "TITLE", "timer.enable");
    g_timerText = IupText(NULL);
    IupSetAttribute(g_timerText, "VISIBLECOLUMNS", "4");
    IupSetAttribute(g_timerText, "VALUE", "30");
    g_secondsLabel = IupLabel(tr("timer.seconds"));
    i18nBind(g_secondsLabel, "TITLE", "timer.seconds");
    g_countdownLabel = IupFlatLabel("");
    IupSetAttribute(g_countdownLabel, "FGCOLOR", COLOR_RED);
    timerRow = IupHbox(g_timerToggle, g_timerText, g_secondsLabel, IupFill(),
                       g_countdownLabel, NULL);
    {
        Ihandle *condHint = IupFlatLabel(tr("condition.hint"));
        i18nBind(condHint, "TITLE", "condition.hint");
        IupSetAttribute(condHint, "WRAP", "YES");
        IupSetAttribute(condHint, "FGCOLOR", COLOR_HINT);
        IupSetAttribute(condHint, "EXPAND", "HORIZONTAL");
        condFrame = IupFrame(IupVbox(sliderRow, g_val, timerRow, condHint,
                                    NULL));
    }
    i18nBind(condFrame, "TITLE", "section.condition");
    applyPercent(100);

    // 4. chart
    g_canvas = trafficCreateCanvas(uiSimpleTraffic());
    IupSetAttribute(g_canvas, "RASTERSIZE", "440x72");
    IupSetAttribute(g_canvas, "EXPAND", "HORIZONTAL");
    downIcon = IupLabel(NULL);
    upIcon = IupLabel(NULL);
    IupSetAttribute(downIcon, "BGCOLOR", "43 125 233");
    IupSetAttribute(upIcon, "BGCOLOR", "109 170 44");
    IupSetAttribute(downIcon, "RASTERSIZE", "10x10");
    IupSetAttribute(upIcon, "RASTERSIZE", "10x10");
    downLabel = IupLabel(tr("chart.down"));
    upLabel = IupLabel(tr("chart.up"));
    i18nBind(downLabel, "TITLE", "chart.down");
    i18nBind(upLabel, "TITLE", "chart.up");
    g_beforeLabel = IupFlatLabel("");
    g_nowLabel = IupFlatLabel("");
    legendRow = IupHbox(downIcon, downLabel, upIcon, upLabel, IupFill(),
                        g_beforeLabel, g_nowLabel, NULL);
    g_chartFrame = IupFrame(IupVbox(legendRow, g_canvas, NULL));
    IupStoreAttribute(g_chartFrame, "TITLE", tr("section.chart"));

    // main action + status
    g_actionBtn = IupButton(tr("action.start"), NULL);
    IupSetAttribute(g_actionBtn, "EXPAND", "HORIZONTAL");
    IupSetAttribute(g_actionBtn, "PADDING", "0x10");
    IupSetAttribute(g_actionBtn, "BGCOLOR", COLOR_GREEN);
    IupSetAttribute(g_actionBtn, "FGCOLOR", "255 255 255");
    IupSetCallback(g_actionBtn, "ACTION", (Icallback)actionCb);
    g_statusLabel = IupFlatLabel(tr("status.ready"));
    i18nBind(g_statusLabel, "TITLE", "status.ready");
    IupSetAttribute(g_statusLabel, "WRAP", "YES");
    IupSetAttribute(g_statusLabel, "EXPAND", "HORIZONTAL");

    g_secondTimer = IupTimer();
    IupSetAttribute(g_secondTimer, "TIME", "1000");
    IupSetCallback(g_secondTimer, "ACTION_CB", (Icallback)secondTimerCb);
    IupSetAttribute(g_secondTimer, "RUN", "YES");
    g_recoveryTimer = IupTimer();
    IupSetAttribute(g_recoveryTimer, "RUN", "NO");

    enumerateApps();
    populateAppList(NULL);
    renderRates();

    return IupVbox(appFrame, dirFrame, condFrame, g_chartFrame,
                   g_actionBtn, g_statusLabel, NULL);
}
