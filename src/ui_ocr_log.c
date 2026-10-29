#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iup.h"
#include "common.h"
#include "i18n.h"
#include "ui_ocr_log.h"

#if defined(CLUMSY_OCR)

#define OCR_LOG_MAX_LINES 500

static Ihandle *g_log, *g_csvLabel, *g_openDirBtn, *g_gearLabel;
static char g_csvPath[512] = "";
static char *g_logText = NULL;

// Live gear shown by the readout and stamped into each line prefix.
// -1 = no automation run driving loss right now.
static int g_gear = -1;
static BOOL g_fixedClock = FALSE;
static SYSTEMTIME g_fixedTime;

static size_t countLines(const char *s) {
    size_t n = 0;
    if (s == NULL) return 0;
    for (; *s != '\0'; s++) {
        if (*s == '\n') n++;
    }
    return n;
}

static void currentLocalTime(SYSTEMTIME *out) {
    if (g_fixedClock) {
        *out = g_fixedTime;
    } else {
        GetLocalTime(out);
    }
}

static void renderGearLabel(void) {
    char title[64];
    if (g_gearLabel == NULL) return;
    if (g_gear >= 0) {
        snprintf(title, sizeof(title), tr("ocr.log.gearNow"), g_gear);
    } else {
        snprintf(title, sizeof(title), "%s", tr("ocr.log.gearIdle"));
    }
    IupStoreAttribute(g_gearLabel, "TITLE", title);
}

void uiOcrLogSetGear(int percent) {
    if (percent < -1) percent = -1;
    if (percent > 100) percent = 100;
    g_gear = percent;
    renderGearLabel();
}

void uiOcrLogRetranslate(void) {
    renderGearLabel();
}

void uiOcrLogSetTimeForTest(const SYSTEMTIME *fixed) {
    if (fixed != NULL) {
        g_fixedTime = *fixed;
        g_fixedClock = TRUE;
    } else {
        g_fixedClock = FALSE;
    }
}

Ihandle *uiOcrLogGearLabelForTest(void) {
    return g_gearLabel;
}

static void rebuildLog(void) {
    IupStoreAttribute(g_log, "VALUE", g_logText != NULL ? g_logText : "");
    // Reverse chronological order: the newest line is the head line, so
    // pin the view to line 1 instead of scrolling to the bottom.
    IupSetInt(g_log, "TOPLINE", 1);
}

// "HH:MM:SS [丢包 60%] " while a run drives the gear; timestamp only
// when idle. 64 covers the UTF-8 tag at three digits with room to spare.
static void buildPrefix(char *out, size_t cap) {
    SYSTEMTIME st;
    currentLocalTime(&st);
    if (g_gear >= 0) {
        char tag[32];
        snprintf(tag, sizeof(tag), tr("ocr.log.gearTag"), g_gear);
        snprintf(out, cap, "%02u:%02u:%02u [%s] ",
                 (unsigned)st.wHour, (unsigned)st.wMinute,
                 (unsigned)st.wSecond, tag);
    } else {
        snprintf(out, cap, "%02u:%02u:%02u ",
                 (unsigned)st.wHour, (unsigned)st.wMinute,
                 (unsigned)st.wSecond);
    }
}

void uiOcrAppendLog(const char *line) {
    char *grown;
    char prefix[64];
    char *full;
    size_t lineLen, prefixLen;
    size_t oldLen = g_logText != NULL ? strlen(g_logText) : 0;
    if (line == NULL || g_log == NULL) return;

    buildPrefix(prefix, sizeof(prefix));
    prefixLen = strlen(prefix);
    lineLen = strlen(line);

    // Compose the timestamped line on a scratch block; the head buffer
    // itself only ever contains fully built lines.
    full = malloc(prefixLen + lineLen + 1);
    if (full == NULL) return;
    memcpy(full, prefix, prefixLen);
    memcpy(full + prefixLen, line, lineLen);
    full[prefixLen + lineLen] = '\0';
    line = full;
    lineLen += prefixLen;
    grown = realloc(g_logText, oldLen + lineLen + 2);
    if (grown == NULL) {
        free(full);
        return;
    }
    g_logText = grown;
    // Head insertion ("line\noldText"); memmove handles the overlap of
    // shifting the existing content right within the grown block.
    if (oldLen > 0) {
        memmove(g_logText + lineLen + 1, g_logText, oldLen + 1);
    }
    memcpy(g_logText, line, lineLen);
    g_logText[lineLen] = oldLen > 0 ? '\n' : '\0';
    if (oldLen > 0) g_logText[lineLen + 1 + oldLen] = '\0';

    // Bounded head: drop whole OLDEST tail lines past the cap.
    while (countLines(g_logText) >= OCR_LOG_MAX_LINES) {
        char *nl = strrchr(g_logText, '\n');
        if (nl == NULL) break;
        *nl = '\0';
    }
    rebuildLog();
    free(full);
}

