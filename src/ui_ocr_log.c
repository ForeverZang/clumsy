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

static Ihandle *g_log, *g_csvLabel, *g_openDirBtn;
static char g_csvPath[512] = "";
static char *g_logText = NULL;

static size_t countLines(const char *s) {
    size_t n = 0;
    if (s == NULL) return 0;
    for (; *s != '\0'; s++) {
        if (*s == '\n') n++;
    }
    return n;
}

static void rebuildLog(void) {
    IupStoreAttribute(g_log, "VALUE", g_logText != NULL ? g_logText : "");
    // Keep the newest line visible: TOPLINE clamps to the last line when
    // the value is past the end (native multiline behavior).
    IupSetInt(g_log, "TOPLINE", (int)countLines(g_logText) + 1);
}

void uiOcrAppendLog(const char *line) {
    char *grown;
    size_t oldLen = g_logText != NULL ? strlen(g_logText) : 0;
    if (line == NULL || g_log == NULL) return;

    grown = realloc(g_logText, oldLen + strlen(line) + 2);
    if (grown == NULL) return;
    g_logText = grown;
    // realloc(NULL,...) (the first append) leaves the bytes
    // uninitialized: terminate at oldLen before any strcat, or the
    // newline/line land after heap garbage and corrupt the heap.
    g_logText[oldLen] = '\0';
    if (oldLen > 0) strcat(g_logText, "\n");
    strcat(g_logText, line);

    // Keep a bounded tail: drop whole leading lines past the cap.
    while (countLines(g_logText) >= OCR_LOG_MAX_LINES) {
        char *nl = strchr(g_logText, '\n');
        if (nl == NULL) break;
        memmove(g_logText, nl + 1, strlen(nl + 1) + 1);
    }
    rebuildLog();
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
    Ihandle *csvLbl, *csvRow, *box;

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

    box = IupVbox(g_log, csvRow, NULL);
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
void uiOcrSetCsvPath(const char *path) { (void)path; }
Ihandle *uiOcrLogForTest(void) { return NULL; }
Ihandle *uiOcrLogOpenDirButtonForTest(void) { return NULL; }

#endif // CLUMSY_OCR
