#ifndef CLUMSY_OCR_APP_H
#define CLUMSY_OCR_APP_H

#include "common.h"
#include "ocr_config.h"
#include "ocr_capture.h"
#include "ocr_engine.h"
#include "ocr_session.h"

/*
 * Headless OCR automation coordinator: the whole Start/Stop lifecycle
 * with no IUP dependency, so every preflight failure path is unit
 * testable. Owns one OcrSession, the engine, the timeline CSV and the
 * target window resolution. The GUI layer (ocr_glue) collects an
 * OcrConfig, passes hooks for gear changes / log events, and calls
 * ocrAppPump from a timer on the UI thread.
 *
 * Start ordering (spec §4 steps 7-9, §5):
 *   strict validate -> resolve target HWND -> open timeline ->
 *   create engine -> build session -> seed arming -> lifecycle rows ->
 *   start worker. Any failure tears down what was already acquired; no
 *   gear change hook can fire before a fully successful start.
 */

typedef struct {
    // Fires on the UI thread (inside ocrAppPump) for every loss_change
    // event carrying a gear percentage, including the last queued events
    // drained during ocrAppStop.
    void (*onLoss)(int percent, void *user);

    // Fires on the UI thread for rule_trigger / capture_unavailable
    // rows (informational; the CSV row is already written by the worker).
    void (*onSessionEvent)(const OcrSessionEvent *ev, void *user);

    void *user;
} OcrAppHooks;

typedef OcrEngine *(*OcrEngineFactory)(const char *modelsDir,
                                       char err[MSG_BUFSIZE]);

// Start one automation run. picked != NULL with a live HWND bypasses
// title re-resolution (the window hand-picked this session); otherwise
// cfg.exeName/cfg.windowTitle are re-resolved against current windows,
// and 0 or >1 matches fail the start. initialLossPercent is the slider
// gear recorded as the first loss_change (source "initial_slider").
// err receives an English "code: detail" message.
BOOL ocrAppStart(const OcrConfig *cfg, const CaptureWindow *picked,
                 const char *timelineDir, const char *modelsDir,
                 int initialLossPercent, const OcrAppHooks *hooks,
                 char err[MSG_BUFSIZE]);

// Stop the worker, deliver its final queued events to the hooks, write
// session_stop (source "manual_stop") and close the timeline/engine.
void ocrAppStop(void);

BOOL ocrAppRunning(void);

// GUI-thread queued-event delivery; timer-driven while running.
void ocrAppPump(void);

// Full path of the current/last run's CSV ("" before the first run).
const char *ocrAppCsvPath(void);

// Test seam: replace ocrEngineCreate (NULL restores the real factory).
void ocrAppSetEngineFactoryForTest(OcrEngineFactory factory);

#endif // CLUMSY_OCR_APP_H
