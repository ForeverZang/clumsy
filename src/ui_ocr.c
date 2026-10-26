#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "i18n.h"
#include "ui_ocr.h"
#include "ui_picker.h"

#if defined(CLUMSY_OCR)

#define OCR_DEFAULT_INTERVAL_MS 500
#define OCR_MIN_INTERVAL_MS 200
#define OCR_MAX_INTERVAL_MS 60000

// Fixed column widths keep the rule rows from stretching the dialog
// past the folded width (plan Task 8 visual acceptance). The header row
// uses the same widths/gap so columns line up.
#define OCR_COL_GAP      "4"
#define OCR_NAME_W       84
#define OCR_KW_W         170
#define OCR_EDGE_W       156
#define OCR_FRAMES_W     44
#define OCR_LOSS_W       48
#define OCR_DEL_W        52
#define OCR_FIELD_H      24

// One interim rule row. Task 2 boundary adapter: each row stands for
// region i plus a single-member group i referencing it (the legacy
// 1-region/1-rule shape and migrated configs are exactly that). The
// fixed-height region/group tables in Task 3 replace this and add real
// AND groups. Column labels live only in the header; rows are created
// and destroyed at runtime and are never i18n-bound.
typedef struct {
    Ihandle *box;
    Ihandle *name;
    Ihandle *keywords;
    Ihandle *edge;
    Ihandle *frames;
    Ihandle *loss;
    Ihandle *del;
    NormRect norm;
} OcrRow;

static Icallback g_actionCb = NULL;
static OcrLossApplier g_lossApplier = NULL;
static OcrLayoutChangedFn g_layoutChanged = NULL;
static BOOL g_uiMapped = FALSE;

static Ihandle *g_frame, *g_enableToggle, *g_body, *g_gateError;
static Ihandle *g_windowLabel, *g_chooseBtn, *g_addBtn;
static Ihandle *g_ruleBox, *g_interval, *g_changeToggle;
static Ihandle *g_startBtn, *g_errorLabel;

static OcrRow g_rows[OCR_MAX_REGIONS];
static size_t g_rowCount = 0;

static char g_appExe[128] = "";
static char g_windowTitle[256] = "";
static CaptureWindow g_pickedWindow;

static int frameMapCb(Ihandle *frame);

