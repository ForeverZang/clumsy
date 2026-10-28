// Unit tests for OCR session orchestration under the region/group model:
// seeded per-group arming, AND-group poll synthesis, late arming after
// unavailable startup, confirm-N edge paths through capture/read fakes,
// union-only capture of referenced regions, change-detection read
// skipping, per-group unavailable rate limiting, same-tick trigger gear
// arbitration, CSV identity/escaping, and the worker queue/busy behavior.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "ocr_session.h"
#include "ocr_worker.h"
#include "ocr_timeline.h"

// --- fakes ---------------------------------------------------------------

struct Frame {
    const char  *text;
    unsigned char pixel;
    BOOL captureOk;
    BOOL blank; // PrintWindow succeeded but returned a black/no-picture frame
};

struct RegFake {
    const struct Frame *frames;
    int n;
    int captures;
    int reads;
    BOOL failRead; // force the OCR read to report failure
};

#define PHYS_COUNT 3
static struct RegFake g_reg[PHYS_COUNT];
static UINT64 g_fakeNow = 0;
static UINT64 fakeNow(void) { return g_fakeNow; }

static BOOL g_blockCapture = FALSE;
static HANDLE g_entered = NULL;  // auto-reset: worker entered blocked capture
static HANDLE g_release = NULL;  // auto-reset: let blocked capture return
static int g_blockedEntries = 0;

// Per-tick target identity gate (production: same HWND still owned by
// the pinned PID, visible and not minimized).
static BOOL g_targetAlive = TRUE;
static BOOL fakeTargetAlive(const CaptureWindow *target) {
    (void)target;
    return g_targetAlive;
}

static BOOL fakeCapture(HWND hwnd, const RECT *rc, unsigned char **outBgra,
                        int *outW, int *outH, char err[MSG_BUFSIZE]) {
    (void)hwnd; (void)err;
    // Session maps each region's norm onto a 1000x1000 fallback client
    // (real GetClientRect only fails on the fake sentinel HWNDs). The
    // three fixture regions start at left = 0 / 500 / 670.
    int ri = rc->left / 300;
    if (ri > PHYS_COUNT - 1) ri = PHYS_COUNT - 1;
    struct RegFake *f = &g_reg[ri];
    f->captures++;

    if (g_blockCapture) {
        g_blockedEntries++;
        SetEvent(g_entered);
        if (WaitForSingleObject(g_release, 10000) != WAIT_OBJECT_0) {
            return FALSE;
        }
    }

    int fi = f->captures - 1; // seed is capture #0
    if (fi >= f->n) fi = f->n - 1;
    if (!f->frames[fi].captureOk) return FALSE;

    int w = 32, h = 8;
    unsigned char *bgra = malloc((size_t)w * h * 4);
    assert(bgra);
    memset(bgra, f->frames[fi].blank ? 0 : f->frames[fi].pixel,
           (size_t)w * h * 4);
    if (!f->frames[fi].blank) {
        bgra[0] = (unsigned char)(0xA0 + ri);
        bgra[4] = (unsigned char)(fi & 0xFF);
    }
    *outBgra = bgra;
    *outW = w;
    *outH = h;
    return TRUE;
}

static BOOL fakeRead(OcrEngine *engine, const unsigned char *bgra,
                     int w, int h, char *out, size_t cap,
                     char err[MSG_BUFSIZE]) {
    (void)engine; (void)w; (void)h; (void)err;
    int ri = bgra[0] - 0xA0;
    int fi = bgra[4];
    struct RegFake *f = &g_reg[ri];
    f->reads++;
    if (f->failRead) {
        strncpy(err, "injected read failure", MSG_BUFSIZE - 1);
        err[MSG_BUFSIZE - 1] = '\0';
        return FALSE;
    }
    const char *text = fi < f->n ? f->frames[fi].text : "";
    strncpy(out, text, cap - 1);
    out[cap - 1] = '\0';
    return TRUE;
}

static OcrSessionRegion g_sessionRegions[OCR_MAX_REGIONS];
static OcrSessionGroup  g_sessionGroups[OCR_MAX_GROUPS];

static void setupSession(OcrSession *s, BOOL changeDetect) {
    memset(s, 0, sizeof(*s));
    memset(g_sessionRegions, 0, sizeof(g_sessionRegions));
    memset(g_sessionGroups, 0, sizeof(g_sessionGroups));
    memset(g_reg, 0, sizeof(g_reg));
    s->regions = g_sessionRegions;
    s->groups = g_sessionGroups;
    s->intervalMs = 200;
    s->changeDetect = changeDetect;
    s->madThreshold = 5;
    s->target.hwnd = (HWND)(INT_PTR)1;
    s->timeline = NULL;
    s->captureFn = fakeCapture;
    s->readFn = fakeRead;
    s->targetAliveFn = fakeTargetAlive;
    s->engine = NULL;
    s->nowMsFn = fakeNow;
    g_fakeNow = 0;
    g_targetAlive = TRUE;
}

