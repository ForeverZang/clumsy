// Unit tests for OCR config persistence: UTF-8 key=value save/load
// roundtrip (Chinese keywords, 4-decimal norms), tolerant parsing of
// corrupt/unknown lines, missing-file failure, and validation rules.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "ocr_config.h"

#define CFG_PATH   "build/tests/t_ocr_config.txt"
#define CFG_BAD    "build/tests/t_ocr_config_bad.txt"
#define CFG_MISS   "build/tests/t_ocr_config_missing.txt"

static OcrRule makeRule(const char *name, const char *kw, OcrEdge edge,
                        int confirm, int loss,
                        double x, double y, double w, double h) {
    OcrRule r;
    memset(&r, 0, sizeof(r));
    strncpy(r.name, name, sizeof(r.name) - 1);
    strncpy(r.keywords, kw, sizeof(r.keywords) - 1);
    r.edge = edge;
    r.confirmFrames = confirm;
    r.lossPercent = loss;
    r.norm.x = x; r.norm.y = y; r.norm.w = w; r.norm.h = h;
    return r;
}

static void test_roundtrip(void) {
    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = TRUE;
    cfg.changeDetect = TRUE;
    cfg.intervalMs = 300;
    strncpy(cfg.exeName, "qa-fixture.exe", sizeof(cfg.exeName) - 1);
    strncpy(cfg.windowTitle, "QA Fixture 窗口", sizeof(cfg.windowTitle) - 1);
    cfg.ruleCount = 2;
    cfg.rules[0] = makeRule("idle-bar", "仓库", OCR_EDGE_DISAPPEAR, 2, 95,
                            0.1234, 0.8567, 0.2345, 0.0876);
    cfg.rules[1] = makeRule("reconnect", "重连,reconnect", OCR_EDGE_APPEAR,
                            1, 50, 0.4000, 0.1000, 0.2000, 0.3000);

    char err[MSG_BUFSIZE];
    assert(ocrConfigSave(CFG_PATH, &cfg, err));

    OcrConfig got;
    memset(&got, 0, sizeof(got));
    assert(ocrConfigLoad(CFG_PATH, &got, err));
    assert(got.enabled);
    assert(got.changeDetect);
    assert(got.intervalMs == 300);
    assert(strcmp(got.exeName, "qa-fixture.exe") == 0);
    assert(strcmp(got.windowTitle, "QA Fixture 窗口") == 0);
    assert(got.ruleCount == 2);

    assert(strcmp(got.rules[0].name, "idle-bar") == 0);
    assert(strcmp(got.rules[0].keywords, "仓库") == 0);
    assert(got.rules[0].edge == OCR_EDGE_DISAPPEAR);
    assert(got.rules[0].confirmFrames == 2);
    assert(got.rules[0].lossPercent == 95);
    assert(fabs(got.rules[0].norm.x - 0.1234) < 5e-5);
    assert(fabs(got.rules[0].norm.y - 0.8567) < 5e-5);
    assert(fabs(got.rules[0].norm.w - 0.2345) < 5e-5);
    assert(fabs(got.rules[0].norm.h - 0.0876) < 5e-5);

    assert(strcmp(got.rules[1].keywords, "重连,reconnect") == 0);
    assert(got.rules[1].edge == OCR_EDGE_APPEAR);
    assert(got.rules[1].confirmFrames == 1);
    assert(got.rules[1].lossPercent == 50);

    assert(ocrConfigValidate(&got, err));

    remove(CFG_PATH);
}

static void test_corrupt_lines_tolerated(void) {
    FILE *f = fopen(CFG_BAD, "wb");
    assert(f);
    fputs(
        "# heading comment\n"
        "garbage line without equals\n"
        "enabled=notanumber\n"
        "unknown.key=ignoreme\n"
        "ruleCount=1\n"
        "exe=qa-fixture.exe\n"
        "rule99.name=outofrange\n"
        "rule0.loss=notanumber\n"
        "rule0.keywords=仓库\n"
        "rule0.edge=sideways\n"
        "rule0.confirm=2\n"
        "rule0.loss=95\n"
        "rule0.norm=0.1,0.9,onlythree\n"
        "rule0.name=bar\n",
        f);
    fclose(f);

    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    char err[MSG_BUFSIZE];
    assert(ocrConfigLoad(CFG_BAD, &cfg, err)); // corrupt lines are nonfatal
    assert(cfg.enabled == FALSE);              // bad bool ignored -> zero
    assert(cfg.ruleCount == 1);
    assert(strcmp(cfg.exeName, "qa-fixture.exe") == 0);
    assert(strcmp(cfg.rules[0].keywords, "仓库") == 0);
    assert(strcmp(cfg.rules[0].name, "bar") == 0);
    assert(cfg.rules[0].confirmFrames == 2);
    assert(cfg.rules[0].lossPercent == 95);
    assert(cfg.rules[0].edge == OCR_EDGE_APPEAR); // bad enum -> default 0
    assert(cfg.rules[0].norm.x == 0.0); // malformed norm line ignored

    remove(CFG_BAD);
}

static void test_missing_file(void) {
    remove(CFG_MISS);
    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    char err[MSG_BUFSIZE];
    assert(!ocrConfigLoad(CFG_MISS, &cfg, err));
    assert(err[0] != '\0');
}

