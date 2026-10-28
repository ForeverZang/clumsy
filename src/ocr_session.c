#include <winsock2.h>
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ocr_session.h"

#define OCR_READ_TEXT 1024

static UINT64 sessionNow(const OcrSession *s) {
    return s->nowMsFn ? s->nowMsFn() : (UINT64)GetTickCount64();
}

// Fake sentinel HWNDs used by unit tests fail GetClientRect; map their
// norms onto a virtual 1000x1000 client so capture fakes still receive
// distinct per-region rectangles. A production HWND always succeeds.
static RECT regionRect(const OcrSession *s, const NormRect *norm) {
    RECT client;
    if (GetClientRect(s->target.hwnd, &client)
        && client.right > 0 && client.bottom > 0) {
        return captureMapNorm(client.right, client.bottom, norm);
    }
    return captureMapNorm(1000, 1000, norm);
}

// Thumbnail scaled to fit OCR_THUMB_W x OCR_THUMB_MAX_H keeping ratio.
static void makeThumb(const unsigned char *bgra, int w, int h,
                      unsigned char *gray, int *thumbW, int *thumbH) {
    int tw;
    if (h <= w) {
        tw = OCR_THUMB_W;
    } else {
        tw = (int)((double)OCR_THUMB_MAX_H * w / h + 0.5);
        if (tw < 1) tw = 1;
    }
    int gotW = tw;
    int gotH = 1;
    captureMakeThumbnail(bgra, w, h, tw, OCR_THUMB_MAX_H,
                         gray, &gotW, &gotH);
    *thumbW = gotW;
    *thumbH = gotH;
}

static void appendEvent(OcrSessionEvent *ev, int *n, int cap, int group,
                        int lossPercent, const char *name, const char *text) {
    if (*n >= cap) return;
    ev[*n].groupIndex = group;
    ev[*n].regionIndex = -1;
    ev[*n].lossPercent = lossPercent;
    ev[*n].event = name;
    ev[*n].text = text;
    ev[*n].status = NULL;
    (*n)++;
}

static char *claimTextSlot(OcrSession *s, const char *text) {
    if (s->evTextNext >= OCR_MAX_REGIONS + OCR_MAX_GROUPS) return NULL;
    char *slot = s->evText[s->evTextNext++];
    strncpy(slot, text ? text : "", OCR_EVENT_TEXT - 1);
    slot[OCR_EVENT_TEXT - 1] = '\0';
    return slot;
}

// Timeline identity for group-sourced rows: region column "group N",
// source column "group:N" (1-based, table order).
static void writeTimeline(OcrSession *s, const char *event, int gi,
                          const char *text, int lossPercent) {
    if (!s->timeline) return;

    const OcrSessionGroup *sg = &s->groups[gi];
    char label[24];
    char source[32];
    snprintf(label, sizeof(label), "group %d", gi + 1);
    snprintf(source, sizeof(source), "group:%d", gi + 1);

    OcrTimelineEvent t;
    memset(&t, 0, sizeof(t));
    t.timeMs = sessionNow(s);
    t.event = event;
    t.region = label;
    t.ocrText = text;
    t.keyword = NULL;
    t.edge = sg->group.edge == OCR_EDGE_APPEAR ? "appear" : "disappear";
    t.lossPercent = lossPercent;
    t.source = source;
    t.note = NULL;
    (void)ocrTimelineEvent(s->timeline, &t);
}

// Timeline identity for per-region scan rows: region column
// "region N", source column "region:N" (1-based), outcome in note.
static void writeScanTimeline(OcrSession *s, int ri, const char *status,
                              const char *text) {
    if (!s->timeline) return;

    char label[24];
    char source[32];
    snprintf(label, sizeof(label), "region %d", ri + 1);
    snprintf(source, sizeof(source), "region:%d", ri + 1);

    OcrTimelineEvent t;
    memset(&t, 0, sizeof(t));
    t.timeMs = sessionNow(s);
    t.event = "scan";
    t.region = label;
    t.ocrText = (text && text[0]) ? text : NULL;
    t.lossPercent = -1;
    t.source = source;
    t.note = status;
    (void)ocrTimelineEvent(s->timeline, &t);
}

