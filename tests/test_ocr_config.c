// Unit tests for OCR config persistence under the region/group model:
// UTF-8 key=value save/load roundtrip (Chinese keywords, 4-decimal
// norms), legacy ruleN.* migration, tolerant parsing of corrupt/unknown
// lines, missing-file failure, and the validation matrix.
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
#define CFG_LEGACY "build/tests/t_ocr_config_legacy.txt"
#define CFG_RESAVE "build/tests/t_ocr_config_resave.txt"

static void putRegion(OcrConfig *cfg, size_t i, const char *name,
                      double x, double y, double w, double h) {
    OcrRegionDef *r = &cfg->regions[i];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->norm.x = x; r->norm.y = y; r->norm.w = w; r->norm.h = h;
    if (i + 1 > cfg->regionCount) cfg->regionCount = i + 1;
}

static void putMember(OcrGroup *g, size_t k, int regionIndex,
                      const char *kw) {
    OcrGroupMember *m = &g->members[k];
    memset(m, 0, sizeof(*m));
    m->regionIndex = regionIndex;
    strncpy(m->keywords, kw, sizeof(m->keywords) - 1);
    if (k + 1 > g->memberCount) g->memberCount = k + 1;
}

static OcrGroup *putGroup(OcrConfig *cfg, size_t j, OcrEdge edge,
                          int frames, int loss) {
    OcrGroup *g = &cfg->groups[j];
    memset(g, 0, sizeof(*g));
    g->edge = edge;
    g->confirmFrames = frames;
    g->lossPercent = loss;
    if (j + 1 > cfg->groupCount) cfg->groupCount = j + 1;
    return g;
}