// Lay a physical region out on the virtual client. x/wfrac are client
// fractions; 0 / 0.5 / 0.67 map onto fake region indices 0 / 1 / 2.
static void setRegion(OcrSession *s, size_t i, double x, double wfrac) {
    g_sessionRegions[i].norm.x = x;
    g_sessionRegions[i].norm.y = 0.0;
    g_sessionRegions[i].norm.w = wfrac;
    g_sessionRegions[i].norm.h = 0.1;
    if (i + 1 > s->regionCount) s->regionCount = i + 1;
}

static void addGroup(OcrSession *s, size_t gi, OcrEdge edge, int loss,
                     int frames, const int *memberRegion,
                     const char *const *memberKw, size_t memberCount) {
    OcrSessionGroup *sg = &g_sessionGroups[gi];
    memset(sg, 0, sizeof(*sg));
    sg->group.edge = edge;
    sg->group.lossPercent = loss;
    sg->group.confirmFrames = frames;
    sg->group.memberCount = memberCount;
    for (size_t k = 0; k < memberCount; k++) {
        sg->group.members[k].regionIndex = memberRegion[k];
        strncpy(sg->group.members[k].keywords, memberKw[k],
                sizeof(sg->group.members[k].keywords) - 1);
        s->referenced[memberRegion[k]] = TRUE;
    }
    ocrRuleStateInit(&sg->state);
    if (gi + 1 > s->groupCount) s->groupCount = gi + 1;
}

static void addSingleGroup(OcrSession *s, OcrEdge edge, const char *kw,
                           int frames, int loss) {
    int region = 0;
    const char *kws[1] = { kw };
    addGroup(s, 0, edge, loss, frames, &region, kws, 1);
}

static int pumpOnce(OcrSession *s, OcrSessionEvent *ev, int cap) {
    int n = 0;
    BOOL ok = ocrSessionPump(s, ev, &n, cap);
    assert(ok);
    return n;
}

static int countEvent(const OcrSessionEvent *ev, int n, const char *name) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(ev[i].event, name) == 0) c++;
    return c;
}

static int findEvent(const OcrSessionEvent *ev, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(ev[i].event, name) == 0) return i;
    return -1;
}

// Index of the k-th (0-based) event with the given name, -1 if absent.
static int findEventNth(const OcrSessionEvent *ev, int n, const char *name,
                        int kth) {
    for (int i = 0; i < n; i++) {
        if (strcmp(ev[i].event, name) != 0) continue;
        if (kth == 0) return i;
        kth--;
    }
    return -1;
}

static void framesFor(int ri, const struct Frame *frames, int n) {
    g_reg[ri].frames = frames;
    g_reg[ri].n = n;
}

// --- (a) seed PRESENT, two empty frames confirm a DISAPPEAR at 95% -------

static void test_disappear_trigger(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE}, {"仓库", 10, TRUE}, {"仓库", 10, TRUE},
        {"", 20, TRUE}, {"", 20, TRUE},
    };
    framesFor(0, frames, 5);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(g_sessionGroups[0].state.armed);
    assert(g_sessionGroups[0].state.stable == OCR_STABLE_PRESENT);

    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 4; i++) {
        g_fakeNow += 1000;
        int n = pumpOnce(&s, ev + total, (int)(16 - total));
        total += n;
    }
    // Every pump reports one region scan before any group event; only
    // the 4th pump additionally fires (trigger + loss_change).
    assert(countEvent(ev, total, "scan") == 4);
    assert(strcmp(ev[0].event, "scan") == 0);
    assert(ev[0].regionIndex == 0);
    assert(ev[0].groupIndex == -1);
    assert(strcmp(ev[0].status, "ok") == 0);
    assert(total == 6);
    {
        int trig = findEvent(ev, total, "rule_trigger");
        int loss = findEvent(ev, total, "loss_change");
        assert(trig == 4 && loss == 5);
        assert(ev[trig].groupIndex == 0);
        assert(ev[trig].lossPercent == 95);
        assert(ev[loss].groupIndex == 0);
        assert(ev[loss].lossPercent == 95);
    }
}

// --- (b) two groups firing the same pump: both get rule_trigger rows, ----
//     but only the first group in table order applies its gear.          --

