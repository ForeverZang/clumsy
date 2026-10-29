#ifndef CLUMSY_OCR_SESSION_H
#define CLUMSY_OCR_SESSION_H

#include <windows.h>

#include "common.h"
#include "ocr_rules.h"
#include "ocr_capture.h"
#include "ocr_engine.h"
#include "ocr_timeline.h"

/*
 * Pure OCR session orchestration under the region/group model. One
 * session drives G independent rule groups against one target window.
 * Each physical region referenced by at least one group is captured and
 * OCR'd at most once per tick; ocrGroupPoll synthesizes one AND-across-
 * regions poll per group, then the group's edge state machine steps.
 * A synchronous initial scan arms every group whose members are all
 * readable (Start only needs one readable region overall). Capture, OCR
 * and the clock are injected so the whole layer is testable without a
 * window, a model or threads.
 */

#define OCR_THUMB_W           32
#define OCR_THUMB_MAX_H       32
#define OCR_EVENT_TEXT        256
#define OCR_UNAVAILABLE_QUIET_MS 5000

// Runtime state for one physical region (shared by every group that
// references it). Only referenced regions are ever captured.
typedef struct {
    NormRect      norm;
    unsigned char thumb[OCR_THUMB_W * OCR_THUMB_MAX_H];
    int           thumbW;
    int           thumbH;
    BOOL          hasThumb;
    char          lastText[OCR_EVENT_TEXT]; // reused when the change gate
    BOOL          hasLastText;              // skips OCR (spec §5/§6.4)
} OcrSessionRegion;

// One group's definition plus its edge state machine and the per-group
// capture_unavailable rate limit.
typedef struct {
    OcrGroup      group;
    OcrRuleState  state;
    UINT64        lastUnavailableMs;
} OcrSessionGroup;

typedef struct {
    int      intervalMs;    // poll period; worker clamps to >=200
    BOOL     changeDetect;
    int      madThreshold;
    CaptureWindow target;    // .hwnd drives captureFn
    OcrTimeline *timeline;   // NULL: timeline writes skipped

    size_t regionCount;
    OcrSessionRegion *regions;
    size_t groupCount;
    OcrSessionGroup  *groups;
    BOOL   referenced[OCR_MAX_REGIONS]; // union captured each tick

    // Injected points (tests pass fakes; production passes real ones).
    // The DWORD is the pinned owner PID re-checked right before capture.
    BOOL (*captureFn)(HWND, DWORD, const RECT *, unsigned char **, int *,
                      int *, char[MSG_BUFSIZE]);
    BOOL (*readFn)(OcrEngine *, const unsigned char *, int, int, char *,
                   size_t, char[MSG_BUFSIZE]);
    OcrEngine *engine;
    UINT64 (*nowMsFn)(void);  // NULL: GetTickCount64()
    // Per-tick identity/liveness gate. NULL skips it (unit tests with
    // sentinel HWNDs); production passes captureTargetAlive so a
    // destroyed/recycled HWND yields capture_unavailable, never OCR.
    BOOL (*targetAliveFn)(const CaptureWindow *target);

    // Per-pump storage backing OcrSessionEvent.text: one slot per
    // referenced region scan plus one per firing group (reset at the
    // start of every pump).
    char evText[OCR_MAX_REGIONS + OCR_MAX_GROUPS][OCR_EVENT_TEXT];
    int  evTextNext;
} OcrSession;

typedef struct {
    int         groupIndex;  // -1 for per-region scan events
    int         regionIndex; // 0-based region for "scan", else -1
    int         lossPercent; // -1 when the event changes no gear
    const char *event;       // static name: scan / rule_trigger /
                             // loss_change / capture_unavailable
    const char *text;        // scan/recognized text, joined member text
                             // for rule_trigger, "" for failed scans
    const char *status;      // scan outcome (ok / unchanged /
                             // capture_fail / blank_frame / read_fail),
                             // NULL for group events
} OcrSessionEvent;

// Synchronous initial scan over the referenced union. Each group whose
// members all read successfully arms with the observed combined polarity;
// partially readable groups stay unarmed and arm on the first pump where
// every member is available. Returns FALSE (with err) only when no
// referenced region could be read, so Start can refuse safely.
BOOL ocrSessionSeed(OcrSession *s, char err[MSG_BUFSIZE]);

// One scan of the referenced union plus one step per group. Events are
// appended to ev starting at *evCount, capped at evCap; *evCount is
// advanced. Every referenced region emits one "scan" event per pump
// FIRST (status ok / unchanged / capture_fail / blank_frame /
// read_fail), so the human log shows the recognition process even when
// no rule fires. Every firing group then emits rule_trigger; the first
// firing group in table order additionally emits loss_change (its gear
// wins). Capture failures additionally become rate-limited per-group
// capture_unavailable events.
BOOL ocrSessionPump(OcrSession *s, OcrSessionEvent *ev, int *evCount,
                    int evCap);

#endif // CLUMSY_OCR_SESSION_H