static void readWholeFile(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static void test_roundtrip(void) {
    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = TRUE;
    cfg.changeDetect = TRUE;
    cfg.intervalMs = 300;
    strncpy(cfg.exeName, "qa-fixture.exe", sizeof(cfg.exeName) - 1);
    strncpy(cfg.windowTitle, "QA Fixture 窗口",
            sizeof(cfg.windowTitle) - 1);
    putRegion(&cfg, 0, "status-bar", 0.1234, 0.8567, 0.2345, 0.0876);
    putRegion(&cfg, 1, "warehouse", 0.4000, 0.1000, 0.2000, 0.3000);

    OcrGroup *g0 = putGroup(&cfg, 0, OCR_EDGE_DISAPPEAR, 2, 95);
    putMember(g0, 0, 0, "就绪");
    putMember(g0, 1, 1, "仓库");
    OcrGroup *g1 = putGroup(&cfg, 1, OCR_EDGE_APPEAR, 1, 50);
    putMember(g1, 0, 1, "重连,reconnect");

    char err[MSG_BUFSIZE];
    assert(ocrConfigSave(CFG_PATH, &cfg, err));

    // Persisted key layout (hand-edited files share this format).
    char text[4096];
    readWholeFile(CFG_PATH, text, sizeof(text));
    assert(strstr(text, "region.0.name=status-bar") != NULL);
    assert(strstr(text, "region.0.x=0.1234") != NULL);
    assert(strstr(text, "region.0.y=0.8567") != NULL);
    assert(strstr(text, "region.0.w=0.2345") != NULL);
    assert(strstr(text, "region.0.h=0.0876") != NULL);
    assert(strstr(text, "group.0.edge=disappear") != NULL);
    assert(strstr(text, "group.0.frames=2") != NULL);
    assert(strstr(text, "group.0.loss=95") != NULL);
    assert(strstr(text, "group.0.member.0.region=0") != NULL);
    assert(strstr(text, "group.0.member.0.keywords=就绪") != NULL);
    assert(strstr(text, "group.0.member.1.region=1") != NULL);
    assert(strstr(text, "group.0.member.1.keywords=仓库") != NULL);
    assert(strstr(text, "group.1.edge=appear") != NULL);

    OcrConfig got;
    memset(&got, 0, sizeof(got));
    assert(ocrConfigLoad(CFG_PATH, &got, err));
    assert(got.enabled);
    assert(got.changeDetect);
    assert(got.intervalMs == 300);
    assert(strcmp(got.exeName, "qa-fixture.exe") == 0);
    assert(strcmp(got.windowTitle, "QA Fixture 窗口") == 0);
    assert(got.regionCount == 2);
    assert(got.groupCount == 2);

    assert(strcmp(got.regions[0].name, "status-bar") == 0);
    assert(fabs(got.regions[0].norm.x - 0.1234) < 5e-5);
    assert(fabs(got.regions[0].norm.y - 0.8567) < 5e-5);
    assert(fabs(got.regions[0].norm.w - 0.2345) < 5e-5);
    assert(fabs(got.regions[0].norm.h - 0.0876) < 5e-5);
    assert(strcmp(got.regions[1].name, "warehouse") == 0);
    assert(fabs(got.regions[1].norm.w - 0.2) < 5e-5);

    assert(got.groups[0].edge == OCR_EDGE_DISAPPEAR);
    assert(got.groups[0].confirmFrames == 2);
    assert(got.groups[0].lossPercent == 95);
    assert(got.groups[0].memberCount == 2);
    assert(got.groups[0].members[0].regionIndex == 0);
    assert(strcmp(got.groups[0].members[0].keywords, "就绪") == 0);
    assert(got.groups[0].members[1].regionIndex == 1);
    assert(strcmp(got.groups[0].members[1].keywords, "仓库") == 0);

    assert(got.groups[1].edge == OCR_EDGE_APPEAR);
    assert(got.groups[1].confirmFrames == 1);
    assert(got.groups[1].lossPercent == 50);
    assert(got.groups[1].memberCount == 1);
    assert(got.groups[1].members[0].regionIndex == 1);
    assert(strcmp(got.groups[1].members[0].keywords,
                  "重连,reconnect") == 0);

    assert(ocrConfigValidate(&got, err));

    remove(CFG_PATH);
}

// A pre-region/group release wrote ruleCount + ruleN.* rows. Loading
// such a file must produce one region and one single-member group per
// old rule; an unnamed rule's region gets the "Region N" fallback. A
// re-save must leave no legacy keys behind.
static void test_legacy_migration(void) {
    FILE *f = fopen(CFG_LEGACY, "wb");
    assert(f);
    fputs(
        "enabled=1\n"
        "changeDetect=1\n"
        "interval=300\n"
        "exe=qa-fixture.exe\n"
        "title=QA Fixture 窗口\n"
        "ruleCount=2\n"
        "rule0.name=idle-bar\n"
        "rule0.keywords=仓库\n"
        "rule0.edge=disappear\n"
        "rule0.confirm=2\n"
        "rule0.loss=95\n"
        "rule0.norm=0.1000,0.9000,0.5000,0.0800\n"
        // rule1 deliberately has no name
        "rule1.keywords=重连,reconnect\n"
        "rule1.edge=appear\n"
        "rule1.confirm=1\n"
        "rule1.loss=50\n"
        "rule1.norm=0.2000,0.3000,0.3000,0.2000\n",
        f);
    fclose(f);

    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    char err[MSG_BUFSIZE];
    assert(ocrConfigLoad(CFG_LEGACY, &cfg, err));
    assert(cfg.regionCount == 2);
    assert(cfg.groupCount == 2);

    assert(strcmp(cfg.regions[0].name, "idle-bar") == 0);
    assert(fabs(cfg.regions[0].norm.x - 0.1) < 5e-5);
    assert(fabs(cfg.regions[0].norm.y - 0.9) < 5e-5);
    assert(fabs(cfg.regions[0].norm.w - 0.5) < 5e-5);
    assert(fabs(cfg.regions[0].norm.h - 0.08) < 5e-5);
    assert(strcmp(cfg.regions[1].name, "Region 2") == 0);
    assert(fabs(cfg.regions[1].norm.x - 0.2) < 5e-5);
    assert(fabs(cfg.regions[1].norm.h - 0.2) < 5e-5);

    assert(cfg.groups[0].edge == OCR_EDGE_DISAPPEAR);
    assert(cfg.groups[0].confirmFrames == 2);
    assert(cfg.groups[0].lossPercent == 95);
    assert(cfg.groups[0].memberCount == 1);
    assert(cfg.groups[0].members[0].regionIndex == 0);
    assert(strcmp(cfg.groups[0].members[0].keywords, "仓库") == 0);

    assert(cfg.groups[1].edge == OCR_EDGE_APPEAR);
    assert(cfg.groups[1].confirmFrames == 1);
    assert(cfg.groups[1].lossPercent == 50);
    assert(cfg.groups[1].memberCount == 1);
    assert(cfg.groups[1].members[0].regionIndex == 1);
    assert(strcmp(cfg.groups[1].members[0].keywords,
                  "重连,reconnect") == 0);

    assert(ocrConfigValidate(&cfg, err));

    // Re-saving emits new-format keys only; reloading yields the same
    // migrated data.
    assert(ocrConfigSave(CFG_RESAVE, &cfg, err));
    char text[4096];
    readWholeFile(CFG_RESAVE, text, sizeof(text));
    assert(strstr(text, "rule") == NULL);
    assert(strstr(text, "region.0.name=idle-bar") != NULL);
    assert(strstr(text, "region.1.name=Region 2") != NULL);

    OcrConfig again;
    memset(&again, 0, sizeof(again));
    assert(ocrConfigLoad(CFG_RESAVE, &again, err));
    assert(again.regionCount == 2);
    assert(again.groupCount == 2);
    assert(strcmp(again.regions[0].name, "idle-bar") == 0);
    assert(strcmp(again.regions[1].name, "Region 2") == 0);
    assert(again.groups[0].memberCount == 1);
    assert(again.groups[0].members[0].regionIndex == 0);
    assert(strcmp(again.groups[0].members[0].keywords, "仓库") == 0);
    assert(again.groups[1].members[0].regionIndex == 1);
    assert(strcmp(again.groups[1].members[0].keywords,
                  "重连,reconnect") == 0);
    assert(ocrConfigValidate(&again, err));

    remove(CFG_LEGACY);
    remove(CFG_RESAVE);
}

static void test_corrupt_lines_tolerated(void) {
    FILE *f = fopen(CFG_BAD, "wb");
    assert(f);
    fputs(
        "# heading comment\n"
        "garbage line without equals\n"
        "enabled=notanumber\n"
        "unknown.key=ignoreme\n"
        "regionCount=1\n"
        "groupCount=1\n"
        "exe=qa-fixture.exe\n"
        "region.99.name=outofrange\n"
        "region.0.name=bar\n"
        "region.0.x=0.1\n"
        "region.0.y=0.9\n"
        "region.0.w=0.5\n"
        "region.0.h=0.08\n"
        "group.16.edge=appear\n"       // index 16 >= OCR_MAX_GROUPS: skipped
        "group.0.edge=sideways\n"      // bad enum -> zeroed default
        "group.0.frames=notanumber\n"  // ignored, stays 0
        "group.0.loss=95\n"
        "group.0.member.0.region=notnum\n" // member 0 never lands
        "group.0.member.0.unknown=x\n"
        "group.0.member.1.region=0\n"
        "group.0.member.1.keywords=仓库\n",
        f);
    fclose(f);

    OcrConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    char err[MSG_BUFSIZE];
    assert(ocrConfigLoad(CFG_BAD, &cfg, err)); // corrupt lines are nonfatal
    assert(cfg.enabled == FALSE);              // bad bool ignored -> zero
    assert(cfg.regionCount == 1);
    assert(strcmp(cfg.exeName, "qa-fixture.exe") == 0);
    assert(strcmp(cfg.regions[0].name, "bar") == 0);
    assert(fabs(cfg.regions[0].norm.x - 0.1) < 5e-5);
    assert(cfg.groupCount == 1);
    assert(cfg.groups[0].edge == OCR_EDGE_APPEAR); // bad enum -> default 0
    assert(cfg.groups[0].confirmFrames == 0);
    assert(cfg.groups[0].lossPercent == 95);
    // The member whose region index failed to parse is dropped; the
    // later slot-1 member compacts into slot 0.
    assert(cfg.groups[0].memberCount == 1);
    assert(cfg.groups[0].members[0].regionIndex == 0);
    assert(strcmp(cfg.groups[0].members[0].keywords, "仓库") == 0);

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
    putRegion(&cfg, 0, "status", 0.0, 0.9, 1.0, 0.1);
    putRegion(&cfg, 1, "warehouse", 0.2, 0.3, 0.3, 0.2);
    OcrGroup *g0 = putGroup(&cfg, 0, OCR_EDGE_DISAPPEAR, 2, 95);
    putMember(g0, 0, 0, "就绪");
    putMember(g0, 1, 1, "仓库");
    OcrGroup *g1 = putGroup(&cfg, 1, OCR_EDGE_APPEAR, 1, 50);
    putMember(g1, 0, 1, "重连");
    return cfg;
}

static void test_validate(void) {
    char err[MSG_BUFSIZE];

    OcrConfig good = validCfg();
    assert(ocrConfigValidate(&good, err));

    // enabled but no target exe -> refuse; disabled may have none.
    OcrConfig noExe = validCfg();
    noExe.exeName[0] = '\0';
    assert(!ocrConfigValidate(&noExe, err));
    assert(err[0] != '\0');
    OcrConfig disabled = validCfg();
    disabled.enabled = FALSE;
    disabled.exeName[0] = '\0';
    assert(ocrConfigValidate(&disabled, err));

    // No groups at all is a legal saved config (the UI gates Start).
    OcrConfig noGroups = validCfg();
    noGroups.groupCount = 0;
    assert(ocrConfigValidate(&noGroups, err));

    // Group-level matrix.
    OcrConfig zeroMembers = validCfg();
    zeroMembers.groups[0].memberCount = 0;
    assert(!ocrConfigValidate(&zeroMembers, err));
    assert(strstr(err, "group 1") != NULL);

    OcrConfig badIndex = validCfg();
    badIndex.groups[0].members[1].regionIndex = 5;
    assert(!ocrConfigValidate(&badIndex, err));
    assert(strstr(err, "group 1") != NULL);
    assert(!ocrGroupValidate(&badIndex.groups[0], 1,
                             badIndex.regionCount, err));

    OcrConfig dupMember = validCfg();
    dupMember.groups[0].members[1].regionIndex = 0;
    assert(!ocrConfigValidate(&dupMember, err));
    assert(strstr(err, "group 1") != NULL);

    OcrConfig emptyKw = validCfg();
    emptyKw.groups[0].members[0].keywords[0] = '\0';
    assert(!ocrConfigValidate(&emptyKw, err));
    assert(strstr(err, "group 1") != NULL);

    OcrConfig sepKw = validCfg();
    strcpy(sepKw.groups[0].members[1].keywords, " , ，");
    assert(!ocrConfigValidate(&sepKw, err));

    OcrConfig frames = validCfg();
    frames.groups[1].confirmFrames = 0;
    assert(!ocrConfigValidate(&frames, err));
    assert(strstr(err, "group 2") != NULL);

    OcrConfig loss = validCfg();
    loss.groups[0].lossPercent = 101;
    assert(!ocrConfigValidate(&loss, err));
    loss.groups[0].lossPercent = -1;
    assert(!ocrConfigValidate(&loss, err));

    // Degenerate geometry on a REFERENCED region is refused (hand-edited
    // file or a shrunken window); the ~1% minimum guards thumb math.
    OcrConfig zeroW = validCfg();
    zeroW.regions[0].norm.w = 0.0;
    assert(!ocrConfigValidate(&zeroW, err));
    OcrConfig tinyH = validCfg();
    tinyH.regions[1].norm.h = 0.001;
    assert(!ocrConfigValidate(&tinyH, err));
    OcrConfig overW = validCfg();
    overW.regions[0].norm.w = 1.5;
    assert(!ocrConfigValidate(&overW, err));

    // A region no group references is never captured: its degenerate
    // geometry is harmless and must validate.
    OcrConfig orphan = validCfg();
    putRegion(&orphan, 2, "unused", 0.0, 0.0, 0.0, 0.001);
    assert(orphan.regionCount == 3);
    assert(ocrConfigValidate(&orphan, err));
    OcrConfig onlyOrphan = validCfg();
    onlyOrphan.enabled = FALSE;
    onlyOrphan.groupCount = 0;
    onlyOrphan.regionCount = 1;
    memset(&onlyOrphan.regions[0], 0, sizeof(onlyOrphan.regions[0]));
    onlyOrphan.regions[0].norm.w = 0.0;
    assert(ocrConfigValidate(&onlyOrphan, err));

    // A small-but-real fraction still validates.
    OcrConfig thin = validCfg();
    thin.regions[0].norm.w = 0.02;
    thin.regions[0].norm.h = 0.02;
    assert(ocrConfigValidate(&thin, err));
}

int main(void) {
    test_roundtrip();
    test_legacy_migration();
    test_corrupt_lines_tolerated();
    test_missing_file();
    test_validate();
    printf("OK: ocr_config\n");
    return 0;
}