static void test_two_groups_first_gear(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    setRegion(&s, 1, 0.5, 0.5);
    {
        int r0 = 0;
        const char *k0[1] = { "仓库" };
        int r1 = 1;
        const char *k1[1] = { "重连" };
        addGroup(&s, 0, OCR_EDGE_DISAPPEAR, 95, 2, &r0, k0, 1);
        addGroup(&s, 1, OCR_EDGE_APPEAR, 50, 2, &r1, k1, 1);
    }

    static const struct Frame f0[] = {
        {"仓库", 10, TRUE}, {"仓库", 10, TRUE}, {"仓库", 10, TRUE},
        {"", 10, TRUE}, {"", 10, TRUE},
    };
    static const struct Frame f1[] = {
        {"", 10, TRUE}, {"", 10, TRUE}, {"", 10, TRUE},
        {"重连", 10, TRUE}, {"重连", 10, TRUE},
    };
    framesFor(0, f0, 5);
    framesFor(1, f1, 5);

    char path[1024], err[MSG_BUFSIZE];
    s.timeline = ocrTimelineOpen("build/tests/tl-twogroups", path,
                                 sizeof(path), err);
    assert(s.timeline != NULL);

    assert(ocrSessionSeed(&s, err));
    assert(g_sessionGroups[1].state.stable == OCR_STABLE_ABSENT);

    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 4; i++) {
        g_fakeNow += 1000;
        total += pumpOnce(&s, ev + total, (int)(16 - total));
    }
    // 4 pumps x 2 region scans, with the final pump's group events last.
    assert(countEvent(ev, total, "scan") == 8);
    assert(total == 11);
    assert(strcmp(ev[8].event, "rule_trigger") == 0);
    assert(ev[8].groupIndex == 0 && ev[8].lossPercent == 95);
    assert(strcmp(ev[9].event, "loss_change") == 0);
    assert(ev[9].groupIndex == 0 && ev[9].lossPercent == 95);
    assert(strcmp(ev[10].event, "rule_trigger") == 0);
    assert(ev[10].groupIndex == 1 && ev[10].lossPercent == 50);
    assert(countEvent(ev, total, "rule_trigger") == 2);
    assert(countEvent(ev, total, "loss_change") == 1);
    ocrTimelineClose(s.timeline);

    // CSV mirrors the arbitration: both trigger rows, one loss_change.
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *csv = malloc((size_t)len + 1);
    assert(csv && fread(csv, 1, (size_t)len, f) == (size_t)len);
    csv[len] = '\0';
    fclose(f);
    assert(strstr(csv, "rule_trigger,group 1,,,disappear,95,group:1,")
           != NULL);
    assert(strstr(csv, "rule_trigger,group 2,重连,,appear,50,group:2,")
           != NULL);
    assert(strstr(csv, "loss_change,group 1,,,disappear,95,group:1,")
           != NULL);
    assert(strstr(csv, "loss_change,group 2,") == NULL);
    free(csv);
}

// --- (c) UNAVAILABLE neither triggers nor resets the confirm run; the ---
//     unavailable event is rate-limited to one per 5 s per group.        --

static void test_unavailable_holds_run(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE},  // seed
        {"", 10, TRUE},      // pump 1: run = 1
        {"", 10, FALSE},     // pump 2: unavailable, event #1
        {"", 10, FALSE},     // pump 3: unavailable, suppressed (<5s)
        {"", 10, TRUE},      // pump 4: run = 2 -> fires
    };
    framesFor(0, frames, 5);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));

    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 4; i++) {
        g_fakeNow += 1000; // 1000,2000,3000,4000 ms
        total += pumpOnce(&s, ev + total, (int)(16 - total));
    }
    assert(countEvent(ev, total, "capture_unavailable") == 1);
    assert(countEvent(ev, total, "rule_trigger") == 1);
    assert(countEvent(ev, total, "loss_change") == 1);
    // One scan attempt per pump, including the failed capture tick.
    assert(countEvent(ev, total, "scan") == 4);
    assert(strcmp(ev[findEventNth(ev, total, "scan", 1)].status,
                  "capture_fail") == 0);
    int trig = findEvent(ev, total, "rule_trigger");
    assert(trig == total - 2); // scan, trigger, loss is the last pump
}

// --- (f) seed fully unavailable: fail closed until the first success, ----
//     then the normal state machine takes over.                           --

static void test_unarmed_seed_fail_closed(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);

    static const struct Frame frames[] = {
        {"", 10, FALSE},     // seed capture fails: stays unarmed
        {"", 10, TRUE},      // pump 1: first success late-arms ABSENT
        {"", 10, TRUE},      // pump 2: steady absent, still no trigger
        {"仓库", 10, TRUE},  // pump 3: safe poll settles PRESENT
        {"", 10, TRUE},      // pump 4: run = 1
        {"", 10, TRUE},      // pump 5: run = 2 -> fires once
    };
    framesFor(0, frames, 6);

    char err[MSG_BUFSIZE];
    assert(!ocrSessionSeed(&s, err)); // nothing armed -> Start must refuse
    assert(!g_sessionGroups[0].state.armed);

    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 5; i++) {
        g_fakeNow += 1000;
        total += pumpOnce(&s, ev + total, (int)(16 - total));
        // The late-arming pumps 1..3 only report scans; no rule fires.
        if (i < 3)
            assert(countEvent(ev, total, "rule_trigger") == 0);
    }
    // Five scan lines plus the final trigger/loss pair.
    assert(countEvent(ev, total, "scan") == 5);
    assert(total == 7);
    {
        int trig = findEvent(ev, total, "rule_trigger");
        assert(trig == 5);
        assert(ev[trig].lossPercent == 95);
    }
}

// --- (d) change detection: identical thumbnail skips read; changed frame -
//         reads. With detection off, every frame reads.                  --

