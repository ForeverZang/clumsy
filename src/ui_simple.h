#ifndef CLUMSY_UI_SIMPLE_H
#define CLUMSY_UI_SIMPLE_H

#include "common.h"
#include "procmap.h"
#include "traffic.h"

/*
 * Beginner main window: app picker, direction, one loss slider, timed
 * recovery and the live traffic chart. Everything language-specific goes
 * through tr()/i18nBind so the title-bar language switch needs no rebuild.
 */

// ---------------------------------------------------------------------------
// Pure helpers (unit-tested in tests/test_ui_simple.c)
// ---------------------------------------------------------------------------

// Slider percent (0..100) clamped to permyriad (0..10000).
int  uiSimpleLossPermyriad(int percent);

// Whole seconds left until the deadline, rounded up; 0 once elapsed.
int  uiSimpleRemainingSeconds(DWORD elapsedMs, DWORD totalMs);

// One dropdown line for an app: product description + exe when known,
// exe-only fallback otherwise.
void uiSimpleFormatAppLine(const AppGroup *app, const char *product,
                           char *out, size_t cap);

// The chart ring shared by the canvas and the accounting callbacks.
Traffic *uiSimpleTraffic(void);

// Whether an affect session is running. While idle the SNIFF observer
// feeds the chart; while running only reinjected survivors do.
void uiSimpleSetAffecting(BOOL affecting);
void uiSimpleOnSniff(const char *packet, UINT packetLen,
                     const WINDIVERT_ADDRESS *addr);
void uiSimpleOnReinject(const char *packet, UINT packetLen,
                        const WINDIVERT_ADDRESS *addr);

// ---------------------------------------------------------------------------
// Widget layer (built once after IupOpen)
// ---------------------------------------------------------------------------

// Called when the user picks a real app (exe is its image name) or the
// "all applications" row (exe == NULL). main.c wires it to the persistent
// SNIFF attach/detach lifecycle.
typedef void (*UiSimpleAppWatchFn)(const char *exe);

// Build the beginner area. actionCb is bound to the main Start/Stop
// button; watchApp notifies on app-list selection changes.
Ihandle *uiSimpleCreate(Icallback actionCb, UiSimpleAppWatchFn watchApp);

// Section ⑤ mount seam: append the OCR frame after frame ④ chart and
// before the main Start/Stop row.
void uiSimpleAppendSection(Ihandle *child);

// OCR-triggered gear changes mirror onto g_percent, slider VALUE, label
// and simpleSetLoss; 0..100 clamped.
void uiSimpleApplyExternalLoss(int percent);

// Slider widget (test seam for the mirroring check).
Ihandle *uiSimpleSlider(void);

// The main Start/Stop button (test seam for callback wiring).
Ihandle *uiSimpleActionButton(void);
// The running-state countdown label (test seam for layout reservation).
Ihandle *uiSimpleCountdownLabel(void);
// The before/now rate readouts (test seam for layout reservation).
void uiSimpleRateLabels(Ihandle **before, Ihandle **now);

// Currently picked image name, or NULL for "all applications".
const char *uiSimpleSelectedApp(void);
// Global direction gate from the segmented control.
void uiSimpleGetDirection(short *outIn, short *outOut);
// Loss in permyriad.
int  uiSimpleLoss(void);
// Timed recovery: FALSE when unchecked; otherwise seconds (>= 1).
BOOL uiSimpleTimerSeconds(int *outSeconds);

// Flip the area between ready/running presentation (button title/color,
// editable controls, start marker and affecting flag). enableLoss also
// flips the beginner loss step; the classic parameterized autostart
// passes FALSE so only advanced modules run.
void uiSimpleSetRunning(BOOL running, BOOL enableLoss);

// Status line under the action button.
void uiSimpleSetStatus(const char *text);
// Arm/disarm the one-shot auto-recovery timer; on expiry the same
// Start/Stop callback fires.
void uiSimpleArmRecovery(int seconds, Icallback stopCb);
void uiSimpleDisarmRecovery(void);

// Rebuild dynamic text after a live language switch (bound controls
// retranslate themselves).
void uiSimpleRetranslate(void);

#endif // CLUMSY_UI_SIMPLE_H
