#include <string.h>

#include "ui_picker_state.h"

/*
 * Pure FRAME/DRAG picker state. All transition rules live here; the
 * overlay only renders the state and forwards raw input as indices and
 * normalized rectangles.
 */

void uiPickerStateInit(PickerMachine *m, BOOL hadWindow, size_t winCount,
                       const NormRect *existing, size_t existingCount) {
    size_t keep = existingCount;
    if (m == NULL) return;
    if (keep > OCR_MAX_REGIONS) keep = OCR_MAX_REGIONS;
    memset(m, 0, sizeof(*m));
    m->hadWindow = hadWindow;
    m->winCount = winCount;
    m->hover = -1;
    m->locked = -1;
    m->keepCount = keep;
    m->rectCount = keep;
    m->phase = PICKER_PHASE_FRAME;
    if (existing != NULL && keep > 0) {
        memcpy(m->rects, existing, keep * sizeof(m->rects[0]));
    }
}

void uiPickerStateSetHover(PickerMachine *m, long winIndex) {
    if (m == NULL) return;
    if (winIndex < 0 || (size_t)winIndex >= m->winCount) {
        m->hover = -1;
    } else {
        m->hover = winIndex;
    }
}

BOOL uiPickerStateLock(PickerMachine *m, long winIndex) {
    if (m == NULL) return FALSE;
    if (m->phase != PICKER_PHASE_FRAME) return FALSE;
    if (winIndex < 0 || (size_t)winIndex >= m->winCount) return FALSE;
    m->locked = winIndex;
    m->phase = PICKER_PHASE_DRAG;
    return TRUE;
}

void uiPickerStateAutoLock(PickerMachine *m) {
    if (m == NULL) return;
    if (m->phase != PICKER_PHASE_FRAME) return;
    if (m->winCount != 1) return;
    m->locked = 0;
    m->phase = PICKER_PHASE_DRAG;
}

void uiPickerStateReframe(PickerMachine *m) {
    if (m == NULL) return;
    // Session rects occupy [keepCount, rectCount); shrinking the count
    // drops them, leaving the pre-existing slots untouched.
    m->rectCount = m->keepCount;
    m->locked = -1;
    m->hover = -1;
    m->phase = PICKER_PHASE_FRAME;
}

BOOL uiPickerStateAddRect(PickerMachine *m, const NormRect *r) {
    if (m == NULL || r == NULL) return FALSE;
    if (m->phase != PICKER_PHASE_DRAG || m->locked < 0) return FALSE;
    if (m->rectCount >= OCR_MAX_REGIONS) return FALSE;
    m->rects[m->rectCount++] = *r;
    return TRUE;
}

BOOL uiPickerStateUndo(PickerMachine *m) {
    if (m == NULL) return FALSE;
    if (m->rectCount <= m->keepCount) return FALSE;
    m->rectCount--;
    memset(&m->rects[m->rectCount], 0, sizeof(m->rects[0]));
    return TRUE;
}

BOOL uiPickerStateCanFinish(const PickerMachine *m) {
    if (m == NULL || m->locked < 0) return FALSE;
    return m->hadWindow || (m->rectCount - m->keepCount) >= 1;
}

size_t uiPickerStateSessionCount(const PickerMachine *m) {
    if (m == NULL || m->rectCount < m->keepCount) return 0;
    return m->rectCount - m->keepCount;
}
