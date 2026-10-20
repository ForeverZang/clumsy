// Unit tests for the software WinDivert filter: enable/disable, valid and
// invalid compiles, per-packet evaluation against raw IPv4 packets, and
// state preservation after a failed recompile.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "swfilter.h"

// Minimal 28-byte IPv4 packet (20-byte header + 8-byte L4 placeholder).
// Only version/IHL, protocol and addresses matter to the filters used.
static UINT makePacket(char *buf, UINT8 protocol) {
    memset(buf, 0, 28);
    buf[0] = 0x45; // IPv4, IHL 5
    buf[2] = 0x00; buf[3] = 28; // total length
    buf[9] = protocol;
    *(UINT32 *)(buf + 12) = htonl(0x0a000001u); // 10.0.0.1
    *(UINT32 *)(buf + 16) = htonl(0x0a000002u); // 10.0.0.2
    if (protocol == IPPROTO_UDP) {
        buf[24] = 0x00; buf[25] = 8; // UDP length
    }
    return 28;
}

static WINDIVERT_ADDRESS makeAddr(BOOL outbound) {
    WINDIVERT_ADDRESS addr;
    memset(&addr, 0, sizeof(addr));
    addr.Layer = WINDIVERT_LAYER_NETWORK;
    addr.Outbound = outbound ? 1 : 0;
    return addr;
}

int main(void) {
    char err[MSG_BUFSIZE];
    char udpPacket[64];
    char tcpPacket[64];
    UINT udpLen = makePacket(udpPacket, IPPROTO_UDP);
    UINT tcpLen = makePacket(tcpPacket, IPPROTO_TCP);
    WINDIVERT_ADDRESS outAddr = makeAddr(TRUE);
    WINDIVERT_ADDRESS inAddr = makeAddr(FALSE);

    // 1. Default and empty filters are inactive: every packet matches.
    assert(!swfilterActive());
    assert(swfilterMatch(udpPacket, udpLen, &outAddr));
    assert(swfilterSet("", err));
    assert(!swfilterActive());

    // 2. A valid filter compiles and distinguishes packets by protocol.
    assert(swfilterSet("udp", err));
    assert(swfilterActive());
    assert(swfilterMatch(udpPacket, udpLen, &outAddr));
    assert(!swfilterMatch(tcpPacket, tcpLen, &outAddr));

    // 3. Direction filters evaluate the captured address.
    assert(swfilterSet("outbound", err));
    assert(!swfilterMatch(udpPacket, udpLen, &inAddr));
    assert(swfilterMatch(udpPacket, udpLen, &outAddr));

    // 4. Invalid syntax fails, reports a message, and keeps the old filter.
    assert(!swfilterSet("this is not a filter !!!", err));
    assert(err[0] != '\0');
    assert(swfilterActive());
    assert(swfilterMatch(udpPacket, udpLen, &outAddr));
    assert(!swfilterMatch(udpPacket, udpLen, &inAddr));

    // 5. NULL packet on an active filter is a non-match (fail safe: the
    //    packet stays untouched), never a crash.
    assert(!swfilterMatch(NULL, udpLen, &outAddr));

    // 6. Clearing disables the gate.
    swfilterClear();
    assert(!swfilterActive());
    assert(swfilterMatch(tcpPacket, tcpLen, &inAddr));

    printf("OK: swfilter\n");
    return 0;
}
