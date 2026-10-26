#include "ocr_rules.h"

#include <ctype.h>
#include <string.h>

#define OCR_KEYWORD_MAX  16
#define OCR_KEYWORD_LEN  128

void ocrRuleStateInit(OcrRuleState *st) {
    st->stable    = OCR_STABLE_ABSENT;
    st->candidate = -1;
    st->run       = 0;
    st->armed     = FALSE;
}

void ocrRuleArm(OcrRuleState *st, BOOL presentNow) {
    st->stable    = presentNow ? OCR_STABLE_PRESENT : OCR_STABLE_ABSENT;
    st->candidate = -1;
    st->run       = 0;
    st->armed     = TRUE;
}

// isspace() only covers ASCII whitespace; U+3000 ideographic space
// (UTF-8 E3 80 80) needs a byte-level check.
static void skipWhitespace(const unsigned char **p) {
    for (;;) {
        unsigned char c = **p;
        if (c == 0) return;
        if (c == 0xE3 && (*p)[1] == 0x80 && (*p)[2] == 0x80) {
            *p += 3;
        } else if (isspace(c)) {
            *p += 1;
        } else {
            return;
        }
    }
}

static void copyNormalized(const char *in, char *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)in;
    size_t n = 0;
    if (cap == 0) return;
    while (*p) {
        if (*p == 0xE3 && p[1] == 0x80 && p[2] == 0x80) {
            p += 3;
        } else if (isspace(*p)) {
            p += 1;
        } else {
            if (n + 1 >= cap) break;
            out[n++] = (char)tolower(*p);
            p += 1;
        }
    }
    out[n] = '\0';
}

BOOL ocrNormalizeText(const char *in, char *out, size_t cap) {
    if (cap == 0) return FALSE;
    out[0] = '\0';
    if (!in) return FALSE;

    const unsigned char *p = (const unsigned char *)in;
    skipWhitespace(&p);
    copyNormalized((const char *)p, out, cap);
    // Leading whitespace was skipped; inner and trailing whitespace never
    // copied, so out is empty exactly when input had no content.
    return out[0] != '\0';
}

static BOOL nextKeyword(char *tokens, char **cursor, char *keyword, size_t cap) {
    char *start = *cursor;
    if (start == NULL) return FALSE;

    char *p = start;
    while (*p) {
        // ASCII comma or U+FF0C fullwidth comma (EF BC 8C)
        if (*p == ',' || (p[0] == (char)0xEF && p[1] == (char)0xBC
                          && p[2] == (char)0x8C)) {
            break;
        }
        p++;
    }

    if (*p == '\0') {
        *cursor = NULL;
    } else if (*p == ',') {
        *p = '\0';
        *cursor = p + 1;
    } else {
        p[0] = '\0';
        *cursor = p + 3;
    }

    char norm[OCR_KEYWORD_LEN];
    (void)ocrNormalizeText(start, norm, sizeof(norm));

    size_t len = strlen(norm);
    if (len == 0) return FALSE;
    if (len >= cap) len = cap - 1;
    memcpy(keyword, norm, len);
    keyword[len] = '\0';
    return TRUE;
}

BOOL ocrKeywordsMatch(const char *keywords, const char *rawText) {
    if (!keywords || !rawText) return FALSE;

    char text[512];
    if (!ocrNormalizeText(rawText, text, sizeof(text))) {
        return FALSE; // whitespace-only OCR frame cannot match
    }

    // Mutable token buffer: keyword strings are bounded at 256 bytes.
    char tokens[256];
    strncpy(tokens, keywords, sizeof(tokens) - 1);
    tokens[sizeof(tokens) - 1] = '\0';

    char *cursor = tokens;
    int count = 0;
    while (count < OCR_KEYWORD_MAX) {
        char keyword[OCR_KEYWORD_LEN];
        BOOL got = nextKeyword(tokens, &cursor, keyword, sizeof(keyword));
        if (!got) {
            if (cursor == NULL) break;
            continue;
        }
        count++;
        if (strstr(text, keyword) != NULL) return TRUE;
        if (cursor == NULL) break;
    }
    return FALSE;
}

BOOL ocrRuleMatches(const OcrRule *rule, const char *rawText) {
    if (!rule) return FALSE;
    return ocrKeywordsMatch(rule->keywords, rawText);
}

BOOL ocrEdgeStep(OcrRuleState *st, OcrEdge edge, int confirmFrames,
                 int lossPercent, OcrPoll poll, int *targetPercent) {
    if (!st || !st->armed) return FALSE;

    if (poll == OCR_POLL_UNAVAILABLE) return FALSE;

    int observed = (poll == OCR_POLL_MATCHED)
                   ? OCR_STABLE_PRESENT : OCR_STABLE_ABSENT;
    int wanted   = (edge == OCR_EDGE_APPEAR)
                   ? OCR_STABLE_PRESENT : OCR_STABLE_ABSENT;
    int opposite = (edge == OCR_EDGE_APPEAR)
                   ? OCR_STABLE_ABSENT : OCR_STABLE_PRESENT;

    // A poll agreeing with the already-settled state is just steady
    // state: never re-arms a confirm run.
    if (observed == st->stable) {
        st->candidate = -1;
        st->run = 0;
        return FALSE;
    }

    if (observed == opposite) {
        // A single poll in the safe direction both cancels any pending
        // confirmation and settles the known state immediately; only the
        // trigger direction needs N-frame confirmation.
        st->stable    = (OcrStable)opposite;
        st->candidate = -1;
        st->run       = 0;
        return FALSE;
    }

    // observed == wanted: accumulate a confirm run toward the edge.
    if (st->candidate != wanted) {
        st->candidate = wanted;
        st->run = 0;
    }
    st->run++;

    int need = confirmFrames >= 1 ? confirmFrames : 1;
    if (st->run < need) return FALSE;

    if (targetPercent) *targetPercent = lossPercent;
    st->stable    = (OcrStable)wanted;
    st->candidate = -1;
    st->run       = 0;
    return TRUE;
}

BOOL ocrRuleStep(OcrRuleState *st, const OcrRule *rule,
                 OcrPoll poll, int *targetPercent) {
    if (!rule) return FALSE;
    return ocrEdgeStep(st, rule->edge, rule->confirmFrames,
                       rule->lossPercent, poll, targetPercent);
}

OcrPoll ocrGroupPoll(const OcrGroup *group, const OcrRegionObs *obs,
                     size_t obsCount) {
    if (!group || !obs) return OCR_POLL_UNAVAILABLE;

    for (size_t i = 0; i < group->memberCount; i++) {
        int idx = group->members[i].regionIndex;
        if (idx < 0 || (size_t)idx >= obsCount || !obs[idx].available) {
            return OCR_POLL_UNAVAILABLE;
        }
    }
    // All referenced regions are available: AND over member matches.
    for (size_t i = 0; i < group->memberCount; i++) {
        const OcrGroupMember *m = &group->members[i];
        if (!ocrKeywordsMatch(m->keywords, obs[m->regionIndex].text)) {
            return OCR_POLL_MISSED;
        }
    }
    return OCR_POLL_MATCHED;
}
