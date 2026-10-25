// Unit tests for OCR session orchestration: seeded arming, late arming
// after unavailable startup, the confirm-N edge path through capture/read
// fakes, change-detection read skipping, unavailable rate limiting, event
// ordering by region index, and the single worker thread's queue, busy
// flag and missed-beat skipping.
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
};

static struct RegFake g_reg[2];
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
    // (real GetClientRect only fails on the fake sentinel HWNDs).
    int ri = (rc->right >= 500 && rc->left >= 500) ? 1 : 0;
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
    const char *text = fi < f->n ? f->frames[fi].text : "";
    strncpy(out, text, cap - 1);
    out[cap - 1] = '\0';
    return TRUE;
}

static OcrRule makeRule(OcrEdge edge, const char *kw, int frames, int pct) {
    OcrRule r;
    memset(&r, 0, sizeof(r));
    strncpy(r.keywords, kw, sizeof(r.keywords) - 1);
    r.edge = edge;
    r.confirmFrames = frames;
    r.lossPercent = pct;
    return r;
}

static OcrRegion g_regions[2];

static void setupSession(OcrSession *s, BOOL changeDetect) {
    memset(s, 0, sizeof(*s));
    memset(g_regions, 0, sizeof(g_regions));
    s->regions = g_regions;
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
    memset(g_reg, 0, sizeof(g_reg));
    g_fakeNow = 0;
    g_targetAlive = TRUE;
}

static void setRegion(OcrSession *s, int i, OcrRule rule,
                      double x, double wfrac) {
    s->regions[i].rule = rule;
    ocrRuleStateInit(&s->regions[i].state);
    s->regions[i].rule.norm.x = x;
    s->regions[i].rule.norm.y = 0.0;
    s->regions[i].rule.norm.w = wfrac;
    s->regions[i].rule.norm.h = 0.1;
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

// --- (a) seed PRESENT, two empty frames confirm a DISAPPEAR at 95% -------

static void test_disappear_trigger(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE}, {"仓库", 10, TRUE}, {"仓库", 10, TRUE},
        {"", 20, TRUE}, {"", 20, TRUE},
    };
    g_reg[0].frames = frames;
    g_reg[0].n = (int)(sizeof(frames) / sizeof(frames[0]));

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(s.regions[0].state.armed);
    assert(s.regions[0].state.stable == OCR_STABLE_PRESENT);

    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 4; i++) {
        g_fakeNow += 1000;
        int n = pumpOnce(&s, ev + total, (int)(16 - total));
        total += n;
    }
    // Only the 4th pump fires; trigger produces two event rows.
    assert(total == 2);
    assert(strcmp(ev[0].event, "rule_trigger") == 0);
    assert(ev[0].regionIndex == 0);
    assert(ev[0].lossPercent == 95);
    assert(strcmp(ev[1].event, "loss_change") == 0);
    assert(ev[1].lossPercent == 95);
}

// --- (b) two regions firing the same pump: events ordered by region index -

static void test_two_region_order(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.regionCount = 2;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);
    setRegion(&s, 1, makeRule(OCR_EDGE_APPEAR, "重连", 2, 50), 0.5, 0.5);

    static const struct Frame f0[] = {
        {"仓库", 10, TRUE}, {"仓库", 10, TRUE}, {"仓库", 10, TRUE},
        {"", 10, TRUE}, {"", 10, TRUE},
    };
    static const struct Frame f1[] = {
        {"", 10, TRUE}, {"", 10, TRUE}, {"", 10, TRUE},
        {"重连", 10, TRUE}, {"重连", 10, TRUE},
    };
    g_reg[0].frames = f0; g_reg[0].n = 5;
    g_reg[1].frames = f1; g_reg[1].n = 5;

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(s.regions[1].state.stable == OCR_STABLE_ABSENT);

    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 4; i++) {
        g_fakeNow += 1000;
        total += pumpOnce(&s, ev + total, (int)(16 - total));
    }
    assert(total == 4);
    // Region 0 (index order first) at 95, then region 1 at 50.
    assert(ev[0].regionIndex == 0 && ev[0].lossPercent == 95);
    assert(strcmp(ev[0].event, "rule_trigger") == 0);
    assert(ev[1].regionIndex == 0 && ev[1].lossPercent == 95);
    assert(strcmp(ev[1].event, "loss_change") == 0);
    assert(ev[2].regionIndex == 1 && ev[2].lossPercent == 50);
    assert(strcmp(ev[2].event, "rule_trigger") == 0);
    assert(ev[3].regionIndex == 1 && ev[3].lossPercent == 50);
    assert(strcmp(ev[3].event, "loss_change") == 0);
}

// --- (c) UNAVAILABLE neither triggers nor resets the confirm run; the ---
//     unavailable event is rate-limited to one per 5 s per region.       --

static void test_unavailable_holds_run(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE},  // seed
        {"", 10, TRUE},      // pump 1: run = 1
        {"", 10, FALSE},     // pump 2: unavailable, event #1
        {"", 10, FALSE},     // pump 3: unavailable, suppressed (<5s)
        {"", 10, TRUE},      // pump 4: run = 2 -> fires
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 5;

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
    int trig = -1;
    for (int i = 0; i < total; i++)
        if (strcmp(ev[i].event, "rule_trigger") == 0) trig = i;
    assert(trig == total - 2); // trigger is the last pump
}

