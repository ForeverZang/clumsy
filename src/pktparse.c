#include <string.h>

#include "pktparse.h"
#include "windivert.h"

BOOL pktparseTuple(const char *packet, UINT len, PacketTuple *out) {
    PWINDIVERT_IPHDR ip = NULL;
    PWINDIVERT_IPV6HDR ip6 = NULL;
    PWINDIVERT_TCPHDR tcp = NULL;
    PWINDIVERT_UDPHDR udp = NULL;
    BOOL parsed;

    if (packet == NULL || out == NULL || len == 0) {
        return FALSE;
    }

    parsed = WinDivertHelperParsePacket(packet, len, &ip, &ip6, NULL,
                                       NULL, NULL, &tcp, &udp,
                                       NULL, NULL, NULL, NULL);
    if (!parsed || (ip == NULL && ip6 == NULL)) {
        return FALSE;
    }

    memset(out, 0, sizeof(*out));

    if (ip != NULL) {
        out->ipv6 = FALSE;
        out->aAddr[0] = ip->SrcAddr;
        out->bAddr[0] = ip->DstAddr;
    } else {
        out->ipv6 = TRUE;
        memcpy(out->aAddr, ip6->SrcAddr, sizeof(out->aAddr));
        memcpy(out->bAddr, ip6->DstAddr, sizeof(out->bAddr));
    }

    // Ownership tracking covers TCP/UDP only; everything else has no
    // 5-tuple available, must not be claimed by the targeting layer.
    // Protocol is the parsed L4 one, not ip6->NextHdr, so IPv6 packets
    // with extension headers key as TCP/UDP like the FLOW events do.
    if (tcp != NULL) {
        out->protocol = IPPROTO_TCP;
        out->aPort = tcp->SrcPort;
        out->bPort = tcp->DstPort;
    } else if (udp != NULL) {
        out->protocol = IPPROTO_UDP;
        out->aPort = udp->SrcPort;
        out->bPort = udp->DstPort;
    } else {
        return FALSE;
    }

    return TRUE;
}
