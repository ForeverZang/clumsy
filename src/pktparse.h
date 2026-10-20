#ifndef CLUMSY_PKTPARSE_H
#define CLUMSY_PKTPARSE_H

#include <windows.h>

/*
 * Parsed 5-tuple of one NETWORK-layer packet.
 * All address words and ports stay in NETWORK byte order so the result
 * feeds flowmapKeyFromPacket() directly; `a` is the source end and `b`
 * the destination end as they appear on the wire.
 */
typedef struct {
    UINT8  protocol;      // IPPROTO_TCP / IPPROTO_UDP
    UINT32 aAddr[4];      // source address (IPv4 uses [0])
    UINT32 bAddr[4];      // destination address
    UINT16 aPort;         // source port
    UINT16 bPort;         // destination port
    BOOL   ipv6;
} PacketTuple;

// Parse a raw IPv4/IPv6 TCP/UDP packet. Returns FALSE for malformed input
// and for non TCP/UDP packets (no 5-tuple available -> fail-open upstream).
BOOL pktparseTuple(const char *packet, UINT len, PacketTuple *out);

#endif // CLUMSY_PKTPARSE_H