// --- (f) seed fully unavailable: fail closed until the first success, ----
//     then the normal state machine takes over.                           --

static void test_unarmed_seed_fail_closed(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"", 10, FALSE},     // seed capture fails: stays unarmed
        {"", 10, TRUE},      // pump 1: first success late-arms ABSENT
        {"", 10, TRUE},      // pump 2: steady absent, still no trigger
        {"仓库", 10, TRUE},  // pump 3: safe poll settles PRESENT
        {"", 10, TRUE},      // pump 4: run = 1
        {"", 10, TRUE},      // pump 5: run = 2 -> fires once
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 6;

    char err[MSG_BUFSIZE];
    assert(!ocrSessionSeed(&s, err)); // nothing armed -> Start must refuse
    assert(!s.regions[0].state.armed);

    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 5; i++) {
        g_fakeNow += 1000;
        total += pumpOnce(&s, ev + total, (int)(16 - total));
        if (i < 3) assert(total == 0); // pumps 1..3 emit nothing
    }
    assert(total == 2);
    assert(strcmp(ev[0].event, "rule_trigger") == 0);
    assert(ev[0].lossPercent == 95);
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
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);
    g_reg[0].frames = frames;
    g_reg[0].n = 3;
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
    s2.regionCount = 1;
    setRegion(&s2, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);
    g_reg[0].frames = frames; // setupSession reset the fake state
    g_reg[0].n = 3;
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
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE}, // seed: armed PRESENT
        {"仓库", 10, TRUE}, // pump 1: identical -> reuse "仓库", steady
        {"", 20, TRUE},     // pump 2: changed -> OCR miss, run = 1
        {"", 20, TRUE},     // pump 3: identical blank -> reuse "", run = 2
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 4;

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));
    assert(g_reg[0].reads == 1);

    OcrSessionEvent ev[8];
    g_fakeNow += 1000;
    assert(pumpOnce(&s, ev, 8) == 0);
    assert(g_reg[0].reads == 1); // unchanged frame did not run OCR

    g_fakeNow += 1000;
    assert(pumpOnce(&s, ev, 8) == 0);
    assert(g_reg[0].reads == 2); // changed frame ran OCR, run = 1 only

    g_fakeNow += 1000;
    int n = pumpOnce(&s, ev, 8);
    assert(n == 2); // reused miss confirms and fires
    assert(g_reg[0].reads == 2); // no extra OCR for the reused poll
    assert(strcmp(ev[0].event, "rule_trigger") == 0);
    assert(ev[0].lossPercent == 95);
    assert(strcmp(ev[1].event, "loss_change") == 0);
    assert(ev[1].lossPercent == 95);
}

// --- (i) the reused text is the one the trigger row carries: an appear ---
//     confirmed by a steady (OCR-skipped) frame still reports the text.    --

static void test_change_gate_reuses_last_text_appear(void) {
    OcrSession s;
    setupSession(&s, TRUE);
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_APPEAR, "重连", 2, 50), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"", 10, TRUE},     // seed: armed ABSENT
        {"重连", 20, TRUE}, // pump 1: changed -> match, run = 1
        {"重连", 20, TRUE}, // pump 2: identical -> reuse, run = 2 -> fires
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 3;

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));

    OcrSessionEvent ev[8];
    g_fakeNow += 1000;
    assert(pumpOnce(&s, ev, 8) == 0);
    assert(g_reg[0].reads == 2);
    g_fakeNow += 1000;
    int n = pumpOnce(&s, ev, 8);
    assert(n == 2);
    assert(g_reg[0].reads == 2); // confirming frame skipped OCR
    assert(strcmp(ev[0].event, "rule_trigger") == 0);
    assert(ev[0].lossPercent == 50);
    assert(ev[0].text && strcmp(ev[0].text, "重连") == 0);
}

// --- (j) a captured-but-black frame (locked desktop, exclusive fullscreen)
//     is a capture failure, never an empty OCR text: an armed disappear
//     rule neither confirms nor triggers on it; when the real picture
//     returns, the state machine resumes where it stood.                  --

static void test_blank_frame_is_unavailable(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE, FALSE}, // seed: armed PRESENT
        {"",    0, TRUE, TRUE},    // pump 1: black frame
        {"",    0, TRUE, TRUE},    // pump 2: black, event rate-limited
        {"",    0, TRUE, TRUE},    // pump 3: black past the 5 s window
        {"",   20, TRUE, FALSE},   // pump 4: real empty frame, run = 1
        {"",   20, TRUE, FALSE},   // pump 5: run = 2 -> fires
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 6;

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

    g_fakeNow = 8000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 9000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(countEvent(ev, total, "rule_trigger") == 1);
    assert(countEvent(ev, total, "loss_change") == 1);
    int trig = -1;
    for (int i = 0; i < total; i++)
        if (strcmp(ev[i].event, "rule_trigger") == 0) trig = i;
    assert(trig == total - 2);
}

