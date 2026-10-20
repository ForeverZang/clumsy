#include "flowwatch.h"

#include <stdlib.h>

#define FLOW_FILTER "tcp or udp"
#define FLOW_PRIORITY 1
#define SEED_INTERVAL_MS 1500

struct FlowWatch {
    FlowMap *map;
    HANDLE   flowHandle;
    HANDLE   flowThread;
    HANDLE   seedThread;
    HANDLE   stopEvent;
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

void flowwatchSeedFromEndpoints(FlowMap *map, const ProcEndpoint *eps,
                                size_t count) {
    if (map == NULL || eps == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const ProcEndpoint *ep = &eps[i];
        FlowKey key;
        // Snapshot rows are always local to this host: pretend an
        // outbound packet; loopback normalization ignores direction.
        flowmapKeyFromPacket(ep->protocol, TRUE,
                             seedIsLoopback(ep->localAddr),
                             ep->localAddr, ep->localPort,
                             ep->remoteAddr, ep->remotePort,
                             FALSE, &key);
        flowmapUpsert(map, &key, ep->pid);
    }
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
            flowwatchSeedFromEndpoints(fw->map, eps, count);
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
    free(fw);
}
