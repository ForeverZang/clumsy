#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "i18n.h"
#include "ui_ocr_groups.h"

#if defined(CLUMSY_OCR)

#include "iupcontrols.h"
#include "ui_ocr_group_editor.h"


/*
 * Fixed-height region/group tables. The static arrays below are the
 * authoritative model; the IupMatrix widgets render them through
 * VALUE_CB and every mutation updates the model first and the matrix
 * second.
 */

#define REGION_LINES_VISIBLE 4
#define GROUP_LINES_VISIBLE  3

// IupMatrix fixed-height geometry, measured on the Win64 IUP 3.30 build
// with the default font (96 DPI): a 17px column-header bar plus 25px per
// data row. NUMLIN_VISIBLE is clamped to NUMLIN and reads -1 before map,
// so an empty table cannot advertise its visible rows through that
// attribute; the fixed visual height is pinned instead with a
// height-only RASTERSIZE ("0xH" -- width 0 leaves horizontal EXPAND to
// the container). The pin survives later NUMLIN updates.
#define MATRIX_HEADER_PX 17
#define MATRIX_ROW_PX    25
#define matrixPinHeight(visibleLines) \
    (MATRIX_HEADER_PX + MATRIX_ROW_PX * (visibleLines))

#define COL_RNUM   0
#define COL_DELETE_REGION 3
#define COL_GNUM   0
#define COL_SUMMARY 1
#define COL_EDGE   2
#define COL_FRAMES 3
#define COL_LOSS   4
#define COL_DELETE_GROUP 5

#define EMPTY_COLOR   "208 70 72"
#define SUMMARY_CAP   280
#define FRAGMENT_CAP  256
#define LABEL_CAP     64

static OcrRegionDef g_regions[OCR_MAX_REGIONS];
static size_t       g_regionCount = 0;
static OcrGroup     g_groups[OCR_MAX_GROUPS];
static size_t       g_groupCount = 0;
static NormRect     g_rectMirror[OCR_MAX_REGIONS];

static Ihandle *g_regionMatrix, *g_groupMatrix;
static Ihandle *g_regionBtn, *g_newGroupBtn;

static BOOL g_running = FALSE;
static BOOL g_confirmSuppressed = FALSE;
static UiOcrGroupsGateFn  g_regionGate = NULL;
static UiOcrGroupsErrorFn g_errorSink = NULL;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static void copyField(char *dst, size_t cap, const char *src) {
    size_t len;
    if (src == NULL) src = "";
    len = strnlen(src, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static size_t utf8RuneCount(const char *s) {
    size_t runes = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) != 0x80) runes++;
        s++;
    }
    return runes;
}

// Keywords separate with an ASCII comma or U+FF0C (EF BC 8C); the
// summary shows the first alternative only.
static void firstFragment(const char *keywords, char *out, size_t cap) {
    const char *ascii = strchr(keywords, ',');
    const char *wide = strstr(keywords, "\xEF\xBC\x8C");
    const char *cut = NULL;
    size_t len;
    if (ascii != NULL && wide != NULL) cut = ascii < wide ? ascii : wide;
    else cut = ascii != NULL ? ascii : wide;
    len = cut != NULL ? (size_t)(cut - keywords) : strlen(keywords);
    if (len >= cap) len = cap - 1;
    memcpy(out, keywords, len);
    out[len] = '\0';
}

static void emitError(const char *text) {
    if (g_errorSink != NULL) g_errorSink(text != NULL ? text : "");
}

// ---------------------------------------------------------------------------
// pure summaries
// ---------------------------------------------------------------------------

static void appendPiece(char *out, size_t cap, size_t *used,
                        const char *piece, size_t pieceRunes,
                        size_t *runes, BOOL *truncated,
                        size_t runeBudget) {
    int n;
    if (*runes + pieceRunes > runeBudget) {
        *truncated = TRUE;
        return;
    }
    n = snprintf(out + *used, cap - *used, "%s", piece);
    if (n < 0) return;
    if ((size_t)n >= cap - *used) {
        *used = cap - 1;
        *truncated = TRUE;
        return;
    }
    *used += (size_t)n;
    *runes += pieceRunes;
}

