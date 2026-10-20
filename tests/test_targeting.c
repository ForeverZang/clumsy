// Unit tests for the unified shouldAffect gate.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "targeting.h"

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

static PacketNode makeNode(BOOL outbound, BOOL valid) {
    PacketNode node;
    memset(&node, 0, sizeof(node));
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

    printf("OK: targeting\n");
    return 0;
}
