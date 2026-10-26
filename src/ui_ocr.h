#ifndef CLUMSY_UI_OCR_H
#define CLUMSY_UI_OCR_H

#include "common.h"
#include "ocr_config.h"
#include "ocr_capture.h"
#include "ui_ocr_log.h"

/*
 * Section ⑤ of the beginner page: collapsible OCR automation block —
 * enable gate, target window row, region rule rows, parameters, the
 * scrolling CSV timeline mirror and the in-block Start/Stop button.
 *
 * The whole implementation compiles to no-op stubs when CLUMSY_OCR is
 * not defined (build.zig); main.c guards its call sites the same way.
 */

// Build the section. actionCb is main's single Start/Stop entry point
// (bound to the block's FlatButton FLAT_ACTION; NULL tolerated).
Ihandle *uiOcrCreate(Icallback actionCb);
void uiOcrRetranslate(void);

// Current selected application image name (NULL / "" = all applications:
// enabling the block is refused). main.c pushes app-list changes here.
void uiOcrSetAppContext(const char *exe);

typedef void (*OcrLossApplier)(int percent, const char *source);
void uiOcrSetLossApplier(OcrLossApplier fn);

// Host hook invoked after a fold/expand changes the section's natural
// size post-map; main re-fits the RESIZE=NO dialog (SetWindowPos path).
typedef void (*OcrLayoutChangedFn)(void);
void uiOcrSetLayoutChangedCallback(OcrLayoutChangedFn fn);

BOOL uiOcrEnabled(void);

// Collect widget state into a validated OcrConfig. Returns FALSE (and
// shows an inline red error) on any invalid row; on failure *out is still
// fully written but must not be used.
BOOL uiOcrGetConfig(OcrConfig *out);

// Fill widgets from a loaded configuration (rules rebuilt as rows).
// The app context set via uiOcrSetAppContext is never overwritten.
BOOL uiOcrPopulate(const OcrConfig *cfg);

// Task 9: pick the target window off the screen. Zero visible windows ->
// FALSE and the inline "no windows" error; one -> picked directly;
// several -> modal IupList chooser (FALSE = user cancel). Refuses while
// running or without an app context.
BOOL uiOcrChooseWindow(CaptureWindow *out);

// Task 9: session-local hand-picked window overrides the saved title.
void uiOcrSetPickedWindow(const CaptureWindow *win);
const char *uiOcrSavedWindowTitle(void);
BOOL uiOcrHasPickedWindow(void);
HWND uiOcrPickedHwnd(void);

void uiOcrSetRunning(BOOL running);

// Show a translated inline error (pass an ocr.error.* key, or literal
// English which tr() passes through). "" clears it.
void uiOcrShowError(const char *keyOrText);

#if defined(CLUMSY_OCR)
// Full copy of the window hand-picked this session (Start prefers this
// over re-resolving the saved title).
BOOL uiOcrPickedWindow(CaptureWindow *out);
#endif

// Test seam: flip the post-map gate (real UI flips it from MAP_CB).
void uiOcrSetMappedForTest(BOOL mapped);

// Test seam: nth rule row's container (post-map append mapping checks).
Ihandle *uiOcrRowBoxForTest(size_t i);

// Test seam: append a dragged region using the production placeholder
// defaults (DISAPPEAR / 2 frames / 95%, "Region N"), skipping the modal
// rubber-band overlay.
BOOL uiOcrAddRegionForTest(const NormRect *norm);

// Test seams (widget-level assertions in tests/test_ui_simple.c).
Ihandle *uiOcrEnableToggleForTest(void);
Ihandle *uiOcrChooseButtonForTest(void);
Ihandle *uiOcrAddButtonForTest(void);
Ihandle *uiOcrStartButtonForTest(void);
Ihandle *uiOcrBodyForTest(void);
// Test seam: always-visible (even when the body is folded) red label that
// explains an enable-gate refusal such as "pick a concrete application".
Ihandle *uiOcrGateErrorForTest(void);

#endif // CLUMSY_UI_OCR_H