static void test_change_detection_gate(void) {
    static const struct Frame frames[] = {
        {"仓库", 40, TRUE}, // seed
        {"仓库", 40, TRUE}, // pump 1: identical pixels
        {"仓库", 210, TRUE}, // pump 2: clearly changed
    };

    OcrSession s;
    setupSession(&s, TRUE);
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);
    framesFor(0, frames, 3);
    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(g_reg[0].reads == 1);

    OcrSessionEvent ev[8];
    g_fakeNow += 1000;
    pumpOnce(&s, ev, 8);
    assert(g_reg[0].reads == 1); // unchanged -> read skipped
    g_fakeNow += 1000;
    pumpOnce(&s, ev, 8);
    assert(g_reg[0].reads == 2); // changed -> read runs

    // changeDetect = FALSE: the same sequence reads every frame.
    OcrSession s2;
    setupSession(&s2, FALSE);
    setRegion(&s2, 0, 0.0, 0.5);
    addSingleGroup(&s2, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);
    framesFor(0, frames, 3); // setupSession reset the fake state
    assert(ocrSessionSeed(&s2, err));
    g_fakeNow += 1000;
    pumpOnce(&s2, ev, 8);
    assert(g_reg[0].reads == 2);
}

// --- (h) spec §5/§6.4: when change detection skips OCR, the region replays
//     its last text into the state machine — steady frames still count as
//     confirming polls, so a disappear held by a static blank frame fires.

static void test_change_gate_reuses_last_text_disappear(void) {
    OcrSession s;
    setupSession(&s, TRUE); // change detection ON, MAD threshold 5
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE}, // seed: armed PRESENT
        {"仓库", 10, TRUE}, // pump 1: identical -> reuse "仓库", steady
        {"", 20, TRUE},     // pump 2: changed -> OCR miss, run = 1
        {"", 20, TRUE},     // pump 3: identical blank -> reuse "", run = 2
    };
    framesFor(0, frames, 4);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(g_reg[0].reads == 1);

    OcrSessionEvent ev[8];
    g_fakeNow += 1000;
    assert(pumpOnce(&s, ev, 8) == 1);
    assert(strcmp(ev[0].event, "scan") == 0);
    assert(strcmp(ev[0].status, "unchanged") == 0);
    assert(g_reg[0].reads == 1); // unchanged frame did not run OCR

    g_fakeNow += 1000;
    assert(pumpOnce(&s, ev, 8) == 1);
    assert(strcmp(ev[0].event, "scan") == 0);
    assert(strcmp(ev[0].status, "ok") == 0);
    assert(g_reg[0].reads == 2); // changed frame ran OCR, run = 1 only

    g_fakeNow += 1000;
    int n = pumpOnce(&s, ev, 8);
    assert(n == 3); // scan + trigger + loss: reused miss confirms
    assert(g_reg[0].reads == 2); // no extra OCR for the reused poll
    assert(strcmp(ev[0].event, "scan") == 0);
    assert(strcmp(ev[0].status, "unchanged") == 0);
    assert(strcmp(ev[1].event, "rule_trigger") == 0);
    assert(ev[1].lossPercent == 95);
    assert(strcmp(ev[2].event, "loss_change") == 0);
    assert(ev[2].lossPercent == 95);
}

// --- (i) the reused text is the one the trigger row carries: an appear ---
//     confirmed by a steady (OCR-skipped) frame still reports the text.    --

static void test_change_gate_reuses_last_text_appear(void) {
    OcrSession s;
    setupSession(&s, TRUE);
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_APPEAR, "重连", 2, 50);

    static const struct Frame frames[] = {
        {"", 10, TRUE},     // seed: armed ABSENT
        {"重连", 20, TRUE}, // pump 1: changed -> match, run = 1
        {"重连", 20, TRUE}, // pump 2: identical -> reuse, run = 2 -> fires
    };
    framesFor(0, frames, 3);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));

    OcrSessionEvent ev[8];
    g_fakeNow += 1000;
    assert(pumpOnce(&s, ev, 8) == 1);
    assert(strcmp(ev[0].event, "scan") == 0);
    assert(strcmp(ev[0].status, "ok") == 0);
    assert(g_reg[0].reads == 2);
    g_fakeNow += 1000;
    int n = pumpOnce(&s, ev, 8);
    assert(n == 3); // unchanged scan carries the reused trigger text
    assert(g_reg[0].reads == 2); // confirming frame skipped OCR
    assert(strcmp(ev[0].status, "unchanged") == 0);
    assert(strcmp(ev[1].event, "rule_trigger") == 0);
    assert(ev[1].lossPercent == 50);
    assert(ev[1].text && strcmp(ev[1].text, "重连") == 0);
}

// --- (j) a captured-but-black frame (locked desktop, exclusive fullscreen)
//     is a capture failure, never an empty OCR text: an armed disappear
//     group neither confirms nor triggers on it; when the real picture
//     returns, the state machine resumes where it stood.                  --