static BOOL g_enabled = FALSE;
static BOOL g_running = FALSE;
static BOOL g_hasPicked = FALSE;
static HWND g_pickedHwnd = NULL;
static unsigned g_regionSeq = 0;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static void copyField(char *dst, size_t cap, const char *src) {
    size_t len;
    if (src == NULL) src = "";
    // Bounded length + memcpy + explicit terminator keeps -O2's
    // -Wstringop-truncation quiet without weakening the cap.
    len = strnlen(src, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Strict integer parse; trailing garbage or an empty field is rejected.
static BOOL parseStrictInt(const char *v, int *out) {
    char *end = NULL;
    long n;
    if (v == NULL || v[0] == '\0') return FALSE;
    n = strtol(v, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return FALSE;
    *out = (int)n;
    return TRUE;
}

// Folding flips BOTH VISIBLE and FLOATING: a VISIBLE=NO child still
// contributes to its parent's natural size, so without FLOATING=YES the
// hidden body keeps the section tall and paints through on this IUP
// build. The same pair drives the basic/advanced tab pages.
static void setBodyVisible(BOOL visible) {
    IupSetAttribute(g_body, "FLOATING", visible ? "NO" : "YES");
    IupSetAttribute(g_body, "VISIBLE", visible ? "YES" : "NO");
    // After map the dialog is RESIZE=NO and fitted to natural size; the
    // host must re-fit. Before map the frame does not exist on screen
    // yet, so a fold there must not reach the host.
    if (g_uiMapped && g_layoutChanged != NULL) {
        g_layoutChanged();
    }
}

static void showErrorText(const char *text) {
    IupStoreAttribute(g_errorLabel, "TITLE", text != NULL ? text : "");
}

// The gate error sits OUTSIDE the foldable body so a refusal stays on
// screen while the body is folded. No refit here: callers flip it right
// before setBodyVisible(), whose host callback performs the single refit.
static void setGateError(BOOL show, const char *text) {
    IupStoreAttribute(g_gateError, "TITLE",
                      (show && text) ? text : "");
    IupSetAttribute(g_gateError, "FLOATING", show ? "NO" : "YES");
    IupSetAttribute(g_gateError, "VISIBLE", show ? "YES" : "NO");
}

// The in-block Start/Stop triggers the OCR session; with OCR disabled it
// must stay inert (it shares the main affect callback).
static void syncStartActive(void) {
    if (g_startBtn != NULL) {
        IupSetAttribute(g_startBtn, "ACTIVE", g_enabled ? "YES" : "NO");
    }
}

static void rowError(size_t idx, const char *key) {
    char msg[MSG_BUFSIZE];
    snprintf(msg, sizeof(msg), tr("ocr.error.row"),
             (unsigned)(idx + 1), tr(key));
    showErrorText(msg);
}

static void refreshWindowLabel(void) {
    IupStoreAttribute(g_windowLabel, "TITLE",
                      g_windowTitle[0] != '\0' ? g_windowTitle
                                               : tr("ocr.winNone"));
    IupSetAttribute(g_windowLabel, "FGCOLOR",
                    g_windowTitle[0] != '\0' ? "0 0 0" : "110 110 110");
}

// Choose needs a concrete app context; Add region needs a window picked
// this session. A running session freezes both (uiOcrSetRunning).
static void refreshPickerButtons(void) {
    if (g_chooseBtn == NULL || g_addBtn == NULL) return;
    IupSetAttribute(g_chooseBtn, "ACTIVE",
                    (!g_running && g_appExe[0] != '\0') ? "YES" : "NO");
    IupSetAttribute(g_addBtn, "ACTIVE",
                    (!g_running && g_hasPicked) ? "YES" : "NO");
}

// ---------------------------------------------------------------------------
// rule rows
// ---------------------------------------------------------------------------

static int deleteRowCb(Ihandle *ih) {
    for (size_t i = 0; i < g_rowCount; i++) {
        if (g_rows[i].del != ih) continue;
        IupDestroy(g_rows[i].box);
        for (size_t j = i; j + 1 < g_rowCount; j++) {
            g_rows[j] = g_rows[j + 1];
        }
        g_rowCount--;
        memset(&g_rows[g_rowCount], 0, sizeof(g_rows[0]));
        return IUP_DEFAULT;
    }
    return IUP_DEFAULT;
}

static Ihandle *labelBound(const char *key) {
    Ihandle *lbl = IupLabel(tr(key));
    i18nBind(lbl, "TITLE", key);
    return lbl;
}

// Interim adapter row from one region and one single-member group. A
// NULL region/group side yields blank/default fields; collection always
// re-emits the parallel region i + group i shape.
static void addRuleRow(const OcrRegionDef *region, const OcrGroup *group) {
    OcrRow *r;
    char value[16];
    if (g_rowCount >= OCR_MAX_REGIONS) return;
    r = &g_rows[g_rowCount++];
    memset(r, 0, sizeof(*r));

    char rs[24];
    r->name = IupText(NULL);
    snprintf(rs, sizeof(rs), "%dx%d", OCR_NAME_W, OCR_FIELD_H);
    IupSetAttribute(r->name, "RASTERSIZE", rs);
    r->keywords = IupText(NULL);
    snprintf(rs, sizeof(rs), "%dx%d", OCR_KW_W, OCR_FIELD_H);
    IupSetAttribute(r->keywords, "RASTERSIZE", rs);
    r->edge = IupList(NULL);
    IupSetAttribute(r->edge, "DROPDOWN", "YES");
    snprintf(rs, sizeof(rs), "%dx%d", OCR_EDGE_W, OCR_FIELD_H);
    IupSetAttribute(r->edge, "RASTERSIZE", rs);
    IupStoreAttribute(r->edge, "1", tr("ocr.edge.appear"));
    IupStoreAttribute(r->edge, "2", tr("ocr.edge.disappear"));
    r->frames = IupText(NULL);
    snprintf(rs, sizeof(rs), "%dx%d", OCR_FRAMES_W, OCR_FIELD_H);
    IupSetAttribute(r->frames, "RASTERSIZE", rs);
    r->loss = IupText(NULL);
    snprintf(rs, sizeof(rs), "%dx%d", OCR_LOSS_W, OCR_FIELD_H);
    IupSetAttribute(r->loss, "RASTERSIZE", rs);
    r->del = IupButton(tr("ocr.delete"), NULL);
    snprintf(rs, sizeof(rs), "%dx%d", OCR_DEL_W, OCR_FIELD_H);
    IupSetAttribute(r->del, "RASTERSIZE", rs);
    IupSetCallback(r->del, "ACTION", (Icallback)deleteRowCb);

    IupStoreAttribute(r->name, "VALUE", region ? region->name : "");
    IupStoreAttribute(r->keywords,
                      "VALUE",
                      (group && group->memberCount >= 1)
                          ? group->members[0].keywords
                          : "");
    IupStoreAttribute(r->edge, "VALUE",
                      (group && group->edge == OCR_EDGE_DISAPPEAR)
                          ? "2" : "1");
    // Render the value verbatim: an invalid 0 must survive populate so
    // collection refuses it. The add-region path passes a pre-filled 2.
    snprintf(value, sizeof(value), "%d", group ? group->confirmFrames : 0);
    IupStoreAttribute(r->frames, "VALUE", value);
    snprintf(value, sizeof(value), "%d",
             group ? group->lossPercent : 95);
    IupStoreAttribute(r->loss, "VALUE", value);
    r->norm = region ? region->norm : (NormRect){ 0, 0, 0, 0 };

    r->box = IupHbox(r->name, r->keywords, r->edge,
                     r->frames, r->loss, r->del, NULL);
    IupSetAttribute(r->box, "GAP", OCR_COL_GAP);
    IupAppend(g_ruleBox, r->box);
    // A child appended to a mapped dialog must be mapped explicitly or
    // it takes layout space but paints blank. Gate on the dialog's native
    // handle, not g_uiMapped: vbox/frame composition nodes never get a
    // WID themselves, config populate can run while the test seam's
    // mapped flag is already set, and before uiOcrCreate's frame is
    // inserted into a dialog IupGetDialog returns NULL (pre-map path).
    {
        Ihandle *dlg = IupGetDialog(g_ruleBox);
        if (dlg != NULL && IupGetAttribute(dlg, "WID") != NULL)
            IupMap(r->box);
    }
    IupRefresh(g_ruleBox);
}

// Append a freshly dragged region as a placeholder region + single-member
// group the user then fills in. Defaults follow the design: DISAPPEAR /
// 2 frames / 95%, and empty keywords fail validation until the user
// types them, so the row can never trigger on its own.
static BOOL addRegionFromNorm(const NormRect *norm) {
    OcrRegionDef region;
    OcrGroup group;
    if (norm == NULL || g_rowCount >= OCR_MAX_REGIONS) return FALSE;
    memset(&region, 0, sizeof(region));
    memset(&group, 0, sizeof(group));
    g_regionSeq++;
    snprintf(region.name, sizeof(region.name), "Region %u", g_regionSeq);
    region.norm = *norm;
    group.edge = OCR_EDGE_DISAPPEAR;
    group.confirmFrames = 2;
    group.lossPercent = 95;
    // The row becomes parallel index g_rowCount when appended.
    group.memberCount = 1;
    group.members[0].regionIndex = (int)g_rowCount;
    addRuleRow(&region, &group);
    return TRUE;
}

static void clearRows(void) {
    for (size_t i = 0; i < g_rowCount; i++) {
        IupDestroy(g_rows[i].box);
    }
    g_rowCount = 0;
    memset(g_rows, 0, sizeof(g_rows));
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

static int enableCb(Ihandle *ih, int state) {
    // Wired on FLAT_ACTION: only genuine clicks reach here. The pre-map
    // gate remains defense in depth for any internal path that reaches
    // the callback before the frame is mapped; folding then would touch
    // sibling layout attributes during map and corrupt the repaint.
    if (!g_uiMapped) return IUP_DEFAULT;
    // Trust the widget VALUE over the int argument (the historical bug
    // was this callback bound to ACTION, whose internal calls passed a
    // garbage second arg during map).
    {
        int on = IupGetInt(ih, "VALUE") != 0;
        (void)state;
        if (on && g_appExe[0] == '\0') {
            // Safety gate: OCR must have a concrete target application.
            // The explanation is the always-visible gate label, NOT the
            // in-body error line: the body is folded here, so an in-body
            // message would leave the click looking completely dead.
            IupSetAttribute(ih, "VALUE", "OFF");
            g_enabled = FALSE;
            setGateError(TRUE, tr("ocr.blockedApp"));
            setBodyVisible(FALSE);
            syncStartActive();
            return IUP_DEFAULT;
        }
        setGateError(FALSE, NULL);
        g_enabled = on;
        setBodyVisible(on);
        syncStartActive();
        if (on) showErrorText("");
    }
    return IUP_DEFAULT;
}

// Enumerate the selected app's visible top-level windows: zero ->
// inline error (the user fixes the app choice); one -> picked directly;
// several -> modal IupList chooser. Shared by the button and the
// headless acceptance harness.
BOOL uiOcrChooseWindow(CaptureWindow *out) {
    CaptureWindow wins[32];
    int n;
    if (out == NULL) return FALSE;
    if (g_running || g_appExe[0] == '\0') return FALSE;

    n = captureEnumWindowsForApp(g_appExe, wins, 32);
    if (n <= 0) {
        uiOcrShowError("ocr.error.noWindows");
        return FALSE;
    }
    if (n == 1) {
        *out = wins[0];
        return TRUE;
    }
    return uiPickerPickWindow(g_appExe, out); // FALSE = user cancel
}

static int chooseWindowCb(Ihandle *ih) {
    CaptureWindow picked;
    UNREFERENCED_PARAMETER(ih);
    if (!uiOcrChooseWindow(&picked)) return IUP_DEFAULT;
    uiOcrSetPickedWindow(&picked);
    showErrorText("");
    return IUP_DEFAULT;
}

// "Add region": rubber-band on the picked window, then append a
// placeholder row. The button is disabled without a picked window, so
// the guards only defend against stale windows.
static int addRegionCb(Ihandle *ih) {
    NormRect norm;
    UNREFERENCED_PARAMETER(ih);
    if (g_running || !g_hasPicked || g_pickedHwnd == NULL) return IUP_DEFAULT;
    if (!captureIsCapturable(g_pickedHwnd)) {
        uiOcrShowError("ocr.error.windowStale");
        return IUP_DEFAULT;
    }
    // FALSE covers user cancel (ESC / right-click) and an undersized drag
    // (the overlay lets the user try again, so it simply returns there).
    if (!uiPickerPickRect(g_pickedHwnd, &norm)) return IUP_DEFAULT;
    if (!addRegionFromNorm(&norm)) return IUP_DEFAULT;
    showErrorText("");
    // The new row grows the body's natural size post-map; re-fit the
    // RESIZE=NO dialog exactly like a fold/expand does.
    if (g_uiMapped && g_layoutChanged != NULL) g_layoutChanged();
    return IUP_DEFAULT;
}

// ---------------------------------------------------------------------------
// config collection / population
// ---------------------------------------------------------------------------

// Collect row i into the interim parallel shape: region i plus one
// single-member group i referencing it.
static BOOL collectRow(size_t i, OcrRegionDef *regionOut,
                       OcrGroup *groupOut) {
    const OcrRow *r = &g_rows[i];
    const char *text;
    int frames, loss;
    int edgeItem;

    text = IupGetAttribute(r->name, "VALUE");
    copyField(regionOut->name, sizeof(regionOut->name), text);
    regionOut->norm = r->norm;

    text = IupGetAttribute(r->keywords, "VALUE");
    copyField(groupOut->members[0].keywords,
              sizeof(groupOut->members[0].keywords), text);
    groupOut->memberCount = 1;
    groupOut->members[0].regionIndex = (int)i;
    edgeItem = IupGetInt(r->edge, "VALUE");
    groupOut->edge = edgeItem == 2 ? OCR_EDGE_DISAPPEAR : OCR_EDGE_APPEAR;

    if (!parseStrictInt(IupGetAttribute(r->frames, "VALUE"), &frames)) {
        rowError(i, "ocr.error.number");
        return FALSE;
    }
    if (frames < 1) { rowError(i, "ocr.error.frames"); return FALSE; }
    groupOut->confirmFrames = frames;

    if (!parseStrictInt(IupGetAttribute(r->loss, "VALUE"), &loss)) {
        rowError(i, "ocr.error.number");
        return FALSE;
    }
    if (loss < 0 || loss > 100) { rowError(i, "ocr.error.loss"); return FALSE; }
    groupOut->lossPercent = loss;
    return TRUE;
}

BOOL uiOcrGetConfig(OcrConfig *out) {
    char err[MSG_BUFSIZE];
    int interval;
    if (out == NULL) return FALSE;
    memset(out, 0, sizeof(*out));

    out->enabled = g_enabled;
    copyField(out->exeName, sizeof(out->exeName), g_appExe);
    copyField(out->windowTitle, sizeof(out->windowTitle), g_windowTitle);
    out->changeDetect = IupGetInt(g_changeToggle, "VALUE") == 1;

    if (!parseStrictInt(IupGetAttribute(g_interval, "VALUE"), &interval)
        || interval < OCR_MIN_INTERVAL_MS
        || interval > OCR_MAX_INTERVAL_MS) {
        uiOcrShowError("ocr.error.interval");
        return FALSE;
    }
    out->intervalMs = interval;

    if (g_rowCount > OCR_MAX_REGIONS) {
        uiOcrShowError("ocr.error.row");
        return FALSE;
    }
    out->regionCount = g_rowCount;
    out->groupCount = g_rowCount;
    for (size_t i = 0; i < g_rowCount; i++) {
        if (!collectRow(i, &out->regions[i], &out->groups[i])) return FALSE;
    }
    // Strict model validation: empty keywords, bad member indices and
    // the enabled-needs-exe gate all land here.
    if (!ocrConfigValidate(out, err)) {
        showErrorText(err);
        return FALSE;
    }
    showErrorText("");
    return TRUE;
}

BOOL uiOcrPopulate(const OcrConfig *cfg) {
    char value[16];
    if (cfg == NULL) return FALSE;
    clearRows();

    copyField(g_windowTitle, sizeof(g_windowTitle), cfg->windowTitle);
    refreshWindowLabel();
    // Populate is a load/reset: the session-local hand pick is dropped,
    // and dragged-region numbering starts over ("Region 1").
    g_hasPicked = FALSE;
    g_pickedHwnd = NULL;
    memset(&g_pickedWindow, 0, sizeof(g_pickedWindow));
    g_regionSeq = 0;
    refreshPickerButtons();

    snprintf(value, sizeof(value), "%d",
             cfg->intervalMs > 0 ? cfg->intervalMs
                                 : OCR_DEFAULT_INTERVAL_MS);
    IupStoreAttribute(g_interval, "VALUE", value);
    IupSetAttribute(g_changeToggle, "VALUE",
                    cfg->changeDetect ? "ON" : "OFF");

    g_enabled = cfg->enabled;
    IupSetAttribute(g_enableToggle, "VALUE", cfg->enabled ? "ON" : "OFF");
    setBodyVisible(cfg->enabled);

    // Interim adapter: one row per group, paired with the region its
    // first member references. Migrated configs (and everything this UI
    // can save) are parallel single-member groups; multi-member groups
    // are fully supported by the engine but only editable once Task 3's
    // group tables replace these rows.
    for (size_t i = 0; i < cfg->groupCount && i < OCR_MAX_GROUPS; i++) {
        const OcrGroup *group = &cfg->groups[i];
        const OcrRegionDef *region = NULL;
        if (group->memberCount >= 1
            && group->members[0].regionIndex >= 0
            && (size_t)group->members[0].regionIndex < cfg->regionCount) {
            region = &cfg->regions[group->members[0].regionIndex];
        }
        addRuleRow(region, group);
    }
    showErrorText("");
    return TRUE;
}

// ---------------------------------------------------------------------------
// public state API
// ---------------------------------------------------------------------------

void uiOcrSetAppContext(const char *exe) {
    copyField(g_appExe, sizeof(g_appExe), exe);
    // A refusal warning naming "pick an application" goes stale the moment
    // the user actually picks one; the next enable attempt is clean.
    if (g_gateError != NULL && exe != NULL && exe[0] != '\0'
        && IupGetInt(g_gateError, "VISIBLE") != 0) {
        setGateError(FALSE, NULL);
    }
    refreshPickerButtons();
}

void uiOcrSetLossApplier(OcrLossApplier fn) {
    g_lossApplier = fn;
}

void uiOcrSetLayoutChangedCallback(OcrLayoutChangedFn fn) {
    g_layoutChanged = fn;
}

BOOL uiOcrEnabled(void) {
    return g_enabled;
}

void uiOcrSetPickedWindow(const CaptureWindow *win) {
    if (win == NULL) {
        g_hasPicked = FALSE;
        g_pickedHwnd = NULL;
        memset(&g_pickedWindow, 0, sizeof(g_pickedWindow));
        g_windowTitle[0] = '\0';
    } else {
        g_hasPicked = TRUE;
        g_pickedHwnd = win->hwnd;
        g_pickedWindow = *win;
        copyField(g_windowTitle, sizeof(g_windowTitle), win->title);
    }
    refreshWindowLabel();
    refreshPickerButtons();
}

const char *uiOcrSavedWindowTitle(void) {
    return g_windowTitle;
}

BOOL uiOcrHasPickedWindow(void) {
    return g_hasPicked;
}

HWND uiOcrPickedHwnd(void) {
    return g_pickedHwnd;
}

// Full copy of the session-picked window (Start resolves targets through
// this without re-enumerating); zero-filled on clear.
BOOL uiOcrPickedWindow(CaptureWindow *out) {
    if (out == NULL || !g_hasPicked) return FALSE;
    *out = g_pickedWindow;
    return TRUE;
}

void uiOcrSetRunning(BOOL running) {
    g_running = running;
    IupSetAttribute(g_enableToggle, "ACTIVE", running ? "NO" : "YES");
    IupSetAttribute(g_body, "ACTIVE", running ? "NO" : "YES");
    // Running: the button is the live Stop control. Stopped: it returns
    // to the enable-gated state (greyed until the section is enabled).
    if (running) {
        IupSetAttribute(g_startBtn, "ACTIVE", "YES");
    } else {
        syncStartActive();
    }
    // While running both picker buttons are frozen; on stop they return
    // to the context-derived states (choose needs the app, add needs a
    // picked window).
    refreshPickerButtons();
    IupStoreAttribute(g_startBtn, "TITLE",
                      running ? tr("ocr.stop") : tr("ocr.start"));
}

void uiOcrShowError(const char *keyOrText) {
    if (keyOrText == NULL || keyOrText[0] == '\0') {
        showErrorText("");
    } else {
        showErrorText(tr(keyOrText));
    }
}

void uiOcrRetranslate(void) {
    refreshWindowLabel();
    for (size_t i = 0; i < g_rowCount; i++) {
        OcrRow *r = &g_rows[i];
        IupStoreAttribute(r->edge, "1", tr("ocr.edge.appear"));
        IupStoreAttribute(r->edge, "2", tr("ocr.edge.disappear"));
        IupStoreAttribute(r->del, "TITLE", tr("ocr.delete"));
    }
    IupStoreAttribute(g_startBtn, "TITLE",
                      g_running ? tr("ocr.stop") : tr("ocr.start"));
    if (g_gateError != NULL && IupGetInt(g_gateError, "VISIBLE") != 0) {
        IupStoreAttribute(g_gateError, "TITLE", tr("ocr.blockedApp"));
    }
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

static Ihandle *buildWindowRow(void) {
    Ihandle *lbl;
    g_windowLabel = IupFlatLabel(tr("ocr.winNone"));
    IupSetAttribute(g_windowLabel, "FGCOLOR", "110 110 110");
    IupSetAttribute(g_windowLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(g_windowLabel, "WRAP", "YES");

    g_chooseBtn = IupButton(tr("ocr.choose"), NULL);
    i18nBind(g_chooseBtn, "TITLE", "ocr.choose");
    IupSetAttribute(g_chooseBtn, "ACTIVE", "NO");
    IupSetCallback(g_chooseBtn, "ACTION", (Icallback)chooseWindowCb);

    lbl = IupLabel(tr("ocr.window"));
    i18nBind(lbl, "TITLE", "ocr.window");
    return IupHbox(lbl, g_windowLabel, g_chooseBtn, NULL);
}

// Header column label: i18n-bound, pinned to the matching field width so
// header and rows share one grid.
static Ihandle *headerLabel(const char *key, int width) {
    Ihandle *lbl = labelBound(key);
    char rs[24];
    snprintf(rs, sizeof(rs), "%dx14", width);
    IupSetAttribute(lbl, "RASTERSIZE", rs);
    return lbl;
}

static Ihandle *buildRuleHeader(void) {
    Ihandle *spacer = IupLabel("");
    char rs[24];
    Ihandle *row;
    snprintf(rs, sizeof(rs), "%dx14", OCR_DEL_W);
    IupSetAttribute(spacer, "RASTERSIZE", rs);
    row = IupHbox(headerLabel("ocr.col.name", OCR_NAME_W),
                  headerLabel("ocr.col.keywords", OCR_KW_W),
                  headerLabel("ocr.col.edge", OCR_EDGE_W),
                  headerLabel("ocr.col.frames", OCR_FRAMES_W),
                  headerLabel("ocr.col.loss", OCR_LOSS_W),
                  spacer, NULL);
    IupSetAttribute(row, "GAP", OCR_COL_GAP);
    return row;
}

// Grey one-line hint explaining the keywords column (bound so the long
// zh/EN text retranslates; WRAP keeps it inside the folded width).
static Ihandle *buildKwHint(void) {
    Ihandle *lbl = IupFlatLabel(tr("ocr.kwHint"));
    i18nBind(lbl, "TITLE", "ocr.kwHint");
    IupSetAttribute(lbl, "FGCOLOR", "110 110 110");
    IupSetAttribute(lbl, "WRAP", "YES");
    IupSetAttribute(lbl, "EXPAND", "HORIZONTAL");
    return lbl;
}

static Ihandle *buildParamsRow(void) {
    Ihandle *intervalLbl;
    g_interval = IupText(NULL);
    IupSetAttribute(g_interval, "VISIBLECOLUMNS", "5");
    IupStoreAttribute(g_interval, "VALUE",
                      "500");
    intervalLbl = IupLabel(tr("ocr.interval"));
    i18nBind(intervalLbl, "TITLE", "ocr.interval");
    g_changeToggle = IupToggle(tr("ocr.changeDetect"), NULL);
    i18nBind(g_changeToggle, "TITLE", "ocr.changeDetect");
    IupSetAttribute(g_changeToggle, "VALUE", "ON");
    return IupHbox(intervalLbl, g_interval, g_changeToggle, IupFill(), NULL);
}

static int frameMapCb(Ihandle *frame) {
    (void)frame;
    g_uiMapped = TRUE;
    // IupFlatToggle's own map path drops a VALUE set before map
    // (observed ON -> NULL, checkbox painted empty). Children map
    // before the frame, so re-assert the populated state here.
    IupSetAttribute(g_enableToggle, "VALUE",
                    g_enabled ? "ON" : "OFF");
    return IUP_DEFAULT;
}

Ihandle *uiOcrCreate(Icallback actionCbImpl) {
    Ihandle *windowRow, *addRow, *headerRow, *paramsRow, *logArea;
    Ihandle *startRow;

    g_actionCb = actionCbImpl;

    g_enableToggle = IupFlatToggle(tr("ocr.enable"));
    i18nBind(g_enableToggle, "TITLE", "ocr.enable");
    // IupFlatToggle reserves ACTION as its internal canvas paint routine,
    // exactly like IupFlatButton: the user click must be wired through
    // FLAT_ACTION. Binding ACTION both blanks the checkbox/title repaint
    // and makes the widget invoke enableCb with a garbage second arg on
    // every internal map/paint pass.
    IupSetCallback(g_enableToggle, "FLAT_ACTION", (Icallback)enableCb);

    windowRow = buildWindowRow();

    g_addBtn = IupButton(tr("ocr.addRegion"), NULL);
    i18nBind(g_addBtn, "TITLE", "ocr.addRegion");
    IupSetAttribute(g_addBtn, "ACTIVE", "NO");
    IupSetCallback(g_addBtn, "ACTION", (Icallback)addRegionCb);
    addRow = IupHbox(g_addBtn, IupFill(), NULL);

    g_ruleBox = IupVbox(NULL);
    headerRow = buildRuleHeader();
    paramsRow = buildParamsRow();

    logArea = uiOcrLogBuild();

    g_errorLabel = IupFlatLabel("");
    IupSetAttribute(g_errorLabel, "FGCOLOR", "208 70 72");
    IupSetAttribute(g_errorLabel, "WRAP", "YES");
    IupSetAttribute(g_errorLabel, "EXPAND", "HORIZONTAL");

    // Short 开始/停止 label: this triggers the OCR session, distinct from
    // the main green 开始影响 affect bar. Greyed until the section is on —
    // a click while OCR is disabled must never silently start ordinary
    // affect (uiOcrGlueStart early-returns when the gate is off).
    g_startBtn = IupFlatButton(tr("ocr.start"));
    IupSetAttribute(g_startBtn, "RASTERSIZE", "120x28");
    IupSetAttribute(g_startBtn, "ACTIVE", "NO");
    // The main green Start lives just below in the beginner window; this
    // in-block copy reads as a standard button (white with grey border)
    // rather than a second full-width bar or a borderless text line.
    IupSetAttribute(g_startBtn, "BGCOLOR", "255 255 255");
    IupSetAttribute(g_startBtn, "HLCOLOR", "232 236 242");
    IupSetAttribute(g_startBtn, "PSCOLOR", "208 214 222");
    IupSetAttribute(g_startBtn, "FGCOLOR", "32 32 36");
    IupSetAttribute(g_startBtn, "TEXTHLCOLOR", "32 32 36");
    IupSetAttribute(g_startBtn, "TEXTPSCOLOR", "32 32 36");
    IupSetAttribute(g_startBtn, "BORDERCOLOR", "170 174 180");
    if (g_actionCb != NULL) {
        IupSetCallback(g_startBtn, "FLAT_ACTION", g_actionCb);
    }
    startRow = IupHbox(IupFill(), g_startBtn, IupFill(), NULL);

    g_body = IupVbox(windowRow, addRow, headerRow, buildKwHint(),
                     g_ruleBox, paramsRow,
                     logArea, g_errorLabel, startRow, NULL);
    IupSetAttribute(g_body, "VISIBLE", "NO");
    IupSetAttribute(g_body, "FLOATING", "YES");
    IupSetAttribute(g_body, "GAP", "6");

    // Gate-refusal line placed OUTSIDE g_body so it can explain why a
    // click on the enable checkbox did nothing while the body is still
    // folded. Hidden + floating until a refusal actually happens.
    g_gateError = IupFlatLabel("");
    IupSetAttribute(g_gateError, "FGCOLOR", "208 70 72");
    IupSetAttribute(g_gateError, "WRAP", "YES");
    IupSetAttribute(g_gateError, "EXPAND", "HORIZONTAL");
    IupSetAttribute(g_gateError, "VISIBLE", "NO");
    IupSetAttribute(g_gateError, "FLOATING", "YES");

    g_frame = IupFrame(IupVbox(g_enableToggle, g_gateError, g_body, NULL));
    i18nBind(g_frame, "TITLE", "ocr.title");
    // Bottom-up mapping: children map before the frame, so once the
    // frame maps all construction-time state is settled; only then let
    // folds trigger the host's dialog refit.
    IupSetCallback(g_frame, "MAP_CB", (Icallback)frameMapCb);
    return g_frame;
}

// ---------------------------------------------------------------------------
// test seams
// ---------------------------------------------------------------------------

void uiOcrSetMappedForTest(BOOL mapped) { g_uiMapped = mapped; }

// Drives the same append path the Add-region button uses, without the
// modal overlay.
BOOL uiOcrAddRegionForTest(const NormRect *norm) {
    return addRegionFromNorm(norm);
}

Ihandle *uiOcrRowBoxForTest(size_t i) {
    if (i >= g_rowCount) return NULL;
    return g_rows[i].box;
}

Ihandle *uiOcrEnableToggleForTest(void) { return g_enableToggle; }
Ihandle *uiOcrChooseButtonForTest(void) { return g_chooseBtn; }
Ihandle *uiOcrAddButtonForTest(void) { return g_addBtn; }
Ihandle *uiOcrStartButtonForTest(void) { return g_startBtn; }
Ihandle *uiOcrBodyForTest(void) { return g_body; }
Ihandle *uiOcrGateErrorForTest(void) { return g_gateError; }

#else // !CLUMSY_OCR — no-op stubs for builds without OCR

#include <stddef.h>

Ihandle *uiOcrCreate(Icallback actionCb) {
    (void)actionCb;
    return NULL;
}
void uiOcrRetranslate(void) {}
void uiOcrSetAppContext(const char *exe) { (void)exe; }
void uiOcrSetLossApplier(OcrLossApplier fn) { (void)fn; }
void uiOcrSetLayoutChangedCallback(OcrLayoutChangedFn fn) { (void)fn; }
BOOL uiOcrEnabled(void) { return FALSE; }
BOOL uiOcrGetConfig(OcrConfig *out) { (void)out; return FALSE; }
BOOL uiOcrPopulate(const OcrConfig *cfg) { (void)cfg; return FALSE; }
BOOL uiOcrChooseWindow(CaptureWindow *out) {
    (void)out;
    return FALSE;
}
void uiOcrSetPickedWindow(const CaptureWindow *win) { (void)win; }
const char *uiOcrSavedWindowTitle(void) { return ""; }
BOOL uiOcrHasPickedWindow(void) { return FALSE; }
HWND uiOcrPickedHwnd(void) { return NULL; }
void uiOcrSetRunning(BOOL running) { (void)running; }
void uiOcrShowError(const char *keyOrText) { (void)keyOrText; }
Ihandle *uiOcrEnableToggleForTest(void) { return NULL; }
Ihandle *uiOcrChooseButtonForTest(void) { return NULL; }
Ihandle *uiOcrAddButtonForTest(void) { return NULL; }
Ihandle *uiOcrStartButtonForTest(void) { return NULL; }
Ihandle *uiOcrBodyForTest(void) { return NULL; }
Ihandle *uiOcrGateErrorForTest(void) { return NULL; }
void uiOcrSetMappedForTest(BOOL mapped) { (void)mapped; }
BOOL uiOcrAddRegionForTest(const NormRect *norm) {
    (void)norm;
    return FALSE;
}
Ihandle *uiOcrRowBoxForTest(size_t i) {
    (void)i;
    return NULL;
}

#endif // CLUMSY_OCR
