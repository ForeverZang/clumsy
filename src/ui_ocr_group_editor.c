/*
 * Modal rule-group editor for section OCR (Task 3 of the picker/groups
 * plan). Pure presentation: the live regions/groups model stays in
 * ui_ocr_groups.c, reached only through the injected commit callback.
 */
#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "i18n.h"
#include "ui_ocr_group_editor.h"

#if defined(CLUMSY_OCR)

#include "iup.h"

#define ERR_COLOR       "208 70 72"
#define DIALOG_SIZE     "580x360"
#define SCROLL_SIZE     "540x170"
#define ROW_LABEL_SIZE  "170x14"
#define EDGE_DROP_SIZE  "170x24"
#define KW_TEXT_COLS    "22"
#define NUM_TEXT_COLS   "4"
#define REGION_NAME_CAP 32

typedef struct {
    Ihandle *check;
    Ihandle *keywords;
} EditorRow;

// IupPopup blocks, so one dialog at a time: callback context in
// file-locals instead of per-widget user data.
static EditorRow g_edRows[OCR_MAX_REGIONS];
static Ihandle  *g_edEdge, *g_edFrames, *g_edLoss, *g_edError;
static const UiOcrGroupEditorInput *g_input;
static UiOcrGroupCommitFn g_commit;

static char g_titleText[80];