static void test_blank_frame_is_unavailable(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE, FALSE}, // seed: armed PRESENT
        {"",    0, TRUE, TRUE},    // pump 1: black frame
        {"",    0, TRUE, TRUE},    // pump 2: black, event rate-limited
        {"",    0, TRUE, TRUE},    // pump 3: black past the 5 s window
        {"",   20, TRUE, FALSE},   // pump 4: real empty frame, run = 1
        {"",   20, TRUE, FALSE},   // pump 5: run = 2 -> fires
    };
    framesFor(0, frames, 6);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(g_reg[0].reads == 1);

    OcrSessionEvent ev[16];
    int total = 0;
    g_fakeNow = 1000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 2000; // within the 5 s quiet window
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 7000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(countEvent(ev, total, "capture_unavailable") == 2);
    assert(countEvent(ev, total, "rule_trigger") == 0);
    assert(g_reg[0].reads == 1); // black frames never reached OCR
    // The three black ticks surface as per-region blank_frame scans even
    // though their group-level unavailable lines are rate-limited.
    assert(countEvent(ev, total, "scan") == 3);
    for (int i = 0; i < 3; i++)
        assert(strcmp(ev[findEventNth(ev, total, "scan", i)].status,
                      "blank_frame") == 0);

    g_fakeNow = 8000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 9000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(countEvent(ev, total, "rule_trigger") == 1);
    assert(countEvent(ev, total, "loss_change") == 1);
    assert(countEvent(ev, total, "scan") == 5);
    assert(strcmp(ev[findEventNth(ev, total, "scan", 3)].status, "ok")
           == 0);
    assert(strcmp(ev[findEventNth(ev, total, "scan", 4)].status, "ok")
           == 0);
    int trig = findEvent(ev, total, "rule_trigger");
    assert(trig == total - 2);
}

// --- (k) per-tick identity gate: a HWND that died and got recycled to ---
//     another process is never captured or OCR'd - every GROUP reports
//     unavailable (independently rate-limited), no group can fire.       --

static void test_target_identity_gate(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    {
        int r0 = 0;
        const char *kw[1] = { "仓库" };
        addGroup(&s, 0, OCR_EDGE_DISAPPEAR, 95, 2, &r0, kw, 1);
        addGroup(&s, 1, OCR_EDGE_DISAPPEAR, 50, 3, &r0, kw, 1);
    }

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE, FALSE}, // seed (capture #0)
        {"",   20, TRUE, FALSE},   // pump when alive again: run = 1
        {"",   20, TRUE, FALSE},   // group 0 run = 2 fires; group 1 waits
    };
    framesFor(0, frames, 3);

    // Seed refuses while the target is dead/recycled (fail closed).
    char err[MSG_BUFSIZE];
    g_targetAlive = FALSE;
    assert(!ocrSessionSeed(&s, err));
    assert(err[0] != '\0');
    assert(g_reg[0].captures == 0);

    // Seed succeeds once it is the target again.
    g_targetAlive = TRUE;
    assert(ocrSessionSeed(&s, err));

    // Dead ticks: no capture, no trigger, one unavailable row per group
    // per 5 s window.
    OcrSessionEvent ev[16];
    int total = 0;
    g_targetAlive = FALSE;
    g_fakeNow = 1000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 2000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 7000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(g_reg[0].captures == 1); // seed only
    assert(countEvent(ev, total, "capture_unavailable") == 4);
    assert(countEvent(ev, total, "rule_trigger") == 0);
    // Dead ticks capture nothing, so they emit no per-region scans.
    assert(countEvent(ev, total, "scan") == 0);

    // Back to the real target: the 2-frame group fires, the 3-frame one
    // keeps waiting.
    g_targetAlive = TRUE;
    g_fakeNow = 8000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 9000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(g_reg[0].captures == 3);
    assert(countEvent(ev, total, "rule_trigger") == 1);
    assert(countEvent(ev, total, "loss_change") == 1);
    assert(countEvent(ev, total, "scan") == 2);
}

// --- AND truth table: a two-member group synthesizes one poll per tick; -
//     any miss -> MISSED, any unavailable -> UNAVAILABLE (freezes), and a
//     trigger carries the member texts joined with " | ".                --

static void test_and_group_truth_table(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    setRegion(&s, 1, 0.5, 0.5);
    {
        int regions[2] = { 0, 1 };
        const char *kw[2] = { "就绪", "仓库" };
        addGroup(&s, 0, OCR_EDGE_DISAPPEAR, 95, 2, regions, kw, 2);
    }

    static const struct Frame f0[] = {
        {"就绪", 10, TRUE}, // seed
        {"其他", 11, TRUE}, // pump 1: r0 misses -> MISSED, run = 1
        {"",  0, FALSE},    // pump 2: r0 unavailable -> UNAVAILABLE
        {"其他", 12, TRUE}, // pump 3: MISSED, run = 2 -> fires
    };
    static const struct Frame f1[] = {
        {"仓库", 20, TRUE}, {"仓库", 20, TRUE},
        {"仓库", 20, TRUE}, {"仓库", 20, TRUE},
    };
    framesFor(0, f0, 4);
    framesFor(1, f1, 4);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(g_sessionGroups[0].state.stable == OCR_STABLE_PRESENT);

    OcrSessionEvent ev[16];
    int total = 0;
    g_fakeNow = 1000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    // Both members read: two scans, neither group event fires yet.
    assert(total == 2);
    assert(countEvent(ev, total, "scan") == 2);
    assert(strcmp(ev[0].status, "ok") == 0);
    assert(ev[0].text && strcmp(ev[0].text, "其他") == 0);
    assert(strcmp(ev[1].status, "ok") == 0);
    g_fakeNow = 2000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(countEvent(ev, total, "capture_unavailable") == 1);
    assert(countEvent(ev, total, "rule_trigger") == 0);
    // The failed member capture is its own scan line.
    assert(strcmp(ev[findEventNth(ev, total, "scan", 2)].status,
                  "capture_fail") == 0);
    g_fakeNow = 3000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(countEvent(ev, total, "rule_trigger") == 1);
    assert(countEvent(ev, total, "loss_change") == 1);
    int trig = -1;
    for (int i = 0; i < total; i++)
        if (strcmp(ev[i].event, "rule_trigger") == 0) trig = i;
    assert(trig == total - 2);
    assert(ev[trig].groupIndex == 0);
    assert(ev[trig].lossPercent == 95);
    assert(ev[trig].text && strcmp(ev[trig].text, "其他 | 仓库") == 0);
}

