#include "flowwatch.h"

#include <stdlib.h>
#include <string.h>

#define FLOW_FILTER "tcp or udp"
#define FLOW_PRIORITY 1
#define SEED_INTERVAL_MS 1500

struct FlowWatch {
    FlowMap *map;
    HANDLE   flowHandle;
    HANDLE   flowThread;
    HANDLE   seedThread;
    HANDLE   stopEvent;
    FlowKey *seedKeys;
    size_t   seedCount;
};

// Loopback detection on network-order address words (as IP Helper and
// WinDivert report them): IPv4 127/8 in the first byte, IPv6 ::1.
static BOOL seedIsLoopback(const UINT32 addr[4]) {
    if ((addr[0] & 0xFFu) == 0x7Fu
        && addr[1] == 0 && addr[2] == 0 && addr[3] == 0) {
        return TRUE;
    }
    if (addr[0] == 0 && addr[1] == 0 && addr[2] == 0
        && addr[3] == htonl(1)) {
        return TRUE;
    }
    return FALSE;
}

static void seedKeyFor(const ProcEndpoint *ep, FlowKey *key) {
    // Snapshot rows are always local to this host: pretend an
    // outbound packet; loopback normalization ignores direction.
    flowmapKeyFromPacket(ep->protocol, TRUE,
                         seedIsLoopback(ep->localAddr),
                         ep->localAddr, ep->localPort,
                         ep->remoteAddr, ep->remotePort,
                         FALSE, key);
}

void flowwatchSeedFromEndpoints(FlowMap *map, const ProcEndpoint *eps,
                                size_t count) {
    if (map == NULL || eps == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        FlowKey key;
        seedKeyFor(&eps[i], &key);
        flowmapUpsert(map, &key, eps[i].pid);
    }
}

// Raw byte order on FlowKey gives a consistent total order because every
// key is zero-initialized (no uninitialized padding); used for bsearch.
static int seedKeyCmp(const void *a, const void *b) {
    return memcmp(a, b, sizeof(FlowKey));
}

void flowwatchReconcileSeeds(FlowMap *map, FlowKey **ioKeys, size_t *ioCount,
                             const ProcEndpoint *eps, size_t count) {
    if (map == NULL || ioKeys == NULL || ioCount == NULL) {
        return;
    }
    FlowKey *oldKeys = *ioKeys;
    size_t oldCount = *ioCount;

    FlowKey *newKeys = NULL;
    if (count > 0) {
        newKeys = malloc(sizeof(FlowKey) * count);
        if (newKeys == NULL) {
            // Keep the previous cycle: fail open, retry next cycle.
            return;
        }
        for (size_t i = 0; i < count; i++) {
            seedKeyFor(&eps[i], &newKeys[i]);
        }
        qsort(newKeys, count, sizeof(FlowKey), seedKeyCmp);
    }

    // Retire keys no longer backed by a live endpoint. FLOW-event entries
    // sharing a key with a dead row are removed as well: with the socket
    // gone there is nothing to attribute anymore.
    for (size_t i = 0; i < oldCount; i++) {
        if (bsearch(&oldKeys[i], newKeys, count, sizeof(FlowKey),
                    seedKeyCmp) == NULL) {
            flowmapRemove(map, &oldKeys[i]);
        }
    }

    flowwatchSeedFromEndpoints(map, eps, count);

    free(oldKeys);
    *ioKeys = newKeys;
    *ioCount = count;
}

static DWORD WINAPI flowEventLoop(LPVOID arg) {
    FlowWatch *fw = (FlowWatch *)arg;
    for (;;) {
        WINDIVERT_ADDRESS addr;
        if (!WinDivertRecv(fw->flowHandle, NULL, 0, NULL, &addr)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) {
                break; // handle shut down for stopping
            }
            Sleep(10);
            continue;
        }
        FlowKey key;
        flowmapKeyFromData(&addr.Flow, &key);
        if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
            flowmapUpsert(fw->map, &key, addr.Flow.ProcessId);
        } else if (addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
            flowmapRemove(fw->map, &key);
        }
    }
    return 0;
}

static DWORD WINAPI seedLoop(LPVOID arg) {
    FlowWatch *fw = (FlowWatch *)arg;
    while (WaitForSingleObject(fw->stopEvent, SEED_INTERVAL_MS)
           == WAIT_TIMEOUT) {
        ProcEndpoint *eps = NULL;
        size_t count = 0;
        if (procmapSnapshotEndpoints(&eps, &count)) {
            flowwatchReconcileSeeds(fw->map, &fw->seedKeys, &fw->seedCount,
                                    eps, count);
            procmapFree(eps);
        }
    }
    return 0;
}

static void releaseThreads(FlowWatch *fw) {
    if (fw->flowThread != NULL) {
        WaitForSingleObject(fw->flowThread, INFINITE);
        CloseHandle(fw->flowThread);
    }
    if (fw->seedThread != NULL) {
        WaitForSingleObject(fw->seedThread, INFINITE);
        CloseHandle(fw->seedThread);
    }
}

FlowWatch *flowwatchStart(FlowMap *map) {
    if (map == NULL) {
        return NULL;
    }
    FlowWatch *fw = calloc(1, sizeof(FlowWatch));
    if (fw == NULL) {
        return NULL;
    }
    fw->map = map;
    fw->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (fw->stopEvent == NULL) {
        free(fw);
        return NULL;
    }
    // FLOW events are pure metadata: upstream flowtrack sample opens
    // this layer SNIFF|RECV_ONLY.
    fw->flowHandle = WinDivertOpen(FLOW_FILTER, WINDIVERT_LAYER_FLOW,
                                   FLOW_PRIORITY,
                                   WINDIVERT_FLAG_SNIFF
                                   | WINDIVERT_FLAG_RECV_ONLY);
    if (fw->flowHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(fw->stopEvent);
        free(fw);
        return NULL;
    }
    fw->flowThread = CreateThread(NULL, 0, flowEventLoop, fw, 0, NULL);
    fw->seedThread = CreateThread(NULL, 0, seedLoop, fw, 0, NULL);
    if (fw->flowThread == NULL || fw->seedThread == NULL) {
        flowwatchStop(fw);
        return NULL;
    }
    return fw;
}

void flowwatchStop(FlowWatch *fw) {
    if (fw == NULL) {
        return;
    }
    SetEvent(fw->stopEvent);
    if (fw->flowHandle != INVALID_HANDLE_VALUE && fw->flowHandle != NULL) {
        WinDivertShutdown(fw->flowHandle, WINDIVERT_SHUTDOWN_BOTH);
    }
    releaseThreads(fw);
    if (fw->flowHandle != INVALID_HANDLE_VALUE && fw->flowHandle != NULL) {
        WinDivertClose(fw->flowHandle);
    }
    CloseHandle(fw->stopEvent);
    free(fw->seedKeys);
    free(fw);
}
