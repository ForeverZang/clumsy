// Unit tests for src/flowmap.[ch]: 5-tuple -> pid map with loopback
// key normalization. Address/ports passed to keyFrom* are in NETWORK
// byte order, exactly as WinDivert reports them.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#include "windivert.h"
#include "flowmap.h"

// Parse "a.b.c.d" into a network-order UINT32.
static UINT32 v4addr(const char *s) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    int n = sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
    assert(n == 4);
    return htonl((a << 24) | (b << 16) | (c << 8) | d);
}

static void setV4(UINT32 net[4], const char *s) {
    net[0] = v4addr(s);
    net[1] = net[2] = net[3] = 0;
}

static void setV6(UINT32 net[4], UINT32 w0, UINT32 w1, UINT32 w2, UINT32 w3) {
    net[0] = htonl(w0);
    net[1] = htonl(w1);
    net[2] = htonl(w2);
    net[3] = htonl(w3);
}

static void testLookupUpsertRemove(void) {
    FlowMap *m = flowmapCreate();
    assert(m != NULL);

    FlowKey k;
    memset(&k, 0, sizeof(k));
    k.protocol = IPPROTO_TCP;
    k.localAddr[0] = htonl(0xC0A8010Au);  // 192.168.1.10
    k.localPort = htons(51000);
    k.remoteAddr[0] = htonl(0x08080808u); // 8.8.8.8
    k.remotePort = htons(443);

    // Miss before insert, outPid left untouched.
    UINT32 pid = 99;
    assert(!flowmapLookup(m, &k, &pid));
    assert(pid == 99);
    assert(!flowmapLookup(m, &k, NULL));

    flowmapUpsert(m, &k, 1234);
    pid = 0;
    assert(flowmapLookup(m, &k, &pid));
    assert(pid == 1234);

    // Upsert overwrites the pid for the same key.
    flowmapUpsert(m, &k, 4321);
    assert(flowmapLookup(m, &k, &pid));
    assert(pid == 4321);

    // Different tuple still misses.
    FlowKey other = k;
    other.remotePort = htons(444);
    assert(!flowmapLookup(m, &other, &pid));

    flowmapRemove(m, &k);
    assert(!flowmapLookup(m, &k, &pid));

    // Removing a missing key is a no-op.
    flowmapRemove(m, &k);

    flowmapDestroy(m);
}

static void testRemovePid(void) {
    FlowMap *m = flowmapCreate();
    FlowKey k1, k2, k3;
    memset(&k1, 0, sizeof(k1));
    memset(&k2, 0, sizeof(k2));
    memset(&k3, 0, sizeof(k3));
    k1.protocol = k2.protocol = k3.protocol = IPPROTO_TCP;
    k1.localAddr[0] = htonl(0x0A000001u); k1.localPort = htons(1000);
    k1.remoteAddr[0] = htonl(0x0A000002u); k1.remotePort = htons(80);
    k2.localAddr[0] = htonl(0x0A000001u); k2.localPort = htons(2000);
    k2.remoteAddr[0] = htonl(0x0A000003u); k2.remotePort = htons(80);
    k3.localAddr[0] = htonl(0x0A000001u); k3.localPort = htons(3000);
    k3.remoteAddr[0] = htonl(0x0A000004u); k3.remotePort = htons(80);

    flowmapUpsert(m, &k1, 77);
    flowmapUpsert(m, &k2, 77);
    flowmapUpsert(m, &k3, 88);

    flowmapRemovePid(m, 77);
    UINT32 pid = 0;
    assert(!flowmapLookup(m, &k1, &pid));
    assert(!flowmapLookup(m, &k2, &pid));
    assert(flowmapLookup(m, &k3, &pid) && pid == 88);

    flowmapDestroy(m);
}

// Both packet directions of one loopback flow must normalize to the
// same key and hit an entry written from FLOW-layer data.
static void testLoopbackV4(void) {
    WINDIVERT_DATA_FLOW d;
    memset(&d, 0, sizeof(d));
    d.Protocol = IPPROTO_TCP;
    d.ProcessId = 42;
    d.LocalAddr[0] = v4addr("127.0.0.1");
    d.LocalPort = htons(12345);
    d.RemoteAddr[0] = v4addr("127.0.0.1");
    d.RemotePort = htons(80);

    FlowKey kd;
    flowmapKeyFromData(&d, &kd);

    UINT32 a[4], b[4];
    setV4(a, "127.0.0.1");
    setV4(b, "127.0.0.1");

    FlowKey k1, k2;
    flowmapKeyFromPacket(IPPROTO_TCP, TRUE, TRUE,
                         a, htons(12345), b, htons(80), FALSE, &k1);
    flowmapKeyFromPacket(IPPROTO_TCP, FALSE, TRUE,
                         b, htons(80), a, htons(12345), FALSE, &k2);

    assert(memcmp(&k1, &k2, sizeof(k1)) == 0);
    assert(memcmp(&kd, &k1, sizeof(kd)) == 0);

    FlowMap *m = flowmapCreate();
    flowmapUpsert(m, &kd, d.ProcessId);
    UINT32 pid = 0;
    assert(flowmapLookup(m, &k1, &pid) && pid == 42);
    assert(flowmapLookup(m, &k2, &pid) && pid == 42);
    flowmapDestroy(m);
}