void uiOcrGroupsSummary(const OcrGroup *group, char *out, size_t cap) {
    static const size_t RUNE_BUDGET = 48;
    BOOL zh = i18nGetLang() == LANG_ZH;
    size_t used = 0, runes = 0;
    BOOL truncated = FALSE;
    size_t i;
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (group == NULL) return;
    for (i = 0; i < group->memberCount && !truncated; i++) {
        const OcrGroupMember *m = &group->members[i];
        char fragment[FRAGMENT_CAP];
        char piece[FRAGMENT_CAP + 24];
        char sep[16];
        int pn;
        firstFragment(m->keywords, fragment, sizeof(fragment));
        pn = snprintf(piece, sizeof(piece),
                      zh ? "R%d「%s」" : "R%d \"%s\"",
                      m->regionIndex + 1, fragment);
        if (pn < 0) break;
        if (i > 0) {
            snprintf(sep, sizeof(sep), zh ? " 且 " : " and ");
            appendPiece(out, cap, &used, sep, utf8RuneCount(sep),
                        &runes, &truncated, RUNE_BUDGET);
        }
        appendPiece(out, cap, &used, piece, utf8RuneCount(piece),
                    &runes, &truncated, RUNE_BUDGET);
    }
    if (truncated && cap - used >= 4) {
        // U+2026 in UTF-8; the cell is wide enough for plain "...",
        // but the tables use the single-character ellipsis.
        memcpy(out + used, "\xE2\x80\xA6", 3);
        out[used + 3] = '\0';
    }
}

// ---------------------------------------------------------------------------
// matrix rendering
// ---------------------------------------------------------------------------

static char g_posText[LABEL_CAP];
static char g_numText[LABEL_CAP];
static char g_summaryText[SUMMARY_CAP];
static char g_intText[16];

static const char *regionCellValue(int lin, int col) {
    const OcrRegionDef *r;
    if (lin < 1 || (size_t)(lin - 1) >= g_regionCount) return "";
    r = &g_regions[lin - 1];
    switch (col) {
    case COL_RNUM:
        snprintf(g_numText, sizeof(g_numText), "R%d", lin);
        return g_numText;
    case 1:
        return r->name;
    case 2:
        snprintf(g_posText, sizeof(g_posText),
                 "%.2f, %.2f %.2f×%.2f", r->norm.x, r->norm.y,
                 r->norm.w, r->norm.h);
        return g_posText;
    case COL_DELETE_REGION:
        return tr("ocr.delete");
    default:
        return "";
    }
}

static const char *groupCellValue(int lin, int col) {
    const OcrGroup *g;
    if (lin < 1 || (size_t)(lin - 1) >= g_groupCount) return "";
    g = &g_groups[lin - 1];
    switch (col) {
    case COL_GNUM:
        snprintf(g_numText, sizeof(g_numText),
                 tr("ocr.groupName"), lin);
        return g_numText;
    case COL_SUMMARY:
        if (g->memberCount == 0) return tr("ocr.group.empty");
        uiOcrGroupsSummary(g, g_summaryText, sizeof(g_summaryText));
        return g_summaryText;
    case COL_EDGE:
        return g->edge == OCR_EDGE_DISAPPEAR
                   ? tr("ocr.edge.disappear") : tr("ocr.edge.appear");
    case COL_FRAMES:
        snprintf(g_intText, sizeof(g_intText), "%d", g->confirmFrames);
        return g_intText;
    case COL_LOSS:
        snprintf(g_intText, sizeof(g_intText), "%d", g->lossPercent);
        return g_intText;
    case COL_DELETE_GROUP:
        return tr("ocr.delete");
    default:
        return "";
    }
}

// IUP callback pointers must keep their 64-bit width: VALUE_CB returns
// char*, never round through int.
static char *regionValueCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    if (lin == 0) {
        switch (col) {
        case 0: return (char *)tr("ocr.col.rnum");
        case 1: return (char *)tr("ocr.col.rname");
        case 2: return (char *)tr("ocr.col.rpos");
        case 3: return (char *)tr("ocr.delete");
        }
        return (char *)"";
    }
    return (char *)regionCellValue(lin, col);
}

