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
 */

#define OCR_CONFIG_RULES_MAX 16

typedef struct {
    char   exeName[128];
    char   windowTitle[256];
    int    intervalMs;
    BOOL   changeDetect;
    BOOL   enabled;
    size_t ruleCount;
    OcrRule rules[OCR_CONFIG_RULES_MAX];
} OcrConfig;

// Fill a zeroed OcrConfig from path. Missing file and open errors return
// FALSE with an English err; malformed individual lines are skipped.
BOOL ocrConfigLoad(const char *path, OcrConfig *out, char err[MSG_BUFSIZE]);

// Write the full configuration (UTF-8, CRLF tolerated by the parser).
BOOL ocrConfigSave(const char *path, const OcrConfig *cfg,
                   char err[MSG_BUFSIZE]);

// enabled configs need an exe target; every rule needs at least one
// keyword, confirmFrames >= 1 and lossPercent in 0..100.
BOOL ocrConfigValidate(const OcrConfig *cfg, char err[MSG_BUFSIZE]);

#endif // CLUMSY_OCR_CONFIG_H
