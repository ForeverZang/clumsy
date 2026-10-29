#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "iup.h"
#include "common.h"
#include "i18n.h"

#if defined(CLUMSY_OCR)

#include "ocr_app.h"
#include "ocr_config.h"
#include "ocr_timeline.h"
#include "ui_ocr.h"
#include "ui_simple.h"

#define GLUE_CONFIG_NAME  "ocr_config.txt"
#define GLUE_MODELS_NAME  "models"
#define GLUE_DRAIN_MS     "100"
#define GLUE_LOG_CAP      512

static Ihandle *g_timer;
static char g_exeDir[1024];
static char g_configPath[1200];
static OcrConfig g_runCfg;

// Bounded length + memcpy + explicit terminator keeps -O2's
// -Wstringop-truncation quiet without weakening the cap.
static void copyBounded(char *dst, size_t cap, const char *src) {
    size_t len = strnlen(src, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int drainCb(Ihandle *ih) {
    (void)ih;
    ocrAppPump();
    return IUP_DEFAULT;
}

static void runDirs(char *timelineDir, size_t dirCap,
                    char *modelsDir, size_t modelsCap) {
    char fallback[1024];
    if (!ocrTimelineDefaultDir(timelineDir, dirCap,
                               fallback, sizeof(fallback))) {
        copyBounded(timelineDir, dirCap, fallback);
    }
    snprintf(modelsDir, modelsCap, "%s\\%s", g_exeDir, GLUE_MODELS_NAME);
}

// Gear changes arrive on the UI thread: mirror onto the slider/loss
// engine and append the bilingual scrolling-log line.
static void glueOnLoss(int percent, void *user) {
    char line[256];
    (void)user;
    // The readout and the line tag update first, so this switch line is
    // already stamped with the gear it announces.
    uiOcrLogSetGear(percent);
    uiSimpleApplyExternalLoss(percent);
    snprintf(line, sizeof(line), tr("ocr.log.loss"), percent);
    uiOcrAppendLog(line);
}

// Groups have no display name yet; the CSV/log identity is "group N"
// (1-based, table order), matching the session's timeline rows.
static const char *runGroupName(int index) {
    // 24 fits "group " (6) + any INT_MAX (10) + NUL, so -O2's
    // -Wformat-truncation stays quiet even though groups are 0..15.
    static char fallback[24];
    snprintf(fallback, sizeof(fallback), "group %d", index + 1);
    return fallback;
}

// Per-region recognition outcome line (one per referenced region per
// tick). Region identity mirrors the tables' R1/R2 labels.
static void glueOnScan(const OcrSessionEvent *ev, char *line, size_t cap) {
    char rname[16];
    const char *text = (ev->text && ev->text[0])
                       ? ev->text : tr("ocr.log.emptyText");
    snprintf(rname, sizeof(rname), "R%d", ev->regionIndex + 1);

    if (strcmp(ev->status, "ok") == 0) {
        snprintf(line, cap, tr("ocr.log.scan.ok"), rname, text);
    } else if (strcmp(ev->status, "unchanged") == 0) {
        snprintf(line, cap, tr("ocr.log.scan.unchanged"), rname, text);
    } else if (strcmp(ev->status, "capture_fail") == 0) {
        snprintf(line, cap, tr("ocr.log.scan.capture"), rname);
    } else if (strcmp(ev->status, "blank_frame") == 0) {
        snprintf(line, cap, tr("ocr.log.scan.blank"), rname);
    } else if (strcmp(ev->status, "read_fail") == 0) {
        snprintf(line, cap, tr("ocr.log.scan.read"), rname);
    } else {
        line[0] = '\0'; // unknown outcome: never log a half-built line
    }
}

static void glueOnEvent(const OcrSessionEvent *ev, void *user) {
    char line[GLUE_LOG_CAP];
    (void)user;

    if (strcmp(ev->event, "scan") == 0) {
        glueOnScan(ev, line, sizeof(line));
        if (line[0] != '\0') uiOcrAppendLog(line);
        return;
    }

    const char *name = runGroupName(ev->groupIndex);

    if (strcmp(ev->event, "rule_trigger") == 0) {
        const char *text = (ev->text && ev->text[0])
                           ? ev->text : tr("ocr.log.emptyText");
        // Deliberate: within the firing tick the worker queues
        // rule_trigger BEFORE loss_change (FIFO drain), so this line is
        // stamped with the gear still in force when the text was seen;
        // the immediately newer "loss switched" line carries the new
        // gear, and the pair reads as the before/after of one switch.
        snprintf(line, sizeof(line), tr("ocr.log.trigger"),
                 name, text, ev->lossPercent);
        uiOcrAppendLog(line);
    } else if (strcmp(ev->event, "capture_unavailable") == 0) {
        snprintf(line, sizeof(line), tr("ocr.log.unavailable"), name);
        uiOcrAppendLog(line);
    }
}

static const char *errorKeyFor(const char *err) {
    if (strncmp(err, "window:", 7) == 0) return "ocr.error.windowStale";
    if (strncmp(err, "engine:", 7) == 0) return "ocr.error.engine";
    if (strncmp(err, "seed:", 5) == 0)   return "ocr.error.seed";
    if (strncmp(err, "timeline:", 9) == 0) return "ocr.error.timeline";
    if (strncmp(err, "worker:", 7) == 0) return "ocr.error.worker";
    return NULL;
}

BOOL uiOcrGlueInit(void) {
    char exe[1024];
    g_exeDir[0] = '\0';
    g_configPath[0] = '\0';
    GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    char *slash = strrchr(exe, '\\');
    if (slash) {
        *slash = '\0';
        copyBounded(g_exeDir, sizeof(g_exeDir), exe);
        snprintf(g_configPath, sizeof(g_configPath), "%s\\%s",
                 exe, GLUE_CONFIG_NAME);
    }

    // An unattached timer is independent of any widget/dialog, so the
    // 100 ms draining survives section folding and dialog rebuilds.
    g_timer = IupTimer();
    if (!g_timer) return FALSE;
    IupSetAttribute(g_timer, "TIME", GLUE_DRAIN_MS);
    IupSetCallback(g_timer, "TIME_CB", (Icallback)drainCb);
    IupSetAttribute(g_timer, "RUN", "YES");
    return TRUE;
}

void uiOcrGlueLoadSavedConfig(void) {
    OcrConfig cfg;
    char err[MSG_BUFSIZE];
    char verr[MSG_BUFSIZE];
    if (g_configPath[0] == '\0') return;
    // Missing file: FALSE, silent all-defaults startup.
    if (!ocrConfigLoad(g_configPath, &cfg, err)) return;
    if (!ocrConfigValidate(&cfg, verr)) {
        uiOcrShowError(verr); // literal English passes through tr()
        return;
    }
    (void)uiOcrPopulate(&cfg);
}

BOOL uiOcrGlueStart(int sliderPercent) {
    if (!uiOcrEnabled()) return TRUE;

    OcrConfig cfg;
    // Row-level invalid input already shows the inline row error.
    if (!uiOcrGetConfig(&cfg)) return FALSE;
    if (cfg.groupCount == 0) {
        uiOcrShowError("ocr.error.noGroups");
        return FALSE;
    }

    CaptureWindow picked;
    CaptureWindow *pickedPtr = NULL;
    if (uiOcrHasPickedWindow() && uiOcrPickedWindow(&picked)) {
        pickedPtr = &picked;
    }

    char timelineDir[1024], modelsDir[1024], err[MSG_BUFSIZE];
    runDirs(timelineDir, sizeof(timelineDir),
            modelsDir, sizeof(modelsDir));

    OcrAppHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.onLoss = glueOnLoss;
    hooks.onSessionEvent = glueOnEvent;

    if (!ocrAppStart(&cfg, pickedPtr, timelineDir, modelsDir,
                     sliderPercent, &hooks, err)) {
        const char *key = errorKeyFor(err);
        uiOcrShowError(key ? tr(key) : err);
        return FALSE;
    }

    g_runCfg = cfg;
    // The readout starts at the slider gear BEFORE any line is logged,
    // so the start line and every following scan line are stamped with
    // the gear in force; a later worker loss_change updates it through
    // glueOnLoss.
    uiOcrLogSetGear(sliderPercent);

    char saveErr[MSG_BUFSIZE];
    if (!ocrConfigSave(g_configPath, &cfg, saveErr)) {
        uiOcrAppendLog(tr("ocr.error.configSave"));
    }

    uiOcrSetCsvPath(ocrAppCsvPath());
    char line[256];
    snprintf(line, sizeof(line), tr("ocr.log.start"), sliderPercent);
    uiOcrAppendLog(line);
    uiOcrSetRunning(TRUE);
    return TRUE;
}

void uiOcrGlueStop(void) {
    if (!ocrAppRunning()) return;
    ocrAppStop();
    // The stop line keeps the last gear tag ("loss at that moment");
    // only then does the readout return to idle, since loss is no
    // longer applied once the affect session tears down.
    uiOcrAppendLog(tr("ocr.log.stop"));
    uiOcrLogSetGear(-1);
    uiOcrSetRunning(FALSE);
}

void uiOcrGlueCleanup(void) {
    if (ocrAppRunning()) ocrAppStop();
    if (g_timer) {
        IupSetAttribute(g_timer, "RUN", "NO");
        IupDestroy(g_timer);
        g_timer = NULL;
    }
}

#else // !CLUMSY_OCR — OCR block compiled out

BOOL uiOcrGlueInit(void) { return TRUE; }
void uiOcrGlueLoadSavedConfig(void) {}
BOOL uiOcrGlueStart(int sliderPercent) { (void)sliderPercent; return TRUE; }
void uiOcrGlueStop(void) {}
void uiOcrGlueCleanup(void) {}

#endif // CLUMSY_OCR
