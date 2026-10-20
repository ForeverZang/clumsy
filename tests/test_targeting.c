// Unit tests for the unified shouldAffect gate.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "targeting.h"
#include "swfilter.h"

// ---- injection stubs ------------------------------------------------------

static int g_ownerCalls = 0;
static BOOL g_ownerResult = FALSE;
static UINT32 g_ownerPid = 42;

static BOOL fakeOwner(const PacketNode *node, UINT32 *outPid) {
    (void)node;
    g_ownerCalls++;
    if (g_ownerResult) {
        *outPid = g_ownerPid;
        return TRUE;
    }
    return FALSE;
}

static const char *g_resolvedName = "a.exe";

static BOOL fakePidName(UINT32 pid, char *outName, size_t cap) {
    (void)pid;
    if (g_resolvedName == NULL) {
        return FALSE;
    }
    strncpy(outName, g_resolvedName, cap - 1);
    outName[cap - 1] = '\0';
    return TRUE;
}

// ---- node builders --------------------------------------------------------

// Minimal valid IPv4 TCP packet (20 IP + 20 TCP) for software-filter
// evaluation and raw-packet ownership parsing.
static unsigned char g_tcpPacket[40] = {
    0x45, 0x00, 0x00, 0x28, 0, 0, 0, 0, 0, 6, 0, 0,
    10, 0, 0, 1, 10, 0, 0, 2,
    0, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x50, 0, 0, 0, 0, 0, 0, 0
};

static PacketNode makeNode(BOOL outbound, BOOL valid) {
    PacketNode node;
    memset(&node, 0, sizeof(node));
    node.packet = (char *)g_tcpPacket;
    node.packetLen = sizeof(g_tcpPacket);
    node.addr.Outbound = outbound ? 1 : 0;
    node.tupleValid = valid ? TRUE : FALSE;
    node.tuple.protocol = IPPROTO_TCP;
    node.tuple.aAddr[0] = htonl(0x01020304u);
    node.tuple.bAddr[0] = htonl(0x05060708u);
    node.tuple.aPort = htons(1234);
    node.tuple.bPort = htons(443);
    return node;
}

