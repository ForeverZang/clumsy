#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
// winsock2 (pulled by divert/flowmap) must precede windows.h
#include <winsock2.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"
#include "divert.h"
#include "i18n.h"
#include "swfilter.h"
#include "ui_simple.h"
#include "ui_advanced.h"
#if defined(CLUMSY_OCR)
#include "iupcontrols.h"
#include "ui_ocr.h"
#include "ocr_glue.h"
#endif

// ! the order decides which module get processed first
Module* modules[MODULE_CNT] = {
    &lagModule,
    &dropModule,
    &throttleModule,
    &dupModule,
    &oodModule,
    &tamperModule,
    &resetModule,
    &bandwidthModule,
};

volatile short sendState = SEND_STATUS_NONE;
BOOL parameterized = 0; // reading args from command line

// global iup handlers
static Ihandle *dialog, *dialogVBox, *iconTimer;
static Ihandle *simpleBox, *advancedBox, *tabHolder, *headerBox;
static Ihandle *basicTabBtn, *advTabBtn;
static Ihandle *zhBtn, *enBtn;
static Ihandle *timeoutTimer = NULL;
// Both pages stay mounted under tabHolder for the dialog's whole life;
// activePage is the one currently VISIBLE and in flow.
static Ihandle *activePage;

static char dialogTitle[128];

static int uiOnDialogShow(Ihandle *ih, int state);
static int uiStartCb(Ihandle *ih);
static int uiStopCb(Ihandle *ih);
static int uiToggleActionCb(Ihandle *ih);
static int uiIconTimerCb(Ihandle *ih);
static int uiTimeoutCb(Ihandle *ih);
static void fitPreMap(void);
static void fitDialogLive(void);
static void selectTab(Ihandle *page);
static void showAdvancedTab(void);

// Persistent SNIFF lifecycle for a picked app. NULL ("all applications")
// tears the persistent session down; global mode attaches at Start.
static void watchApp(const char *exe) {
    char err[MSG_BUFSIZE];
#if defined(CLUMSY_OCR)
    // App-list changes only set the context for window enumeration; the
    // configured rules are never cleared by switching the dropdown.
    uiOcrSetAppContext(exe);
#endif
    if (exe != NULL) {
        if (!divertIsAttached() && !divertAttach("true", err)) {
            uiSimpleSetStatus(err);
            return;
        }
        divertSetSniffObserver(uiSimpleOnSniff);
    } else {
        divertSetSniffObserver(NULL);
        divertDetach();
    }
}

// Compile the advanced filter text into the software AND leg. A syntax
// error shows inline in the expanded advanced area, never as a native box.
static BOOL compileAdvancedFilter(void) {
    char err[MSG_BUFSIZE];
    char msg[MSG_BUFSIZE];
    const char *filter = uiAdvancedFilterValue();
    if (swfilterSet(filter, err)) {
        uiAdvancedSetFilterError(NULL);
        return TRUE;
    }
    snprintf(msg, sizeof(msg), tr("advanced.filterError"), err);
    uiAdvancedSetFilterError(msg);
    // switch to the advanced tab so the red line is visible
    showAdvancedTab();
    uiSimpleSetStatus(msg);
    return FALSE;
}

static void statusAffecting(void) {
    char line[MSG_BUFSIZE];
    const char *exe = uiSimpleSelectedApp();
    snprintf(line, sizeof(line), tr("status.affecting"),
             exe != NULL ? exe : tr("app.all"));
    uiSimpleSetStatus(line);
}