// --- union capture: regions no group references are never captured. -----

static void test_union_capture_only_referenced(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.33);
    setRegion(&s, 1, 0.34, 0.2);
    setRegion(&s, 2, 0.67, 0.33);
    {
        int r2 = 2;
        const char *kw[1] = { "仓库" };
        addGroup(&s, 0, OCR_EDGE_DISAPPEAR, 95, 2, &r2, kw, 1);
    }

    static const struct Frame f2[] = {
        {"仓库", 30, TRUE}, {"仓库", 31, TRUE},
    };
    framesFor(2, f2, 2);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    OcrSessionEvent ev[8];
    g_fakeNow = 1000;
    // Exactly one scan event, for the sole referenced region (R3).
    assert(pumpOnce(&s, ev, 8) == 1);
    assert(strcmp(ev[0].event, "scan") == 0);
    assert(ev[0].regionIndex == 2);
    assert(strcmp(ev[0].status, "ok") == 0);
    assert(ev[0].text && strcmp(ev[0].text, "仓库") == 0);

    assert(g_reg[0].captures == 0);
    assert(g_reg[1].captures == 0);
    assert(g_reg[2].captures == 2); // seed + one pump
}

// --- lenient seed: one readable member region is enough for Start; the ---
//     group stays unarmed until the first tick every member is readable.  --

static void test_lenient_seed(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    setRegion(&s, 1, 0.5, 0.5);
    {
        int regions[2] = { 0, 1 };
        const char *kw[2] = { "就绪", "仓库" };
        addGroup(&s, 0, OCR_EDGE_DISAPPEAR, 95, 2, regions, kw, 2);
    }

    static const struct Frame f0[] = {
        {"就绪", 10, TRUE}, {"就绪", 10, TRUE}, {"就绪", 10, TRUE},
    };
    static const struct Frame f1[] = {
        {"", 0, FALSE},     // seed capture fails
        {"仓库", 20, TRUE}, // pump 1: both readable -> arm PRESENT
        {"仓库", 20, TRUE}, // pump 2: steady
    };
    framesFor(0, f0, 3);
    framesFor(1, f1, 3);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err)); // >=1 referenced region readable
    assert(!g_sessionGroups[0].state.armed);

    OcrSessionEvent ev[8];
    int total = 0;
    g_fakeNow = 1000;
    total += pumpOnce(&s, ev + total, 8 - total);
    assert(g_sessionGroups[0].state.armed);
    assert(g_sessionGroups[0].state.stable == OCR_STABLE_PRESENT);
    // Arming tick: two scans (one member just became readable), no
    // trigger, no unavailable line.
    assert(countEvent(ev, total, "scan") == 2);
    assert(countEvent(ev, total, "capture_unavailable") == 0);
    assert(countEvent(ev, total, "rule_trigger") == 0);
    g_fakeNow = 2000;
    total += pumpOnce(&s, ev + total, 8 - total);
    assert(countEvent(ev, total, "scan") == 4);
    assert(countEvent(ev, total, "rule_trigger") == 0);
}

// --- CSV identity and RFC4180 escaping: group columns + joined text. -----

