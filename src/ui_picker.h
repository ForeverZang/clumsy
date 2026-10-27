#ifndef CLUMSY_UI_PICKER_H
#define CLUMSY_UI_PICKER_H

#include "common.h"
#include "ocr_capture.h" // CaptureWindow
#include "ocr_rules.h"   // NormRect, OCR_MAX_REGIONS

/*
 * WeChat-style window/region picker for section ⑤: one full-screen
 * modal overlay with two phases. FRAME highlights the hovered visible
 * top-level window of the target app (a single candidate auto-locks);
 * DRAG keeps the locked window bright and lets the user rubber-band
 * several OCR regions, undo them, reselect the window, cancel, or
 * finish. HWND and rects are written out only on finish, so cancel and
 * reselect leave previously picked state untouched.
 *
 * The whole implementation compiles to no-op stubs when CLUMSY_OCR is
 * not defined (build.zig); main.c guards its call sites the same way.
 */

typedef enum {
    UI_PICKER_OK = 0,          // outWindow/outRects hold the commit
    UI_PICKER_CANCEL = 1,      // Esc / right-click in FRAME / cancel
    UI_PICKER_WINDOW_DIED = 2  // the locked window closed or minimized
} UiPickerResult;

// Run the overlay.
//   startFrame=TRUE   enter FRAME (the "选择窗口…" entry); FALSE skips
//                     straight into DRAG on initialWindow ("+ 添加区域").
//   hadWindow         FALSE on the very first pick: finishing needs at
//                     least one newly dragged region.
//   initialWindow     required when startFrame is FALSE.
//   existing/count    regions configured before the session; they are
//                     shown (orange) and always kept.
// On OK *outWindow is the locked window (freshly enumerated) and
// *outRects receives ALL regions (existing followed by this session's);
// the caller tells new ones apart by count > existingCount. Output
// arrays must hold OCR_MAX_REGIONS entries.
UiPickerResult uiPickerPick(const char *exeName,
                            BOOL startFrame,
                            BOOL hadWindow,
                            const CaptureWindow *initialWindow,
                            const NormRect *existing, size_t existingCount,
                            CaptureWindow *outWindow,
                            NormRect *outRects, size_t *outRectCount);

// Ownership re-validation right before a FRAME click locks a window:
// the HWND must still exist, be visible and not minimized, and still be
// owned by the pid captured at enumeration. Fails closed on NULL/0.
BOOL uiPickerWindowStillOwned(HWND hwnd, DWORD pid);

// Pure geometry behind DRAG: order the two screen points, map them into
// the target's client area, clamp to its client rect, refuse a drag
// smaller than minPixels on a side, and write normalized fractions.
BOOL uiPickerDragToNorm(HWND target, POINT screenA, POINT screenB,
                        int minPixels, NormRect *out);

// Test seams driving the overlay from a timer while its modal loop runs.
// SetDrag paints a pending band; CompleteDrag runs the same release
// path the mouse uses (clamp + machine AddRect); Finish activates Done.
void uiPickerSetDragForTest(POINT screenA, POINT screenB);
BOOL uiPickerCompleteDragForTest(void);
UiPickerResult uiPickerFinishForTest(void);
// Native HWND of the full-screen overlay for headless per-window grabs;
// NULL when the overlay is not open.
HWND uiPickerOverlayHwndForTest(void);
// Overwrite the locked candidate's pinned pid with a non-matching one,
// emulating HWND recycling onto another process after FRAME lock.
void uiPickerSetLockedPidForTest(DWORD pid);
// Deliver one WM_TIMER (the 300 ms death backstop) to the overlay and
// report whether it finished the pick; FALSE when the pick survives.
BOOL uiPickerTimerTickForTest(void);
// Compute the bottom toolbar's top-left for the active overlay from its
// measured size (centered, 12px above the work-area bottom, clamped on
// screen). FALSE when no overlay is active.
BOOL uiPickerBarPlacementForTest(int barW, int barH, POINT *out);
// Native HWND of the bottom toolbar while its modal popup is running;
// NULL outside a pick. Lets the suite assert zero-chrome framing and the
// real on-screen rect against the overlay monitor work area.
HWND uiPickerBarHwndForTest(void);

#endif // CLUMSY_UI_PICKER_H