static int uiStartCb(Ihandle *ih) {
    char err[MSG_BUFSIZE];
    int seconds;
    const char *exe;
    UNREFERENCED_PARAMETER(ih);

    if (!compileAdvancedFilter()) {
        return IUP_DEFAULT;
    }
#if defined(CLUMSY_OCR)
    // OCR preflight before any WinDivert attach: a failed start shows an
    // inline red error and leaves the system exactly as it was.
    if (!uiOcrGlueStart(uiSimpleLoss() / 100)) {
        swfilterClear();
        return IUP_DEFAULT;
    }
#endif
    exe = uiSimpleSelectedApp();
    if (!divertIsAttached() && !divertAttach("true", err)) {
#if defined(CLUMSY_OCR)
        // OCR preflight succeeded: tear its worker down, or it would keep
        // scanning and moving the loss gear with no intercept running.
        uiOcrGlueStop();
#endif
        swfilterClear();
        uiSimpleSetStatus(err);
        return IUP_DEFAULT;
    }
    // Beginner mode intercepts everything at the kernel; ownership and
    // the advanced text gate packets in shouldAffect().
    if (!divertStartAffect("true", err)) {
        if (exe == NULL) {
            divertDetach();
        }
#if defined(CLUMSY_OCR)
        uiOcrGlueStop();
#endif
        swfilterClear();
        uiSimpleSetStatus(err);
        return IUP_DEFAULT;
    }

    divertSetReinjectObserver(uiSimpleOnReinject);
    if (uiSimpleTimerSeconds(&seconds)) {
        uiSimpleArmRecovery(seconds, (Icallback)uiStopCb);
    }
    uiSimpleSetRunning(TRUE, TRUE);
    uiAdvancedSetFilterEditable(FALSE);
    IupSetAttribute(iconTimer, "RUN", "YES");
    statusAffecting();
    return IUP_DEFAULT;
}

static int uiStopCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
#if defined(CLUMSY_OCR)
    // Stop the observer and deliver its final queued gear change before
    // the affect session tears down.
    uiOcrGlueStop();
#endif
    divertSetReinjectObserver(NULL);
    divertStopAffect();
    swfilterClear();
    if (uiSimpleSelectedApp() == NULL) {
        // Global mode: the persistent session only existed for this run.
        divertDetach();
    } else {
        // App mode: SNIFF goes back to feeding the idle chart.
        divertSetSniffObserver(uiSimpleOnSniff);
    }
    uiSimpleSetRunning(FALSE, TRUE);
    uiAdvancedSetFilterEditable(TRUE);
    IupSetAttribute(iconTimer, "RUN", "NO");
    uiAdvancedResetActivity();
    uiSimpleSetStatus(tr("status.stopped"));
    return IUP_DEFAULT;
}

static int uiToggleActionCb(Ihandle *ih) {
    if (divertIsAffecting()) {
        return uiStopCb(ih);
    }
    return uiStartCb(ih);
}

// Classic command-line autostart: the filter text is the real kernel
// filter and the beginner loss step stays off; advanced modules run.
static void startParameterized(void) {
    char err[MSG_BUFSIZE];
    uiAdvancedApplyFilterParameter();
    if (!compileAdvancedFilter()) {
        return;
    }
    if (!divertAttach("true", err)
        || !divertStartAffect(uiAdvancedFilterValue(), err)) {
        uiSimpleSetStatus(err);
        swfilterClear();
        return;
    }
    divertSetReinjectObserver(uiSimpleOnReinject);
    uiSimpleSetRunning(TRUE, FALSE);
    uiAdvancedSetFilterEditable(FALSE);
    IupSetAttribute(iconTimer, "RUN", "YES");
    statusAffecting();
}

// Language buttons: flat, blue-and-white for the active language, light
// gray for the other, so the default (Chinese) state reads as selected.
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
    Lang lang = i18nGetLang();
    styleLangButton(zhBtn, lang == LANG_ZH);
    styleLangButton(enBtn, lang == LANG_EN);
}

// Custom tab headers: two flat buttons above a holder that mounts exactly
// one page at a time. A native IupTabs reports one natural size for BOTH
// pages (widest/tallest child wins) and IUP never shrinks a child below its
// natural width, so the basic window always reserved the advanced page's
// width and clipped. Only the mounted page contributes to the vbox natural
// size, so fitting is just vbox NATURALSIZE + slack.
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

static void syncTabHeaders(void) {
    styleTabButton(basicTabBtn, activePage == simpleBox);
    styleTabButton(advTabBtn, activePage == advancedBox);
}

// Outer slack around the vbox natural size in this IUP build (NCMARGIN
// "6x6" + NCGAP "6x4" still leave a few unreported pixels); tuned in the
// headless harness so neither page clips. Keep in sync with those attrs.
#define VBOX_SLACK_W    12
#define VBOX_SLACK_H    12
// Title bar + borders measured on Win11 at RESIZE=NO
// (RASTERSIZE 500x600 -> client 494x571). The post-show pass measures the
// real native chrome; these only set the opening size pre-map.
#define CHROME_W_INIT   6
#define CHROME_H_INIT   29

