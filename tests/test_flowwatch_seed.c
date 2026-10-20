// Unit tests for flowwatchSeedFromEndpoints: IP Helper endpoint rows must
// become flowmap entries whose keys match real NETWORK-layer packets.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#include "flowmap.h"
#include "procmap.h"
#include "flowwatch.h"

static UINT32 v4addr(const char *s) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    assert(sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4);
    return htonl((a << 24) | (b << 16) | (c << 8) | d);
}

static void setV6(UINT32 net[4], UINT32 w0, UINT32 w1, UINT32 w2, UINT32 w3) {
    net[0] = htonl(w0);
    net[1] = htonl(w1);
    net[2] = htonl(w2);
    net[3] = htonl(w3);
}

static void testSeedsV4Tcp(FlowMap *m) {
    ProcEndpoint eps[1];
    memset(eps, 0, sizeof(eps));
    eps[0].protocol = IPPROTO_TCP;
    eps[0].localAddr[0] = v4addr("192.168.1.20");
    eps[0].localPort = htons(51000);
    eps[0].remoteAddr[0] = v4addr("1.1.1.1");
    eps[0].remotePort = htons(443);
    eps[0].pid = 111;

    flowwatchSeedFromEndpoints(m, eps, 1);

    // Outbound packet from this host hits.
    UINT32 local[4] = {0}, remote[4] = {0};
    local[0] = v4addr("192.168.1.20");
    remote[0] = v4addr("1.1.1.1");
    FlowKey outK;
    flowmapKeyFromPacket(IPPROTO_TCP, TRUE, FALSE,
                         local, htons(51000), remote, htons(443), FALSE, &outK);
    UINT32 pid = 0;
    assert(flowmapLookup(m, &outK, &pid) && pid == 111);

    // Reply traffic (inbound) hits the same flow.
    FlowKey inK;
    flowmapKeyFromPacket(IPPROTO_TCP, FALSE, FALSE,
                         remote, htons(443), local, htons(51000), FALSE, &inK);
    assert(flowmapLookup(m, &inK, &pid) && pid == 111);
}

static void testSeedsV6Udp(FlowMap *m) {
    ProcEndpoint eps[1];
    memset(eps, 0, sizeof(eps));
    eps[0].protocol = IPPROTO_UDP;
    setV6(eps[0].localAddr, 0x20010db8u, 0, 0, 0x10);
    eps[0].localPort = htons(55000);
    setV6(eps[0].remoteAddr, 0x20010db8u, 0, 0, 0x20);
    eps[0].remotePort = htons(53);
    eps[0].pid = 222;

    flowwatchSeedFromEndpoints(m, eps, 1);

    UINT32 local[4], remote[4];
    setV6(local, 0x20010db8u, 0, 0, 0x10);
    setV6(remote, 0x20010db8u, 0, 0, 0x20);
    FlowKey outK;
    flowmapKeyFromPacket(IPPROTO_UDP, TRUE, FALSE,
                         local, htons(55000), remote, htons(53), TRUE, &outK);
    UINT32 pid = 0;
    assert(flowmapLookup(m, &outK, &pid) && pid == 222);
}

// Loopback rows appear twice in the IP Helper table with swapped ends;
// a seeded row must match whichever direction the packet takes.
static void testSeedsLoopback(FlowMap *m) {
    ProcEndpoint eps[1];
    memset(eps, 0, sizeof(eps));
    eps[0].protocol = IPPROTO_TCP;
    eps[0].localAddr[0] = v4addr("127.0.0.1");
    eps[0].localPort = htons(12321);
    eps[0].remoteAddr[0] = v4addr("127.0.0.1");
    eps[0].remotePort = htons(4321);
    eps[0].pid = 333;

    flowwatchSeedFromEndpoints(m, eps, 1);

    UINT32 a[4] = {0}, b[4] = {0};
    a[0] = v4addr("127.0.0.1");
    b[0] = v4addr("127.0.0.1");
    FlowKey k1, k2;
    flowmapKeyFromPacket(IPPROTO_TCP, TRUE, TRUE,
                         a, htons(4321), b, htons(12321), FALSE, &k1);
    flowmapKeyFromPacket(IPPROTO_TCP, FALSE, TRUE,
                         a, htons(12321), b, htons(4321), FALSE, &k2);
    UINT32 pid = 0;
    assert(flowmapLookup(m, &k1, &pid) && pid == 333);
    assert(flowmapLookup(m, &k2, &pid) && pid == 333);
}

// UDP listener rows have a zero remote end; seeding must not crash and
// must remain queryable (harmless until a FLOW event supplies the tuple).
static void testSeedsUdpListener(FlowMap *m) {
    ProcEndpoint eps[1];
    memset(eps, 0, sizeof(eps));
    eps[0].protocol = IPPROTO_UDP;
    eps[0].localAddr[0] = v4addr("0.0.0.0");
    eps[0].localPort = htons(5353);
    eps[0].pid = 444;

    flowwatchSeedFromEndpoints(m, eps, 1);

    UINT32 local[4] = {0};
    UINT32 remote[4] = {0};
    FlowKey k;
    flowmapKeyFromPacket(IPPROTO_UDP, TRUE, FALSE,
                         local, htons(5353), remote, htons(0), FALSE, &k);
    UINT32 pid = 0;
    assert(flowmapLookup(m, &k, &pid) && pid == 444);

    // NULL/empty inputs are safe.
    flowwatchSeedFromEndpoints(m, NULL, 0);
    flowwatchSeedFromEndpoints(NULL, eps, 1);
}

int main(void) {
    FlowMap *m = flowmapCreate();
    assert(m != NULL);
    testSeedsV4Tcp(m);
    testSeedsV6Udp(m);
    testSeedsLoopback(m);
    testSeedsUdpListener(m);
    flowmapDestroy(m);
    printf("OK: flowwatch_seed\n");
    return 0;
}
