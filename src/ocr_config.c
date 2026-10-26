#include <winsock2.h>
#include <windows.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ocr_config.h"

#define CONFIG_LINE_MAX 1024

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static void stripBom(char *s) {
    static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    if ((unsigned char)s[0] == bom[0] && (unsigned char)s[1] == bom[1]
        && (unsigned char)s[2] == bom[2]) {
        memmove(s, s + 3, strlen(s + 3) + 1);
    }
}

static BOOL parseBool(const char *v, BOOL *out) {
    if (strcmp(v, "1") == 0 || _stricmp(v, "true") == 0
        || _stricmp(v, "yes") == 0) {
        *out = TRUE;
        return TRUE;
    }
    if (strcmp(v, "0") == 0 || _stricmp(v, "false") == 0
        || _stricmp(v, "no") == 0) {
        *out = FALSE;
        return TRUE;
    }
    return FALSE;
}

static BOOL parseInt(const char *v, int *out) {
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || end == v) return FALSE;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return FALSE;
    *out = (int)n;
    return TRUE;
}

static BOOL parseDouble(const char *v, double *out) {
    char *end = NULL;
    errno = 0;
    double n = strtod(v, &end);
    if (errno != 0 || end == v) return FALSE;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return FALSE;
    *out = n;
    return TRUE;
}

