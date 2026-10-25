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

// One rule row. Column labels live only in the header; rows are created
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
    // TRUE for an unfinished region just dragged in: empty keywords
    // are tolerated at collection (the row is inert), but Start's
    // strict ocrConfigValidate gate still refuses them.
    BOOL draft;
} OcrRow;

static Icallback g_actionCb = NULL;
static OcrLossApplier g_lossApplier = NULL;
static OcrLayoutChangedFn g_layoutChanged = NULL;
static BOOL g_uiMapped = FALSE;

static Ihandle *g_frame, *g_enableToggle, *g_body;
static Ihandle *g_windowLabel, *g_chooseBtn, *g_addBtn;
static Ihandle *g_ruleBox, *g_interval, *g_changeToggle;
static Ihandle *g_startBtn, *g_errorLabel;

static OcrRow g_rows[OCR_CONFIG_RULES_MAX];
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
    if (src == NULL) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
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

static void addRuleRow(const OcrRule *rule) {
    OcrRow *r;
    char value[16];
    if (g_rowCount >= OCR_CONFIG_RULES_MAX) return;
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

    IupStoreAttribute(r->name, "VALUE", rule->name);
    IupStoreAttribute(r->keywords, "VALUE", rule->keywords);
    IupStoreAttribute(r->edge, "VALUE",
                      rule->edge == OCR_EDGE_DISAPPEAR ? "2" : "1");
    // Render the value verbatim: an invalid 0 must survive populate so
    // collection refuses it. Task 9's add-row path passes a pre-filled 2.
    snprintf(value, sizeof(value), "%d", rule->confirmFrames);
    IupStoreAttribute(r->frames, "VALUE", value);
    snprintf(value, sizeof(value), "%d", rule->lossPercent);
    IupStoreAttribute(r->loss, "VALUE", value);
    r->norm = rule->norm;

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

// Append a freshly dragged region as a placeholder rule the user then
// fills in. Defaults follow the design: DISAPPEAR / 2 frames / 95% are
// NOT auto-armed, because the empty keywords fail Start validation.
static BOOL addRegionFromNorm(const NormRect *norm) {
    OcrRule rule;
    if (norm == NULL || g_rowCount >= OCR_CONFIG_RULES_MAX) return FALSE;
    memset(&rule, 0, sizeof(rule));
    g_regionSeq++;
    snprintf(rule.name, sizeof(rule.name), "Region %u", g_regionSeq);
    rule.edge = OCR_EDGE_DISAPPEAR;
    rule.confirmFrames = 2;
    rule.lossPercent = 95;
    rule.norm = *norm;
    addRuleRow(&rule);
    g_rows[g_rowCount - 1].draft = TRUE;
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
            IupSetAttribute(ih, "VALUE", "OFF");
            g_enabled = FALSE;
            setBodyVisible(FALSE);
            uiOcrShowError("ocr.blockedApp");
            return IUP_DEFAULT;
        }
        g_enabled = on;
        setBodyVisible(on);
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

static BOOL collectRow(size_t i, OcrRule *out) {
    const OcrRow *r = &g_rows[i];
    const char *text;
    int frames, loss;
    int edgeItem;

    text = IupGetAttribute(r->name, "VALUE");
    copyField(out->name, sizeof(out->name), text);
    text = IupGetAttribute(r->keywords, "VALUE");
    copyField(out->keywords, sizeof(out->keywords), text);
    edgeItem = IupGetInt(r->edge, "VALUE");
    out->edge = edgeItem == 2 ? OCR_EDGE_DISAPPEAR : OCR_EDGE_APPEAR;

    if (!parseStrictInt(IupGetAttribute(r->frames, "VALUE"), &frames)) {
        rowError(i, "ocr.error.number");
        return FALSE;
    }
    if (frames < 1) { rowError(i, "ocr.error.frames"); return FALSE; }
    out->confirmFrames = frames;

    if (!parseStrictInt(IupGetAttribute(r->loss, "VALUE"), &loss)) {
        rowError(i, "ocr.error.number");
        return FALSE;
    }
    if (loss < 0 || loss > 100) { rowError(i, "ocr.error.loss"); return FALSE; }
    out->lossPercent = loss;

    out->norm = r->norm;
    return TRUE;
}

BOOL uiOcrGetConfig(OcrConfig *out) {
    char err[MSG_BUFSIZE];
    BOOL drafts[OCR_CONFIG_RULES_MAX];
    int interval;
    if (out == NULL) return FALSE;
    memset(drafts, 0, sizeof(drafts));
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

    if (g_rowCount > OCR_CONFIG_RULES_MAX) {
        uiOcrShowError("ocr.error.row");
        return FALSE;
    }
    out->ruleCount = g_rowCount;
    for (size_t i = 0; i < g_rowCount; i++) {
        if (!collectRow(i, &out->rules[i])) return FALSE;
        drafts[i] = g_rows[i].draft;
    }
    if (!ocrConfigValidateDrafts(out, drafts, err)) {
        // Keyword emptiness on finished rows and the enabled-needs-exe
        // gate land here. Freshly dragged, unfinished rows are exempt
        // (they stay inert); Start runs the strict gate before arming.
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

    for (size_t i = 0; i < cfg->ruleCount && i < OCR_CONFIG_RULES_MAX; i++) {
        addRuleRow(&cfg->rules[i]);
    }
    showErrorText("");
    return TRUE;
}

// ---------------------------------------------------------------------------
// public state API
// ---------------------------------------------------------------------------

void uiOcrSetAppContext(const char *exe) {
    copyField(g_appExe, sizeof(g_appExe), exe);
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
    IupSetAttribute(g_startBtn, "ACTIVE", "YES");
    // While running both picker buttons are frozen; on stop they return
    // to the context-derived states (choose needs the app, add needs a
    // picked window).
    refreshPickerButtons();
    IupStoreAttribute(g_startBtn, "TITLE",
                      running ? tr("action.stop") : tr("action.start"));
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
                      g_running ? tr("action.stop") : tr("action.start"));
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

    g_startBtn = IupFlatButton(tr("action.start"));
    IupSetAttribute(g_startBtn, "RASTERSIZE", "120x28");
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

    g_frame = IupFrame(IupVbox(g_enableToggle, g_body, NULL));
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
