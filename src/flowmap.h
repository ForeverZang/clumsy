#ifndef CLUMSY_FLOWMAP_H
#define CLUMSY_FLOWMAP_H

#include <winsock2.h>
#include <windows.h>

#include "windivert.h"

/*
 * A normalized TCP/UDP 5-tuple. Address words and ports are always stored
 * in HOST byte order. IPv4 uses localAddr[0]/remoteAddr[0] with the rest
 * zero; IPv6 uses all four words. The "local" end is always an endpoint
 * on this machine, so outbound/inbound packets of one flow share a key.
 */
typedef struct {
    UINT8  protocol;          // IPPROTO_TCP / IPPROTO_UDP
    UINT32 localAddr[4];
    UINT16 localPort;
    UINT32 remoteAddr[4];
    UINT16 remotePort;
} FlowKey;

typedef struct FlowMap FlowMap;

FlowMap* flowmapCreate(void);
void     flowmapDestroy(FlowMap *map);

// FLOW_ESTABLISHED event or IP-Helper table seed. Insert/update one flow.
void     flowmapUpsert(FlowMap *map, const FlowKey *key, UINT32 pid);
// FLOW_DELETED event; removing an unknown key is a no-op.
void     flowmapRemove(FlowMap *map, const FlowKey *key);
// Returns TRUE and writes *outPid on hit (outPid may be NULL).
BOOL     flowmapLookup(const FlowMap *map, const FlowKey *key, UINT32 *outPid);
// Drop every flow owned by a process (process exit cleanup).
void     flowmapRemovePid(FlowMap *map, UINT32 pid);

// Host-order test for 127.0.0.0/8 or ::1 over the 4-word (v4 zero-padded)
// address form used by FlowKey. PacketTuple words are network order and
// must be converted (or tested byte-wise) by the caller.
BOOL     flowmapAddrIsLoopback(const UINT32 addr[4]);

// Build a normalized key from a FLOW-layer event record. Input addresses
// and ports are in network byte order as WinDivert reports them.
void     flowmapKeyFromData(const WINDIVERT_DATA_FLOW *data, FlowKey *out);

// Build a normalized key from a parsed NETWORK-layer packet. src/dst
// addresses and ports are in network byte order; `outbound` and `loopback`
// come from WINDIVERT_ADDRESS.
void     flowmapKeyFromPacket(UINT8 protocol, BOOL outbound, BOOL loopback,
                              const UINT32 *srcAddr, UINT16 srcPort,
                              const UINT32 *dstAddr, UINT16 dstPort,
                              BOOL ipv6, FlowKey *out);

#endif // CLUMSY_FLOWMAP_H