static double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static void copyField(char *dst, size_t cap, const char *src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

// -----------------------------------------------------------------------
// Legacy ruleN.* rows (pre-group release), kept only for load migration.
// -----------------------------------------------------------------------

static void legacyRuleValue(OcrRule *rule, const char *key,
                            const char *val) {
    int n;
    if (strcmp(key, "name") == 0) {
        copyField(rule->name, sizeof(rule->name), val);
    } else if (strcmp(key, "keywords") == 0) {
        copyField(rule->keywords, sizeof(rule->keywords), val);
    } else if (strcmp(key, "edge") == 0) {
        if (strcmp(val, "appear") == 0) rule->edge = OCR_EDGE_APPEAR;
        else if (strcmp(val, "disappear") == 0)
            rule->edge = OCR_EDGE_DISAPPEAR;
        // unknown value: leave the zeroed default (APPEAR)
    } else if (strcmp(key, "confirm") == 0) {
        if (parseInt(val, &n)) rule->confirmFrames = n;
    } else if (strcmp(key, "loss") == 0) {
        if (parseInt(val, &n)) rule->lossPercent = n;
    }
    // norm is routed through parseNormRect by applyLegacy; other
    // unknown legacy keys are ignored.
}

// norm=x,y,w,h : malformed or incomplete values leave the norm untouched.
static void parseNormRect(NormRect *norm, const char *v) {
    double x, y, w, h;
    if (sscanf(v, " %lf , %lf , %lf , %lf", &x, &y, &w, &h) != 4) return;
    norm->x = clamp01(x);
    norm->y = clamp01(y);
    norm->w = clamp01(w);
    norm->h = clamp01(h);
}

typedef struct {
    OcrRule rules[OCR_MAX_REGIONS];
    BOOL    touched[OCR_MAX_REGIONS];
    size_t  count;
    BOOL    seen;
} LegacyAcc;

static void applyLegacy(LegacyAcc *leg, long idx, const char *key,
                        const char *val) {
    if (idx < 0 || idx >= OCR_MAX_REGIONS) return;
    if (!strcmp(key, "norm")) {
        parseNormRect(&leg->rules[idx].norm, val);
    } else {
        legacyRuleValue(&leg->rules[idx], key, val);
    }
    leg->touched[idx] = TRUE;
    if ((size_t)idx + 1 > leg->count) leg->count = (size_t)idx + 1;
    leg->seen = TRUE;
}

static void applyRegionValue(OcrRegionDef *r, const char *key,
                             const char *val) {
    double d;
    if (strcmp(key, "name") == 0) {
        copyField(r->name, sizeof(r->name), val);
    } else if (strcmp(key, "x") == 0 || strcmp(key, "y") == 0
               || strcmp(key, "w") == 0 || strcmp(key, "h") == 0) {
        if (parseDouble(val, &d)) {
            d = clamp01(d);
            if (key[0] == 'x') r->norm.x = d;
            else if (key[0] == 'y') r->norm.y = d;
            else if (key[0] == 'w') r->norm.w = d;
            else r->norm.h = d;
        }
    }
    // unknown region key: ignored
}

static void applyGroupValue(OcrGroup *g, const char *key,
                            const char *val) {
    int n;
    if (strcmp(key, "edge") == 0) {
        if (strcmp(val, "appear") == 0) g->edge = OCR_EDGE_APPEAR;
        else if (strcmp(val, "disappear") == 0)
            g->edge = OCR_EDGE_DISAPPEAR;
    } else if (strcmp(key, "frames") == 0) {
        if (parseInt(val, &n)) g->confirmFrames = n;
    } else if (strcmp(key, "loss") == 0) {
        if (parseInt(val, &n)) g->lossPercent = n;
    }
}

static void applyMemberValue(OcrGroup *g, size_t k, const char *key,
                             const char *val) {
    OcrGroupMember *m = &g->members[k];
    int n;
    if (strcmp(key, "region") == 0) {
        if (parseInt(val, &n)) m->regionIndex = n;
    } else if (strcmp(key, "keywords") == 0) {
        copyField(m->keywords, sizeof(m->keywords), val);
    }
    if (k + 1 > g->memberCount) g->memberCount = k + 1;
}

// Parse the numeric prefix between s and the expected dot, e.g.
// indexedKey("region.3.name", "region.") -> 3 and sub="name".
static BOOL indexedKey(const char *key, const char *prefix, long *idx,
                       const char **sub) {
    size_t plen = strlen(prefix);
    if (strncmp(key, prefix, plen) != 0) return FALSE;
    char *dot = strchr(key + plen, '.');
    if (!dot) return FALSE;
    char *end = NULL;
    errno = 0;
    long n = strtol(key + plen, &end, 10);
    if (errno != 0 || end != dot || n < 0) return FALSE;
    *idx = n;
    *sub = dot + 1;
    return TRUE;
}

static void applyLine(OcrConfig *cfg, LegacyAcc *leg, char *key,
                      const char *val) {
    long idx;
    const char *sub = NULL;

    if (strcmp(key, "enabled") == 0) {
        parseBool(val, &cfg->enabled);
    } else if (strcmp(key, "changeDetect") == 0) {
        parseBool(val, &cfg->changeDetect);
    } else if (strcmp(key, "interval") == 0) {
        parseInt(val, &cfg->intervalMs);
    } else if (strcmp(key, "exe") == 0) {
        copyField(cfg->exeName, sizeof(cfg->exeName), val);
    } else if (strcmp(key, "title") == 0) {
        copyField(cfg->windowTitle, sizeof(cfg->windowTitle), val);
    } else if (strcmp(key, "regionCount") == 0) {
        // Count rows only seed an empty config; structural rows below
        // derive the real count from the highest seen index.
        int n = 0;
        if (parseInt(val, &n) && n > 0 && cfg->regionCount == 0) {
            cfg->regionCount = (n > OCR_MAX_REGIONS)
                               ? OCR_MAX_REGIONS : (size_t)n;
        }
    } else if (strcmp(key, "groupCount") == 0) {
        int n = 0;
        if (parseInt(val, &n) && n > 0 && cfg->groupCount == 0) {
            cfg->groupCount = (n > OCR_CONFIG_GROUPS_MAX)
                              ? OCR_CONFIG_GROUPS_MAX : (size_t)n;
        }
    } else if (strcmp(key, "ruleCount") == 0) {
        // Legacy count row; migration derives slots from ruleN.* rows.
    } else if (indexedKey(key, "region.", &idx, &sub)
               && idx < OCR_MAX_REGIONS) {
        applyRegionValue(&cfg->regions[idx], sub, val);
        if ((size_t)idx + 1 > cfg->regionCount)
            cfg->regionCount = (size_t)idx + 1;
    } else if (indexedKey(key, "group.", &idx, &sub)
               && idx < OCR_CONFIG_GROUPS_MAX) {
        if (strncmp(sub, "member.", 7) == 0) {
            char *mEnd = NULL;
            errno = 0;
            long k = strtol(sub + 7, &mEnd, 10);
            if (errno == 0 && mEnd && *mEnd == '.' && k >= 0
                && k < OCR_MAX_REGIONS) {
                applyMemberValue(&cfg->groups[idx], (size_t)k,
                                 mEnd + 1, val);
            }
        } else {
            applyGroupValue(&cfg->groups[idx], sub, val);
        }
        if ((size_t)idx + 1 > cfg->groupCount)
            cfg->groupCount = (size_t)idx + 1;
    } else if (strncmp(key, "rule", 4) == 0) {
        char *dot = strchr(key, '.');
        if (dot) {
            char *end = NULL;
            errno = 0;
            long n = strtol(key + 4, &end, 10);
            if (errno == 0 && end == dot)
                applyLegacy(leg, n, dot + 1, val);
        }
    }
    // unknown top-level key: ignored
}

// Turn legacy ruleN.* rows into one region plus one single-member group
// per old rule. Only runs when no new-format structural rows exist.
static void migrateLegacy(OcrConfig *cfg, const LegacyAcc *leg) {
    if (!leg->seen || cfg->regionCount > 0 || cfg->groupCount > 0) return;
    for (size_t i = 0; i < leg->count; i++) {
        if (!leg->touched[i]) continue; // sparse hand-edit: leave a hole
        const OcrRule *r = &leg->rules[i];
        OcrRegionDef *rd = &cfg->regions[i];
        if (r->name[0] != '\0') {
            copyField(rd->name, sizeof(rd->name), r->name);
        } else {
            snprintf(rd->name, sizeof(rd->name), "Region %u",
                     (unsigned)(i + 1));
        }
        rd->norm = r->norm;

        OcrGroup *g = &cfg->groups[i];
        g->edge = r->edge;
        g->confirmFrames = r->confirmFrames;
        g->lossPercent = r->lossPercent;
        g->memberCount = 1;
        g->members[0].regionIndex = (int)i;
        copyField(g->members[0].keywords,
                  sizeof(g->members[0].keywords), r->keywords);
    }
    cfg->regionCount = leg->count;
    cfg->groupCount = leg->count;
}

static int cmpMemberRegion(const void *a, const void *b) {
    int ia = ((const OcrGroupMember *)a)->regionIndex;
    int ib = ((const OcrGroupMember *)b)->regionIndex;
    return (ia > ib) - (ia < ib);
}

// A member whose region index never parsed stays at its -1 sentinel and
// is dropped; the rest compact to the front sorted by region index.
static void compactMembers(OcrGroup *g) {
    OcrGroupMember kept[OCR_MAX_REGIONS];
    size_t n = 0;
    for (size_t k = 0; k < OCR_MAX_REGIONS; k++) {
        if (g->members[k].regionIndex >= 0)
            kept[n++] = g->members[k];
    }
    if (n > 1) qsort(kept, n, sizeof(kept[0]), cmpMemberRegion);
    memset(g->members, 0, sizeof(g->members));
    for (size_t k = 0; k < n; k++) g->members[k] = kept[k];
    for (size_t k = n; k < OCR_MAX_REGIONS; k++)
        g->members[k].regionIndex = -1;
    g->memberCount = n;
}

BOOL ocrConfigLoad(const char *path, OcrConfig *out, char err[MSG_BUFSIZE]) {
    if (!path || !out) {
        if (err) strncpy(err, "bad ocrConfigLoad arguments", MSG_BUFSIZE - 1);
        return FALSE;
    }
    // The loader owns a fully defined output: production passes a stack
    // struct (ocr_glue), and a comment-only/partial file must leave zero
    // counts rather than garbage that the compaction loop would walk OOB.
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) {
            snprintf(err, MSG_BUFSIZE, "cannot open %s: %s", path,
                     strerror(errno));
        }
        return FALSE;
    }

    LegacyAcc leg;
    memset(&leg, 0, sizeof(leg));
    for (size_t j = 0; j < OCR_CONFIG_GROUPS_MAX; j++)
        for (size_t k = 0; k < OCR_MAX_REGIONS; k++)
            out->groups[j].members[k].regionIndex = -1;

    char line[CONFIG_LINE_MAX];
    int firstLine = 1;
    while (fgets(line, sizeof(line), f)) {
        if (firstLine) { stripBom(line); firstLine = 0; }
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue; // comments, blank lines, corrupt lines: skip
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (key[0] == '\0' || key[0] == '#') continue;
        applyLine(out, &leg, key, val);
    }

    fclose(f);
    migrateLegacy(out, &leg);
    for (size_t j = 0; j < out->groupCount; j++)
        compactMembers(&out->groups[j]);
    if (err) err[0] = '\0';
    return TRUE;
}

