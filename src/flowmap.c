#include "flowmap.h"

#include <stdlib.h>
#include <string.h>

// Power-of-two bucket count keeps load factor under 1 at the required
// >= 8192 flows; collisions chain.
#define FLOWMAP_BUCKETS 16384

typedef struct FlowEntry {
    struct FlowEntry *next;
    FlowKey key;
    UINT32 pid;
} FlowEntry;

struct FlowMap {
    CRITICAL_SECTION lock;
    FlowEntry *buckets[FLOWMAP_BUCKETS];
};

static UINT32 flowHash(const FlowKey *key) {
    const UINT8 *bytes = (const UINT8 *)key;
    // FNV-1a over the (zero-padded) key bytes.
    UINT32 h = 2166136261u;
    for (size_t i = 0; i < sizeof(*key); i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h & (FLOWMAP_BUCKETS - 1);
}

static BOOL flowKeyEqual(const FlowKey *a, const FlowKey *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

// Host-order check for 127.0.0.0/8 or ::1.
BOOL flowmapAddrIsLoopback(const UINT32 addr[4]) {
    if (addr == NULL) {
        return FALSE;
    }
    if ((addr[0] & 0xFF000000u) == 0x7F000000u
        && addr[1] == 0 && addr[2] == 0 && addr[3] == 0) {
        return TRUE;
    }
    if (addr[0] == 0 && addr[1] == 0 && addr[2] == 0 && addr[3] == 1) {
        return TRUE;
    }
    return FALSE;
}

static int flowEndpointCmp(const UINT32 aAddr[4], UINT16 aPort,
                           const UINT32 bAddr[4], UINT16 bPort) {
    for (int i = 0; i < 4; i++) {
        if (aAddr[i] < bAddr[i]) return -1;
        if (aAddr[i] > bAddr[i]) return 1;
    }
    if (aPort < bPort) return -1;
    if (aPort > bPort) return 1;
    return 0;
}

// For loopback flows both ends are local; the lexicographically smaller
// endpoint becomes the key's local end, in both directions.
static void flowNormalizeLoopback(FlowKey *k) {
    if (flowEndpointCmp(k->localAddr, k->localPort,
                        k->remoteAddr, k->remotePort) > 0) {
        UINT32 tmpAddr[4];
        UINT16 tmpPort;
        memcpy(tmpAddr, k->localAddr, sizeof(tmpAddr));
        memcpy(k->localAddr, k->remoteAddr, sizeof(k->localAddr));
        memcpy(k->remoteAddr, tmpAddr, sizeof(k->remoteAddr));
        tmpPort = k->localPort;
        k->localPort = k->remotePort;
        k->remotePort = tmpPort;
    }
}

static void flowConvertEndpoints(FlowKey *k,
                                 const UINT32 srcNet[4], UINT16 srcPortNet,
                                 const UINT32 dstNet[4], UINT16 dstPortNet,
                                 BOOL outbound, BOOL loopback) {
    UINT32 src[4], dst[4];
    for (int i = 0; i < 4; i++) {
        src[i] = ntohl(srcNet[i]);
        dst[i] = ntohl(dstNet[i]);
    }
    UINT16 srcPort = ntohs(srcPortNet);
    UINT16 dstPort = ntohs(dstPortNet);

    if (loopback) {
        if (flowEndpointCmp(src, srcPort, dst, dstPort) <= 0) {
            memcpy(k->localAddr, src, sizeof(src));
            memcpy(k->remoteAddr, dst, sizeof(dst));
            k->localPort = srcPort;
            k->remotePort = dstPort;
        } else {
            memcpy(k->localAddr, dst, sizeof(dst));
            memcpy(k->remoteAddr, src, sizeof(src));
            k->localPort = dstPort;
            k->remotePort = srcPort;
        }
    } else if (outbound) {
        memcpy(k->localAddr, src, sizeof(src));
        memcpy(k->remoteAddr, dst, sizeof(dst));
        k->localPort = srcPort;
        k->remotePort = dstPort;
    } else {
        memcpy(k->localAddr, dst, sizeof(dst));
        memcpy(k->remoteAddr, src, sizeof(src));
        k->localPort = dstPort;
        k->remotePort = srcPort;
    }
}

FlowMap *flowmapCreate(void) {
    FlowMap *map = calloc(1, sizeof(FlowMap));
    if (map == NULL) {
        return NULL;
    }
    InitializeCriticalSection(&map->lock);
    return map;
}

void flowmapDestroy(FlowMap *map) {
    if (map == NULL) {
        return;
    }
    EnterCriticalSection(&map->lock);
    for (size_t b = 0; b < FLOWMAP_BUCKETS; b++) {
        FlowEntry *e = map->buckets[b];
        while (e != NULL) {
            FlowEntry *next = e->next;
            free(e);
            e = next;
        }
    }
    LeaveCriticalSection(&map->lock);
    DeleteCriticalSection(&map->lock);
    free(map);
}

void flowmapUpsert(FlowMap *map, const FlowKey *key, UINT32 pid) {
    if (map == NULL || key == NULL) {
        return;
    }
    UINT32 h = flowHash(key);
    EnterCriticalSection(&map->lock);
    for (FlowEntry *e = map->buckets[h]; e != NULL; e = e->next) {
        if (flowKeyEqual(&e->key, key)) {
            e->pid = pid;
            LeaveCriticalSection(&map->lock);
            return;
        }
    }
    FlowEntry *e = malloc(sizeof(FlowEntry));
    if (e == NULL) {
        // Untracked flow: its packets fail ownership lookup and are
        // allowed to pass, which is the required safe default.
        LeaveCriticalSection(&map->lock);
        return;
    }
    e->key = *key;
    e->pid = pid;
    e->next = map->buckets[h];
    map->buckets[h] = e;
    LeaveCriticalSection(&map->lock);
}

void flowmapRemove(FlowMap *map, const FlowKey *key) {
    if (map == NULL || key == NULL) {
        return;
    }
    UINT32 h = flowHash(key);
    EnterCriticalSection(&map->lock);
    FlowEntry **link = &map->buckets[h];
    while (*link != NULL) {
        if (flowKeyEqual(&(*link)->key, key)) {
            FlowEntry *dead = *link;
            *link = dead->next;
            free(dead);
            LeaveCriticalSection(&map->lock);
            return;
        }
        link = &(*link)->next;
    }
    LeaveCriticalSection(&map->lock);
}

BOOL flowmapLookup(const FlowMap *map, const FlowKey *key, UINT32 *outPid) {
    if (map == NULL || key == NULL) {
        return FALSE;
    }
    // The CRITICAL_SECTION API is not const-correct; a read still takes
    // the lock, so cast away the logical const only for locking.
    FlowMap *self = (FlowMap *)map;
    UINT32 h = flowHash(key);
    EnterCriticalSection(&self->lock);
    for (const FlowEntry *e = self->buckets[h]; e != NULL; e = e->next) {
        if (flowKeyEqual(&e->key, key)) {
            if (outPid != NULL) {
                *outPid = e->pid;
            }
            LeaveCriticalSection(&self->lock);
            return TRUE;
        }
    }
    LeaveCriticalSection(&self->lock);
    return FALSE;
}

void flowmapRemovePid(FlowMap *map, UINT32 pid) {
    if (map == NULL) {
        return;
    }
    EnterCriticalSection(&map->lock);
    for (size_t b = 0; b < FLOWMAP_BUCKETS; b++) {
        FlowEntry **link = &map->buckets[b];
        while (*link != NULL) {
            if ((*link)->pid == pid) {
                FlowEntry *dead = *link;
                *link = dead->next;
                free(dead);
            } else {
                link = &(*link)->next;
            }
        }
    }
    LeaveCriticalSection(&map->lock);
}

void flowmapKeyFromData(const WINDIVERT_DATA_FLOW *data, FlowKey *out) {
    if (data == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->protocol = data->Protocol;
    for (int i = 0; i < 4; i++) {
        out->localAddr[i] = ntohl(data->LocalAddr[i]);
        out->remoteAddr[i] = ntohl(data->RemoteAddr[i]);
    }
    out->localPort = ntohs(data->LocalPort);
    out->remotePort = ntohs(data->RemotePort);
    if (flowmapAddrIsLoopback(out->localAddr)
        || flowmapAddrIsLoopback(out->remoteAddr)) {
        flowNormalizeLoopback(out);
    }
}

void flowmapKeyFromPacket(UINT8 protocol, BOOL outbound, BOOL loopback,
                          const UINT32 *srcAddr, UINT16 srcPort,
                          const UINT32 *dstAddr, UINT16 dstPort,
                          BOOL ipv6, FlowKey *out) {
    (void)ipv6;
    if (srcAddr == NULL || dstAddr == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->protocol = protocol;
    flowConvertEndpoints(out, srcAddr, srcPort, dstAddr, dstPort,
                         outbound, loopback);
}

void flowmapKeyLocalEnd(UINT8 protocol, BOOL loopback,
                        const UINT32 *localAddr, UINT16 localPort,
                        BOOL ipv6, FlowKey *out) {
    static const UINT32 zeroAddr[4] = {0, 0, 0, 0};
    // Same transform seeding uses for a zero-remote row.
    flowmapKeyFromPacket(protocol, TRUE, loopback,
                         localAddr, localPort, zeroAddr, 0, ipv6, out);
}

BOOL flowmapLookupUdpLocal(const FlowMap *map, BOOL outbound, BOOL loopback,
                           const UINT32 *srcAddr, UINT16 srcPort,
                           const UINT32 *dstAddr, UINT16 dstPort,
                           BOOL ipv6, UINT32 *outPid) {
    if (srcAddr == NULL || dstAddr == NULL) {
        return FALSE;
    }
    const UINT32 *localAddr = outbound ? srcAddr : dstAddr;
    UINT16 localPort = outbound ? srcPort : dstPort;
    FlowKey key;
    flowmapKeyLocalEnd(IPPROTO_UDP, loopback, localAddr, localPort,
                       ipv6, &key);
    if (flowmapLookup(map, &key, outPid)) {
        return TRUE;
    }
    if (!loopback) {
        // Unconnected UDP sockets are listed against 0.0.0.0/:: even
        // though packets carry the concrete interface address.
        static const UINT32 wildcard[4] = {0, 0, 0, 0};
        flowmapKeyLocalEnd(IPPROTO_UDP, FALSE, wildcard, localPort,
                           ipv6, &key);
        if (flowmapLookup(map, &key, outPid)) {
            return TRUE;
        }
    }
    return FALSE;
}
