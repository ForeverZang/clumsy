#ifndef CLUMSY_FLOWWATCH_H
#define CLUMSY_FLOWWATCH_H

#include "flowmap.h"
#include "procmap.h"

/*
 * Background owner of a WinDivert FLOW-layer handle. FLOW events give
 * incremental ownership for new connections; periodic IP Helper snapshots
 * seed connections that existed before the handle was opened.
 */
typedef struct FlowWatch FlowWatch;

// Open the FLOW handle ("tcp or udp") and start the event + seed threads.
// Returns NULL on failure (driver unavailable / no administrator).
FlowWatch *flowwatchStart(FlowMap *map);
// Shutdown the handle, join both threads and release everything.
void       flowwatchStop(FlowWatch *fw);

// Merge one IP Helper endpoint snapshot into the map. Testable without
// any WinDivert handle.
void       flowwatchSeedFromEndpoints(FlowMap *map,
                                      const ProcEndpoint *endpoints,
                                      size_t count);

// Replace the seed-key set with one snapshot cycle: upsert every endpoint
// key and remove keys from earlier cycles that are gone, so a reused local
// port can never attribute a new process's packets to the old owner.
// *ioKeys/*ioCount is the cross-cycle state, initialized to NULL/0 by the
// caller and freed/replaced here. Testable without any WinDivert handle.
void       flowwatchReconcileSeeds(FlowMap *map,
                                   FlowKey **ioKeys, size_t *ioCount,
                                   const ProcEndpoint *endpoints,
                                   size_t count);

#endif // CLUMSY_FLOWWATCH_H
