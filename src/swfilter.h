#ifndef CLUMSY_SWFILTER_H
#define CLUMSY_SWFILTER_H

#include "common.h"

/*
 * Software WinDivert filter for the advanced text box. The intercept
 * handle always opens with the kernel filter "true" (PIDs cannot be
 * expressed in kernel syntax); the user's text is compiled here and
 * evaluated per packet in software, ANDed after shouldAffect() and
 * before modules run.
 *
 * Lifecycle: swfilterSet() runs on the UI thread before the recv/clock
 * threads start; swfilterClear() runs after they join, so no locking.
 */

// Compile and activate a NETWORK-layer filter. Empty/NULL/whitespace
// disables the gate and returns TRUE. A syntax error returns FALSE,
// fills err (when non-NULL) with the WinDivert diagnostic and keeps
// the previously active filter untouched.
BOOL swfilterSet(const char *text, char err[MSG_BUFSIZE]);
void swfilterClear(void);
BOOL swfilterActive(void);

// TRUE when no filter is set or the packet matches the active filter.
// Any error or NULL input yields FALSE (the packet is then passed
// through untouched by the pipeline).
BOOL swfilterMatch(const char *packet, UINT packetLen,
                   const WINDIVERT_ADDRESS *addr);

#endif // CLUMSY_SWFILTER_H