static OcrConfig validCfg(void) {
    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = TRUE;
    strncpy(cfg.exeName, "qa-fixture.exe", sizeof(cfg.exeName) - 1);
    cfg.intervalMs = 300;
    cfg.ruleCount = 1;
    cfg.rules[0] = makeRule("bar", "仓库", OCR_EDGE_DISAPPEAR, 2, 95,
                            0.0, 0.9, 1.0, 0.1);
    return cfg;
}

static void test_validate(void) {
    char err[MSG_BUFSIZE];

    OcrConfig good = validCfg();
    assert(ocrConfigValidate(&good, err));

    // enabled but no target exe -> refuse
    OcrConfig noExe = validCfg();
    noExe.exeName[0] = '\0';
    assert(!ocrConfigValidate(&noExe, err));
    assert(err[0] != '\0');

    // disabled configs may have an empty exe
    OcrConfig disabled = validCfg();
    disabled.enabled = FALSE;
    disabled.exeName[0] = '\0';
    assert(ocrConfigValidate(&disabled, err));

    OcrConfig loss = validCfg();
    loss.rules[0].lossPercent = 101;
    assert(!ocrConfigValidate(&loss, err));
    loss.rules[0].lossPercent = -1;
    assert(!ocrConfigValidate(&loss, err));

    OcrConfig frames = validCfg();
    frames.rules[0].confirmFrames = 0;
    assert(!ocrConfigValidate(&frames, err));

    OcrConfig emptyKw = validCfg();
    emptyKw.rules[0].keywords[0] = '\0';
    assert(!ocrConfigValidate(&emptyKw, err));

    OcrConfig sepKw = validCfg();
    strcpy(sepKw.rules[0].keywords, " , ，");
    assert(!ocrConfigValidate(&sepKw, err));

    // Degenerate region geometry (hand-edited saved config, shrunken
    // window) must be refused before it can overflow thumb buffers or
    // map to an empty crop.
    OcrConfig zeroW = validCfg();
    zeroW.rules[0].norm.w = 0.0;
    assert(!ocrConfigValidate(&zeroW, err));
    assert(err[0] != '\0');

    OcrConfig tinyH = validCfg();
    tinyH.rules[0].norm.h = 0.001; // under the ~1% minimum fraction
    assert(!ocrConfigValidate(&tinyH, err));

    OcrConfig overW = validCfg();
    overW.rules[0].norm.w = 1.5;
    assert(!ocrConfigValidate(&overW, err));

    // A small-but-real fraction still validates.
    OcrConfig thin = validCfg();
    thin.rules[0].norm.w = 0.02;
    thin.rules[0].norm.h = 0.02;
    assert(ocrConfigValidate(&thin, err));
}

// A freshly dragged region is an unfinished draft: its empty keywords
// do not make the collected config invalid (the rule is inert at
// runtime because no keyword can match), but every other gate still
// applies, and a finished row with empty keywords is still refused.
static void test_validate_drafts(void) {
    char err[MSG_BUFSIZE];
    BOOL drafts[OCR_CONFIG_RULES_MAX];

    OcrConfig cfg = validCfg();
    cfg.ruleCount = 2;
    cfg.rules[1] = makeRule("Region 1", "", OCR_EDGE_DISAPPEAR, 2, 95,
                            0.1, 0.9, 0.5, 0.08);
    memset(drafts, 0, sizeof(drafts));
    drafts[1] = TRUE;
    assert(ocrConfigValidateDrafts(&cfg, drafts, err));

    // NULL drafts is the strict gate: the same config fails.
    assert(!ocrConfigValidateDrafts(&cfg, NULL, err));

    // Draft status exempts only the keyword gate: bad frames/loss fail.
    cfg.rules[1].confirmFrames = 0;
    assert(!ocrConfigValidateDrafts(&cfg, drafts, err));
    cfg.rules[1].confirmFrames = 2;
    cfg.rules[1].lossPercent = 120;
    assert(!ocrConfigValidateDrafts(&cfg, drafts, err));
    cfg.rules[1].lossPercent = 95;
    assert(ocrConfigValidateDrafts(&cfg, drafts, err));

    // A finished (non-draft) row keeps the strict keyword requirement.
    drafts[1] = FALSE;
    assert(!ocrConfigValidateDrafts(&cfg, drafts, err));
}

static void test_validate_draft_geometry(void) {
    char err[MSG_BUFSIZE];
    BOOL drafts[OCR_CONFIG_RULES_MAX];
    OcrConfig cfg = validCfg();
    cfg.ruleCount = 2;
    cfg.rules[1] = makeRule("Region 1", "", OCR_EDGE_DISAPPEAR, 2, 95,
                            0.1, 0.9, 0.5, 0.08);
    memset(drafts, 0, sizeof(drafts));
    drafts[1] = TRUE;
    assert(ocrConfigValidateDrafts(&cfg, drafts, err));
    cfg.rules[1].norm.w = 0.0;
    assert(!ocrConfigValidateDrafts(&cfg, drafts, err));
}

int main(void) {
    test_roundtrip();
    test_corrupt_lines_tolerated();
    test_missing_file();
    test_validate();
    test_validate_drafts();
    test_validate_draft_geometry();
    printf("OK: ocr_config\n");
    return 0;
}
