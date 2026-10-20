// Unit tests for pktparseTuple: raw NETWORK-layer IP packets -> PacketTuple.
// Packets are hand-built (pseudo headers/checksums need not be valid;
// WinDivertHelperParsePacket only reads structure).
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "pktparse.h"

static void put16(unsigned char *p, UINT16 v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFF);
}

// 20-byte IPv4 header + 8-byte UDP header, total 28
static size_t buildUdp4(char *buf, UINT32 src, UINT32 dst,
                        UINT16 sp, UINT16 dp, int ihlWords) {
    size_t ipLen = (size_t)ihlWords * 4;
    size_t total = ipLen + 8;
    unsigned char *p = (unsigned char *)buf;
    memset(buf, 0, total);
    p[0] = (unsigned char)(0x40 | ihlWords);
    put16(p + 2, (UINT16)total);
    p[8] = 64;  // ttl
    p[9] = 17;  // UDP
    memcpy(p + 12, &src, 4);
    memcpy(p + 16, &dst, 4);
    put16(p + ipLen, sp);
    put16(p + ipLen + 2, dp);
    put16(p + ipLen + 4, 8);
    return total;
}

// 20-byte IPv4 header + 20-byte minimal TCP header, total 40
static size_t buildTcp4(char *buf, UINT32 src, UINT32 dst,
                        UINT16 sp, UINT16 dp) {
    unsigned char *p = (unsigned char *)buf;
    size_t total = 40;
    memset(buf, 0, total);
    p[0] = 0x45;
    put16(p + 2, (UINT16)total);
    p[8] = 64;
    p[9] = 6;  // TCP
    memcpy(p + 12, &src, 4);
    memcpy(p + 16, &dst, 4);
    put16(p + 20, sp);
    put16(p + 22, dp);
    p[32] = 0x50;  // data offset 5
    return total;
}

// 40-byte IPv6 header + 8-byte UDP
static size_t buildUdp6(char *buf, const UINT32 src[4], const UINT32 dst[4],
                        UINT16 sp, UINT16 dp) {
    unsigned char *p = (unsigned char *)buf;
    memset(buf, 0, 48);
    p[0] = 0x60;
    put16(p + 4, 8);  // payload length
    p[6] = 17;        // next header UDP
    p[7] = 64;        // hop limit
    memcpy(p + 8, src, 16);
    memcpy(p + 24, dst, 16);
    put16(p + 40, sp);
    put16(p + 42, dp);
    put16(p + 44, 8);
    return 48;
}

int main(void) {
    char buf[128];
    PacketTuple t;
    size_t len;
    UINT32 src4 = htonl(0x01020304u), dst4 = htonl(0x05060708u);
    UINT32 src6[4] = { htonl(0x20010DB8u), 0, 0, htonl(1u) };
    UINT32 dst6[4] = { 0, 0, 0, htonl(2u) };

    // IPv4/UDP
    len = buildUdp4(buf, src4, dst4, 1234, 53, 5);
    assert(pktparseTuple(buf, (UINT)len, &t));
    assert(t.protocol == 17);
    assert(t.ipv6 == FALSE);
    assert(t.aAddr[0] == src4 && t.bAddr[0] == dst4);
    assert(t.aAddr[1] == 0 && t.bAddr[3] == 0);
    assert(t.aPort == htons(1234));
    assert(t.bPort == htons(53));

    // IPv4/TCP
    len = buildTcp4(buf, src4, dst4, 80, 8080);
    assert(pktparseTuple(buf, (UINT)len, &t));
    assert(t.protocol == 6);
    assert(t.ipv6 == FALSE);
    assert(t.aAddr[0] == src4 && t.bAddr[0] == dst4);
    assert(t.aPort == htons(80));
    assert(t.bPort == htons(8080));

    // IPv6/UDP
    len = buildUdp6(buf, src6, dst6, 443, 5000);
    assert(pktparseTuple(buf, (UINT)len, &t));
    assert(t.protocol == 17);
    assert(t.ipv6 == TRUE);
    assert(memcmp(t.aAddr, src6, 16) == 0);
    assert(memcmp(t.bAddr, dst6, 16) == 0);
    assert(t.aPort == htons(443));
    assert(t.bPort == htons(5000));

    // IPv4 with 4 option bytes: UDP header sits at offset 24
    len = buildUdp4(buf, src4, dst4, 1000, 2000, 6);
    assert(pktparseTuple(buf, (UINT)len, &t));
    assert(t.protocol == 17);
    assert(t.aPort == htons(1000));
    assert(t.bPort == htons(2000));

    // Malformed packets must return FALSE, never crash
    assert(!pktparseTuple(NULL, 10, &t));
    memset(buf, 0, sizeof(buf));
    assert(!pktparseTuple(buf, 3, &t));             // too short for IP
    buf[0] = 0x00;
    assert(!pktparseTuple(buf, 40, &t));            // not IP v4/v6
    len = buildTcp4(buf, src4, dst4, 80, 8080);
    assert(!pktparseTuple(buf, (UINT)len - 20, &t)); // IP only, TCP absent
    assert(!pktparseTuple(buf, (UINT)len, NULL));

    printf("OK: pktparse\n");
    return 0;
}
