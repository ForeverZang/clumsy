#if defined(CLUMSY_OCR)

#include <winsock2.h>
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "rapidocrlite/rapidocrlite.h"

#include "common.h"
#include "ocr_engine.h"

#define OCR_DET_MODEL   "PP-OCRv6_det_small.onnx"
#define OCR_REC_MODEL   "PP-OCRv6_rec_small.onnx"
#define OCR_DICT        "ppocrv6_dict.txt"
#define OCR_MAX_SIDE    4096
#define OCR_MAX_BOXES   128
#define OCR_ROW_BAND_PX 12

struct OcrEngine {
    rpocr_ocr *handle;
};

static BOOL fileExists(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES
        && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

OcrEngine *ocrEngineCreate(const char *modelsDir, char err[MSG_BUFSIZE]) {
    if (err) err[0] = '\0';
    if (!modelsDir || modelsDir[0] == '\0') {
        if (err) strncpy(err, "models directory is empty", MSG_BUFSIZE - 1);
        return NULL;
    }

    char detPath[1024];
    char recPath[1024];
    char dictPath[1024];
    snprintf(detPath, sizeof(detPath), "%s\\%s", modelsDir, OCR_DET_MODEL);
    snprintf(recPath, sizeof(recPath), "%s\\%s", modelsDir, OCR_REC_MODEL);
    snprintf(dictPath, sizeof(dictPath), "%s\\%s", modelsDir, OCR_DICT);

    if (!fileExists(detPath) || !fileExists(recPath)
        || !fileExists(dictPath)) {
        if (err) {
            snprintf(err, MSG_BUFSIZE,
                     "OCR model files not found in %s", modelsDir);
        }
        return NULL;
    }

    rpocr_options opt;
    rpocr_options_init(&opt);
    opt.backend = RPOCR_BACKEND_CPU; // GPU path crashes at exit (spike)
    opt.model_type = RPOCR_MODEL_PPOCR;
    opt.use_cls = 0;
    opt.rec_fast_mode = 0; // rapidocr 3.10 exact recognition

    rpocr_ocr *handle = rpocr_create(detPath, recPath, dictPath, &opt);
    if (!handle) {
        if (err) {
            const char *detail = rpocr_last_error();
            snprintf(err, MSG_BUFSIZE, "OCR engine init failed: %s",
                     detail ? detail : "unknown error");
        }
        return NULL;
    }

    OcrEngine *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        rpocr_destroy(handle);
        if (err) strncpy(err, "out of memory", MSG_BUFSIZE - 1);
        return NULL;
    }
    engine->handle = handle;
    return engine;
}

void ocrEngineDestroy(OcrEngine *engine) {
    if (!engine) return;
    if (engine->handle) rpocr_destroy(engine->handle);
    free(engine);
}

// Reading order: band rows of OCR_ROW_BAND_PX physical pixels, then x.
static int boxCmp(const void *pa, const void *pb) {
    const rpocr_text_box *a = *(const rpocr_text_box * const *)pa;
    const rpocr_text_box *b = *(const rpocr_text_box * const *)pb;
    int ay = a->bbox_y / OCR_ROW_BAND_PX;
    int by = b->bbox_y / OCR_ROW_BAND_PX;
    if (ay != by) return ay < by ? -1 : 1;
    if (a->bbox_x != b->bbox_x) return a->bbox_x < b->bbox_x ? -1 : 1;
    return 0;
}

static void appendText(char *out, size_t cap, const char *text, BOOL *first) {
    if (!text || !text[0]) return;
    if (!*first) {
        size_t n = strlen(out);
        if (n + 2 < cap) {
            out[n] = ' ';
            out[n + 1] = '\0';
        }
    }
    size_t used = strlen(out);
    if (used + 1 < cap) strncat(out, text, cap - 1 - used);
    out[cap - 1] = '\0';
    *first = FALSE;
}

BOOL ocrEngineRead(OcrEngine *engine, const unsigned char *bgra,
                   int w, int h, char *out, size_t cap,
                   char err[MSG_BUFSIZE]) {
    if (err) err[0] = '\0';
    if (!engine || !engine->handle || !bgra || !out || cap == 0) {
        if (err) strncpy(err, "bad OCR read arguments", MSG_BUFSIZE - 1);
        return FALSE;
    }
    if (w <= 0 || h <= 0 || w > OCR_MAX_SIDE || h > OCR_MAX_SIDE) {
        if (err) {
            snprintf(err, MSG_BUFSIZE,
                     "OCR frame size out of range: %dx%d", w, h);
        }
        return FALSE;
    }
    out[0] = '\0';

    size_t pixels = (size_t)w * h;
    unsigned char *rgb = malloc(pixels * 3);
    if (!rgb) {
        if (err) strncpy(err, "out of memory", MSG_BUFSIZE - 1);
        return FALSE;
    }
    for (size_t i = 0; i < pixels; i++) {
        rgb[i*3+0] = bgra[i*4+2]; // R
        rgb[i*3+1] = bgra[i*4+1]; // G
        rgb[i*3+2] = bgra[i*4+0]; // B
    }

    rpocr_image img;
    img.width = w;
    img.height = h;
    img.rgb = rgb;

    rpocr_result result;
    memset(&result, 0, sizeof(result));
    int status = rpocr_recognize(engine->handle, &img, &result);
    free(rgb);
    if (status != RPOCR_OK) {
        if (err) {
            const char *detail = rpocr_last_error();
            snprintf(err, MSG_BUFSIZE, "OCR recognize failed: %s",
                     detail ? detail : "unknown error");
        }
        return FALSE;
    }

    size_t n = result.count < OCR_MAX_BOXES ? result.count : OCR_MAX_BOXES;
    if (n > 0) {
        rpocr_text_box *sorted[OCR_MAX_BOXES];
        for (size_t i = 0; i < n; i++) sorted[i] = &result.items[i];
        qsort(sorted, n, sizeof(sorted[0]), boxCmp);
        BOOL first = TRUE;
        for (size_t i = 0; i < n; i++) {
            appendText(out, cap, sorted[i]->text, &first);
        }
    }

    rpocr_result_free(&result);
    return TRUE;
}

#else
// --- non-OCR builds (build.zig): everything compiles, nothing runs ------

#include "ocr_engine.h"

OcrEngine *ocrEngineCreate(const char *modelsDir, char err[MSG_BUFSIZE]) {
    (void)modelsDir;
    if (err) strncpy(err, "OCR support is not built in", MSG_BUFSIZE - 1);
    return NULL;
}

void ocrEngineDestroy(OcrEngine *engine) { (void)engine; }

BOOL ocrEngineRead(OcrEngine *engine, const unsigned char *bgra,
                   int w, int h, char *out, size_t cap,
                   char err[MSG_BUFSIZE]) {
    (void)engine; (void)bgra; (void)w; (void)h; (void)cap;
    if (out && cap) out[0] = '\0';
    if (err) strncpy(err, "OCR support is not built in", MSG_BUFSIZE - 1);
    return FALSE;
}

#endif
