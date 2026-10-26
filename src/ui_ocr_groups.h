#ifndef CLUMSY_UI_OCR_GROUPS_H
#define CLUMSY_UI_OCR_GROUPS_H

#include "common.h"
#include "iup.h"
#include "ocr_config.h"

/*
 * Section ⑤ region + rule-group tables: two fixed-height IupMatrix
 * widgets backed by an in-file static regions/groups model. The group
 * edit dialog (double-click a group row) builds AND groups of
 * per-region keyword members. All model mutation goes through this
 * module; the matrices render the model via VALUE_CB, never the other
 * way around.
 *
 * The whole module compiles to empty stubs when CLUMSY_OCR is not
 * defined (ui_ocr.h includes this header on both build paths).
 */

// Build the vbox: region frame (matrix + drag-to-add button) and group
// frame (matrix + new-group button). The region button's ACTION is left
// unbound: ui_ocr owns the rubber-band overlay and wires its callback.
Ihandle *uiOcrGroupsCreate(void);

// Re-render dynamic table text (VALUE_CB reads tr() at paint time).
void uiOcrGroupsRetranslate(void);

// Freeze both matrices and both add buttons while a session runs.
void uiOcrGroupsSetRunning(BOOL running);

// Model access. Output arrays may be NULL when only the counts are
// wanted (button-state refresh); counts may be NULL as well.
void uiOcrGroupsGetModel(OcrRegionDef *regions, size_t *regionCount,
                         OcrGroup *groups, size_t *groupCount);

// Replace the whole model (loaded config / test setup). Counts above
// the 16/16 caps are refused; groups are stored verbatim (a 0-member
// group is legal in the model and renders as a red empty row).
BOOL uiOcrGroupsSetModel(const OcrRegionDef *regions, size_t regionCount,
                         const OcrGroup *groups, size_t groupCount);

// Append freshly dragged regions with default "Region N" names. Either
// all rects are appended or none (cap overflow -> FALSE).
BOOL uiOcrGroupsAppendRegions(const NormRect *rects, size_t count);

// Live normalized rects of every region (contiguous mirror kept for
// the picker overlay to highlight existing boxes).
const NormRect *uiOcrGroupsRegionRects(size_t *count);

// Pure condition summary in the current table language:
// zh: R1「就绪」 且 R2「仓库」  en: R1 "ready" and R2 "warehouse".
// Each member contributes the fragment before its first comma; a text
// past 48 characters gets a trailing ellipsis.
void uiOcrGroupsSummary(const OcrGroup *group, char *out, size_t cap);

// Re-evaluate the add buttons' ACTIVE/TIP from running state, the
// picked-window gate and the 16/16 caps. ui_ocr calls this whenever
// picked-window state changes.
typedef BOOL (*UiOcrGroupsGateFn)(void);
void uiOcrGroupsSetRegionGate(UiOcrGroupsGateFn hasPickedWindow);
void uiOcrGroupsRefresh(void);

// Inline error channel shared with ui_ocr's red label; the group dialog
// shows its own in-dialog errors instead and never routes through this.
typedef void (*UiOcrGroupsErrorFn)(const char *text);
void uiOcrGroupsSetErrorSink(UiOcrGroupsErrorFn sink);

// The drag-to-add region button living in the region frame; ui_ocr
// wires its ACTION and the test suite reads ACTIVE/TIP off it.
Ihandle *uiOcrGroupsRegionButton(void);

// Validate + store an editor dialog draft. editRow -1 appends; a
// non-negative row replaces that group. On rejection FALSE is returned
// and errOut receives the localized message for the dialog to show.
BOOL uiOcrGroupsCommitDraft(const OcrGroup *draft, long editRow,
                            char *errOut, size_t errCap);

// Test seams -----------------------------------------------------------

Ihandle *uiOcrRegionsMatrixForTest(void);
Ihandle *uiOcrGroupsMatrixForTest(void);
Ihandle *uiOcrGroupsNewGroupButtonForTest(void);
size_t  uiOcrGroupsRegionCountForTest(void);
size_t  uiOcrGroupsGroupCountForTest(void);

// Same commit path as the dialog's OK button. editRow -1 appends; a
// non-negative row replaces that group. Validation failure routes the
// validator's English error to the bound error sink.
BOOL uiOcrGroupsCommitGroupForTest(const OcrGroup *draft, long editRow);
void uiOcrGroupsDeleteRegionForTest(size_t index);
void uiOcrGroupsRenameRegionForTest(size_t index, const char *name);

// Suppress the delete-region IupAlarm (test harness acts as "confirm").
void uiOcrGroupsSetConfirmSuppressedForTest(BOOL suppress);

#endif // CLUMSY_UI_OCR_GROUPS_H