static BOOL parseNatural(Ihandle *ih, int *w, int *h) {
    const char *s = IupGetAttribute(ih, "NATURALSIZE");
    return s != NULL && sscanf(s, "%dx%d", w, h) == 2
        && *w > 0 && *w < 10000 && *h > 0 && *h < 10000;
}

static BOOL computeContentSize(int *outW, int *outH) {
    int vw, vh;
    IupRefresh(dialog);
    if (!parseNatural(dialogVBox, &vw, &vh)) {
        return FALSE;
    }
    *outW = vw + VBOX_SLACK_W;
    *outH = vh + VBOX_SLACK_H;
    return TRUE;
}

// Size the opening window before map: storing RASTERSIZE works pre-map.
static void fitPreMap(void) {
    int contentW, contentH;
    char raster[32];
    if (!computeContentSize(&contentW, &contentH)) {
        return;
    }
    snprintf(raster, sizeof(raster), "%dx%d",
             contentW + CHROME_W_INIT, contentH + CHROME_H_INIT);
    IupStoreAttribute(dialog, "RASTERSIZE", raster);
    IupRefresh(dialog);
}

// Post-map fit: in IUP 3.30 storing a new dialog RASTERSIZE after map is a
// no-op, but a native SetWindowPos flows back through WM_SIZE and IUP
// re-lays the children. Re-centers at the same time.
static void fitDialogLive(void) {
    int contentW, contentH;
    int chromeW = CHROME_W_INIT, chromeH = CHROME_H_INIT;
    HWND dialogHwnd;
    RECT wr, cr;
    MONITORINFO mi;
    int outerW, outerH, x, y;

    if (dialog == NULL || !computeContentSize(&contentW, &contentH)) {
        return;
    }
    dialogHwnd = (HWND)(intptr_t)IupGetAttribute(dialog, "HWND");
    if (dialogHwnd == NULL || !GetWindowRect(dialogHwnd, &wr)
        || !GetClientRect(dialogHwnd, &cr)) {
        return;
    }
    chromeW = (wr.right - wr.left) - cr.right;
    chromeH = (wr.bottom - wr.top) - cr.bottom;
    outerW = contentW + chromeW;
    outerH = contentH + chromeH;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(dialogHwnd,
                                          MONITOR_DEFAULTTONEAREST), &mi)) {
        x = mi.rcWork.left
            + (mi.rcWork.right - mi.rcWork.left - outerW) / 2;
        y = mi.rcWork.top
            + (mi.rcWork.bottom - mi.rcWork.top - outerH) / 2;
    } else {
        x = wr.left;
        y = wr.top;
    }
    SetWindowPos(dialogHwnd, NULL, x, y, outerW, outerH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    IupRefresh(dialog);
}

// Both pages are permanent children of the holder. Detaching a page and
// re-appending + IupMap on IUP 3.30 mingw6 loses native interactive state
// (combo selection, radio dots, slider thumb, text fields come back blank,
// even with IupUnmap in between), so a tab switch never remaps: the active
// page is shown and kept in normal flow, the other is hidden AND taken out
// of flow with FLOATING=YES. A merely VISIBLE=NO child is still walked when
// the holder computes its natural size (which would force max-page sizing);
// FLOATING is what keeps the per-page window fit tight.
static void selectTab(Ihandle *page) {
    Ihandle *other;
    BOOL mapped;
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
    mapped = dialog != NULL
             && IupGetAttribute(dialog, "HWND") != NULL;
    if (mapped) {
        fitDialogLive();
    }
}

static int basicTabCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    selectTab(simpleBox);
    return IUP_DEFAULT;
}

static int advTabCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    selectTab(advancedBox);
    return IUP_DEFAULT;
}

static void showAdvancedTab(void) {
    selectTab(advancedBox);
}

static int languageCb(Ihandle *ih);