static int openDirCb(Ihandle *ih) {
    char args[600];
    UNREFERENCED_PARAMETER(ih);
    if (g_csvPath[0] == '\0') return IUP_DEFAULT;
    snprintf(args, sizeof(args), "/select,\"%s\"", g_csvPath);
    ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
    return IUP_DEFAULT;
}

void uiOcrSetCsvPath(const char *path) {
    if (path == NULL) path = "";
    strncpy(g_csvPath, path, sizeof(g_csvPath) - 1);
    g_csvPath[sizeof(g_csvPath) - 1] = '\0';
    IupStoreAttribute(g_csvLabel, "TITLE", g_csvPath);
}

Ihandle *uiOcrLogBuild(void) {
    Ihandle *csvLbl, *csvRow, *gearRow, *box;

    // Always-visible live gear readout: gear changes scroll out of the
    // 500-line log within minutes, so the loss currently in force sits
    // above the box and updates synchronously with every OCR switch.
    g_gearLabel = IupFlatLabel("");
    IupSetAttribute(g_gearLabel, "FGCOLOR", "30 30 30");
    IupSetAttribute(g_gearLabel, "FONT", "Segoe UI, 12");
    renderGearLabel();
    gearRow = IupHbox(g_gearLabel, IupFill(), NULL);

    g_log = IupMultiLine(NULL);
    IupSetAttribute(g_log, "READONLY", "YES");
    IupSetAttribute(g_log, "EXPAND", "HORIZONTAL");
    IupSetAttribute(g_log, "RASTERSIZE", "0x112");
    IupSetAttribute(g_log, "SCROLLBAR", "VERTICAL");

    csvLbl = IupLabel(tr("ocr.csvPath"));
    i18nBind(csvLbl, "TITLE", "ocr.csvPath");
    g_csvLabel = IupFlatLabel("");
    IupSetAttribute(g_csvLabel, "FGCOLOR", "110 110 110");
    IupSetAttribute(g_csvLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(g_csvLabel, "WRAP", "YES");
    g_openDirBtn = IupFlatButton(tr("ocr.openDir"));
    i18nBind(g_openDirBtn, "TITLE", "ocr.openDir");
    IupSetCallback(g_openDirBtn, "FLAT_ACTION", (Icallback)openDirCb);
    csvRow = IupHbox(csvLbl, g_csvLabel, g_openDirBtn, NULL);

    box = IupVbox(gearRow, g_log, csvRow, NULL);
    return box;
}

Ihandle *uiOcrLogForTest(void) {
    return g_log;
}

Ihandle *uiOcrLogOpenDirButtonForTest(void) {
    return g_openDirBtn;
}

#else // !CLUMSY_OCR — no-op stubs for builds without OCR

#include <stddef.h>

Ihandle *uiOcrLogBuild(void) { return NULL; }
void uiOcrAppendLog(const char *line) { (void)line; }
void uiOcrLogSetGear(int percent) { (void)percent; }
void uiOcrLogRetranslate(void) {}
void uiOcrLogSetTimeForTest(const SYSTEMTIME *fixed) { (void)fixed; }
Ihandle *uiOcrLogGearLabelForTest(void) { return NULL; }
void uiOcrSetCsvPath(const char *path) { (void)path; }
Ihandle *uiOcrLogForTest(void) { return NULL; }
Ihandle *uiOcrLogOpenDirButtonForTest(void) { return NULL; }

#endif // CLUMSY_OCR