// Non-loopback: outbound and inbound packets normalize to the FLOW key
// whose local end is this host.
static void testDirectionV4(void) {
    WINDIVERT_DATA_FLOW d;
    memset(&d, 0, sizeof(d));
    d.Protocol = IPPROTO_UDP;
    d.ProcessId = 7;
    d.LocalAddr[0] = v4addr("192.168.1.10");
    d.LocalPort = htons(51000);
    d.RemoteAddr[0] = v4addr("1.1.1.1");
    d.RemotePort = htons(443);

    FlowKey kd;
    flowmapKeyFromData(&d, &kd);

    UINT32 local[4], remote[4];
    setV4(local, "192.168.1.10");
    setV4(remote, "1.1.1.1");

    FlowKey outK, inK;
    flowmapKeyFromPacket(IPPROTO_UDP, TRUE, FALSE,
                         local, htons(51000), remote, htons(443), FALSE, &outK);
    flowmapKeyFromPacket(IPPROTO_UDP, FALSE, FALSE,
                         remote, htons(443), local, htons(51000), FALSE, &inK);

    assert(memcmp(&kd, &outK, sizeof(kd)) == 0);
    assert(memcmp(&outK, &inK, sizeof(outK)) == 0);
}

// IPv6 loopback ::1, both directions.
static void testLoopbackV6(void) {
    WINDIVERT_DATA_FLOW d;
    memset(&d, 0, sizeof(d));
    d.Protocol = IPPROTO_TCP;
    d.ProcessId = 99;
    setV6(d.LocalAddr, 0, 0, 0, 1);   // ::1
    d.LocalPort = htons(9000);
    setV6(d.RemoteAddr, 0, 0, 0, 1);
    d.RemotePort = htons(9001);

    FlowKey kd;
    flowmapKeyFromData(&d, &kd);

    UINT32 a[4], b[4];
    setV6(a, 0, 0, 0, 1);
    setV6(b, 0, 0, 0, 1);

    FlowKey k1, k2;
    flowmapKeyFromPacket(IPPROTO_TCP, TRUE, TRUE,
                         a, htons(9000), b, htons(9001), TRUE, &k1);
    flowmapKeyFromPacket(IPPROTO_TCP, FALSE, TRUE,
                         b, htons(9001), a, htons(9000), TRUE, &k2);

    assert(memcmp(&k1, &k2, sizeof(k1)) == 0);
    assert(memcmp(&kd, &k1, sizeof(kd)) == 0);
}

// IPv6 global addresses, both packet directions.
static void testDirectionV6(void) {
    WINDIVERT_DATA_FLOW d;
    memset(&d, 0, sizeof(d));
    d.Protocol = IPPROTO_UDP;
    d.ProcessId = 5;
    setV6(d.LocalAddr, 0x20010db8u, 0, 0, 0x10);   // 2001:db8::10
    d.LocalPort = htons(55000);
    setV6(d.RemoteAddr, 0x20010db8u, 0, 0, 0x20);  // 2001:db8::20
    d.RemotePort = htons(443);

    FlowKey kd;
    flowmapKeyFromData(&d, &kd);

    UINT32 local[4], remote[4];
    setV6(local, 0x20010db8u, 0, 0, 0x10);
    setV6(remote, 0x20010db8u, 0, 0, 0x20);

    FlowKey outK, inK;
    flowmapKeyFromPacket(IPPROTO_UDP, TRUE, FALSE,
                         local, htons(55000), remote, htons(443), TRUE, &outK);
    flowmapKeyFromPacket(IPPROTO_UDP, FALSE, FALSE,
                         remote, htons(443), local, htons(55000), TRUE, &inK);

    assert(memcmp(&kd, &outK, sizeof(kd)) == 0);
    assert(memcmp(&outK, &inK, sizeof(outK)) == 0);
}

// Capacity must cover at least 8192 simultaneous flows.
static void testBulkCapacity(void) {
    enum { N = 10000 };
    FlowMap *m = flowmapCreate();
    FlowKey *keys = malloc(sizeof(FlowKey) * N);
    assert(keys != NULL);

    srand(12345);
    for (int i = 0; i < N; i++) {
        FlowKey *k = &keys[i];
        memset(k, 0, sizeof(*k));
        k->protocol = (i % 2 == 0) ? IPPROTO_TCP : IPPROTO_UDP;
        k->localAddr[0] = (UINT32)i;          // unique
        k->localPort = (UINT16)(0x8000u + i); // unique
        k->localAddr[1] = rand();
        k->remoteAddr[0] = rand();
        k->remoteAddr[1] = rand();
        k->remoteAddr[2] = rand();
        k->remotePort = (UINT16)(rand() & 0xFFFF);
        flowmapUpsert(m, k, (UINT32)(i + 1));
    }
    for (int i = 0; i < N; i++) {
        UINT32 pid = 0;
        assert(flowmapLookup(m, &keys[i], &pid));
        assert(pid == (UINT32)(i + 1));
    }

    free(keys);
    flowmapDestroy(m);
}

int main(void) {
    testLookupUpsertRemove();
    testRemovePid();
    testLoopbackV4();
    testDirectionV4();
    testLoopbackV6();
    testDirectionV6();
    testBulkCapacity();
    printf("OK: flowmap\n");
    return 0;
}