void init(int argc, char* argv[]) {
    // filter presets before anything touches the UI
    uiAdvancedLoadConfig();

    IupOpen(&argc, &argv);
    // source files are UTF-8; switch IUP to UTF-8 before any dialog exists
    IupSetGlobal("UTF8MODE", "YES");
#if defined(CLUMSY_OCR)
    // IupMatrix lives in iupcontrols; needed by the OCR region/group tables
    IupControlsOpen();
#endif
    i18nSetLang(i18nDetect());

    // parse arguments before building module rows (parameterized layout)
    LOG("argc: %d", argc);
    if (argc > 1) {
        if (!parseArgs(argc, argv)) {
            fprintf(stderr, "invalid argument count. ensure you're using options as \"--drop on\"");
            exit(-1);
        }
        parameterized = 1;
    }

    simpleBox = uiSimpleCreate((Icallback)uiToggleActionCb, watchApp);
    advancedBox = uiAdvancedCreate();

#if defined(CLUMSY_OCR)
    {
        // Section ⑤ mounts between the live chart and the Start row;
        // its own Start/Stop button drives the same uiToggleActionCb.
        Ihandle *ocrSection =
            uiOcrCreate((Icallback)uiToggleActionCb);
        uiSimpleAppendSection(ocrSection);
        uiOcrGlueInit();
        uiOcrGlueLoadSavedConfig(); // missing file: silent defaults
    }
#endif

    // One header row: tab headers flush-left, language switch flush-right,
    // all controls the same 26px height so their edges and text baseline
    // align. Basic is the default tab.
    basicTabBtn = IupFlatButton(NULL);
    advTabBtn = IupFlatButton(NULL);
    i18nBind(basicTabBtn, "TITLE", "tab.basic");
    i18nBind(advTabBtn, "TITLE", "advanced.toggle");
    IupSetCallback(basicTabBtn, "FLAT_ACTION", (Icallback)basicTabCb);
    IupSetCallback(advTabBtn, "FLAT_ACTION", (Icallback)advTabCb);
    IupSetAttribute(basicTabBtn, "RASTERSIZE", "96x26");
    IupSetAttribute(advTabBtn, "RASTERSIZE", "96x26");

    // The active language reads as a pressed/selected button.
    zhBtn = IupFlatButton("中");
    enBtn = IupFlatButton("EN");
    IupSetCallback(zhBtn, "FLAT_ACTION", (Icallback)languageCb);
    IupSetCallback(enBtn, "FLAT_ACTION", (Icallback)languageCb);
    IupSetInt(zhBtn, "LANGID", LANG_ZH);
    IupSetInt(enBtn, "LANGID", LANG_EN);
    IupSetAttribute(zhBtn, "RASTERSIZE", "36x26");
    IupSetAttribute(enBtn, "RASTERSIZE", "40x26");
    syncLangButtons();

    headerBox = IupHbox(basicTabBtn, advTabBtn, IupFill(), zhBtn, enBtn, NULL);
    IupSetAttribute(headerBox, "GAP", "4");

    // Both pages are permanent children; selectTab only flips VISIBLE and
    // FLOATING, so native control state survives every tab round trip.
    tabHolder = IupVbox(simpleBox, advancedBox, NULL);
    IupSetAttribute(tabHolder, "EXPAND", "YES");
    dialogVBox = IupVbox(headerBox, tabHolder, NULL);

    dialog = IupDialog(dialogVBox);
    snprintf(dialogTitle, sizeof(dialogTitle), "%s v%s",
             tr("app.title"), CLUMSY_VERSION);
    IupSetAttribute(dialog, "TITLE", dialogTitle);
    IupSetAttribute(dialog, "RASTERSIZE", "500x600");
    IupSetAttribute(dialog, "RESIZE", "NO");
    IupSetCallback(dialog, "SHOW_CB", (Icallback)uiOnDialogShow);
#if defined(CLUMSY_OCR)
    // Section ⑤ expands in place on the fixed-size (RESIZE=NO) dialog:
    // grow/re-center the mapped window so the rule table, log and the
    // in-block Start button are never clipped.
    uiOcrSetLayoutChangedCallback(fitDialogLive);
#endif
    IupSetAttribute(dialogVBox, "ALIGNMENT", "ALEFT");
    IupSetAttribute(dialogVBox, "NCMARGIN", "6x6");
    IupSetAttribute(dialogVBox, "NCGAP", "6x4");
    // Classic CLI mode opens on the advanced page; basic otherwise. Done
    // after the dialog tree exists but still pre-map, so the opening fit
    // measures only the visible, non-floating page.
    activePage = NULL;
    selectTab(parameterized ? advancedBox : simpleBox);
    // Fit the opening window to the active page before map (no flash).
    fitPreMap();

    iconTimer = IupTimer();
    IupSetAttribute(iconTimer, "TIME", STR(ICON_UPDATE_MS));
    IupSetCallback(iconTimer, "ACTION_CB", (Icallback)uiIconTimerCb);

    // program timeout (seconds via global), unchanged from classic clumsy
    if (IupGetGlobal("timeout") != NULL) {
        const char *arg_value = IupGetGlobal("timeout");
        char valueBuf[16];
        snprintf(valueBuf, sizeof(valueBuf), "%s000", arg_value);
        timeoutTimer = IupTimer();
        IupStoreAttribute(timeoutTimer, "TIME", valueBuf);
        IupSetCallback(timeoutTimer, "ACTION_CB", (Icallback)uiTimeoutCb);
        IupSetAttribute(timeoutTimer, "RUN", "YES");
    }
}