// --- (k) per-tick identity gate: a HWND that died and got recycled to ---
//     another process is never captured or OCR'd - every region reports
//     unavailable, no capture happens, no rule can fire. Seed refuses.  --

static void test_target_identity_gate(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE, FALSE}, // seed (capture #0)
        {"",   20, TRUE, FALSE},    // pump when alive again: run = 1
        {"",   20, TRUE, FALSE},    // run = 2 -> fires
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 3;

    // Seed refuses while the target is dead/recycled (fail closed).
    char err[MSG_BUFSIZE];
    g_targetAlive = FALSE;
    assert(!ocrSessionSeed(&s, err));
    assert(err[0] != '\0');
    assert(g_reg[0].captures == 0);

    // Seed succeeds once it is the target again.
    g_targetAlive = TRUE;
    assert(ocrSessionSeed(&s, err));

    // Dead ticks: no capture, no trigger, rate-limited unavailable rows.
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
    assert(countEvent(ev, total, "capture_unavailable") == 2);
    assert(countEvent(ev, total, "rule_trigger") == 0);

    // Back to the real target: state machine resumes.
    g_targetAlive = TRUE;
    g_fakeNow = 8000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    g_fakeNow = 9000;
    total += pumpOnce(&s, ev + total, (int)(16 - total));
    assert(g_reg[0].captures == 3);
    assert(countEvent(ev, total, "rule_trigger") == 1);
    assert(countEvent(ev, total, "loss_change") == 1);
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
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_APPEAR, "重连", 2, 50), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE},    // seed: keyword absent -> armed ABSENT
        {"重连", 10, TRUE},
        {"重连", 10, TRUE},
        {"重连", 10, TRUE},
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 4;

    char err[MSG_BUFSIZE];
    assert(ocrSessionSeed(&s, err));

    assert(ocrWorkerStart(&s, collectSink, err));
    Sleep(900); // ticks at ~200/400/600 ms: trigger lands by 400 ms
    ocrWorkerStop();
    assert(!ocrWorkerBusy());

    ocrWorkerDrain(collectSink);
    int trigger = countEvent(g_collected, g_collectedCount, "rule_trigger");
    int change = countEvent(g_collected, g_collectedCount, "loss_change");
    assert(trigger == 1);
    assert(change == 1);
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
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);
    static const struct Frame frames[] = {{"仓库", 10, TRUE}};
    g_reg[0].frames = frames;
    g_reg[0].n = 1;

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

// --- (g) spec §5: trigger rows written to the CSV carry source ---------
//         "rule:<region name>" (unnamed -> rule:rN), not a generic tag. --

static void test_timeline_rule_source(void) {
    OcrSession s;
    setupSession(&s, FALSE);
    s.regionCount = 1;
    setRegion(&s, 0, makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95), 0.0, 0.5);

    static const struct Frame frames[] = {
        {"仓库", 10, TRUE}, {"仓库", 10, TRUE}, {"仓库", 10, TRUE},
        {"", 20, TRUE}, {"", 20, TRUE},
    };
    g_reg[0].frames = frames;
    g_reg[0].n = 5;

    char path[1024], err[MSG_BUFSIZE];
    char *csv = NULL;
    long len = 0;
    FILE *f;
    s.timeline = ocrTimelineOpen("build/tests/tl-source", path,
                                 sizeof(path), err);
    assert(s.timeline != NULL);

    assert(ocrSessionSeed(&s, err));
    OcrSessionEvent ev[16];
    int total = 0;
    for (int i = 0; i < 4; i++) {
        g_fakeNow += 1000;
        total += pumpOnce(&s, ev + total, (int)(16 - total));
    }
    assert(total == 2);
    ocrTimelineClose(s.timeline);

    f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    len = ftell(f);
    assert(len > 0 && len < 1 << 20);
    rewind(f);
    csv = malloc((size_t)len + 1);
    assert(csv != NULL);
    assert(fread(csv, 1, (size_t)len, f) == (size_t)len);
    csv[len] = '\0';
    fclose(f);

    // rule_trigger and loss_change rows both tag the rule by name; the
    // unnamed test rule falls back to r0, and the edge column is set.
    // Columns: event,region=r0,ocr_text="",keyword="",edge,loss,source,note
    assert(strstr(csv, "rule_trigger,r0,,,disappear,95,rule:r0,") != NULL);
    assert(strstr(csv, "loss_change,r0,,,disappear,95,rule:r0,") != NULL);
    free(csv);
}

int main(void) {
    test_disappear_trigger();      // (a)
    test_two_region_order();       // (b)
    test_unavailable_holds_run();  // (c)
    test_change_detection_gate();  // (d)
    test_unarmed_seed_fail_closed(); // (f)
    test_worker_queue();
    test_worker_busy_skips_beats(); // (e)
    test_change_gate_reuses_last_text_disappear(); // (h)
    test_change_gate_reuses_last_text_appear();    // (i)
    test_blank_frame_is_unavailable();             // (j)
    test_target_identity_gate();                   // (k)
    test_timeline_rule_source();    // (g)

    printf("OK: ocr_session\n");
    return 0;
}
