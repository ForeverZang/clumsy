#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "ocr_app.h"
#include "ocr_config.h"
#include "ocr_capture.h"
#include "ocr_engine.h"
#include "ocr_session.h"
#include "ocr_timeline.h"
#include "ocr_worker.h"

// Internal default (spec §4.6: difference threshold is an internal
// default); the session tests pin their scenarios at the same value.
#define OCR_APP_MAD_THRESHOLD 5
#define OCR_APP_ENUM_CAP      64
#define OCR_APP_CSV_CAP       1200

static struct {
    OcrSession        session;
    OcrSessionRegion  regions[OCR_MAX_REGIONS];
    OcrSessionGroup   groups[OCR_MAX_GROUPS];
    OcrEngine     *engine;
    OcrTimeline   *timeline;
    char           csvPath[OCR_APP_CSV_CAP];
    OcrAppHooks    hooks;
    OcrEngineFactory factory;
    BOOL           running;
} g_app;

static void prefixError(char *err, const char *code) {
    char tmp[MSG_BUFSIZE];
    if (!err) return;
    snprintf(tmp, sizeof(tmp), "%s: %s", code, err[0] ? err : "failed");
    memcpy(err, tmp, sizeof(tmp));
}

// Release everything acquired during a (possibly partial) start. When
// discard is TRUE the run never began, so the header-only CSV is removed
// rather than left in the runs directory.
static void releaseResources(BOOL discardCsv) {
    if (g_app.timeline) {
        ocrTimelineClose(g_app.timeline);
        g_app.timeline = NULL;
        if (discardCsv && g_app.csvPath[0]) {
            DeleteFileA(g_app.csvPath);
        }
    }
    if (g_app.engine) {
        ocrEngineDestroy(g_app.engine);
        g_app.engine = NULL;
    }
}

static BOOL resolveTarget(const OcrConfig *cfg,
                          const CaptureWindow *picked,
                          CaptureWindow *out, char *err) {
    if (picked && picked->hwnd) {
        if (!captureIsCapturable(picked->hwnd)) {
            if (err) {
                strncpy(err, "picked window is gone or minimized",
                        MSG_BUFSIZE - 1);
            }
            return FALSE;
        }
        *out = *picked;
        if (out->pid == 0) {
            // Pin the identity even when the picker omitted the PID;
            // per-tick captureTargetAlive then detects HWND recycling.
            GetWindowThreadProcessId(picked->hwnd, &out->pid);
        }
        return TRUE;
    }

    CaptureWindow cands[OCR_APP_ENUM_CAP];
    int n = captureEnumWindowsForApp(cfg->exeName, cands, OCR_APP_ENUM_CAP);
    int matchCount = 0;
    if (!captureResolveWindow(cands, n, cfg->windowTitle, out, &matchCount)
        || matchCount != 1) {
        if (err) {
            strncpy(err, "saved window title matches 0 or several windows",
                    MSG_BUFSIZE - 1);
        }
        return FALSE;
    }
    return TRUE;
}

static void buildSession(const OcrConfig *cfg,
                         const CaptureWindow *target) {
    memset(&g_app.session, 0, sizeof(g_app.session));
    memset(g_app.regions, 0, sizeof(g_app.regions));
    memset(g_app.groups, 0, sizeof(g_app.groups));
    g_app.session.intervalMs = cfg->intervalMs;
    g_app.session.changeDetect = cfg->changeDetect;
    g_app.session.madThreshold = OCR_APP_MAD_THRESHOLD;
    g_app.session.target = *target;
    g_app.session.timeline = g_app.timeline;
    g_app.session.regionCount = cfg->regionCount;
    g_app.session.regions = g_app.regions;
    g_app.session.groupCount = cfg->groupCount;
    g_app.session.groups = g_app.groups;
    for (size_t i = 0; i < cfg->regionCount; i++) {
        g_app.regions[i].norm = cfg->regions[i].norm;
    }
    for (size_t gi = 0; gi < cfg->groupCount; gi++) {
        g_app.groups[gi].group = cfg->groups[gi];
        for (size_t k = 0; k < cfg->groups[gi].memberCount; k++) {
            int ri = cfg->groups[gi].members[k].regionIndex;
            if (ri >= 0 && (size_t)ri < cfg->regionCount) {
                g_app.session.referenced[ri] = TRUE;
            }
        }
    }
    g_app.session.captureFn = captureClientRegion;
    g_app.session.readFn = ocrEngineRead;
    g_app.session.engine = g_app.engine;
    g_app.session.nowMsFn = NULL; // wall clock on the worker thread
    g_app.session.targetAliveFn = captureTargetAlive;
}