void startup(void) {
    srand((unsigned int)time(NULL));
    IupShowXY(dialog, IUP_CENTER, IUP_CENTER);
    IupMainLoop();
}

void cleanup(void) {
#if defined(CLUMSY_OCR)
    uiOcrGlueCleanup();
#endif
    divertStopAffect();
    divertDetach();
    IupDestroy(iconTimer);
    if (timeoutTimer != NULL) {
        IupDestroy(timeoutTimer);
    }
    IupClose();
    endTimePeriod();
}

// in fact only 32bit binary would run on 64 bit os
static BOOL check32RunningOn64(HWND hWnd) {
    BOOL is64ret;
    if (IsWow64Process(GetCurrentProcess(), &is64ret) && is64ret) {
        MessageBox(hWnd, (LPCSTR)"You're running 32bit clumsy on 64bit Windows, which wouldn't work. Please use the 64bit clumsy version.",
            (LPCSTR)"Aborting", MB_OK);
        return TRUE;
    }
    return FALSE;
}

static BOOL checkIsRunning(void) {
    HANDLE hStartEvent = CreateEventW(NULL, FALSE, FALSE,
                                      L"Global\\CLUMSY_IS_RUNNING_EVENT_NAME");
    if (hStartEvent == NULL) {
        return TRUE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hStartEvent);
        return TRUE;
    }
    return FALSE;
}

static int uiOnDialogShow(Ihandle *ih, int state) {
    HWND hWnd;
    BOOL exit;
    HICON icon;
    HINSTANCE hInstance;
    if (state != IUP_SHOW) return IUP_DEFAULT;
    hWnd = (HWND)IupGetAttribute(ih, "HWND");
    hInstance = GetModuleHandle(NULL);

    // Re-fit to the mounted page against live native chrome; fitDialogLive
    // also re-centers.
    fitDialogLive();

    icon = LoadIcon(hInstance, "CLUMSY_ICON");
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);

    if (checkIsRunning()) {
        MessageBox(hWnd, (LPCSTR)"Theres' already an instance of clumsy running.",
            (LPCSTR)"Aborting", MB_OK);
        return IUP_CLOSE;
    }

#ifdef _WIN32
    if (check32RunningOn64(hWnd)) {
        return IUP_CLOSE;
    }
#endif

    exit = tryElevate(hWnd, parameterized);
    if (!exit && parameterized) {
        LOG("is parameterized, start filtering upon execution.");
        startParameterized();
    }
    return exit ? IUP_CLOSE : IUP_DEFAULT;
}

static int uiIconTimerCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    uiAdvancedTimerTick();
    return IUP_DEFAULT;
}

static int uiTimeoutCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    return IUP_CLOSE;
}

static int languageCb(Ihandle *ih) {
    Lang lang = (Lang)IupGetInt(ih, "LANGID");
    if (i18nGetLang() == lang) {
        return IUP_DEFAULT;
    }
    i18nSetLang(lang);
    i18nRetranslate();
    uiSimpleRetranslate();
#if defined(CLUMSY_OCR)
    uiOcrRetranslate();
#endif
    snprintf(dialogTitle, sizeof(dialogTitle), "%s v%s",
             tr("app.title"), CLUMSY_VERSION);
    IupSetAttribute(dialog, "TITLE", dialogTitle);
    // Tab titles and page label widths changed with the language; restyle
    // the headers (bound TITLE already retranslated) and re-fit the window.
    syncLangButtons();
    syncTabHeaders();
    fitDialogLive();
    return IUP_DEFAULT;
}

int main(int argc, char* argv[]) {
    LOG("Is Run As Admin: %d", IsRunAsAdmin());
    LOG("Is Elevated: %d", IsElevated());
    init(argc, argv);
    startup();
    cleanup();
    return 0;
}