int main(void) {
    PacketNode out = makeNode(TRUE, TRUE);
    PacketNode in = makeNode(FALSE, TRUE);
    PacketNode bad = makeNode(TRUE, FALSE);
    const char *targets[] = { "a.exe" };
    const char *upper[] = { "CHROME.EXE" };

    // Baseline: both global directions on.
    targetingSetDirection(1, 1);
    targetingSetOwnerLookup(fakeOwner);
    targetingSetPidNameResolver(fakePidName);

    // 1. No restriction: TRUE, owner never consulted.
    targetingClear();
    g_ownerCalls = 0;
    assert(shouldAffect(&out, 1, 1) == TRUE);
    assert(shouldAffect(&in, 1, 1) == TRUE);
    assert(g_ownerCalls == 0);

    // Direction gate still applies with no restriction.
    assert(shouldAffect(&out, 1, 0) == FALSE);
    assert(shouldAffect(&in, 0, 1) == FALSE);

    // 2. Targeted, unowned -> FALSE (safety rule, any direction)
    g_ownerResult = FALSE;
    targetingSetImages(targets, 1);
    g_ownerCalls = 0;
    assert(shouldAffect(&out, 1, 1) == FALSE);
    assert(shouldAffect(&in, 1, 1) == FALSE);
    assert(g_ownerCalls == 2);

    // 3. Targeted, owned but name resolves to a different image -> FALSE
    g_ownerResult = TRUE;
    g_resolvedName = "z.exe";
    assert(shouldAffect(&out, 1, 1) == FALSE);

    // 4. Unparseable packet with an owner "hit" still fails closed.
    g_resolvedName = "a.exe";
    assert(shouldAffect(&bad, 1, 1) == FALSE);

    // 5. Hit case: owned + matching name -> full 2x2x2 direction matrix:
    // global in/out x module in/out x packet direction.
    for (int gi = 0; gi <= 1; gi++) {
        for (int go = 0; go <= 1; go++) {
            for (int mi = 0; mi <= 1; mi++) {
                for (int mo = 0; mo <= 1; mo++) {
                    short wantIn = (short)(gi && mi);
                    short wantOut = (short)(go && mo);
                    targetingSetDirection((short)gi, (short)go);
                    assert(shouldAffect(&in, (short)mi, (short)mo)
                           == (BOOL)(wantIn != 0));
                    assert(shouldAffect(&out, (short)mi, (short)mo)
                           == (BOOL)(wantOut != 0));
                }
            }
        }
    }
    targetingSetDirection(1, 1);

    // 6. Runtime target flip on the SAME node (a.exe -> b.exe -> a.exe).
    {
        const char *bTargets[] = { "b.exe" };
        g_resolvedName = "a.exe";
        assert(shouldAffect(&out, 1, 1) == TRUE);
        targetingSetImages(bTargets, 1);
        assert(shouldAffect(&out, 1, 1) == FALSE);
        targetingSetImages(targets, 1);
        assert(shouldAffect(&out, 1, 1) == TRUE);
    }

    // 7. Case-insensitive image name matching.
    g_resolvedName = "chrome.exe";
    targetingSetImages(upper, 1);
    assert(shouldAffect(&out, 1, 1) == TRUE);

    // 8. Resolver failure fails closed.
    g_resolvedName = NULL;
    targetingSetImages(targets, 1);
    assert(shouldAffect(&out, 1, 1) == FALSE);

    // 9. Clear removes the restriction again.
    g_resolvedName = "a.exe";
    targetingClear();
    g_ownerCalls = 0;
    assert(shouldAffect(&out, 1, 1) == TRUE);
    assert(g_ownerCalls == 0);

    // 10. The advanced software filter is ANDed after the ownership gate.
    char err[MSG_BUFSIZE];
    // No target but a filter rejecting TCP -> still FALSE.
    assert(swfilterSet("udp", err));
    assert(shouldAffect(&out, 1, 1) == FALSE);
    // Owner hit is gated by the filter as well.
    targetingSetImages(targets, 1);
    assert(shouldAffect(&out, 1, 1) == FALSE);
    // Direction filter: outbound hit passes, inbound rejected.
    assert(swfilterSet("outbound", err));
    assert(shouldAffect(&out, 1, 1) == TRUE);
    assert(shouldAffect(&in, 1, 1) == FALSE);
    // Invalid compile keeps the previous outbound filter.
    assert(swfilterSet("!!!", err) == FALSE);
    assert(err[0] != '\0');
    assert(shouldAffect(&in, 1, 1) == FALSE);
    // Clearing the filter restores the ownership-only result.
    swfilterClear();
    assert(shouldAffect(&out, 1, 1) == TRUE);
    targetingClear();

    // 11. targetingOwnsPacket: chart membership is ownership only. It takes
    // a raw packet and ignores the global direction gate and software
    // filter, so the chart keeps both directions and survives filter text.
    WINDIVERT_ADDRESS rawOut;
    memset(&rawOut, 0, sizeof(rawOut));
    rawOut.Outbound = 1;
    g_ownerResult = TRUE;
    g_resolvedName = "a.exe";
    targetingClear();
    g_ownerCalls = 0;
    assert(targetingOwnsPacket((const char *)g_tcpPacket, sizeof(g_tcpPacket),
                               &rawOut) == TRUE);
    assert(g_ownerCalls == 0); // no images: short-circuit, owner untouched

    targetingSetDirection(1, 0); // gate allows inbound only...
    targetingSetImages(targets, 1);
    // ...but membership is independent of direction.
    assert(targetingOwnsPacket((const char *)g_tcpPacket, sizeof(g_tcpPacket),
                               &rawOut) == TRUE);
    // ...and independent of the software filter.
    assert(swfilterSet("udp", err)); // rejects the TCP fixture
    assert(targetingOwnsPacket((const char *)g_tcpPacket, sizeof(g_tcpPacket),
                               &rawOut) == TRUE);
    swfilterClear();

    // Owner miss fails closed for the chart too.
    g_ownerResult = FALSE;
    assert(targetingOwnsPacket((const char *)g_tcpPacket, sizeof(g_tcpPacket),
                               &rawOut) == FALSE);
    g_ownerResult = TRUE;

    // A malformed packet can never be claimed.
    {
        unsigned char garbage[8] = {0};
        assert(targetingOwnsPacket((const char *)garbage, sizeof(garbage),
                                   &rawOut) == FALSE);
    }
    assert(targetingOwnsPacket(NULL, 0, &rawOut) == FALSE);
    targetingClear();
    targetingSetDirection(1, 1);

    printf("OK: targeting\n");
    return 0;
}
