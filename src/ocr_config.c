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

static double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

// norm=x,y,w,h : malformed or incomplete values leave the norm untouched.
static void parseNorm(OcrRule *rule, const char *v) {
    double x, y, w, h;
    if (sscanf(v, " %lf , %lf , %lf , %lf", &x, &y, &w, &h) != 4) return;
    rule->norm.x = clamp01(x);
    rule->norm.y = clamp01(y);
    rule->norm.w = clamp01(w);
    rule->norm.h = clamp01(h);
}

static void copyField(char *dst, size_t cap, const char *src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void applyRuleValue(OcrRule *rule, const char *key, const char *val) {
    int n;
    if (strcmp(key, "name") == 0) {
        copyField(rule->name, sizeof(rule->name), val);
    } else if (strcmp(key, "keywords") == 0) {
        copyField(rule->keywords, sizeof(rule->keywords), val);
    } else if (strcmp(key, "edge") == 0) {
        if (strcmp(val, "appear") == 0) rule->edge = OCR_EDGE_APPEAR;
        else if (strcmp(val, "disappear") == 0) rule->edge = OCR_EDGE_DISAPPEAR;
        // unknown value: leave the default (APPEAR)
    } else if (strcmp(key, "confirm") == 0) {
        if (parseInt(val, &n)) rule->confirmFrames = n;
    } else if (strcmp(key, "loss") == 0) {
        if (parseInt(val, &n)) rule->lossPercent = n;
    } else if (strcmp(key, "norm") == 0) {
        parseNorm(rule, val);
    }
    // unknown rule key: ignored
}

static void applyLine(OcrConfig *cfg, char *key, const char *val) {
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
    } else if (strcmp(key, "ruleCount") == 0) {
        int n = 0;
        if (parseInt(val, &n)) {
            if (n < 0) n = 0;
            if (n > OCR_CONFIG_RULES_MAX) n = OCR_CONFIG_RULES_MAX;
            cfg->ruleCount = (size_t)n;
        }
    } else if (strncmp(key, "rule", 4) == 0) {
        char *dot = strchr(key, '.');
        if (!dot) return; // unknown key
        char *end = NULL;
        errno = 0;
        long idx = strtol(key + 4, &end, 10);
        if (errno != 0 || end != dot || idx < 0
            || idx >= OCR_CONFIG_RULES_MAX) {
            return; // out-of-range or malformed index
        }
        applyRuleValue(&cfg->rules[idx], dot + 1, val);
        if ((size_t)idx + 1 > cfg->ruleCount) cfg->ruleCount = (size_t)idx + 1;
    }
    // unknown top-level key: ignored
}

BOOL ocrConfigLoad(const char *path, OcrConfig *out, char err[MSG_BUFSIZE]) {
    if (!path || !out) {
        if (err) strncpy(err, "bad ocrConfigLoad arguments", MSG_BUFSIZE - 1);
        return FALSE;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) {
            snprintf(err, MSG_BUFSIZE, "cannot open %s: %s", path,
                     strerror(errno));
        }
        return FALSE;
    }

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
        applyLine(out, key, val);
    }

    fclose(f);
    if (err) err[0] = '\0';
    return TRUE;
}

static BOOL fputsChecked(FILE *f, const char *s) {
    return fputs(s, f) >= 0;
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
    ok &= fprintf(f, "ruleCount=%u\n", (unsigned)cfg->ruleCount) >= 0;

    for (size_t i = 0; i < cfg->ruleCount; i++) {
        const OcrRule *r = &cfg->rules[i];
        ok &= fprintf(f, "rule%u.name=%s\n", (unsigned)i, r->name) >= 0;
        ok &= fprintf(f, "rule%u.keywords=%s\n", (unsigned)i, r->keywords) >= 0;
        ok &= fprintf(f, "rule%u.edge=%s\n", (unsigned)i,
                      r->edge == OCR_EDGE_APPEAR ? "appear" : "disappear") >= 0;
        ok &= fprintf(f, "rule%u.confirm=%d\n", (unsigned)i,
                      r->confirmFrames) >= 0;
        ok &= fprintf(f, "rule%u.loss=%d\n", (unsigned)i,
                      r->lossPercent) >= 0;
        ok &= fprintf(f,
                      "rule%u.norm=%.4f,%.4f,%.4f,%.4f\n",
                      (unsigned)i, r->norm.x, r->norm.y, r->norm.w,
                      r->norm.h) >= 0;
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

// isDraft[i] == TRUE marks an unfinished rule freshly dragged in the
// UI: its empty keywords are allowed (the rule can never match until
// the user fills them in), while every other gate still applies.
// NULL means every rule is finished (strict keyword gate).
BOOL ocrConfigValidateDrafts(const OcrConfig *cfg, const BOOL *isDraft,
                             char err[MSG_BUFSIZE]) {
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
    if (cfg->ruleCount > OCR_CONFIG_RULES_MAX) {
        if (err) strncpy(err, "too many OCR rules", MSG_BUFSIZE - 1);
        return FALSE;
    }
    for (size_t i = 0; i < cfg->ruleCount; i++) {
        const OcrRule *r = &cfg->rules[i];
        if ((isDraft == NULL || !isDraft[i]) && !hasAnyKeyword(r->keywords)) {
            if (err) {
                snprintf(err, MSG_BUFSIZE,
                         "rule %u has no keywords", (unsigned)i + 1);
            }
            return FALSE;
        }
        if (r->confirmFrames < 1) {
            if (err) {
                snprintf(err, MSG_BUFSIZE,
                         "rule %u needs at least 1 confirm frame",
                         (unsigned)i + 1);
            }
            return FALSE;
        }
        if (r->lossPercent < 0 || r->lossPercent > 100) {
            if (err) {
                snprintf(err, MSG_BUFSIZE,
                         "rule %u loss percent must be 0..100",
                         (unsigned)i + 1);
            }
            return FALSE;
        }
    }
    if (err) err[0] = '\0';
    return TRUE;
}

BOOL ocrConfigValidate(const OcrConfig *cfg, char err[MSG_BUFSIZE]) {
    return ocrConfigValidateDrafts(cfg, NULL, err);
}
