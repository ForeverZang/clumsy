#ifndef CLUMSY_DIVERT_H
#define CLUMSY_DIVERT_H

#include "common.h"
#include "flowmap.h"

/*
 * Two-layer handle lifecycle:
 * - attach/detach: persistent FLOW watcher + SNIFF network handle.
 *   SNIFF never alters traffic; packets go only to the chart observer.
 * - startAffect/stopAffect: an intercepting NETWORK handle plus the
 *   recv/clock module pipeline, opened and closed per Start/Stop.
 */

// Called once an app is selected. Opens the FLOW handle and a SNIFF
// NETWORK handle with the given (usually "true") kernel filter.
BOOL divertAttach(const char *kernelFilter, char err[MSG_BUFSIZE]);
void divertDetach(void);
BOOL divertIsAttached(void);

// Begin/end an interception session on top of the persistent session.
BOOL divertStartAffect(const char *kernelFilter, char err[MSG_BUFSIZE]);
void divertStopAffect(void);
BOOL divertIsAffecting(void);

// Flow ownership map (NULL when detached); used by shouldAffect().
FlowMap *divertFlowMap(void);

typedef void (*SniffObserver)(const char *packet, UINT len,
                              const WINDIVERT_ADDRESS *addr);
// Chart stats hook; pass NULL to clear. Called on the sniff thread.
void divertSetSniffObserver(SniffObserver observer);

// Interim wrappers preserving the old main.c contract until Task 11:
// attach+affect / stop+detach. Removed in the UI rework task.
int  divertStart(const char *filter, char buf[]);
void divertStop(void);

#endif // CLUMSY_DIVERT_H
