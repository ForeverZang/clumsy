#ifndef CLUMSY_OCR_TIMELINE_H
#define CLUMSY_OCR_TIMELINE_H

#include <windows.h>

#include "common.h"

/*
 * Per-session CSV timeline of OCR automation events. One file per
 * Start, UTF-8 with BOM, fixed 10-column header, flushed per event so
 * a crash never loses the record.
 */

typedef struct OcrTimeline OcrTimeline;

typedef struct {
    UINT64      timeMs;      // milliseconds since session Start
    const char *event;      // session_start/loss_change/rule_trigger/...
    const char *region;
    const char *ocrText;
    const char *keyword;
    const char *edge;       // "appear"/"disappear" or NULL
    int         lossPercent; // -1 = not applicable -> empty cell
    const char *source;     // initial_slider / group:<n> / system / manual_stop
    const char *note;
} OcrTimelineEvent;

// Create dirHint (recursively) and open a uniquely named CSV inside it.
// On success returns the timeline and fills pathOut with the file path.
OcrTimeline *ocrTimelineOpen(const char *dirHint, char *pathOut,
                             size_t pathCap, char err[MSG_BUFSIZE]);
void         ocrTimelineClose(OcrTimeline *tl);
BOOL         ocrTimelineEvent(OcrTimeline *tl, const OcrTimelineEvent *ev);

// RFC4180: fields containing comma, quote, CR or LF are wrapped in
// quotes with inner quotes doubled. NULL renders an empty field.
void ocrCsvEscape(const char *field, char *out, size_t cap);

// %LOCALAPPDATA%\clumsy\ocr-runs, created on demand. On failure (env
// missing, path too long) returns FALSE and fills fallback with the
// executable's directory instead.
BOOL ocrTimelineDefaultDir(char *out, size_t cap, char *fallback,
                           size_t fbCap);

#endif // CLUMSY_OCR_TIMELINE_H