static void writeLifecycle(const char *event, int lossPercent,
                           const char *source) {
    OcrTimelineEvent t;
    memset(&t, 0, sizeof(t));
    t.timeMs = (UINT64)GetTickCount64();
    t.event = event;
    t.lossPercent = lossPercent;
    t.source = source;
    (void)ocrTimelineEvent(g_app.timeline, &t);
}

static void appSink(const OcrSessionEvent *ev) {
    if (strcmp(ev->event, "loss_change") == 0 && ev->lossPercent >= 0
        && g_app.hooks.onLoss) {
        g_app.hooks.onLoss(ev->lossPercent, g_app.hooks.user);
    } else if (g_app.hooks.onSessionEvent) {
        g_app.hooks.onSessionEvent(ev, g_app.hooks.user);
    }
}

BOOL ocrAppStart(const OcrConfig *cfg, const CaptureWindow *picked,
                 const char *timelineDir, const char *modelsDir,
                 int initialLossPercent, const OcrAppHooks *hooks,
                 char err[MSG_BUFSIZE]) {
    if (err) err[0] = '\0';
    if (!cfg || !timelineDir || !modelsDir) {
        if (err) strncpy(err, "config: bad arguments", MSG_BUFSIZE - 1);
        return FALSE;
    }
    if (g_app.running) {
        if (err) strncpy(err, "config: OCR already running",
                         MSG_BUFSIZE - 1);
        return FALSE;
    }
    if (hooks) g_app.hooks = *hooks;
    else memset(&g_app.hooks, 0, sizeof(g_app.hooks));

    if (!ocrConfigValidate(cfg, err)) {
        prefixError(err, "config");
        return FALSE;
    }

    CaptureWindow target;
    if (!resolveTarget(cfg, picked, &target, err)) {
        prefixError(err, "window");
        return FALSE;
    }

    g_app.csvPath[0] = '\0';
    g_app.timeline = ocrTimelineOpen(timelineDir, g_app.csvPath,
                                     sizeof(g_app.csvPath), err);
    if (!g_app.timeline) {
        prefixError(err, "timeline");
        return FALSE;
    }

    OcrEngineFactory make = g_app.factory ? g_app.factory : ocrEngineCreate;
    g_app.engine = make(modelsDir, err);
    if (!g_app.engine) {
        prefixError(err, "engine");
        releaseResources(TRUE);
        return FALSE;
    }

    buildSession(cfg, &target);
    if (!ocrSessionSeed(&g_app.session, err)) {
        prefixError(err, "seed");
        releaseResources(TRUE);
        return FALSE;
    }

    int clamped = initialLossPercent;
    if (clamped < 0) clamped = 0;
    if (clamped > 100) clamped = 100;
    writeLifecycle("session_start", -1, "system");
    writeLifecycle("loss_change", clamped, "initial_slider");

    if (!ocrWorkerStart(&g_app.session, NULL, err)) {
        prefixError(err, "worker");
        releaseResources(TRUE);
        return FALSE;
    }

    g_app.running = TRUE;
    return TRUE;
}

void ocrAppStop(void) {
    if (!g_app.running) return;
    ocrWorkerStop();
    // Deliver the final queued events first: the last gear change of a
    // run must still reach the applier before the session_stop row.
    ocrWorkerDrain(appSink);
    writeLifecycle("session_stop", -1, "manual_stop");
    releaseResources(FALSE); // the completed CSV stays on disk
    g_app.running = FALSE;
    memset(&g_app.session, 0, sizeof(g_app.session));
}

BOOL ocrAppRunning(void) {
    return g_app.running;
}

void ocrAppPump(void) {
    if (!g_app.running) return;
    ocrWorkerDrain(appSink);
}

const char *ocrAppCsvPath(void) {
    return g_app.csvPath;
}

void ocrAppSetEngineFactoryForTest(OcrEngineFactory factory) {
    g_app.factory = factory;
}