static char *groupValueCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    if (lin == 0) {
        switch (col) {
        case 0: return (char *)tr("ocr.col.gnum");
        case 1: return (char *)tr("ocr.col.gsummary");
        case 2: return (char *)tr("ocr.col.edge");
        case 3: return (char *)tr("ocr.col.frames");
        case 4: return (char *)tr("ocr.col.loss");
        case 5: return (char *)tr("ocr.delete");
        }
        return (char *)"";
    }
    return (char *)groupCellValue(lin, col);
}

static int regionReadonlyCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    // Only the name column of a data row is editable.
    return (lin >= 1 && col == 1) ? 0 : 1;
}

static int groupReadonlyCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    (void)lin;
    (void)col;
    return 1; // groups are edited exclusively through the dialog
}

static char *groupFgCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    (void)col;
    if (lin >= 1 && (size_t)(lin - 1) < g_groupCount
        && g_groups[lin - 1].memberCount == 0) {
        return (char *)EMPTY_COLOR;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// model mutation
// ---------------------------------------------------------------------------

static void syncRectMirror(void) {
    size_t i;
    for (i = 0; i < g_regionCount; i++) {
        g_rectMirror[i] = g_regions[i].norm;
    }
}

static void refreshMatrices(void) {
    if (g_regionMatrix != NULL) {
        IupSetInt(g_regionMatrix, "NUMLIN", (int)g_regionCount);
        // Re-assert the scroll window on every model change: mapping
        // with fewer rows clamps the stored value down to NUMLIN and
        // IUP does not restore it when rows grow back. The RASTERSIZE
        // pin keeps the visual height constant either way.
        IupSetInt(g_regionMatrix, "NUMLIN_VISIBLE", REGION_LINES_VISIBLE);
        IupUpdate(g_regionMatrix);
    }
    if (g_groupMatrix != NULL) {
        IupSetInt(g_groupMatrix, "NUMLIN", (int)g_groupCount);
        IupSetInt(g_groupMatrix, "NUMLIN_VISIBLE", GROUP_LINES_VISIBLE);
        IupUpdate(g_groupMatrix);
    }
}

void uiOcrGroupsRefresh(void) {
    BOOL gate = g_regionGate != NULL && g_regionGate();
    BOOL regionFull = g_regionCount >= OCR_MAX_REGIONS;
    BOOL groupFull = g_groupCount >= OCR_MAX_GROUPS;
    if (g_regionBtn != NULL) {
        IupSetAttribute(g_regionBtn, "ACTIVE",
                        (!g_running && gate && !regionFull) ? "YES" : "NO");
        IupSetAttribute(g_regionBtn, "TIP",
                        regionFull ? tr("ocr.limitRegions") : NULL);
    }
    if (g_newGroupBtn != NULL) {
        // Groups only reference regions on the picked window, so the
        // picked-window gate arms this button too (a restored config
        // clears session pick state and must be re-picked before edits).
        IupSetAttribute(g_newGroupBtn, "ACTIVE",
                        (!g_running && gate && !groupFull) ? "YES" : "NO");
        IupSetAttribute(g_newGroupBtn, "TIP",
                        groupFull ? tr("ocr.limitGroups") : NULL);
    }
}

// Reindex every group after region i is removed: members pointing at i
// are dropped, indices above i step down. Surviving indices stay unique
// because the input was sorted+unique and the shift preserves order.
static void remapGroupsAfterRegionDelete(size_t removed) {
    size_t gi;
    for (gi = 0; gi < g_groupCount; gi++) {
        OcrGroup *g = &g_groups[gi];
        size_t w = 0;
        size_t r;
        for (r = 0; r < g->memberCount; r++) {
            int idx = g->members[r].regionIndex;
            if ((size_t)idx == removed) continue;
            if ((size_t)idx > removed) idx--;
            if (w != r) g->members[w] = g->members[r];
            g->members[w].regionIndex = idx;
            w++;
        }
        g->memberCount = w;
    }
}

static BOOL confirmRegionDelete(size_t i) {
    char msg[MSG_BUFSIZE];
    int answer;
    if (g_confirmSuppressed) return TRUE;
    snprintf(msg, sizeof(msg), tr("ocr.delRegion.confirm"),
             g_regions[i].name);
    answer = IupAlarm(tr("ocr.region.title"), msg,
                      tr("ocr.picker.ok"), tr("ocr.picker.cancel"), NULL);
    return answer == 1;
}

static void deleteRegion(size_t i) {
    if (i >= g_regionCount) return;
    if (!confirmRegionDelete(i)) return;
    memmove(&g_regions[i], &g_regions[i + 1],
            (g_regionCount - i - 1) * sizeof(g_regions[0]));
    g_regionCount--;
    memset(&g_regions[g_regionCount], 0, sizeof(g_regions[0]));
    remapGroupsAfterRegionDelete(i);
    syncRectMirror();
    refreshMatrices();
    uiOcrGroupsRefresh();
}

static void deleteGroup(size_t i) {
    if (i >= g_groupCount) return;
    memmove(&g_groups[i], &g_groups[i + 1],
            (g_groupCount - i - 1) * sizeof(g_groups[0]));
    g_groupCount--;
    memset(&g_groups[g_groupCount], 0, sizeof(g_groups[0]));
    refreshMatrices();
    uiOcrGroupsRefresh();
}

static int regionClickCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    if (lin >= 1 && col == COL_DELETE_REGION) {
        deleteRegion((size_t)(lin - 1));
    }
    return IUP_DEFAULT;
}