// One per referenced region per pump, emitted BEFORE the group events so
// the log reads "recognition process first, consequences after".
static void emitScan(OcrSession *s, int ri, const char *status,
                     const char *text, OcrSessionEvent *ev,
                     int *n, int cap) {
    writeScanTimeline(s, ri, status, text);
    if (*n >= cap) return;
    char *slot = claimTextSlot(s, text ? text : "");
    ev[*n].groupIndex = -1;
    ev[*n].regionIndex = ri;
    ev[*n].lossPercent = -1;
    ev[*n].event = "scan";
    ev[*n].text = slot != NULL ? slot : "";
    ev[*n].status = status;
    (*n)++;
}

static void emitUnavailable(OcrSession *s, int gi,
                            OcrSessionEvent *ev, int *n, int cap) {
    OcrSessionGroup *sg = &s->groups[gi];
    UINT64 now = sessionNow(s);
    if (sg->lastUnavailableMs != 0
        && now - sg->lastUnavailableMs < OCR_UNAVAILABLE_QUIET_MS) {
        return;
    }
    sg->lastUnavailableMs = now;
    appendEvent(ev, n, cap, gi, -1, "capture_unavailable", NULL);
    writeTimeline(s, "capture_unavailable", gi, NULL, -1);
}

static BOOL sessionValid(const OcrSession *s) {
    return s && s->regions && s->regionCount > 0
        && s->regionCount <= OCR_MAX_REGIONS
        && s->groups && s->groupCount > 0
        && s->groupCount <= OCR_MAX_GROUPS
        && s->captureFn && s->readFn;
}

// Per-tick identity gate before any capture: the HWND must still be the
// live target. A dead/recycled/minimized tick emits the rate-limited
// unavailable event for every group, triggers nothing, captures nothing.
static BOOL targetAlive(const OcrSession *s) {
    return !s->targetAliveFn || s->targetAliveFn(&s->target);
}

// One capture+OCR attempt on one physical region. The distinct failure
// codes feed the per-pump scan event so the log says WHY recognition
// did not produce text.
#define REGION_CAPTURE_FAIL 0 // screen capture itself failed
#define REGION_READ         1 // *text holds fresh OCR text
#define REGION_UNCHANGED   -1 // change detection skipped OCR this tick
#define REGION_BLANK        2 // captured a black/no-picture frame
#define REGION_READ_FAIL    3 // OCR engine reported failure
static int readPhysicalRegion(OcrSession *s, size_t ri, char *text,
                              size_t textCap) {
    OcrSessionRegion *rgn = &s->regions[ri];
    RECT rc = regionRect(s, &rgn->norm);
    unsigned char *bgra = NULL;
    int w = 0, h = 0;
    char localErr[MSG_BUFSIZE];
    if (!s->captureFn(s->target.hwnd, &rc, &bgra, &w, &h, localErr)) {
        return REGION_CAPTURE_FAIL;
    }
    // PrintWindow success with a black/no-picture frame (locked desktop,
    // exclusive fullscreen) is not empty OCR text: classify it as a
    // blank-frame failure so no armed group can fire on a frame we never
    // got, and the scan log can name the cause distinctly.
    if (captureFrameBlank(bgra, w, h)) {
        captureFreeBitmap(bgra);
        return REGION_BLANK;
    }

    unsigned char gray[OCR_THUMB_W * OCR_THUMB_MAX_H];
    int tw = 0, th = 0;
    makeThumb(bgra, w, h, gray, &tw, &th);

    if (s->changeDetect && rgn->hasThumb
        && rgn->thumbW == tw && rgn->thumbH == th
        && !captureChanged(rgn->thumb, gray, tw * th, s->madThreshold)) {
        captureFreeBitmap(bgra);
        return REGION_UNCHANGED;
    }

    memcpy(rgn->thumb, gray, (size_t)tw * th);
    rgn->thumbW = tw;
    rgn->thumbH = th;
    rgn->hasThumb = TRUE;

    BOOL ok = s->readFn(s->engine, bgra, w, h, text, textCap, localErr);
    captureFreeBitmap(bgra);
    return ok ? REGION_READ : REGION_READ_FAIL;
}

