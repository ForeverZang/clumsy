#ifndef CLUMSY_OCR_RULES_H
#define CLUMSY_OCR_RULES_H

#include <windows.h>

/*
 * Pure OCR rule logic: normalized substring matching plus an
 * edge-triggered per-rule state machine. No GUI, no capture, no heap.
 */

typedef enum {
    OCR_EDGE_APPEAR    = 0,  // fire when keyword text appears
    OCR_EDGE_DISAPPEAR = 1   // fire when keyword text disappears
} OcrEdge;

// Region rectangle as client-area fractions, clamped to 0..1.
typedef struct {
    double x, y, w, h;
} NormRect;

typedef struct {
    char    name[32];
    char    keywords[256];   // comma separated (',' or U+FF0C), substring OR
    OcrEdge edge;
    int     confirmFrames;   // consecutive confirming polls needed; >=1
    int     lossPercent;     // gear to apply on trigger; 0..100
    NormRect norm;           // captured region as fractions of client area
} OcrRule;

typedef enum {
    OCR_STABLE_ABSENT  = 0,
    OCR_STABLE_PRESENT = 1
} OcrStable;

// armed=FALSE until the first successful OCR: no edge can fire, so a
// capture/engine failure on the first frame can never read as "disappear".
typedef struct {
    OcrStable stable;    // settled polarity
    int       candidate; // OcrStable currently being confirmed, or -1
    int       run;       // consecutive confirming polls
    BOOL      armed;
} OcrRuleState;

typedef enum {
    OCR_POLL_MISSED      = 0,  // OCR succeeded; no keyword matched
    OCR_POLL_MATCHED     = 1,  // OCR succeeded; a keyword matched
    OCR_POLL_UNAVAILABLE = 2   // capture/OCR failed this tick: ignore
} OcrPoll;

void ocrRuleStateInit(OcrRuleState *st);

// Arm after the first successful OCR, seeding the settled polarity.
void ocrRuleArm(OcrRuleState *st, BOOL presentNow);

// Trim outer whitespace, remove ALL inner whitespace (ASCII whitespace
// and U+3000 ideographic space), fold ASCII A-Z to lower case. UTF-8
// multibyte sequences pass through untouched. Returns FALSE (and an
// empty out) when no non-whitespace content remains.
BOOL ocrNormalizeText(const char *in, char *out, size_t cap);

// Split the rule keywords on ',' / U+FF0C, normalize each side, and
// return TRUE when any keyword is a substring of the normalized text.
BOOL ocrRuleMatches(const OcrRule *rule, const char *rawText);

// Advance the state machine by one OCR tick. Fires exactly once when a
// confirmFrames run reaches the rule edge; on fire returns TRUE and
// sets *targetPercent to rule->lossPercent. UNAVAILABLE changes
// nothing; *targetPercent is untouched on a non-firing step.
BOOL ocrRuleStep(OcrRuleState *st, const OcrRule *rule,
                 OcrPoll poll, int *targetPercent);

#endif // CLUMSY_OCR_RULES_H