static BOOL fputsChecked(FILE *f, const char *s) {
    return fputs(s, f) >= 0;
}

static BOOL writeNorm(FILE *f, size_t i, const NormRect *n) {
    return fprintf(f, "region.%u.x=%.4f\n", (unsigned)i, n->x) >= 0
        && fprintf(f, "region.%u.y=%.4f\n", (unsigned)i, n->y) >= 0
        && fprintf(f, "region.%u.w=%.4f\n", (unsigned)i, n->w) >= 0
        && fprintf(f, "region.%u.h=%.4f\n", (unsigned)i, n->h) >= 0;
}

BOOL ocrConfigSave(const char *path, const OcrConfig *cfg,
                   char err[MSG_BUFSIZE]) {
    if (!path || !cfg) {
        if (err) strncpy(err, "bad ocrConfigSave arguments", MSG_BUFSIZE - 1);
        return FALSE;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        if (err) {
            snprintf(err, MSG_BUFSIZE, "cannot write %s: %s", path,
                     strerror(errno));
        }
        return FALSE;
    }

    BOOL ok = TRUE;
    ok &= fputsChecked(f, "# clumsy OCR automation config; UTF-8 key=value\n");
    ok &= fprintf(f, "enabled=%d\n", cfg->enabled ? 1 : 0) >= 0;
    ok &= fprintf(f, "changeDetect=%d\n", cfg->changeDetect ? 1 : 0) >= 0;
    ok &= fprintf(f, "interval=%d\n", cfg->intervalMs) >= 0;
    ok &= fprintf(f, "exe=%s\n", cfg->exeName) >= 0;
    ok &= fprintf(f, "title=%s\n", cfg->windowTitle) >= 0;
    ok &= fprintf(f, "regionCount=%u\n", (unsigned)cfg->regionCount) >= 0;

    for (size_t i = 0; i < cfg->regionCount; i++) {
        const OcrRegionDef *r = &cfg->regions[i];
        ok &= fprintf(f, "region.%u.name=%s\n", (unsigned)i, r->name) >= 0;
        ok &= writeNorm(f, i, &r->norm);
    }

    ok &= fprintf(f, "groupCount=%u\n", (unsigned)cfg->groupCount) >= 0;
    for (size_t j = 0; j < cfg->groupCount; j++) {
        const OcrGroup *g = &cfg->groups[j];
        ok &= fprintf(f, "group.%u.edge=%s\n", (unsigned)j,
                      g->edge == OCR_EDGE_APPEAR ? "appear" : "disappear")
              >= 0;
        ok &= fprintf(f, "group.%u.frames=%d\n", (unsigned)j,
                      g->confirmFrames) >= 0;
        ok &= fprintf(f, "group.%u.loss=%d\n", (unsigned)j,
                      g->lossPercent) >= 0;
        for (size_t k = 0; k < g->memberCount; k++) {
            const OcrGroupMember *m = &g->members[k];
            ok &= fprintf(f, "group.%u.member.%u.region=%d\n", (unsigned)j,
                          (unsigned)k, m->regionIndex) >= 0;
            ok &= fprintf(f, "group.%u.member.%u.keywords=%s\n",
                          (unsigned)j, (unsigned)k, m->keywords) >= 0;
        }
    }

    if (fclose(f) != 0) ok = FALSE;
    if (!ok && err) {
        snprintf(err, MSG_BUFSIZE, "failed writing %s", path);
    } else if (err) {
        err[0] = '\0';
    }
    return ok;
}

