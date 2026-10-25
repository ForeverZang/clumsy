#ifndef CLUMSY_OCR_WORKER_H
#define CLUMSY_OCR_WORKER_H

#include "ocr_session.h"

/*
 * Single background OCR worker. One thread pumps the shared OcrSession
 * on an absolute beat: if a pump overruns its interval the missed beats
 * are skipped, never caught up. Events are copied into a CRITICAL_SECTION
 * guarded heap queue and delivered on the caller's thread through
 * ocrWorkerDrain, so the GUI thread is the only one touching IUP.
 */

typedef void (*OcrEventSink)(const OcrSessionEvent *ev);

// Start the process-wide worker. FALSE (with err) when already running
// or the session is invalid. The session stays owned by the caller and
// must outlive the worker until ocrWorkerStop.
BOOL ocrWorkerStart(OcrSession *session, OcrEventSink sink,
                    char err[MSG_BUFSIZE]);

// Signal the worker, join its thread, and drop any queued events.
void ocrWorkerStop(void);

// TRUE while the worker thread is inside a pump.
BOOL ocrWorkerBusy(void);

// Deliver queued events to sink on the calling (GUI) thread, then clear
// the queue. Safe to call repeatedly; safe with the worker running.
void ocrWorkerDrain(OcrEventSink sink);

#endif // CLUMSY_OCR_WORKER_H