// Member OCR texts in member order joined with " | ", truncated to cap-1.
static void joinMemberTexts(const OcrGroup *g, const OcrRegionObs *obs,
                            size_t obsCount, char *out, size_t cap) {
    size_t used = 0;
    if (cap == 0) return;
    out[0] = '\0';
    for (size_t k = 0; k < g->memberCount; k++) {
        int ri = g->members[k].regionIndex;
        const char *text = (ri >= 0 && (size_t)ri < obsCount
                            && obs[ri].available) ? obs[ri].text : "";
        const char *sep = k == 0 ? "" : " | ";
        size_t sepLen = strlen(sep);
        if (used + sepLen < cap) {
            memcpy(out + used, sep, sepLen);
            used += sepLen;
            out[used] = '\0';
        }
        size_t room = cap - 1 - used;
        size_t len = strlen(text);
        if (len > room) len = room;
        memcpy(out + used, text, len);
        used += len;
        out[used] = '\0';
        if (used >= cap - 1) break;
    }
}

// Outcome code -> static scan status name used by event/CSV/log.
static const char *scanStatusName(int code) {
    switch (code) {
    case REGION_READ:        return "ok";
    case REGION_UNCHANGED:   return "unchanged";
    case REGION_BLANK:       return "blank_frame";
    case REGION_READ_FAIL:   return "read_fail";
    case REGION_CAPTURE_FAIL:
    default:                 return "capture_fail";
    }
}

// Read every referenced physical region once, filling this tick's obs
// and per-region outcome codes (-1: unreferenced, no event emitted).
// Unreferenced regions are never captured.
static void readReferencedUnion(OcrSession *s, OcrRegionObs *obs,
                                char readText[OCR_MAX_REGIONS][OCR_READ_TEXT],
                                int codes[OCR_MAX_REGIONS]) {
    for (size_t i = 0; i < s->regionCount; i++) {
        obs[i].available = FALSE;
        obs[i].text = NULL;
        codes[i] = -1;
        if (!s->referenced[i]) continue;

        OcrSessionRegion *rgn = &s->regions[i];
        int got = readPhysicalRegion(s, i, readText[i], OCR_READ_TEXT);
        codes[i] = got;
        if (got != REGION_READ && got != REGION_UNCHANGED) continue;

        // spec §5/§6.4: below the change threshold OCR is skipped and the
        // previous text is reused AS this period's observation. Thumb and
        // text are always stored together, so UNCHANGED implies cache.
        if (got == REGION_UNCHANGED) {
            if (rgn->hasLastText) {
                obs[i].available = TRUE;
                obs[i].text = rgn->lastText;
            }
            continue;
        }

        strncpy(rgn->lastText, readText[i], OCR_EVENT_TEXT - 1);
        rgn->lastText[OCR_EVENT_TEXT - 1] = '\0';
        rgn->hasLastText = TRUE;
        obs[i].available = TRUE;
        obs[i].text = rgn->lastText;
    }
}

BOOL ocrSessionSeed(OcrSession *s, char err[MSG_BUFSIZE]) {
    if (!sessionValid(s)) {
        if (err) strncpy(err, "invalid OCR session", MSG_BUFSIZE - 1);
        return FALSE;
    }
    if (err) err[0] = '\0';

    if (s->targetAliveFn && !s->targetAliveFn(&s->target)) {
        if (err) {
            strncpy(err, "target window is gone or minimized",
                    MSG_BUFSIZE - 1);
        }
        return FALSE;
    }

    for (size_t gi = 0; gi < s->groupCount; gi++) {
        ocrRuleStateInit(&s->groups[gi].state);
        s->groups[gi].lastUnavailableMs = 0;
    }
    for (size_t i = 0; i < s->regionCount; i++) {
        if (!s->referenced[i]) continue;
        s->regions[i].hasThumb = FALSE;
        s->regions[i].hasLastText = FALSE;
    }

    char readText[OCR_MAX_REGIONS][OCR_READ_TEXT];
    OcrRegionObs obs[OCR_MAX_REGIONS];
    memset(obs, 0, sizeof(obs));

    // Lenient seed: capture the referenced union once; a group arms only
    // when ALL its members read, but Start succeeds when >=1 region reads.
    int readable = 0;
    for (size_t i = 0; i < s->regionCount; i++) {
        if (!s->referenced[i]) continue;
        OcrSessionRegion *rgn = &s->regions[i];
        RECT rc = regionRect(s, &rgn->norm);
        unsigned char *bgra = NULL;
        int w = 0, h = 0;
        char localErr[MSG_BUFSIZE];
        if (!s->captureFn(s->target.hwnd, &rc, &bgra, &w, &h, localErr)
            || captureFrameBlank(bgra, w, h)) {
            captureFreeBitmap(bgra); // NULL-safe; blank frame dropped
            continue;
        }
        BOOL readOk = s->readFn(s->engine, bgra, w, h, readText[i],
                                OCR_READ_TEXT, localErr);
        if (readOk) {
            unsigned char gray[OCR_THUMB_W * OCR_THUMB_MAX_H];
            int tw = 0, th = 0;
            makeThumb(bgra, w, h, gray, &tw, &th);
            memcpy(rgn->thumb, gray, (size_t)tw * th);
            rgn->thumbW = tw;
            rgn->thumbH = th;
            rgn->hasThumb = TRUE;
            strncpy(rgn->lastText, readText[i], OCR_EVENT_TEXT - 1);
            rgn->lastText[OCR_EVENT_TEXT - 1] = '\0';
            rgn->hasLastText = TRUE;
            obs[i].available = TRUE;
            obs[i].text = rgn->lastText;
            readable++;
        }
        captureFreeBitmap(bgra);
    }

    if (readable == 0) {
        if (err) {
            strncpy(err, "no OCR region could be read at start",
                    MSG_BUFSIZE - 1);
        }
        return FALSE;
    }

    for (size_t gi = 0; gi < s->groupCount; gi++) {
        OcrSessionGroup *sg = &s->groups[gi];
        OcrPoll poll = ocrGroupPoll(&sg->group, obs, s->regionCount);
        if (poll == OCR_POLL_UNAVAILABLE) {
            continue; // late arm on the first all-readable tick
        }
        ocrRuleArm(&sg->state, poll == OCR_POLL_MATCHED);
    }
    return TRUE;
}