static int groupClickCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    if (lin >= 1 && col == COL_DELETE_GROUP) {
        deleteGroup((size_t)(lin - 1));
    }
    return IUP_DEFAULT;
}

static int regionChangedCb(Ihandle *ih, int lin, int col,
                           char *newValue, int mode) {
    (void)ih;
    // mode 0 = edit committed (Enter / focus loss).
    if (mode == 0 && lin >= 1 && col == 1
        && (size_t)(lin - 1) < g_regionCount) {
        copyField(g_regions[lin - 1].name,
                  sizeof(g_regions[lin - 1].name), newValue);
    }
    return IUP_DEFAULT;
}

// Validate + store. The error buffer distinguishes this from the
// presentation layer: the editor shows it in-dialog, the test seam
// routes it through the shared inline error sink.
static BOOL commitGroup(const OcrGroup *draft, long editRow,
                        char err[MSG_BUFSIZE]) {
    size_t target;
    if (draft == NULL || err == NULL) return FALSE;
    target = editRow < 0 ? g_groupCount : (size_t)editRow;
    if (editRow >= 0 && target >= g_groupCount) return FALSE;
    if (editRow < 0 && g_groupCount >= OCR_MAX_GROUPS) {
        copyField(err, MSG_BUFSIZE, tr("ocr.limitGroups"));
        return FALSE;
    }
    if (!ocrGroupValidate(draft, target + 1, g_regionCount, err)) {
        return FALSE;
    }
    g_groups[target] = *draft;
    if (editRow < 0) g_groupCount++;
    refreshMatrices();
    uiOcrGroupsRefresh();
    return TRUE;
}

// ---------------------------------------------------------------------------
// group edit dialog (modal; presentation lives in ui_ocr_group_editor.c)
// ---------------------------------------------------------------------------

// Dialog commit callback: run the draft through the same validation and
// storage the test seam uses, returning the localized error for the
// dialog to display in place of the shared inline error sink.
BOOL uiOcrGroupsCommitDraft(const OcrGroup *draft, long editRow,
                            char *errOut, size_t errCap) {
    char err[MSG_BUFSIZE];
    if (errOut == NULL || errCap == 0) return FALSE;
    errOut[0] = '\0';
    if (!commitGroup(draft, editRow, err)) {
        copyField(errOut, errCap, err);
        return FALSE;
    }
    return TRUE;
}

