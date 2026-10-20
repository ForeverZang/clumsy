#ifndef CLUMSY_TARGETING_H
#define CLUMSY_TARGETING_H

#include "common.h"

/*
 * Global beginner-mode targeting state, consulted by every module via
 * shouldAffect(). Security rule: when a target is active a packet is
 * affected only when it positively belongs to a targeted process; any
 * unknown/unparseable/unowned packet is passed through untouched.
 */

// Returns TRUE and fills *outPid when the packet's 5-tuple maps to a
// local process. Production wiring (divert.c) looks the tuple up in the
// live FlowMap; tests inject a stub.
typedef BOOL (*TargetingOwnerFn)(const PacketNode *node, UINT32 *outPid);
void targetingSetOwnerLookup(TargetingOwnerFn fn);

// Resolves a pid to its image name (basename, e.g. "chrome.exe").
// NULL selects the built-in procmap-backed periodic cache; tests inject
// a stub.
typedef BOOL (*TargetingPidNameFn)(UINT32 pid, char *outName, size_t cap);
void targetingSetPidNameResolver(TargetingPidNameFn fn);

// No app restriction: every direction-matching packet is affected.
void targetingClear(void);
// Restrict to the given image name set; empty list behaves as Clear.
void targetingSetImages(const char *const *names, size_t count);
// Global direction gate (beginner segmented control), default both.
void targetingSetDirection(short in, short out);

// Unified gate used by all modules: ownership (when targeted) AND the
// global direction intersected with the module's own direction check.
BOOL shouldAffect(const PacketNode *node, short modIn, short modOut);

#endif // CLUMSY_TARGETING_H
