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
    uiSimpleApplyExternalLoss(percent);
    snprintf(line, sizeof(line), tr("ocr.log.loss"), percent);
    uiOcrAppendLog(line);
}

static const char *runRegionName(int index) {
    static char fallback[16];
    if ((size_t)index < g_runCfg.ruleCount
        && g_runCfg.rules[index].name[0]) {
        return g_runCfg.rules[index].name;
    }
    snprintf(fallback, sizeof(fallback), "r%d", index);
    return fallback;
}

static void glueOnEvent(const OcrSessionEvent *ev, void *user) {
    char line[GLUE_LOG_CAP];
    (void)user;
    const char *name = runRegionName(ev->regionIndex);

    if (strcmp(ev->event, "rule_trigger") == 0) {
        const char *text = (ev->text && ev->text[0])
                           ? ev->text : tr("ocr.log.emptyText");
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
    if (cfg.ruleCount == 0) {
        uiOcrShowError("ocr.error.noRegions");
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
    uiOcrAppendLog(tr("ocr.log.stop"));
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
