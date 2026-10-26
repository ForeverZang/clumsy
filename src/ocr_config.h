#ifndef CLUMSY_OCR_CONFIG_H
#define CLUMSY_OCR_CONFIG_H

#include <windows.h>

#include "common.h"
#include "ocr_rules.h"

/*
 * OCR automation configuration persisted as UTF-8 key=value text
 * (ocr_config.txt next to config.txt). Parsing is tolerant: unknown
 * keys and malformed values are skipped so older/newer files keep
 * working; ocrConfigValidate decides whether the result is usable.
 *
 * Files from the pre-group release (ruleCount + ruleN.* rows) are
 * migrated on load into one region plus one single-member group per
 * old rule; ocrConfigSave writes the new format only.
 */

#define OCR_CONFIG_GROUPS_MAX OCR_MAX_GROUPS

// A named rectangle on the picked target window, stored as client-area
// fractions. A region no group references is never captured.
typedef struct {
    char     name[32];
    NormRect norm;
} OcrRegionDef;

typedef struct {
    char   exeName[128];
    char   windowTitle[256];
    int    intervalMs;
    BOOL   changeDetect;
    BOOL   enabled;
    size_t regionCount;
    OcrRegionDef regions[OCR_MAX_REGIONS];
    size_t groupCount;
    OcrGroup     groups[OCR_CONFIG_GROUPS_MAX];
} OcrConfig;

// Fill a zeroed OcrConfig from path. Missing file and open errors return
// FALSE with an English err; malformed individual lines are skipped.
BOOL ocrConfigLoad(const char *path, OcrConfig *out, char err[MSG_BUFSIZE]);

// Write the full configuration (UTF-8, CRLF tolerated by the parser).
BOOL ocrConfigSave(const char *path, const OcrConfig *cfg,
                   char err[MSG_BUFSIZE]);

// enabled configs need an exe target; groups must pass ocrGroupValidate,
// and a region's geometry is gated only when some group references it.
BOOL ocrConfigValidate(const OcrConfig *cfg, char err[MSG_BUFSIZE]);

// Validate one group: at least one member, every member region index in
// 0..regionCount-1 with no duplicates, non-empty keywords, confirmFrames
// >= 1 and lossPercent in 0..100. Error text names "group N".
BOOL ocrGroupValidate(const OcrGroup *g, size_t groupNumber1Based,
                      size_t regionCount, char err[MSG_BUFSIZE]);

#endif // CLUMSY_OCR_CONFIG_H