// TRUE when keywords has at least one non-separator character.
static BOOL hasAnyKeyword(const char *kw) {
    const unsigned char *p = (const unsigned char *)kw;
    while (*p) {
        if (isspace(*p)
            || (*p == ',')
            || (*p == 0xE3 && p[1] == 0x80 && p[2] == 0x80)
            || (*p == 0xEF && p[1] == 0xBC && p[2] == 0x8C)) {
            p += (*p >= 0xE0) ? 3 : 1;
            continue;
        }
        return TRUE;
    }
    return FALSE;
}

// Regions smaller than ~1% of the client in either dimension cannot
// hold legible text and can drive the thumbnail math into degenerate
// aspect ratios; a picked drag always yields a larger rect, so this
// only rejects corrupt/hand-edited saved configs.
#define OCR_RULE_MIN_NORM_FRACTION 0.01

BOOL ocrGroupValidate(const OcrGroup *g, size_t groupNumber1Based,
                      size_t regionCount, char err[MSG_BUFSIZE]) {
    if (!g) {
        if (err) strncpy(err, "null OCR group", MSG_BUFSIZE - 1);
        return FALSE;
    }
    if (g->memberCount == 0 || g->memberCount > OCR_MAX_REGIONS) {
        if (err) {
            snprintf(err, MSG_BUFSIZE, "group %u has no regions",
                     (unsigned)groupNumber1Based);
        }
        return FALSE;
    }
    for (size_t i = 0; i < g->memberCount; i++) {
        int idx = g->members[i].regionIndex;
        if (idx < 0 || (size_t)idx >= regionCount) {
            if (err) {
                snprintf(err, MSG_BUFSIZE,
                         "group %u references a missing region",
                         (unsigned)groupNumber1Based);
            }
            return FALSE;
        }
        for (size_t prev = 0; prev < i; prev++) {
            if (g->members[prev].regionIndex == idx) {
                if (err) {
                    snprintf(err, MSG_BUFSIZE,
                             "group %u references the same region twice",
                             (unsigned)groupNumber1Based);
                }
                return FALSE;
            }
        }
        if (!hasAnyKeyword(g->members[i].keywords)) {
            if (err) {
                snprintf(err, MSG_BUFSIZE,
                         "group %u has a member with no keywords",
                         (unsigned)groupNumber1Based);
            }
            return FALSE;
        }
    }
    if (g->confirmFrames < 1) {
        if (err) {
            snprintf(err, MSG_BUFSIZE,
                     "group %u needs at least 1 confirm frame",
                     (unsigned)groupNumber1Based);
        }
        return FALSE;
    }
    if (g->lossPercent < 0 || g->lossPercent > 100) {
        if (err) {
            snprintf(err, MSG_BUFSIZE,
                     "group %u loss percent must be 0..100",
                     (unsigned)groupNumber1Based);
        }
        return FALSE;
    }
    if (err) err[0] = '\0';
    return TRUE;
}

