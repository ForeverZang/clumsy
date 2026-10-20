// Unit tests for the beginner window's non-widget logic: percent clamp,
// countdown math, app-list line formatting, and the chart accounting
// routing (SNIFF raw bytes while idle, reinjected bytes while affecting,
// ownership-gated in both modes).
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "targeting.h"
#include "i18n.h"
#include "procmap.h"
#include "traffic.h"
#include "ui_simple.h"

// Minimal valid IPv4 TCP packet (20 IP + 20 TCP) for tuple parsing.
static unsigned char g_tcpPacket[40] = {
    0x45, 0x00, 0x00, 0x28, 0, 0, 0, 0, 0, 6, 0, 0,
    10, 0, 0, 1, 10, 0, 0, 2,
    0, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x50, 0, 0, 0, 0, 0, 0, 0
};
static unsigned char g_udpPacket[28] = {
    0x45, 0x00, 0x00, 0x1c, 0, 0, 0, 0, 0, 17, 0, 0,
    10, 0, 0, 1, 10, 0, 0, 2,
    0, 80, 0, 53, 0, 8, 0, 0
};

static BOOL fakeOwnerHit(const PacketNode *node, UINT32 *pid) {
    (void)node;
    *pid = 42;
    return TRUE;
}
static BOOL fakeOwnerMiss(const PacketNode *node, UINT32 *pid) {
    (void)node;
    (void)pid;
    return FALSE;
}
static BOOL fakeName(UINT32 pid, char *out, size_t cap) {
    (void)pid;
    strncpy(out, "a.exe", cap - 1);
    out[cap - 1] = '\0';
    return TRUE;
}

static void liveRates(UINT64 *down, UINT64 *up) {
    UINT64 d[TRAFFIC_BUCKETS], u[TRAFFIC_BUCKETS];
    trafficSeries(uiSimpleTraffic(), d, u);
    *down = d[TRAFFIC_BUCKETS - 1];
    *up = u[TRAFFIC_BUCKETS - 1];
}

int main(void) {
    WINDIVERT_ADDRESS out, in;
    memset(&out, 0, sizeof(out));
    memset(&in, 0, sizeof(in));
    out.Outbound = 1;

    // 1. Slider percent -> permyriad, clamped.
    assert(uiSimpleLossPermyriad(0) == 0);
    assert(uiSimpleLossPermyriad(50) == 5000);
    assert(uiSimpleLossPermyriad(95) == 9500);
    assert(uiSimpleLossPermyriad(100) == 10000);
    assert(uiSimpleLossPermyriad(-5) == 0);
    assert(uiSimpleLossPermyriad(120) == 10000);

    // 2. Countdown seconds: ceil of remaining ms, 0 once past the deadline.
    assert(uiSimpleRemainingSeconds(0, 5000) == 5);
    assert(uiSimpleRemainingSeconds(4900, 5000) == 1);
    assert(uiSimpleRemainingSeconds(5000, 5000) == 0);
    assert(uiSimpleRemainingSeconds(9000, 5000) == 0);

    // 3. App-list line: product description present -> two-part line,
    // otherwise the exe-only fallback.
    AppGroup app;
    char line[256], expect[256];
    memset(&app, 0, sizeof(app));
    strcpy(app.exeName, "chrome.exe");
    app.pidCount = 12;
    uiSimpleFormatAppLine(&app, "Google Chrome", line, sizeof(line));
    snprintf(expect, sizeof(expect), tr("app.lineProduct"),
             "Google Chrome", "chrome.exe", 12u);
    assert(strcmp(line, expect) == 0);
    uiSimpleFormatAppLine(&app, "", line, sizeof(line));
    snprintf(expect, sizeof(expect), tr("app.line"), "chrome.exe", 12u);
    assert(strcmp(line, expect) == 0);
    uiSimpleFormatAppLine(&app, NULL, line, sizeof(line));
    snprintf(expect, sizeof(expect), tr("app.line"), "chrome.exe", 12u);
    assert(strcmp(line, expect) == 0);

    // 4. Chart routing.
    Traffic *t = uiSimpleTraffic();
    assert(t != NULL);
    targetingSetOwnerLookup(fakeOwnerHit);
    targetingSetPidNameResolver(fakeName);
    targetingClear();
    uiSimpleSetAffecting(FALSE);

    // Idle: SNIFF observer counts raw bytes, direction split by addr.
    uiSimpleOnSniff((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleOnSniff((const char *)g_udpPacket, sizeof(g_udpPacket), &in);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == sizeof(g_udpPacket));
        assert(up == sizeof(g_tcpPacket));
    }
    // Reinject events are ignored while idle.
    trafficReset(t);
    uiSimpleOnReinject((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == 0 && up == 0);
    }

    // Affecting: reinjected survivor bytes count; SNIFF bytes do not.
    uiSimpleSetAffecting(TRUE);
    uiSimpleOnSniff((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleOnReinject((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleOnReinject((const char *)g_udpPacket, sizeof(g_udpPacket), &in);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == sizeof(g_udpPacket));
        assert(up == sizeof(g_tcpPacket));
    }

    // Both modes are ownership-gated: an unowned packet never charts.
    trafficReset(t);
    targetingSetOwnerLookup(fakeOwnerMiss);
    {
        const char *targets[] = { "a.exe" };
        targetingSetImages(targets, 1);
    }
    uiSimpleOnReinject((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    uiSimpleSetAffecting(FALSE);
    uiSimpleOnSniff((const char *)g_tcpPacket, sizeof(g_tcpPacket), &out);
    {
        UINT64 down, up;
        liveRates(&down, &up);
        assert(down == 0 && up == 0);
    }
    targetingClear();
    targetingSetOwnerLookup(fakeOwnerHit);
    uiSimpleSetAffecting(FALSE);

    printf("OK: ui_simple\n");
    return 0;
}
