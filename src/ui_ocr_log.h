#ifndef CLUMSY_UI_OCR_LOG_H
#define CLUMSY_UI_OCR_LOG_H

#include "common.h"

/*
 * The OCR section's scrolling human-readable mirror of the CSV timeline
 * plus the CSV path label and the open-folder button. Split out of
 * ui_ocr.c so the widget module stays under the file-size ceiling. The
 * CSV file stays the authoritative record; this log only renders the
 * same events bilingually.
 */

// Build the log multiline and the row beneath it ("CSV timeline: <path>"
// plus the open-folder button). Returns the vbox holding both.
Ihandle *uiOcrLogBuild(void);

// Append one human-readable line, keeping a bounded tail and the newest
// line visible.
void uiOcrAppendLog(const char *line);

// Show the current run's CSV path under the log (also the /select target
// for the open-folder button).
void uiOcrSetCsvPath(const char *path);

// Test seam: the read-only multiline holding the log text.
Ihandle *uiOcrLogForTest(void);

// Test seam: the "open folder" flat button next to the CSV path.
Ihandle *uiOcrLogOpenDirButtonForTest(void);

#endif // CLUMSY_UI_OCR_LOG_H
