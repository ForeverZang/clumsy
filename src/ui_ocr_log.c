#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iup.h"
#include "common.h"
#include "i18n.h"
#include "ocr_timeline.h"
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

// "HH:MM:SS.mmm [丢包 60%] " while a run drives the gear; timestamp with
// milliseconds only when idle. 96 covers the UTF-8 tag at three digits
// with room to spare; lines can arrive within the same second, so the
// millisecond field is what orders them visually.
static void buildPrefix(char *out, size_t cap) {
    SYSTEMTIME st;
    currentLocalTime(&st);
    if (g_gear >= 0) {
        char tag[32];
        snprintf(tag, sizeof(tag), tr("ocr.log.gearTag"), g_gear);
        snprintf(out, cap, "%02u:%02u:%02u.%03u [%s] ",
                 (unsigned)st.wHour, (unsigned)st.wMinute,
                 (unsigned)st.wSecond, (unsigned)st.wMilliseconds, tag);
    } else {
        snprintf(out, cap, "%02u:%02u:%02u.%03u ",
                 (unsigned)st.wHour, (unsigned)st.wMinute,
                 (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
    }
}

void uiOcrAppendLog(const char *line) {
    char *grown;
    char prefix[96];
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

// UTF-8 -> UTF-16 for the wide shell APIs. Returns the character count
// excluding the terminating NUL; 0 on invalid input or truncation, with
// an empty string written so callers can never act on a half path.
static int utf8ToWide(const char *s, wchar_t *out, int cap) {
    int n;
    if (s == NULL || out == NULL || cap <= 0) return 0;
    out[0] = L'\0';
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    if (n <= 0) {
        out[0] = L'\0';
        return 0;
    }
    return n - 1;
}

// What the button reveals: the current CSV once a run has produced one,
// otherwise the default ocr-runs directory (created on demand), so the
// button is never a silent no-op. Returns buf, or NULL if even the
// executable-directory fallback is unavailable.
static const char *revealTarget(char *buf, size_t cap) {
    char fallback[1024];
    if (buf == NULL || cap == 0) return NULL;
    buf[0] = '\0';
    if (g_csvPath[0] != '\0') {
        strncpy(buf, g_csvPath, cap - 1);
        buf[cap - 1] = '\0';
        return buf;
    }
    if (ocrTimelineDefaultDir(buf, cap, fallback, sizeof(fallback)))
        return buf;
    if (fallback[0] != '\0') {
        strncpy(buf, fallback, cap - 1);
        buf[cap - 1] = '\0';
        return buf;
    }
    return NULL;
}

// The reveal button must never fail silently: every refused shell call
// lands a line in the OCR log with the shell error code.
static void logOpenDirError(unsigned long code) {
    char msg[160];
    snprintf(msg, sizeof(msg), tr("ocr.log.openDirFailed"), code);
    uiOcrAppendLog(msg);
}

// Local-resolution failure (no run directory, no backslash in the CSV
// path) is not a shell error, so it gets its own wording instead of
// "system error 0".
static void logOpenDirUnavailable(void) {
    uiOcrAppendLog(tr("ocr.log.openDirUnavailable"));
}

// Open a directory in Explorer via the wide explore verb. ShellExecuteW
// returns an HINSTANCE at or below 32 on failure (the SE_ERR_* code), not
// a usable instance.
static BOOL exploreDir(const wchar_t *dir) {
    HINSTANCE r = ShellExecuteW(NULL, L"explore", dir, NULL, NULL,
                                SW_SHOWNORMAL);
    if ((ULONG_PTR)r <= 32) {
        logOpenDirError((unsigned long)(ULONG_PTR)r);
        return FALSE;
    }
    return TRUE;
}

static int openDirCb(Ihandle *ih) {
    char target[1024];
    wchar_t wtarget[1024];
    const char *path;
    UNREFERENCED_PARAMETER(ih);

    path = revealTarget(target, sizeof(target));
    if (path == NULL
        || !utf8ToWide(path, wtarget,
                       (int)(sizeof(wtarget) / sizeof(wtarget[0])))) {
        logOpenDirUnavailable();
        return IUP_DEFAULT;
    }

    if (g_csvPath[0] != '\0') {
        // Canonical "open the folder and select the file" call. Wide, so
        // non-ANSI paths (a Chinese user name inside %LOCALAPPDATA%)
        // survive; it also brokers from an elevated clumsy to the user's
        // medium-IL Explorer, where ShellExecuteA("explorer.exe",
        // "/select,...") historically had its switches dropped and
        // opened a default folder instead of revealing the CSV.
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(wtarget);
        if (pidl != NULL) {
            // IUP/OLE normally owns apartment init on this GUI thread.
            // Every SUCCEEDED CoInitializeEx (S_OK or S_FALSE) bumps the
            // per-thread refcount and gets one CoUninitialize; a failure
            // such as RPC_E_CHANGED_MODE adds nothing and means the STA
            // shell API must not be called here -- fall through.
            HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            HRESULT hr = E_FAIL;
            if (SUCCEEDED(com)) {
                // S_OK and S_FALSE both bump this thread's init count, so
                // both must be paired; the apartment only actually tears
                // down when the last reference (e.g. IUP/OLE's own init)
                // is balanced. RPC_E_CHANGED_MODE is a failure: no pair.
                hr = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
                CoUninitialize();
            }
            ILFree(pidl);
            if (SUCCEEDED(hr)) return IUP_DEFAULT;
        }
        // Select unavailable (e.g. broker refusal or changed apartment):
        // open the containing folder itself through the wide verb.
        {
            wchar_t *slash = wcsrchr(wtarget, L'\\');
            if (slash != NULL && slash != wtarget) {
                *slash = L'\0';
                exploreDir(wtarget);
            } else {
                logOpenDirUnavailable();
            }
        }
        return IUP_DEFAULT;
    }

    // Before the first run: open the ocr-runs directory itself.
    exploreDir(wtarget);
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
    // The title is rewritten after map on every gear change and after a
    // live language switch; a natural-width canvas keeps its creation
    // width and clips the longer wording (up to and including the '%'
    // unit). Spanning the row reserves room for any bilingual wording at
    // any gear, and left alignment keeps the readout anchored.
    // Default alignment is "ALEFT:ACENTER" (probe-verified on this IUP
    // build), so the text stays anchored once it spans the full row.
    IupSetAttribute(g_gearLabel, "EXPAND", "HORIZONTAL");
    renderGearLabel();
    gearRow = IupHbox(g_gearLabel, NULL);

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

int uiOcrLogUtf8ToWideForTest(const char *utf8, wchar_t *out, int cap) {
    return utf8ToWide(utf8, out, cap);
}
const char *uiOcrLogRevealTargetForTest(char *buf, size_t cap) {
    return revealTarget(buf, cap);
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
int uiOcrLogUtf8ToWideForTest(const char *utf8, wchar_t *out, int cap) {
    (void)utf8; (void)out; (void)cap; return 0;
}
const char *uiOcrLogRevealTargetForTest(char *buf, size_t cap) {
    (void)buf; (void)cap; return NULL;
}

#endif // CLUMSY_OCR