BOOL ocrSessionPump(OcrSession *s, OcrSessionEvent *ev, int *evCount,
                    int evCap) {
    if (!sessionValid(s) || !ev || !evCount || *evCount > evCap) return FALSE;

    s->evTextNext = 0;
    if (!targetAlive(s)) {
        for (size_t gi = 0; gi < s->groupCount; gi++) {
            emitUnavailable(s, (int)gi, ev, evCount, evCap);
        }
        return TRUE;
    }

    char readText[OCR_MAX_REGIONS][OCR_READ_TEXT];
    OcrRegionObs obs[OCR_MAX_REGIONS];
    int scanCodes[OCR_MAX_REGIONS];
    readReferencedUnion(s, obs, readText, scanCodes);

    // One scan line per referenced region before any group consequence,
    // so the log shows every recognition attempt, success or failure.
    // REGION_UNCHANGED is encoded as -1, so gate on the referenced bit,
    // never on the sign of the code.
    for (size_t i = 0; i < s->regionCount; i++) {
        if (!s->referenced[i]) continue;
        emitScan(s, (int)i, scanStatusName(scanCodes[i]),
                 obs[i].available ? obs[i].text : NULL,
                 ev, evCount, evCap);
    }

    int gearGroup = -1; // first firing group in table order wins gear
    for (size_t gi = 0; gi < s->groupCount; gi++) {
        OcrSessionGroup *sg = &s->groups[gi];
        OcrPoll poll = ocrGroupPoll(&sg->group, obs, s->regionCount);

        if (poll == OCR_POLL_UNAVAILABLE) {
            emitUnavailable(s, (int)gi, ev, evCount, evCap);
            continue;
        }
        // Seed missed this group: the first all-readable tick arms it
        // with what is observed right now, so nothing can pre-trigger.
        if (!sg->state.armed) {
            ocrRuleArm(&sg->state, poll == OCR_POLL_MATCHED);
            continue;
        }

        int target = -1;
        if (!ocrEdgeStep(&sg->state, sg->group.edge,
                         sg->group.confirmFrames, sg->group.lossPercent,
                         poll, &target)) {
            continue;
        }

        char joined[OCR_EVENT_TEXT];
        joinMemberTexts(&sg->group, obs, s->regionCount,
                        joined, sizeof(joined));
        char *slot = claimTextSlot(s, joined);
        appendEvent(ev, evCount, evCap, (int)gi, target,
                    "rule_trigger", slot);
        writeTimeline(s, "rule_trigger", (int)gi, slot, target);

        // Same tick multiple groups fire: every group gets its
        // rule_trigger row, only the first gets a loss_change event.
        if (gearGroup < 0) {
            gearGroup = (int)gi;
            appendEvent(ev, evCount, evCap, (int)gi, target,
                        "loss_change", NULL);
            writeTimeline(s, "loss_change", (int)gi, NULL, target);
        }
    }
    return TRUE;
}
