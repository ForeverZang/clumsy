#ifndef CLUMSY_UI_PICKER_H
#define CLUMSY_UI_PICKER_H

#include "common.h"
#include "ocr_capture.h" // CaptureWindow
#include "ocr_rules.h"   // NormRect

/*
 * Modal target-window chooser and in-window region rubber band for
 * section ⑤. Both dialogs are UI-thread, one-at-a-time modals.
 *
 * The whole implementation compiles to no-op stubs when CLUMSY_OCR is
 * not defined (build.zig); main.c guards its call sites the same way.
 */

// Modal IupList of the app's visible top-level windows (title + size).
// Returns FALSE when the user cancels; the caller distinguishes "no
// windows" beforehand with captureEnumWindowsForApp and shows the
// inline error itself.
BOOL uiPickerPickWindow(const char *exeName, CaptureWindow *out);

// Semi-transparent topmost overlay over the target window's monitor
// work area; the user drags a rectangle (right-click / ESC cancels).
// On success *out is the drag rect as client-area fractions. FALSE on
// cancel or an uncapturable target.
BOOL uiPickerPickRect(HWND target, NormRect *out);

// Pure geometry behind the overlay, split out for unit tests: order the
// two screen points, map them into the target's client area, clamp to
// its client rect, refuse a drag smaller than minPixels on a side, and
// write normalized fractions.
BOOL uiPickerDragToNorm(HWND target, POINT screenA, POINT screenB,
                        int minPixels, NormRect *out);

// Test seams driving the UI-thread modals from a timer while IupPopup
// runs its nested loop. No-ops when the matching dialog is not open.
void uiPickerAcceptChooserForTest(void);
void uiPickerSetDragForTest(POINT screenA, POINT screenB);
BOOL uiPickerCompleteDragForTest(void);
// Native HWND of the open modal (chooser dialog / rubber-band canvas)
// for headless per-window grabs; NULL when the modal is not open.
HWND uiPickerChooserHwndForTest(void);
HWND uiPickerOverlayHwndForTest(void);

#endif // CLUMSY_UI_PICKER_H