static void openGroupEditor(long editRow) {
    UiOcrGroupEditorInput in;
    if (g_running) return;
    if (editRow < -1) return;
    if (editRow >= 0 && (size_t)editRow >= g_groupCount) return;
    in.regions = g_regions;
    in.regionCount = g_regionCount;
    in.editRow = editRow;
    in.editGroup = editRow >= 0 ? &g_groups[editRow] : NULL;
    uiOcrGroupEditorOpen(&in, uiOcrGroupsCommitDraft);
}

static int newGroupCb(Ihandle *ih) {
    (void)ih;
    openGroupEditor(-1);
    return IUP_DEFAULT;
}

static int groupDblClickCb(Ihandle *ih, int lin, int col) {
    (void)ih;
    (void)col;
    if (lin >= 1) openGroupEditor((long)(lin - 1));
    return IUP_DEFAULT;
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

static Ihandle *newMatrix(int numCol, int visibleLines,
                          Icallback valueCb, Icallback readonlyCb,
                          Icallback clickCb, Icallback dblClickCb,
                          Icallback enterCb, Icallback fgCb,
                          Icallback changedCb, const int *widths) {
    Ihandle *m = IupMatrix(NULL);
    char wbuf[16];
    char pin[24];
    int c;
    IupSetInt(m, "NUMCOL", numCol);
    IupSetInt(m, "NUMLIN", 0);
    IupSetInt(m, "NUMLIN_VISIBLE", visibleLines);
    IupSetInt(m, "NUMCOL_VISIBLE", numCol);
    IupSetAttribute(m, "FLAT", "YES");
    // Height-only pin: fixed pixel height no matter how many rows the
    // model later holds; "0" width keeps the widget EXPAND-ing with its
    // frame. Once mapped and NUMLIN >= visibleLines, NUMLIN_VISIBLE
    // recomputes to the requested window over the pinned height.
    snprintf(pin, sizeof(pin), "0x%d", matrixPinHeight(visibleLines));
    IupSetAttribute(m, "RASTERSIZE", pin);
    for (c = 0; c < numCol; c++) {
        snprintf(wbuf, sizeof(wbuf), "%d", widths[c]);
        IupSetAttributeId(m, "RASTERWIDTH", c, wbuf);
    }
    IupSetCallback(m, "VALUE_CB", valueCb);
    IupSetCallback(m, "READONLY_CB", readonlyCb);
    if (clickCb != NULL) IupSetCallback(m, "CLICK_CB", clickCb);
    if (dblClickCb != NULL) IupSetCallback(m, "DBLCLICK_CB", dblClickCb);
    if (enterCb != NULL) IupSetCallback(m, "ENTERKEY_CB", enterCb);
    if (fgCb != NULL) IupSetCallback(m, "FGCOLOR_CB", fgCb);
    if (changedCb != NULL) IupSetCallback(m, "VALUECHANGED_CB", changedCb);
    return m;
}

Ihandle *uiOcrGroupsCreate(void) {
    static const int regionWidths[4] = { 34, 110, 200, 48 };
    static const int groupWidths[6] = { 46, 238, 104, 40, 48, 48 };
    Ihandle *regionFrame, *groupFrame, *root, *regionBtnRow, *groupBtnRow;

    g_regionMatrix = newMatrix(4, REGION_LINES_VISIBLE,
        (Icallback)regionValueCb, (Icallback)regionReadonlyCb,
        (Icallback)regionClickCb, NULL, NULL, NULL,
        (Icallback)regionChangedCb, regionWidths);
    g_groupMatrix = newMatrix(6, GROUP_LINES_VISIBLE,
        (Icallback)groupValueCb, (Icallback)groupReadonlyCb,
        (Icallback)groupClickCb, (Icallback)groupDblClickCb,
        (Icallback)groupDblClickCb, (Icallback)groupFgCb, NULL,
        groupWidths);

    g_regionBtn = IupButton(tr("ocr.addRegion"), NULL);
    i18nBind(g_regionBtn, "TITLE", "ocr.addRegion");
    regionBtnRow = IupHbox(g_regionBtn, IupFill(), NULL);

    g_newGroupBtn = IupButton(tr("ocr.addGroup"), NULL);
    i18nBind(g_newGroupBtn, "TITLE", "ocr.addGroup");
    IupSetCallback(g_newGroupBtn, "ACTION", (Icallback)newGroupCb);
    groupBtnRow = IupHbox(g_newGroupBtn, IupFill(), NULL);

    regionFrame = IupFrame(IupVbox(g_regionMatrix, regionBtnRow, NULL));
    i18nBind(regionFrame, "TITLE", "ocr.region.title");
    groupFrame = IupFrame(IupVbox(g_groupMatrix, groupBtnRow, NULL));
    i18nBind(groupFrame, "TITLE", "ocr.group.title");

    root = IupVbox(regionFrame, groupFrame, NULL);
    IupSetAttribute(root, "GAP", "6");
    refreshMatrices();
    uiOcrGroupsRefresh();
    return root;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void uiOcrGroupsRetranslate(void) {
    refreshMatrices();
    uiOcrGroupsRefresh();
}

void uiOcrGroupsSetRunning(BOOL running) {
    g_running = running;
    if (g_regionMatrix != NULL) {
        IupSetAttribute(g_regionMatrix, "ACTIVE", running ? "NO" : "YES");
    }
    if (g_groupMatrix != NULL) {
        IupSetAttribute(g_groupMatrix, "ACTIVE", running ? "NO" : "YES");
    }
    uiOcrGroupsRefresh();
}

void uiOcrGroupsGetModel(OcrRegionDef *regions, size_t *regionCount,
                         OcrGroup *groups, size_t *groupCount) {
    size_t i;
    if (regionCount != NULL) *regionCount = g_regionCount;
    if (groupCount != NULL) *groupCount = g_groupCount;
    if (regions != NULL) {
        for (i = 0; i < g_regionCount; i++) regions[i] = g_regions[i];
    }
    if (groups != NULL) {
        for (i = 0; i < g_groupCount; i++) groups[i] = g_groups[i];
    }
}

BOOL uiOcrGroupsSetModel(const OcrRegionDef *regions, size_t regionCount,
                         const OcrGroup *groups, size_t groupCount) {
    size_t i;
    if (regionCount > OCR_MAX_REGIONS || groupCount > OCR_MAX_GROUPS) {
        return FALSE;
    }
    memset(g_regions, 0, sizeof(g_regions));
    memset(g_groups, 0, sizeof(g_groups));
    g_regionCount = regionCount;
    g_groupCount = groupCount;
    for (i = 0; i < regionCount; i++) {
        if (regions != NULL) g_regions[i] = regions[i];
    }
    for (i = 0; i < groupCount; i++) {
        if (groups != NULL) g_groups[i] = groups[i];
    }
    syncRectMirror();
    refreshMatrices();
    uiOcrGroupsRefresh();
    return TRUE;
}

BOOL uiOcrGroupsAppendRegions(const NormRect *rects, size_t count) {
    size_t i;
    if (rects == NULL || count == 0) return count == 0;
    if (g_regionCount + count > OCR_MAX_REGIONS) {
        emitError(tr("ocr.limitRegions"));
        return FALSE;
    }
    for (i = 0; i < count; i++) {
        OcrRegionDef *r = &g_regions[g_regionCount++];
        memset(r, 0, sizeof(*r));
        r->norm = rects[i];
        snprintf(r->name, sizeof(r->name), "Region %u",
                 (unsigned)g_regionCount);
    }
    syncRectMirror();
    refreshMatrices();
    uiOcrGroupsRefresh();
    return TRUE;
}

const NormRect *uiOcrGroupsRegionRects(size_t *count) {
    if (count != NULL) *count = g_regionCount;
    return g_rectMirror;
}

void uiOcrGroupsSetRegionGate(UiOcrGroupsGateFn hasPickedWindow) {
    g_regionGate = hasPickedWindow;
}

void uiOcrGroupsSetErrorSink(UiOcrGroupsErrorFn sink) {
    g_errorSink = sink;
}

Ihandle *uiOcrGroupsRegionButton(void) {
    return g_regionBtn;
}

// ---------------------------------------------------------------------------
// test seams
// ---------------------------------------------------------------------------

Ihandle *uiOcrRegionsMatrixForTest(void) { return g_regionMatrix; }
Ihandle *uiOcrGroupsMatrixForTest(void) { return g_groupMatrix; }
Ihandle *uiOcrGroupsNewGroupButtonForTest(void) { return g_newGroupBtn; }
size_t  uiOcrGroupsRegionCountForTest(void) { return g_regionCount; }
size_t  uiOcrGroupsGroupCountForTest(void) { return g_groupCount; }

BOOL uiOcrGroupsCommitGroupForTest(const OcrGroup *draft, long editRow) {
    char err[MSG_BUFSIZE];
    if (commitGroup(draft, editRow, err)) return TRUE;
    emitError(err);
    return FALSE;
}

void uiOcrGroupsDeleteRegionForTest(size_t index) {
    deleteRegion(index);
}

void uiOcrGroupsRenameRegionForTest(size_t index, const char *name) {
    if (index >= g_regionCount) return;
    copyField(g_regions[index].name, sizeof(g_regions[index].name), name);
    refreshMatrices();
}

void uiOcrGroupsSetConfirmSuppressedForTest(BOOL suppress) {
    g_confirmSuppressed = suppress;
}

#else // !CLUMSY_OCR — no-op stubs

#include <stddef.h>

Ihandle *uiOcrGroupsCreate(void) { return NULL; }
void uiOcrGroupsRetranslate(void) {}
void uiOcrGroupsSetRunning(BOOL running) { (void)running; }
void uiOcrGroupsGetModel(OcrRegionDef *regions, size_t *regionCount,
                         OcrGroup *groups, size_t *groupCount) {
    (void)regions;
    (void)groups;
    if (regionCount) *regionCount = 0;
    if (groupCount) *groupCount = 0;
}
BOOL uiOcrGroupsSetModel(const OcrRegionDef *regions, size_t regionCount,
                         const OcrGroup *groups, size_t groupCount) {
    (void)regions; (void)regionCount; (void)groups; (void)groupCount;
    return FALSE;
}
BOOL uiOcrGroupsAppendRegions(const NormRect *rects, size_t count) {
    (void)rects; (void)count;
    return FALSE;
}
const NormRect *uiOcrGroupsRegionRects(size_t *count) {
    if (count) *count = 0;
    return NULL;
}
void uiOcrGroupsSummary(const OcrGroup *group, char *out, size_t cap) {
    (void)group;
    if (out != NULL && cap > 0) out[0] = '\0';
}
void uiOcrGroupsSetRegionGate(UiOcrGroupsGateFn fn) { (void)fn; }
void uiOcrGroupsRefresh(void) {}
void uiOcrGroupsSetErrorSink(UiOcrGroupsErrorFn fn) { (void)fn; }
Ihandle *uiOcrGroupsRegionButton(void) { return NULL; }
Ihandle *uiOcrRegionsMatrixForTest(void) { return NULL; }
Ihandle *uiOcrGroupsMatrixForTest(void) { return NULL; }
Ihandle *uiOcrGroupsNewGroupButtonForTest(void) { return NULL; }
size_t  uiOcrGroupsRegionCountForTest(void) { return 0; }
size_t  uiOcrGroupsGroupCountForTest(void) { return 0; }
BOOL    uiOcrGroupsCommitGroupForTest(const OcrGroup *draft, long editRow) {
    (void)draft; (void)editRow;
    return FALSE;
}
BOOL    uiOcrGroupsCommitDraft(const OcrGroup *draft, long editRow,
                               char *errOut, size_t errCap) {
    (void)draft; (void)editRow;
    if (errOut != NULL && errCap > 0) errOut[0] = '\0';
    return FALSE;
}
void uiOcrGroupsDeleteRegionForTest(size_t index) { (void)index; }
void uiOcrGroupsRenameRegionForTest(size_t index, const char *name) {
    (void)index; (void)name;
}
void uiOcrGroupsSetConfirmSuppressedForTest(BOOL suppress) {
    (void)suppress;
}

#endif // CLUMSY_OCR
