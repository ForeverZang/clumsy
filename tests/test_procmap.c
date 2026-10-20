// Integration tests for src/procmap.[ch]: live IP Helper tables.
// Runs without administrator; sees at least the current user's sockets
// plus system listeners on any modern Windows.
#include <assert.h>
#include <stdio.h>
#include <winsock2.h>
#include <windows.h>

#include "procmap.h"

static void testEndpointSnapshot(void) {
    ProcEndpoint *eps = NULL;
    size_t count = 0;
    BOOL ok = procmapSnapshotEndpoints(&eps, &count);
    assert(ok);
    assert(eps != NULL);
    assert(count > 0);

    int tcp = 0, udp = 0, v6 = 0;
    for (size_t i = 0; i < count; i++) {
        const ProcEndpoint *e = &eps[i];
        assert(e->protocol == IPPROTO_TCP || e->protocol == IPPROTO_UDP);
        assert(e->pid != 0);
        if (e->protocol == IPPROTO_TCP) tcp++; else udp++;
        if (e->localAddr[1] != 0 || e->localAddr[2] != 0
            || e->localAddr[3] != 0) {
            v6++;
        }
    }
    // A live machine has at least one TCP or UDP endpoint.
    assert(tcp + udp > 0);
    printf("  snapshot: %zu endpoints (tcp=%d udp=%d v6=%d)\n",
           count, tcp, udp, v6);
    procmapFree(eps);
    procmapFree(NULL); // must be a no-op
}

static void testEnumApps(void) {
    AppGroup groups[256];
    size_t count = 0;
    BOOL ok = procmapEnumApps(groups, 256, &count);
    assert(ok);
    assert(count > 0);
    assert(count <= 256);

    for (size_t i = 0; i < count; i++) {
        const AppGroup *g = &groups[i];
        assert(g->pidCount >= 1);
        assert(g->pidCount <= 256);
        assert(g->exeName[0] != '\0');
        assert(g->exePath[0] != L'\0');
        // Every listed pid is non-zero.
        for (UINT j = 0; j < g->pidCount; j++) {
            assert(g->pids[j] != 0);
        }
    }
    printf("  enum apps: %zu groups (first: %s)\n",
           count, groups[0].exeName);
}

// Repeated enumeration must not leak process handles.
static void testNoHandleLeak(void) {
    HANDLE cur = GetCurrentProcess();
    DWORD before = 0, after = 0;
    AppGroup groups[256];
    size_t count = 0;

    for (int i = 0; i < 10; i++) {
        assert(procmapEnumApps(groups, 256, &count));
    }
    BOOL ok1 = GetProcessHandleCount(cur, &before);
    assert(ok1);
    for (int i = 0; i < 100; i++) {
        assert(procmapEnumApps(groups, 256, &count));
        ProcEndpoint *eps = NULL;
        size_t epCount = 0;
        assert(procmapSnapshotEndpoints(&eps, &epCount));
        procmapFree(eps);
    }
    BOOL ok2 = GetProcessHandleCount(cur, &after);
    assert(ok2);
    printf("  handles before=%lu after=%lu\n", before, after);
    assert(after <= before + 4);
}

int main(void) {
    testEndpointSnapshot();
    testEnumApps();
    testNoHandleLeak();
    printf("OK: procmap\n");
    return 0;
}
