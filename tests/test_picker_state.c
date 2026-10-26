// Unit tests for the pure WeChat-style picker state machine: FRAME
// (hover among the target app's windows, single auto-lock) and DRAG
// (rubber-band regions on the locked window, undo/reselect/done).
// Existing regions are always kept; only this session's new rects are
// discarded by a reselect.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <assert.h>

#include "ui_picker_state.h"

static NormRect makeRect(double x) {
    NormRect r;
    r.x = x;
    r.y = 0.20;
    r.w = 0.10;
    r.h = 0.05;
    return r;
}

// Two pre-existing regions standing in for config-loaded boxes; the
// machine must never mutate or drop them except via explicit Undo of a
// session rect (which never reaches these slots).
static void makeExisting(NormRect out[2]) {
    out[0] = makeRect(0.31);
    out[1] = makeRect(0.42);
}

int main(void) {
    NormRect existing[2];

    // --- init: FRAME, nothing hovered/locked, cannot finish ----------
    {
        PickerMachine m;
        uiPickerStateInit(&m, FALSE, 2, NULL, 0);
        assert(m.phase == PICKER_PHASE_FRAME);
        assert(m.locked == -1);
        assert(m.hover == -1);
        assert(m.keepCount == 0);
        assert(m.rectCount == 0);
        assert(uiPickerStateSessionCount(&m) == 0);
        assert(!uiPickerStateCanFinish(&m));
    }

    // --- hover clamps; Lock enters DRAG and is one-shot --------------
    {
        PickerMachine m;
        uiPickerStateInit(&m, FALSE, 2, NULL, 0);
        uiPickerStateSetHover(&m, 1);
        assert(m.hover == 1);
        uiPickerStateSetHover(&m, 5);   // past winCount -> none
        assert(m.hover == -1);
        uiPickerStateSetHover(&m, -1);
        assert(m.hover == -1);
        assert(!uiPickerStateLock(&m, 7)); // out of range stays in FRAME
        assert(m.phase == PICKER_PHASE_FRAME);
        assert(uiPickerStateLock(&m, 1));
        assert(m.phase == PICKER_PHASE_DRAG);
        assert(m.locked == 1);
        assert(!uiPickerStateLock(&m, 0)); // already DRAG: no re-lock
        assert(m.locked == 1);
    }

    // --- auto-lock: exactly one window; three windows stay manual ----
    {
        PickerMachine one, many;
        uiPickerStateInit(&one, FALSE, 1, NULL, 0);
        uiPickerStateAutoLock(&one);
        assert(one.phase == PICKER_PHASE_DRAG);
        assert(one.locked == 0);
        uiPickerStateInit(&many, FALSE, 3, NULL, 0);
        uiPickerStateAutoLock(&many);
        assert(many.phase == PICKER_PHASE_FRAME);
        assert(many.locked == -1);
    }

    // --- add rects: two fresh boxes make finish legal; 16 is the cap --
    {
        PickerMachine m;
        NormRect r1 = makeRect(0.01), r2 = makeRect(0.02);
        uiPickerStateInit(&m, FALSE, 1, NULL, 0);
        uiPickerStateAutoLock(&m);
        assert(!uiPickerStateCanFinish(&m)); // locked but 0 new boxes
        assert(uiPickerStateAddRect(&m, &r1));
        assert(uiPickerStateAddRect(&m, &r2));
        assert(uiPickerStateSessionCount(&m) == 2);
        assert(m.rectCount == 2);
        assert(m.rects[0].x == 0.01);
        assert(m.rects[1].x == 0.02);
        assert(uiPickerStateCanFinish(&m));
    }
    {
        // 14 pre-existing + this session fills 15..16; the 17th refuses.
        NormRect oldRegions[14];
        PickerMachine full;
        int i;
        for (i = 0; i < 14; i++) oldRegions[i] = makeRect(0.10 + 0.01 * i);
        uiPickerStateInit(&full, FALSE, 1, oldRegions, 14);
        uiPickerStateAutoLock(&full);
        assert(uiPickerStateAddRect(&full, &(NormRect){0.90, 0, 0, 0}));
        assert(uiPickerStateAddRect(&full, &(NormRect){0.91, 0, 0, 0}));
        assert(full.rectCount == OCR_MAX_REGIONS);
        assert(!uiPickerStateAddRect(&full, &(NormRect){0.92, 0, 0, 0}));
        assert(full.rectCount == OCR_MAX_REGIONS);
    }
    {
        // AddRect refuses before a window is locked.
        PickerMachine m;
        NormRect r = makeRect(0.50);
        uiPickerStateInit(&m, FALSE, 2, NULL, 0);
        assert(!uiPickerStateAddRect(&m, &r));
        assert(m.rectCount == 0);
    }

    // --- finish rule for an already-configured window (R3) -----------
    {
        PickerMachine rePick, firstPick;
        makeExisting(existing);
        uiPickerStateInit(&rePick, TRUE, 2, existing, 2);
        assert(uiPickerStateLock(&rePick, 0));
        // had a picked window: zero new boxes is still a valid finish.
        assert(uiPickerStateSessionCount(&rePick) == 0);
        assert(uiPickerStateCanFinish(&rePick));

        uiPickerStateInit(&firstPick, FALSE, 2, existing, 2);
        assert(uiPickerStateLock(&firstPick, 0));
        // first ever pick with no new box cannot finish.
        assert(!uiPickerStateCanFinish(&firstPick));
    }

    // --- undo pops only this session's rects; old regions are fixed --
    {
        PickerMachine m;
        NormRect r1 = makeRect(0.71), r2 = makeRect(0.72);
        makeExisting(existing);
        uiPickerStateInit(&m, FALSE, 1, existing, 2);
        uiPickerStateAutoLock(&m);
        assert(uiPickerStateAddRect(&m, &r1));
        assert(uiPickerStateAddRect(&m, &r2));
        assert(m.rectCount == 4);
        assert(uiPickerStateUndo(&m));
        assert(m.rectCount == 3);
        assert(uiPickerStateUndo(&m));
        assert(m.rectCount == 2);
        assert(!uiPickerStateUndo(&m)); // old regions are not undoable
        assert(m.rects[0].x == 0.31);
        assert(m.rects[1].x == 0.42);
    }

    // --- reselect (R1): back to FRAME, session boxes discarded --------
    {
        PickerMachine m;
        NormRect r1 = makeRect(0.81);
        makeExisting(existing);
        uiPickerStateInit(&m, FALSE, 2, existing, 2);
        assert(uiPickerStateLock(&m, 0));
        assert(uiPickerStateAddRect(&m, &r1));
        uiPickerStateSetHover(&m, 0);
        uiPickerStateReframe(&m);
        assert(m.phase == PICKER_PHASE_FRAME);
        assert(m.locked == -1);
        assert(m.hover == -1);
        assert(m.rectCount == 2);
        assert(m.keepCount == 2);
        assert(m.rects[0].x == 0.31);
        assert(m.rects[1].x == 0.42);
        // Re-lock; the next box lands right after the kept old regions.
        assert(uiPickerStateLock(&m, 1));
        assert(uiPickerStateAddRect(&m, &r1));
        assert(m.rectCount == 3);
        assert(m.rects[2].x == 0.81);
        assert(uiPickerStateSessionCount(&m) == 1);
    }

    printf("OK: picker_state\n");
    return 0;
}