static void test_csv_identity_and_escape(void) {
    OcrSession s;
    setupSession(&s, FALSE); // every frame is OCR-read on purpose
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_APPEAR, "b", 2, 50);

    static const struct Frame frames[] = {
        {"init", 10, TRUE},       // seed: armed ABSENT
        {"a,b\"c", 20, TRUE},     // pump 1: match, run = 1
        {"a,b\"c", 30, TRUE},     // pump 2: run = 2 -> fires
    };
    framesFor(0, frames, 3);

    char path[1024], err[MSG_BUFSIZE];
    s.timeline = ocrTimelineOpen("build/tests/tl-escape", path,
                                 sizeof(path), err);
    assert(s.timeline != NULL);
    assert(ocrSessionSeed(&s, err));

    OcrSessionEvent ev[8];
    int total = 0;
    g_fakeNow += 1000;
    total += pumpOnce(&s, ev + total, 8 - total);
    g_fakeNow += 1000;
    total += pumpOnce(&s, ev + total, 8 - total);
    // Two scans plus the firing pump's trigger/loss pair.
    assert(countEvent(ev, total, "scan") == 2);
    assert(total == 4);
    {
        int trig = findEvent(ev, total, "rule_trigger");
        assert(ev[trig].text && strcmp(ev[trig].text, "a,b\"c") == 0);
    }
    ocrTimelineClose(s.timeline);

    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *csv = malloc((size_t)len + 1);
    assert(csv && fread(csv, 1, (size_t)len, f) == (size_t)len);
    csv[len] = '\0';
    fclose(f);
    assert(strstr(csv, "rule_trigger,group 1,\"a,b\"\"c\",,appear,50,"
                       "group:1,") != NULL);
    assert(strstr(csv, "loss_change,group 1,,,appear,50,group:1,")
           != NULL);
    // Every pump additionally writes a per-region scan row: escaped
    // text in ocr_text, source region:1, outcome in note.
    assert(strstr(csv, "scan,region 1,\"a,b\"\"c\",,,,region:1,ok")
           != NULL);
    free(csv);
}

// --- (l) every referenced region emits one scan event per pump with the --
//     exact outcome: capture_fail / blank_frame / read_fail / ok, plus a
//     change-detection unchanged tick; a zero cap drops events silently. -

static void test_scan_outcomes_and_cap(void) {
    OcrSession s;
    setupSession(&s, FALSE); // every frame reaches capture/OCR gates
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE, FALSE}, // seed
        {"",    0, FALSE, FALSE}, // pump 1: capture failed
        {"",    0, TRUE,  TRUE},  // pump 2: black/blank frame
        {"重连", 20, TRUE, FALSE}, // pump 3: OCR read fails
        {"其他", 30, TRUE, FALSE}, // pump 4: read ok, no trigger yet
    };
    framesFor(0, frames, 5);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));

    // Event text aliases per-pump scratch (s->evText, reset every pump),
    // so snapshot each tick's scan status/text before the next pump; only
    // the static status strings and integer fields stay valid afterwards.
    const char *scanStatus[4];
    char scanText[4][OCR_EVENT_TEXT];
    static const char *const want[4] = {
        "capture_fail", "blank_frame", "read_fail", "ok"
    };
    for (int i = 0; i < 4; i++) {
        OcrSessionEvent tick[16];
        int got;
        g_reg[0].failRead = (i == 2);
        g_fakeNow = (UINT64)(1000 * (i + 1));
        got = pumpOnce(&s, tick, 16);

        int si = findEvent(tick, got, "scan");
        assert(si >= 0);
        assert(tick[si].regionIndex == 0);
        assert(tick[si].groupIndex == -1);
        scanStatus[i] = tick[si].status;
        strncpy(scanText[i], tick[si].text ? tick[si].text : "",
                OCR_EVENT_TEXT - 1);
        scanText[i][OCR_EVENT_TEXT - 1] = '\0';
    }
    g_reg[0].failRead = FALSE;
    for (int k = 0; k < 4; k++) {
        assert(strcmp(scanStatus[k], want[k]) == 0);
        if (k == 3) {
            assert(strcmp(scanText[k], "其他") == 0);
        } else {
            // Failed scans carry no OCR text into the log/CSV.
            assert(scanText[k][0] == '\0');
        }
    }

    // A full event cap drops the scan silently instead of overflowing.
    {
        OcrSessionEvent none[1];
        int n = 0;
        assert(ocrSessionPump(&s, none, &n, 0));
        assert(n == 0);
    }

    // Change detection ON: the same picture again logs "unchanged" and
    // replays the cached text without running OCR.
    {
        OcrSession s2;
        setupSession(&s2, TRUE);
        setRegion(&s2, 0, 0.0, 0.5);
        addSingleGroup(&s2, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);
        static const struct Frame same[] = {
            {"仓库", 10, TRUE, FALSE},
            {"仓库", 10, TRUE, FALSE},
        };
        framesFor(0, same, 2);
        assert(ocrSessionSeed(&s2, err));
        assert(g_reg[0].reads == 1);
        OcrSessionEvent ev2[4];
        g_fakeNow = 20000;
        assert(pumpOnce(&s2, ev2, 4) == 1);
        assert(strcmp(ev2[0].event, "scan") == 0);
        assert(strcmp(ev2[0].status, "unchanged") == 0);
        assert(ev2[0].text && strcmp(ev2[0].text, "仓库") == 0);
        assert(g_reg[0].reads == 1);
    }
}

// --- (m) scan rows in the CSV timeline: region identity, text, source, --
//     outcome note; failed scans leave ocr_text empty.                   --

