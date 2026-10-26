#ifndef CLUMSY_UI_OCR_GROUP_EDITOR_H
#define CLUMSY_UI_OCR_GROUP_EDITOR_H

#include "common.h"
#include "iup.h"
#include "ocr_config.h"

/*
 * Modal rule-group editor (IupPopup): one checkbox + keyword field per
 * region, plus edge / confirm-frames / loss rows. The dialog owns only
 * its draft; validation and model writes go through the injected commit
 * callback. The popup blocks until OK succeeds or the dialog is
 * cancelled (Esc / cancel / close), in which case nothing is committed.
 *
 * Compiles to an empty translation unit without CLUMSY_OCR (the OCR
 * build path is the only caller).
 */

typedef struct {
    const OcrRegionDef *regions;     // region snapshot, regionCount slots
    size_t              regionCount;
    const OcrGroup     *editGroup;   // NULL when creating a new group
    long                editRow;     // -1 when creating a new group
} UiOcrGroupEditorInput;

// Validate and store the draft. On rejection return FALSE and fill
// errOut (capped, already in the UI language); the editor keeps the
// dialog open and shows the message.
typedef BOOL (*UiOcrGroupCommitFn)(const OcrGroup *draft, long editRow,
                                   char *errOut, size_t errCap);

void uiOcrGroupEditorOpen(const UiOcrGroupEditorInput *input,
                          UiOcrGroupCommitFn commit);

#endif // CLUMSY_UI_OCR_GROUP_EDITOR_H
