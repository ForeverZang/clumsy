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
static Ihandle *dialog, *iconTimer;
static Ihandle *advancedBox, *advancedToggle;
static Ihandle *timeoutTimer = NULL;

static char dialogTitle[128];

static int uiOnDialogShow(Ihandle *ih, int state);
static int uiStartCb(Ihandle *ih);
static int uiStopCb(Ihandle *ih);
static int uiToggleActionCb(Ihandle *ih);
static int uiIconTimerCb(Ihandle *ih);
static int uiTimeoutCb(Ihandle *ih);

// Persistent SNIFF lifecycle for a picked app. NULL ("all applications")
// tears the persistent session down; global mode attaches at Start.
static void watchApp(const char *exe) {
    char err[MSG_BUFSIZE];
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
    // reveal the folded area so the red line is visible
    IupSetAttribute(advancedBox, "VISIBLE", "YES");
    IupSetAttribute(advancedToggle, "VALUE", "ON");
    IupRefresh(dialog);
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
    exe = uiSimpleSelectedApp();
    if (!divertIsAttached() && !divertAttach("true", err)) {
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

static int advancedToggleCb(Ihandle *ih, int state) {
    IupSetAttribute(advancedBox, "VISIBLE", state ? "YES" : "NO");
    IupRefresh(dialog);
    return IUP_DEFAULT;
}

static int languageCb(Ihandle *ih);

void init(int argc, char* argv[]) {
    Ihandle *dialogVBox, *simpleBox, *advancedHint;
    Ihandle *langRow, *zhBtn, *enBtn;

    // filter presets before anything touches the UI
    uiAdvancedLoadConfig();

    IupOpen(&argc, &argv);
    // source files are UTF-8; switch IUP to UTF-8 before any dialog exists
    IupSetGlobal("UTF8MODE", "YES");
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
    IupSetAttribute(advancedBox, "VISIBLE", "NO");
    advancedToggle = IupToggle(tr("advanced.toggle"), NULL);
    i18nBind(advancedToggle, "TITLE", "advanced.toggle");
    IupSetCallback(advancedToggle, "ACTION", (Icallback)advancedToggleCb);
    advancedHint = IupFlatLabel(tr("advanced.hint"));
    i18nBind(advancedHint, "TITLE", "advanced.hint");
    IupSetAttribute(advancedHint, "FGCOLOR", "110 110 110");
    IupSetAttribute(advancedHint, "WRAP", "YES");
    IupSetAttribute(advancedHint, "EXPAND", "HORIZONTAL");

    // title-bar language switch lives at the window's top-right
    zhBtn = IupButton("中", NULL);
    enBtn = IupButton("EN", NULL);
    IupSetCallback(zhBtn, "ACTION", (Icallback)languageCb);
    IupSetCallback(enBtn, "ACTION", (Icallback)languageCb);
    IupSetInt(zhBtn, "LANGID", LANG_ZH);
    IupSetInt(enBtn, "LANGID", LANG_EN);
    langRow = IupHbox(IupFill(), zhBtn, enBtn, NULL);
    IupSetAttribute(zhBtn, "RASTERSIZE", "36x22");
    IupSetAttribute(enBtn, "RASTERSIZE", "40x22");

    dialog = IupDialog(
        dialogVBox = IupVbox(
            langRow,
            simpleBox,
            advancedToggle,
            advancedHint,
            advancedBox,
            NULL
        )
    );
    snprintf(dialogTitle, sizeof(dialogTitle), "%s v%s",
             tr("app.title"), CLUMSY_VERSION);
    IupSetAttribute(dialog, "TITLE", dialogTitle);
    IupSetAttribute(dialog, "SIZE", "500x");
    IupSetAttribute(dialog, "RESIZE", "NO");
    IupSetCallback(dialog, "SHOW_CB", (Icallback)uiOnDialogShow);
    IupSetAttribute(dialogVBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(dialogVBox, "NCMARGIN", "6x6");
    IupSetAttribute(dialogVBox, "NCGAP", "6x4");

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
    snprintf(dialogTitle, sizeof(dialogTitle), "%s v%s",
             tr("app.title"), CLUMSY_VERSION);
    IupSetAttribute(dialog, "TITLE", dialogTitle);
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
