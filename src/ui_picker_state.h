#ifndef CLUMSY_UI_PICKER_STATE_H
#define CLUMSY_UI_PICKER_STATE_H

#include <stddef.h>

#include "ocr_rules.h" // NormRect, OCR_MAX_REGIONS

/*
 * Pure state machine for the WeChat-style window/region picker. No IUP,
 * no Win32 calls: the overlay module (Task 6) renders from this state
 * and feeds mouse events back into these functions, so the interaction
 * is unit-testable headless.
 *
 * FRAME: a full-screen frame highlights the hovered window of the
 * target app; one candidate window auto-locks, several need a click.
 * DRAG: the locked window is highlighted and the user rubber-bands
 * regions on it; undo pops this session's rects, reselect (R1) returns
 * to FRAME and discards them, done keeps the pre-existing rects plus
 * this session's additions.
 */

typedef enum {
    PICKER_PHASE_FRAME = 0,
    PICKER_PHASE_DRAG  = 1
} PickerPhase;

typedef struct {
    BOOL    hadWindow;  // a window was already picked/configured before
    size_t  winCount;   // visible top-level windows of the target app
    long    hover;      // FRAME-phase hovered window index, -1 = none
    long    locked;     // locked window index, -1 = none
    size_t  keepCount;  // rects present before the session: always kept
    size_t  rectCount;  // kept rects followed by this session's rects
    NormRect rects[OCR_MAX_REGIONS];
    PickerPhase phase;
} PickerMachine;

// existing/existingCount are the regions already configured (copied in
// verbatim and never mutated by the picker).
void uiPickerStateInit(PickerMachine *m, BOOL hadWindow, size_t winCount,
                       const NormRect *existing, size_t existingCount);

// Track the hovered FRAME candidate; an out-of-range index clears it.
void uiPickerStateSetHover(PickerMachine *m, long winIndex);

// Lock the hovered/clicked candidate. FRAME only, index must be in
// range; TRUE moves the machine into DRAG.
BOOL uiPickerStateLock(PickerMachine *m, long winIndex);

// Single-window sessions skip the hover/click entirely.
void uiPickerStateAutoLock(PickerMachine *m);

// Reselect (R1): return to FRAME and discard every rect added this
// session; the pre-existing rects survive unchanged.
void uiPickerStateReframe(PickerMachine *m);

// Commit one dragged region. DRAG only, with a locked window and room
// under OCR_MAX_REGIONS.
BOOL uiPickerStateAddRect(PickerMachine *m, const NormRect *r);

// Drop the most recent session rect. FALSE when none remain (the
// pre-existing rects are never undoable).
BOOL uiPickerStateUndo(PickerMachine *m);

// Done is legal with a locked window that was already configured, or
// once at least one new region was dragged. A first-time pick with no
// new region is not finishable.
BOOL uiPickerStateCanFinish(const PickerMachine *m);

// Number of regions dragged this session (rectCount - keepCount).
size_t uiPickerStateSessionCount(const PickerMachine *m);

#endif // CLUMSY_UI_PICKER_STATE_H
