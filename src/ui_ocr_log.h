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

// Append one human-readable line. A timestamp is prepended to every
// line; while an automation run drives the gear (see uiOcrLogSetGear)
// the loss in force at that moment is prepended too, so each scrolling
// recognition line answers "what was seen, and what the loss was, at
// that moment". Keeps a bounded head (oldest tail lines drop past the
// cap) with the newest line pinned on top.
void uiOcrAppendLog(const char *line);

// Set the live loss percentage (0..100) shown by the co-located
// readout and stamped on subsequent log lines. -1 returns the readout
// to its idle state (no run / run stopped) and drops the gear tag from
// the prefix.
void uiOcrLogSetGear(int percent);

// Refresh the gear readout wording after a live language switch.
void uiOcrLogRetranslate(void);

// Test seam: freeze the timestamp prefix at the given local time; NULL
// restores the wall clock.
void uiOcrLogSetTimeForTest(const SYSTEMTIME *fixed);

// Test seam: the always-visible live-loss readout above the multiline.
Ihandle *uiOcrLogGearLabelForTest(void);

// Show the current run's CSV path under the log (also the /select target
// for the open-folder button).
void uiOcrSetCsvPath(const char *path);

// Test seam: the read-only multiline holding the log text.
Ihandle *uiOcrLogForTest(void);

// Test seam: the "open folder" flat button next to the CSV path.
Ihandle *uiOcrLogOpenDirButtonForTest(void);

#endif // CLUMSY_UI_OCR_LOG_H