static void copyField(char *dst, size_t cap, const char *src) {
    size_t len;
    if (src == NULL) src = "";
    len = strnlen(src, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static BOOL parseDecimal(const char *v, int *out) {
    char *end = NULL;
    long n;
    if (v == NULL || v[0] == '\0') return FALSE;
    n = strtol(v, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return FALSE;
    *out = (int)n;
    return TRUE;
}

static int editorCheckCb(Ihandle *ih) {
    int row = IupGetInt(ih, "ROWID");
    if (row >= 0 && row < OCR_MAX_REGIONS && g_edRows[row].keywords != NULL) {
        IupSetAttribute(g_edRows[row].keywords, "ACTIVE",
                        IupGetInt(ih, "VALUE") ? "YES" : "NO");
    }
    return IUP_DEFAULT;
}

static Ihandle *buildEditorRow(size_t i) {
    Ihandle *check, *nameLbl, *kw, *row;
    char label[8 + REGION_NAME_CAP];
    check = IupToggle("", NULL);
    IupSetInt(check, "ROWID", (int)i);
    IupSetCallback(check, "ACTION", (Icallback)editorCheckCb);
    g_edRows[i].check = check;

    snprintf(label, sizeof(label), "R%u %s", (unsigned)(i + 1),
             g_input->regions[i].name);
    nameLbl = IupLabel(label);
    IupSetAttribute(nameLbl, "RASTERSIZE", ROW_LABEL_SIZE);

    kw = IupText(NULL);
    IupSetAttribute(kw, "VISIBLECOLUMNS", KW_TEXT_COLS);
    IupSetAttribute(kw, "ACTIVE", "NO");
    g_edRows[i].keywords = kw;

    row = IupHbox(check, nameLbl, kw, NULL);
    IupSetAttribute(row, "GAP", "6");
    return row;
}

static int editorOkCb(Ihandle *dlg) {
    OcrGroup draft;
    char err[MSG_BUFSIZE];
    size_t n = 0;
    int frames, loss;
    size_t i;
    (void)dlg;
    memset(&draft, 0, sizeof(draft));

    for (i = 0; i < g_input->regionCount; i++) {
        const char *text;
        if (!IupGetInt(g_edRows[i].check, "VALUE")) continue;
        text = IupGetAttribute(g_edRows[i].keywords, "VALUE");
        copyField(draft.members[n].keywords,
                  sizeof(draft.members[n].keywords), text);
        draft.members[n].regionIndex = (int)i;
        n++;
    }
    draft.memberCount = n;
    if (n == 0) {
        IupStoreAttribute(g_edError, "TITLE",
                          tr("ocr.error.groupMembers"));
        return IUP_DEFAULT;
    }
    for (i = 0; i < n; i++) {
        if (draft.members[i].keywords[0] == '\0') {
            IupStoreAttribute(g_edError, "TITLE",
                              tr("ocr.error.groupKeywords"));
            return IUP_DEFAULT;
        }
    }
    draft.edge = IupGetInt(g_edEdge, "VALUE") == 2
                     ? OCR_EDGE_DISAPPEAR : OCR_EDGE_APPEAR;
    if (!parseDecimal(IupGetAttribute(g_edFrames, "VALUE"), &frames)
        || frames < 1) {
        IupStoreAttribute(g_edError, "TITLE", tr("ocr.error.frames"));
        return IUP_DEFAULT;
    }
    draft.confirmFrames = frames;
    if (!parseDecimal(IupGetAttribute(g_edLoss, "VALUE"), &loss)
        || loss < 0 || loss > 100) {
        IupStoreAttribute(g_edError, "TITLE", tr("ocr.error.loss"));
        return IUP_DEFAULT;
    }
    draft.lossPercent = loss;

    if (!g_commit(&draft, g_input->editRow, err, sizeof(err))) {
        IupStoreAttribute(g_edError, "TITLE", err);
        return IUP_DEFAULT;
    }
    return IUP_CLOSE;
}

// Cancel/Esc: the stack-local draft dies with the dialog, so simply
// ending the popup loop commits nothing.
static int editorCancelCb(Ihandle *ih) {
    (void)ih;
    return IUP_CLOSE;
}

void uiOcrGroupEditorOpen(const UiOcrGroupEditorInput *input,
                          UiOcrGroupCommitFn commit) {
    Ihandle *rows = NULL, *scroll, *hint, *kwHint;
    Ihandle *edgeLbl, *framesLbl, *lossLbl, *paramRow, *btnRow;
    Ihandle *ok, *cancel, *dlg, *titleLbl;
    size_t i;
    if (input == NULL || commit == NULL) return;
    if (input->editRow < -1) return;
    if (input->editRow >= 0 && input->editGroup == NULL) return;
    g_input = input;
    g_commit = commit;
    memset(g_edRows, 0, sizeof(g_edRows));
    g_edEdge = NULL;
    g_edFrames = g_edLoss = g_edError = NULL;

    for (i = 0; i < input->regionCount; i++) {
        Ihandle *row = buildEditorRow(i);
        rows = rows == NULL ? IupVbox(row, NULL)
                            : IupAppend(rows, row);
    }
    if (rows == NULL) rows = IupVbox(IupLabel(""), NULL);
    scroll = IupScrollBox(rows);
    IupSetAttribute(scroll, "RASTERSIZE", SCROLL_SIZE);
    IupSetAttribute(scroll, "EXPAND", "HORIZONTAL");

    hint = IupFlatLabel(tr("ocr.group.members"));
    IupSetAttribute(hint, "WRAP", "YES");
    IupSetAttribute(hint, "EXPAND", "HORIZONTAL");
    kwHint = IupFlatLabel(tr("ocr.group.kwHint"));
    IupSetAttribute(kwHint, "FGCOLOR", "110 110 110");
    IupSetAttribute(kwHint, "WRAP", "YES");
    IupSetAttribute(kwHint, "EXPAND", "HORIZONTAL");

    g_edEdge = IupList(NULL);
    IupSetAttribute(g_edEdge, "DROPDOWN", "YES");
    IupSetAttribute(g_edEdge, "RASTERSIZE", EDGE_DROP_SIZE);
    IupStoreAttribute(g_edEdge, "1", tr("ocr.edge.appear"));
    IupStoreAttribute(g_edEdge, "2", tr("ocr.edge.disappear"));
    g_edFrames = IupText(NULL);
    IupSetAttribute(g_edFrames, "VISIBLECOLUMNS", NUM_TEXT_COLS);
    g_edLoss = IupText(NULL);
    IupSetAttribute(g_edLoss, "VISIBLECOLUMNS", NUM_TEXT_COLS);
    edgeLbl = IupLabel(tr("ocr.col.edge"));
    framesLbl = IupLabel(tr("ocr.col.frames"));
    lossLbl = IupLabel(tr("ocr.col.loss"));
    paramRow = IupHbox(edgeLbl, g_edEdge, framesLbl, g_edFrames,
                       lossLbl, g_edLoss, NULL);
    IupSetAttribute(paramRow, "GAP", "6");

    g_edError = IupFlatLabel("");
    IupSetAttribute(g_edError, "FGCOLOR", ERR_COLOR);
    IupSetAttribute(g_edError, "WRAP", "YES");
    IupSetAttribute(g_edError, "EXPAND", "HORIZONTAL");

    ok = IupButton(tr("ocr.group.ok"), NULL);
    IupSetCallback(ok, "ACTION", (Icallback)editorOkCb);
    cancel = IupButton(tr("ocr.group.cancel"), NULL);
    IupSetCallback(cancel, "ACTION", (Icallback)editorCancelCb);
    btnRow = IupHbox(IupFill(), ok, cancel, NULL);
    IupSetAttribute(btnRow, "GAP", "8");

    if (input->editRow >= 0) {
        snprintf(g_titleText, sizeof(g_titleText),
                 tr("ocr.group.dlgEdit"), (int)(input->editRow + 1));
    } else {
        copyField(g_titleText, sizeof(g_titleText),
                  tr("ocr.group.dlgNew"));
    }
    titleLbl = IupLabel(g_titleText);
    dlg = IupDialog(IupVbox(titleLbl, hint, kwHint, scroll, paramRow,
                            g_edError, btnRow, NULL));
    IupSetAttribute(dlg, "MENUBOX", "NO");
    IupSetAttribute(dlg, "MAXBOX", "NO");
    IupSetAttribute(dlg, "MINBOX", "NO");
    IupSetAttribute(dlg, "RASTERSIZE", DIALOG_SIZE);
    IupSetCallback(dlg, "K_ESC", (Icallback)editorCancelCb);

    if (input->editGroup != NULL) {
        const OcrGroup *src = input->editGroup;
        size_t m;
        for (m = 0; m < src->memberCount; m++) {
            int ri = src->members[m].regionIndex;
            if (ri < 0 || (size_t)ri >= input->regionCount) continue;
            IupSetAttribute(g_edRows[ri].check, "VALUE", "ON");
            IupSetAttribute(g_edRows[ri].keywords, "ACTIVE", "YES");
            IupStoreAttribute(g_edRows[ri].keywords, "VALUE",
                              src->members[m].keywords);
        }
        IupSetAttribute(g_edEdge, "VALUE",
                        src->edge == OCR_EDGE_DISAPPEAR ? "2" : "1");
        {
            char numText[16];
            snprintf(numText, sizeof(numText), "%d", src->confirmFrames);
            IupStoreAttribute(g_edFrames, "VALUE", numText);
            snprintf(numText, sizeof(numText), "%d", src->lossPercent);
            IupStoreAttribute(g_edLoss, "VALUE", numText);
        }
    } else {
        IupSetAttribute(g_edEdge, "VALUE", "2");
        IupStoreAttribute(g_edFrames, "VALUE", "2");
        IupStoreAttribute(g_edLoss, "VALUE", "95");
    }

    IupPopup(dlg, IUP_CENTERPARENT, IUP_CENTERPARENT);
    IupDestroy(dlg);
}

#else // !CLUMSY_OCR

void uiOcrGroupEditorOpen(const UiOcrGroupEditorInput *input,
                          UiOcrGroupCommitFn commit) {
    (void)input;
    (void)commit;
}

#endif // CLUMSY_OCR