BOOL ocrConfigValidate(const OcrConfig *cfg, char err[MSG_BUFSIZE]) {
    if (!cfg) {
        if (err) strncpy(err, "null OCR config", MSG_BUFSIZE - 1);
        return FALSE;
    }
    if (cfg->enabled && cfg->exeName[0] == '\0') {
        if (err) {
            strncpy(err, "OCR enabled but no target application selected",
                    MSG_BUFSIZE - 1);
        }
        return FALSE;
    }
    if (cfg->regionCount > OCR_MAX_REGIONS
        || cfg->groupCount > OCR_CONFIG_GROUPS_MAX) {
        if (err) strncpy(err, "too many OCR regions or groups",
                         MSG_BUFSIZE - 1);
        return FALSE;
    }

    for (size_t j = 0; j < cfg->groupCount; j++) {
        if (!ocrGroupValidate(&cfg->groups[j], j + 1, cfg->regionCount,
                              err)) {
            return FALSE;
        }
    }

    BOOL referenced[OCR_MAX_REGIONS];
    memset(referenced, 0, sizeof(referenced));
    for (size_t j = 0; j < cfg->groupCount; j++) {
        for (size_t k = 0; k < cfg->groups[j].memberCount; k++) {
            int idx = cfg->groups[j].members[k].regionIndex;
            if (idx >= 0 && (size_t)idx < cfg->regionCount)
                referenced[idx] = TRUE;
        }
    }
    for (size_t i = 0; i < cfg->regionCount; i++) {
        if (!referenced[i]) continue;
        const NormRect *n = &cfg->regions[i].norm;
        if (n->w < OCR_RULE_MIN_NORM_FRACTION
            || n->h < OCR_RULE_MIN_NORM_FRACTION
            || n->w > 1.0 || n->h > 1.0) {
            if (err) {
                snprintf(err, MSG_BUFSIZE,
                         "region %u is empty or outside the window",
                         (unsigned)(i + 1));
            }
            return FALSE;
        }
    }
    if (err) err[0] = '\0';
    return TRUE;
}