static void test_scan_timeline_rows(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_APPEAR, "重连", 2, 50);

    static const struct Frame frames[] = {
        {"init", 10, TRUE, FALSE}, // seed: armed ABSENT
        {"仓库", 20, TRUE, FALSE}, // pump 1: ok, absent stays steady
        {"",    0, FALSE, FALSE}, // pump 2: capture failed
    };
    framesFor(0, frames, 3);

    char path[1024], err[MSG_BUFSIZE];
    s.timeline = ocrTimelineOpen("build/tests/tl-scan", path,
                                 sizeof(path), err);
    assert(s.timeline != NULL);
    assert(ocrSessionSeed(&s, err));

    OcrSessionEvent ev[8];
    g_fakeNow = 1000;
    assert(pumpOnce(&s, ev, 8) >= 1);
    g_fakeNow = 2000;
    assert(pumpOnce(&s, ev, 8) >= 1);
    ocrTimelineClose(s.timeline);

    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *csv = malloc((size_t)len + 1);
    assert(csv && fread(csv, 1, (size_t)len, f) == (size_t)len);
    csv[len] = '\0';
    fclose(f);
    assert(strstr(csv, "scan,region 1,仓库,,,,region:1,ok") != NULL);
    assert(strstr(csv, "scan,region 1,,,,,region:1,capture_fail") != NULL);
    free(csv);
}

// --- worker: events go through the queue to a main-thread drain ----------

static OcrSessionEvent g_collected[32];
static int g_collectedCount = 0;
static void collectSink(const OcrSessionEvent *ev) {
    assert(g_collectedCount < 32);
    g_collected[g_collectedCount++] = *ev;
}

static void test_worker_queue(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.nowMsFn = NULL; // worker uses the wall clock
    s.intervalMs = 200;
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_APPEAR, "重连", 2, 50);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE},    // seed: keyword absent -> armed ABSENT
        {"重连", 10, TRUE},
        {"重连", 10, TRUE},
        {"重连", 10, TRUE},
    };
    framesFor(0, frames, 4);

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));

    assert(ocrWorkerStart(&s, collectSink, err));
    Sleep(900); // ticks at ~200/400/600 ms: trigger lands by 400 ms
    ocrWorkerStop();
    assert(!ocrWorkerBusy());

    ocrWorkerDrain(collectSink);
    int trigger = countEvent(g_collected, g_collectedCount, "rule_trigger");
    int change = countEvent(g_collected, g_collectedCount, "loss_change");
    int scans = countEvent(g_collected, g_collectedCount, "scan");
    assert(trigger == 1);
    assert(change == 1);
    assert(scans >= 3); // one scan event per worker tick
    int found = 0;
    for (int i = 0; i < g_collectedCount; i++)
        if (strcmp(g_collected[i].event, "rule_trigger") == 0)
            found = g_collected[i].lossPercent;
    assert(found == 50);
}

// --- worker: slow pump shows the busy flag and skips the missed beats -----

static void test_worker_busy_skips_beats(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.nowMsFn = NULL;
    s.intervalMs = 100;
    setRegion(&s, 0, 0.0, 0.5);
    addSingleGroup(&s, OCR_EDGE_DISAPPEAR, "仓库", 2, 95);
    static const struct Frame frames[] = {{"仓库", 10, TRUE}};
    framesFor(0, frames, 1);

    g_entered = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    g_release = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_blockedEntries = 0;
    g_blockCapture = TRUE;

    char err[MSG_BUFSIZE];
    assert(ocrWorkerStart(&s, collectSink, err));

    // First tick (~100 ms) enters the blocked fake capture.
    DWORD wr = WaitForSingleObject(g_entered, 5000);
    assert(wr == WAIT_OBJECT_0);
    assert(ocrWorkerBusy());

    // Hold one pump across ~7 missed 100 ms beats.
    Sleep(700);
    assert(g_blockedEntries == 1);
    SetEvent(g_release);

    // Schedule resyncs to the next future beat: exactly one more entry,
    // not the 7+ a naive fixed-sleep loop would produce.
    wr = WaitForSingleObject(g_entered, 5000);
    assert(wr == WAIT_OBJECT_0);
    assert(g_blockedEntries == 2);
    SetEvent(g_release);

    ocrWorkerStop(); // releases/joins before a third beat can enter
    assert(g_blockedEntries <= 3);
    assert(!ocrWorkerBusy());

    CloseHandle(g_entered);
    CloseHandle(g_release);
    g_blockCapture = FALSE;
}

int main(void) {
    test_disappear_trigger();                 // (a)
    test_two_groups_first_gear();             // (b) + CSV arbitration
    test_unavailable_holds_run();             // (c)
    test_change_detection_gate();             // (d)
    test_unarmed_seed_fail_closed();          // (f)
    test_worker_queue();
    test_worker_busy_skips_beats();           // (e)
    test_change_gate_reuses_last_text_disappear(); // (h)
    test_change_gate_reuses_last_text_appear();     // (i)
    test_blank_frame_is_unavailable();        // (j)
    test_target_identity_gate();              // (k)
    test_and_group_truth_table();
    test_union_capture_only_referenced();
    test_lenient_seed();
    test_csv_identity_and_escape();
    test_scan_outcomes_and_cap();           // (l)
    test_scan_timeline_rows();              // (m)

    printf("OK: ocr_session\n");
    return 0;
}
