#ifndef CLUMSY_OCR_SESSION_H
#define CLUMSY_OCR_SESSION_H

#include <windows.h>

#include "common.h"
#include "ocr_rules.h"
#include "ocr_capture.h"
#include "ocr_engine.h"
#include "ocr_timeline.h"

/*
 * Pure OCR session orchestration. One session drives N independent
 * regions/rules against one target window: a synchronous initial scan
 * arms every region, then repeated pumps capture, change-check, OCR and
 * step the rule state machines. Capture, OCR and the clock are injected
 * so the whole layer is testable without a window, a model or threads.
 */

#define OCR_MAX_REGIONS       16
#define OCR_THUMB_W           32
#define OCR_THUMB_MAX_H       32
#define OCR_EVENT_TEXT        256
#define OCR_UNAVAILABLE_QUIET_MS 5000

typedef struct {
    OcrRule       rule;
    OcrRuleState  state;
    unsigned char thumb[OCR_THUMB_W * OCR_THUMB_MAX_H];
    int           thumbW;
    int           thumbH;
    BOOL          hasThumb;
    char          lastText[OCR_EVENT_TEXT]; // reused when the change gate
    BOOL          hasLastText;              // skips OCR (spec §5/§6.4)
    UINT64        lastUnavailableMs; // rate limit for unavailable events
} OcrRegion;

typedef struct {
    int      intervalMs;    // poll period; worker clamps to >=200
    BOOL     changeDetect;
    int      madThreshold;
    CaptureWindow target;    // .hwnd drives captureFn
    OcrTimeline *timeline;   // NULL: timeline writes skipped
    size_t   regionCount;
    OcrRegion *regions;

    // Injected points (tests pass fakes; production passes real ones).
    BOOL (*captureFn)(HWND, const RECT *, unsigned char **, int *, int *,
                      char[MSG_BUFSIZE]);
    BOOL (*readFn)(OcrEngine *, const unsigned char *, int, int, char *,
                   size_t, char[MSG_BUFSIZE]);
    OcrEngine *engine;
    UINT64 (*nowMsFn)(void);  // NULL: GetTickCount64()

    // Per-pump storage backing OcrSessionEvent.text (one slot per region
    // trigger; reset at the start of every pump).
    char evText[OCR_MAX_REGIONS][OCR_EVENT_TEXT];
    int  evTextNext;
} OcrSession;

typedef struct {
    int         regionIndex;
    int         lossPercent; // -1 when the event changes no gear
    const char *event;       // static name: rule_trigger / loss_change /
                             // capture_unavailable
    const char *text;        // OCR text for rule_trigger, else NULL
} OcrSessionEvent;

// Synchronous per-region initial scan. Successfully read regions arm with
// the observed polarity; failed regions stay unarmed and get armed by the
// first successful pump (nothing can trigger before that). Returns FALSE
// (with err) when no region could be read, so Start can refuse safely.
BOOL ocrSessionSeed(OcrSession *s, char err[MSG_BUFSIZE]);

// One scan of every region. Events are appended to ev starting at
// *evCount, capped at evCap; *evCount is advanced. Capture/read failures
// become rate-limited capture_unavailable events, never triggers.
BOOL ocrSessionPump(OcrSession *s, OcrSessionEvent *ev, int *evCount,
                    int evCap);

#endif // CLUMSY_OCR_SESSION_H
