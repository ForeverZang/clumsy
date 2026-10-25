#ifndef CLUMSY_OCR_ENGINE_H
#define CLUMSY_OCR_ENGINE_H

#include <windows.h>

#include "common.h"

/*
 * Thin bridge to the vendored RapidOCRLite C ABI. This is the only
 * product file that includes rapidocrlite.h; everything else talks in
 * BGRA frames and UTF-8 text. Built only when CLUMSY_OCR is defined
 * (gcc x64 debug/release); other builds get NULL-returning stubs.
 */

typedef struct OcrEngine OcrEngine;

// Load det/rec/dict from modelsDir (fixed PP-OCRv6 small filenames),
// CPU backend, no orientation classifier, rapidocr-3.10 exact mode.
OcrEngine *ocrEngineCreate(const char *modelsDir, char err[MSG_BUFSIZE]);
void       ocrEngineDestroy(OcrEngine *engine);

// Recognize one cropped region (top-down BGRA). All detected text
// boxes are sorted top-to-bottom, left-to-right and joined with spaces
// into out as UTF-8. A frame with no text yields an empty string.
BOOL ocrEngineRead(OcrEngine *engine, const unsigned char *bgra,
                   int w, int h, char *out, size_t cap,
                   char err[MSG_BUFSIZE]);

#endif // CLUMSY_OCR_ENGINE_H
