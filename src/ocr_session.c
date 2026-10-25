#include <winsock2.h>
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "ocr_session.h"

#define OCR_READ_TEXT 1024

static UINT64 sessionNow(const OcrSession *s) {
    return s->nowMsFn ? s->nowMsFn() : (UINT64)GetTickCount64();
}

// Fake sentinel HWNDs used by unit tests fail GetClientRect; map their
// norms onto a virtual 1000x1000 client so capture fakes still receive
// distinct per-region rectangles. A production HWND always succeeds.
static RECT regionRect(const OcrSession *s, const OcrRegion *rgn) {
    RECT client;
    if (GetClientRect(s->target.hwnd, &client)
        && client.right > 0 && client.bottom > 0) {
        return captureMapNorm(client.right, client.bottom, &rgn->rule.norm);
    }
    return captureMapNorm(1000, 1000, &rgn->rule.norm);
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

static void appendEvent(OcrSessionEvent *ev, int *n, int cap, int region,
                        int lossPercent, const char *name, const char *text) {
    if (*n >= cap) return;
    ev[*n].regionIndex = region;
    ev[*n].lossPercent = lossPercent;
    ev[*n].event = name;
    ev[*n].text = text;
    (*n)++;
}

static char *claimTextSlot(OcrSession *s, const char *text) {
    if (s->evTextNext >= OCR_MAX_REGIONS) return NULL;
    char *slot = s->evText[s->evTextNext++];
    strncpy(slot, text ? text : "", OCR_EVENT_TEXT - 1);
    slot[OCR_EVENT_TEXT - 1] = '\0';
    return slot;
}

static void writeTimeline(OcrSession *s, const char *event, int region,
                          const char *text, const OcrRule *rule,
                          int lossPercent) {
    if (!s->timeline) return;

    char label[32];
    if (rule->name[0]) snprintf(label, sizeof(label), "%s", rule->name);
    else snprintf(label, sizeof(label), "r%d", region);

    // spec §5: rule rows are attributable to the region that fired them;
    // unnamed regions fall back to rN. Label lives in source, note stays
    // free for operator annotations.
    char source[40];
    snprintf(source, sizeof(source), "rule:%s", label);

    OcrTimelineEvent t;
    memset(&t, 0, sizeof(t));
    t.timeMs = sessionNow(s);
    t.event = event;
    t.region = label;
    t.ocrText = text;
    t.keyword = NULL;
    t.edge = rule->edge == OCR_EDGE_APPEAR ? "appear" : "disappear";
    t.lossPercent = lossPercent;
    t.source = source;
    t.note = NULL;
    (void)ocrTimelineEvent(s->timeline, &t);
}

static void emitTrigger(OcrSession *s, int region, OcrRegion *rgn,
                        const char *rawText, int target,
                        OcrSessionEvent *ev, int *n, int cap) {
    char *slot = claimTextSlot(s, rawText);
    appendEvent(ev, n, cap, region, target, "rule_trigger", slot);
    writeTimeline(s, "rule_trigger", region, slot, &rgn->rule, target);
    appendEvent(ev, n, cap, region, target, "loss_change", NULL);
    writeTimeline(s, "loss_change", region, NULL, &rgn->rule, target);
}

static void emitUnavailable(OcrSession *s, int region, OcrRegion *rgn,
                            OcrSessionEvent *ev, int *n, int cap) {
    UINT64 now = sessionNow(s);
    if (rgn->lastUnavailableMs != 0
        && now - rgn->lastUnavailableMs < OCR_UNAVAILABLE_QUIET_MS) {
        return;
    }
    rgn->lastUnavailableMs = now;
    appendEvent(ev, n, cap, region, -1, "capture_unavailable", NULL);
    writeTimeline(s, "capture_unavailable", region, NULL, &rgn->rule, -1);
}

static BOOL sessionValid(const OcrSession *s) {
    return s && s->regions && s->regionCount > 0
        && s->regionCount <= OCR_MAX_REGIONS
        && s->captureFn && s->readFn;
}

// Per-tick identity gate before any capture: the HWND must still be the
// live target. A dead/recycled/minimized tick emits the rate-limited
// unavailable event for every region, triggers nothing, captures nothing.
static BOOL targetAlive(const OcrSession *s) {
    return !s->targetAliveFn || s->targetAliveFn(&s->target);
}

// One capture+OCR attempt on a region.
#define REGION_UNAVAILABLE 0 // capture or OCR failed
#define REGION_READ        1 // *text holds fresh OCR text
#define REGION_UNCHANGED  -1 // change detection skipped OCR this tick
static int readRegion(OcrSession *s, OcrRegion *rgn, char *text,
                      size_t textCap) {
    RECT rc = regionRect(s, rgn);
    unsigned char *bgra = NULL;
    int w = 0, h = 0;
    char localErr[MSG_BUFSIZE];
    if (!s->captureFn(s->target.hwnd, &rc, &bgra, &w, &h, localErr)) {
        return REGION_UNAVAILABLE;
    }
    // PrintWindow success with a black/no-picture frame (locked desktop,
    // exclusive fullscreen) is not empty OCR text: treat it as
    // unavailable so no armed rule can fire on a frame we never got.
    if (captureFrameBlank(bgra, w, h)) {
        captureFreeBitmap(bgra);
        return REGION_UNAVAILABLE;
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
    return ok ? REGION_READ : REGION_UNAVAILABLE;
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

    int armed = 0;
    for (size_t i = 0; i < s->regionCount; i++) {
        OcrRegion *rgn = &s->regions[i];
        ocrRuleStateInit(&rgn->state);
        rgn->hasThumb = FALSE;
        rgn->lastUnavailableMs = 0;

        char text[OCR_READ_TEXT];
        RECT rc = regionRect(s, rgn);
        unsigned char *bgra = NULL;
        int w = 0, h = 0;
        char localErr[MSG_BUFSIZE];
        if (!s->captureFn(s->target.hwnd, &rc, &bgra, &w, &h, localErr)
            || captureFrameBlank(bgra, w, h)) {
            captureFreeBitmap(bgra); // NULL-safe; blank frame dropped
            continue;
        }
        BOOL readOk = s->readFn(s->engine, bgra, w, h, text, sizeof(text),
                                localErr);
        if (readOk) {
            unsigned char gray[OCR_THUMB_W * OCR_THUMB_MAX_H];
            int tw = 0, th = 0;
            makeThumb(bgra, w, h, gray, &tw, &th);
            memcpy(rgn->thumb, gray, (size_t)tw * th);
            rgn->thumbW = tw;
            rgn->thumbH = th;
            rgn->hasThumb = TRUE;
            BOOL present = ocrRuleMatches(&rgn->rule, text);
            ocrRuleArm(&rgn->state, present);
            strncpy(rgn->lastText, text, OCR_EVENT_TEXT - 1);
            rgn->lastText[OCR_EVENT_TEXT - 1] = '\0';
            rgn->hasLastText = TRUE;
            armed++;
        }
        captureFreeBitmap(bgra);
    }

    if (armed == 0) {
        if (err) {
            strncpy(err, "no OCR region could be read at start",
                    MSG_BUFSIZE - 1);
        }
        return FALSE;
    }
    return TRUE;
}

BOOL ocrSessionPump(OcrSession *s, OcrSessionEvent *ev, int *evCount,
                    int evCap) {
    if (!sessionValid(s) || !ev || !evCount || *evCount > evCap) return FALSE;

    s->evTextNext = 0;
    if (!targetAlive(s)) {
        for (size_t i = 0; i < s->regionCount; i++) {
            emitUnavailable(s, (int)i, &s->regions[i], ev, evCount, evCap);
        }
        return TRUE;
    }
    for (size_t i = 0; i < s->regionCount; i++) {
        OcrRegion *rgn = &s->regions[i];
        char text[OCR_READ_TEXT];
        int got = readRegion(s, rgn, text, sizeof(text));
        if (got == REGION_UNAVAILABLE) {
            emitUnavailable(s, (int)i, rgn, ev, evCount, evCap);
            continue;
        }

        // spec §5/§6.4: below the change threshold OCR is skipped and the
        // previous text is reused AS this period's observation, so steady
        // frames still advance confirm runs. No cached text yet (seed read
        // failed and no pump has succeeded) -> wait for a real frame.
        const char *observed;
        if (got == REGION_UNCHANGED) {
            if (!rgn->hasLastText) continue;
            observed = rgn->lastText;
        } else {
            strncpy(rgn->lastText, text, OCR_EVENT_TEXT - 1);
            rgn->lastText[OCR_EVENT_TEXT - 1] = '\0';
            rgn->hasLastText = TRUE;
            observed = rgn->lastText;
        }

        OcrPoll poll = ocrRuleMatches(&rgn->rule, observed)
                       ? OCR_POLL_MATCHED : OCR_POLL_MISSED;

        // Seed failed for this region: the first successful read arms it
        // with what is observed right now, so nothing can pre-trigger.
        if (!rgn->state.armed) ocrRuleArm(&rgn->state, poll == OCR_POLL_MATCHED);

        int target = -1;
        if (ocrRuleStep(&rgn->state, &rgn->rule, poll, &target)) {
            emitTrigger(s, (int)i, rgn, observed, target, ev, evCount, evCap);
        }
    }
    return TRUE;
}
