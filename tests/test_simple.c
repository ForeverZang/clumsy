// Unit tests for the beginner loss module:
// 0% keeps everything; 100% turns TCP into RST and drops UDP/other;
// below 100% is a per-packet random drop by volume; untargeted packets
// are always left alone.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "common.h"
#include "targeting.h"
#include "simple.h"

// intrusive list anchors owned by packet.c
extern PacketNode * const head;
extern PacketNode * const tail;

static void put16(unsigned char *p, UINT16 v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFF);
}

static PacketNode *makeTcpNode(void) {
    unsigned char buf[40];
    WINDIVERT_ADDRESS addr;
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x45;
    put16(buf + 2, 40);
    buf[8] = 64;
    buf[9] = 6;
    *(UINT32 *)(buf + 12) = htonl(0x01020304u);
    *(UINT32 *)(buf + 16) = htonl(0x05060708u);
    put16(buf + 20, 1234);
    put16(buf + 22, 443);
    buf[32] = 0x50;
    memset(&addr, 0, sizeof(addr));
    addr.Outbound = 1;
    return createNode((char *)buf, sizeof(buf), &addr);
}

static PacketNode *makeUdpNode(void) {
    unsigned char buf[28];
    WINDIVERT_ADDRESS addr;
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x45;
    put16(buf + 2, 28);
    buf[8] = 64;
    buf[9] = 17;
    *(UINT32 *)(buf + 12) = htonl(0x01020304u);
    *(UINT32 *)(buf + 16) = htonl(0x05060708u);
    put16(buf + 20, 1234);
    put16(buf + 22, 53);
    put16(buf + 24, 8);
    memset(&addr, 0, sizeof(addr));
    addr.Outbound = 1;
    return createNode((char *)buf, sizeof(buf), &addr);
}

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

static void drainList(void) {
    while (!isListEmpty()) {
        freeNode(popNode(head->next));
    }
}

static void parseTcp(PacketNode *node, PWINDIVERT_TCPHDR *tcp,
                     PWINDIVERT_IPHDR *ip) {
    WinDivertHelperParsePacket(node->packet, node->packetLen, ip, NULL, NULL,
                               NULL, NULL, tcp, NULL, NULL, NULL, NULL, NULL);
}

int main(void) {
    PacketNode *n;
    PWINDIVERT_TCPHDR tcp;
    PWINDIVERT_IPHDR ip;
    const char *targets[] = { "a.exe" };

    initPacketNodeList();
    targetingSetOwnerLookup(fakeOwnerHit);
    targetingSetPidNameResolver(fakeName);
    targetingClear();
    *simpleModule.enabledFlag = 1;
    simpleSetLoss(0);

    // 1. loss 0, no restriction: packet kept, RST clear
    n = makeTcpNode();
    appendNode(n);
    assert(simpleModule.process(head, tail) == 0);
    assert(!isListEmpty());
    parseTcp(n, &tcp, &ip);
    assert(tcp != NULL && tcp->Rst == 0);
    drainList();

    // 2. loss 10000, TCP: kept in list, RST set, checksums recomputed
    simpleSetLoss(10000);
    n = makeTcpNode();
    appendNode(n);
    assert(simpleModule.process(head, tail) != 0);
    assert(!isListEmpty()); // RST packet is reinjected, not dropped
    parseTcp(n, &tcp, &ip);
    assert(tcp->Rst == 1);
    assert(tcp->Checksum != 0);
    assert(ip->Checksum != 0);
    drainList();

    // 3. loss 10000, UDP: dropped outright, list empties
    n = makeUdpNode();
    appendNode(n);
    simpleModule.process(head, tail);
    assert(isListEmpty());

    // 4. Security rule: targeted but owner miss -> packet survives, no RST
    targetingSetImages(targets, 1);
    targetingSetOwnerLookup(fakeOwnerMiss);
    simpleSetLoss(10000);
    n = makeTcpNode();
    appendNode(n);
    assert(simpleModule.process(head, tail) == 0);
    assert(!isListEmpty());
    parseTcp(n, &tcp, &ip);
    assert(tcp->Rst == 0);
    drainList();

    // 5. Partial loss is per-packet by volume: 50% over a large batch must
    // land well away from 0% and 100%.
    targetingSetOwnerLookup(fakeOwnerHit);
    simpleSetLoss(5000);
    srand(1234);
    {
        const int N = 4000;
        int kept = 0;
        for (int i = 0; i < N; i++) {
            appendNode(makeUdpNode());
        }
        simpleModule.process(head, tail);
        PacketNode *p = head->next;
        while (p != tail) {
            kept++;
            p = p->next;
        }
        assert(kept > N / 2 - 400 && kept < N / 2 + 400);
        drainList();
    }

    // 6. closeDown idempotent
    simpleModule.closeDown(head, tail);
    simpleModule.closeDown(head, tail);

    targetingClear();
    printf("OK: simple\n");
    return 0;
}
