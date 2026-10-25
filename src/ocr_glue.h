#ifndef CLUMSY_OCR_GLUE_H
#define CLUMSY_OCR_GLUE_H

#include "common.h"

/*
 * IUP bindings between the beginner window and the headless OCR
 * coordinator (ocr_app). Owns the 100 ms UI-thread drain timer, config
 * persistence next to the executable, bilingual scrolling-log lines,
 * and the inline-red-error mapping for every Start preflight failure.
 * No WinDivert call happens here; main.c invokes Start before attaching
 * and Stop while tearing down, so a failed OCR start leaves no
 * diversion attached.
 */

// Create the drain timer and resolve config/models/timeline paths.
// Call once after IupOpen + uiOcrCreate.
BOOL uiOcrGlueInit(void);

// Load ocr_config.txt next to the executable and populate the section.
// A missing file is the silent all-defaults case; parse errors are
// reported inline.
void uiOcrGlueLoadSavedConfig(void);

// Run the enabled OCR block end to end (collect -> validate -> resolve ->
// persist -> timeline -> engine -> seed -> worker) and flip the section
// into its running presentation. FALSE means the inline red error is
// already shown and the caller must not attach WinDivert. When the block
// is disabled this is a TRUE no-op so the non-OCR path stays unchanged.
BOOL uiOcrGlueStart(int sliderPercent);

// Stop the run (final events delivered, session_stop row written).
void uiOcrGlueStop(void);

// Process teardown: stop any run and destroy the timer.
void uiOcrGlueCleanup(void);

#endif // CLUMSY_OCR_GLUE_H
