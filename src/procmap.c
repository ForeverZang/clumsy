#include "procmap.h"

#include <iphlpapi.h>
#include <tcpmib.h>
#include <udpmib.h>
#include <stdlib.h>
#include <string.h>

// One fetched IP Helper table (heap buffer must be freed).
typedef struct {
    void  *buf;
    BOOL   tcp;
    BOOL   ipv6;
} RawTable;

// Fetch one table using the documented size-query pattern.
static BOOL fetchTable(ULONG family, BOOL tcp, RawTable *t) {
    DWORD size = 0;
    DWORD rc = tcp
        ? GetExtendedTcpTable(NULL, &size, FALSE, family,
                              TCP_TABLE_OWNER_PID_ALL, 0)
        : GetExtendedUdpTable(NULL, &size, FALSE, family,
                              UDP_TABLE_OWNER_PID, 0);
    t->buf = NULL;
    t->tcp = tcp;
    t->ipv6 = (family == AF_INET6);
    if (rc == NO_ERROR && size == 0) {
        return TRUE; // empty table
    }
    if (rc != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return FALSE;
    }
    void *buf = malloc(size);
    if (buf == NULL) {
        return FALSE;
    }
    rc = tcp
        ? GetExtendedTcpTable(buf, &size, FALSE, family,
                              TCP_TABLE_OWNER_PID_ALL, 0)
        : GetExtendedUdpTable(buf, &size, FALSE, family,
                              UDP_TABLE_OWNER_PID, 0);
    if (rc != NO_ERROR) {
        free(buf);
        return FALSE;
    }
    t->buf = buf;
    return TRUE;
}

// The port DWORD stores a network-order port in its low 16 bits.
static UINT16 mibPort(DWORD dwPort) {
    return (UINT16)(dwPort & 0xFFFFu);
}

static void fillTcp4(ProcEndpoint **cursor,
                     const MIB_TCPROW_OWNER_PID *r) {
    if (r->dwState != MIB_TCP_STATE_ESTAB
        && r->dwState != MIB_TCP_STATE_LISTEN) {
        return;
    }
    ProcEndpoint *e = (*cursor)++;
    memset(e, 0, sizeof(*e));
    e->protocol = IPPROTO_TCP;
    e->localAddr[0] = r->dwLocalAddr;
    e->localPort = mibPort(r->dwLocalPort);
    e->remoteAddr[0] = r->dwRemoteAddr;
    e->remotePort = mibPort(r->dwRemotePort);
    e->pid = r->dwOwningPid;
}

static void fillTcp6(ProcEndpoint **cursor,
                     const MIB_TCP6ROW_OWNER_PID *r) {
    if (r->dwState != MIB_TCP_STATE_ESTAB
        && r->dwState != MIB_TCP_STATE_LISTEN) {
        return;
    }
    ProcEndpoint *e = (*cursor)++;
    memset(e, 0, sizeof(*e));
    e->protocol = IPPROTO_TCP;
    memcpy(e->localAddr, r->ucLocalAddr, 16);
    e->localPort = mibPort(r->dwLocalPort);
    memcpy(e->remoteAddr, r->ucRemoteAddr, 16);
    e->remotePort = mibPort(r->dwRemotePort);
    e->pid = r->dwOwningPid;
}

static void fillUdp4(ProcEndpoint **cursor,
                     const MIB_UDPROW_OWNER_PID *r) {
    ProcEndpoint *e = (*cursor)++;
    memset(e, 0, sizeof(*e));
    e->protocol = IPPROTO_UDP;
    e->localAddr[0] = r->dwLocalAddr;
    e->localPort = mibPort(r->dwLocalPort);
    e->pid = r->dwOwningPid;
}

static void fillUdp6(ProcEndpoint **cursor,
                     const MIB_UDP6ROW_OWNER_PID *r) {
    ProcEndpoint *e = (*cursor)++;
    memset(e, 0, sizeof(*e));
    e->protocol = IPPROTO_UDP;
    memcpy(e->localAddr, r->ucLocalAddr, 16);
    e->localPort = mibPort(r->dwLocalPort);
    e->pid = r->dwOwningPid;
}

static void freeTables(RawTable tables[4]) {
    for (int i = 0; i < 4; i++) {
        free(tables[i].buf);
    }
}

static size_t tableRows(const RawTable *t) {
    if (t->buf == NULL) {
        return 0;
    }
    if (t->tcp) {
        return t->ipv6
            ? ((const MIB_TCP6TABLE_OWNER_PID *)t->buf)->dwNumEntries
            : ((const MIB_TCPTABLE_OWNER_PID *)t->buf)->dwNumEntries;
    }
    return t->ipv6
        ? ((const MIB_UDP6TABLE_OWNER_PID *)t->buf)->dwNumEntries
        : ((const MIB_UDPTABLE_OWNER_PID *)t->buf)->dwNumEntries;
}

static void fillTable(ProcEndpoint **cursor, const RawTable *t) {
    size_t n = tableRows(t);
    if (t->tcp && !t->ipv6) {
        const MIB_TCPTABLE_OWNER_PID *tb = t->buf;
        for (DWORD i = 0; i < n; i++) fillTcp4(cursor, &tb->table[i]);
    } else if (t->tcp) {
        const MIB_TCP6TABLE_OWNER_PID *tb = t->buf;
        for (DWORD i = 0; i < n; i++) fillTcp6(cursor, &tb->table[i]);
    } else if (!t->ipv6) {
        const MIB_UDPTABLE_OWNER_PID *tb = t->buf;
        for (DWORD i = 0; i < n; i++) fillUdp4(cursor, &tb->table[i]);
    } else {
        const MIB_UDP6TABLE_OWNER_PID *tb = t->buf;
        for (DWORD i = 0; i < n; i++) fillUdp6(cursor, &tb->table[i]);
    }
}

static void fillFromTables(RawTable tables[4], ProcEndpoint *out,
                           size_t *outCount) {
    ProcEndpoint *cursor = out;
    for (int i = 0; i < 4; i++) {
        fillTable(&cursor, &tables[i]);
    }
    *outCount = (size_t)(cursor - out);
}

BOOL procmapSnapshotEndpoints(ProcEndpoint **out, size_t *outCount) {
    if (out == NULL || outCount == NULL) {
        return FALSE;
    }
    RawTable tables[4];
    if (!fetchTable(AF_INET, TRUE, &tables[0])
        || !fetchTable(AF_INET6, TRUE, &tables[1])
        || !fetchTable(AF_INET, FALSE, &tables[2])
        || !fetchTable(AF_INET6, FALSE, &tables[3])) {
        freeTables(tables);
        return FALSE;
    }

    size_t maxRows = 0;
    for (int i = 0; i < 4; i++) maxRows += tableRows(&tables[i]);
    ProcEndpoint *eps = malloc(sizeof(ProcEndpoint) * (maxRows + 1));
    if (eps == NULL) {
        freeTables(tables);
        return FALSE;
    }
    fillFromTables(tables, eps, outCount);
    freeTables(tables);
    *out = eps;
    return TRUE;
}

void procmapFree(ProcEndpoint *endpoints) {
    free(endpoints);
}

// Resolve a pid to its image basename (UTF-8) and full wide path.
// Protected/inaccessible processes return FALSE and are skipped.
static BOOL resolveImage(UINT32 pid, char *name, size_t nameCap,
                         wchar_t *path, DWORD pathCap) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL) {
        return FALSE;
    }
    DWORD size = pathCap;
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &size);
    CloseHandle(h);
    if (!ok) {
        return FALSE;
    }
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p != L'\0'; p++) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, base, -1, name,
                                  (int)nameCap, NULL, NULL);
    return len > 0;
}

static AppGroup *findGroup(AppGroup *groups, size_t count,
                          const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (_stricmp(groups[i].exeName, name) == 0) {
            return &groups[i];
        }
    }
    return NULL;
}

static void addPid(AppGroup *g, UINT32 pid) {
    for (UINT i = 0; i < g->pidCount; i++) {
        if (g->pids[i] == pid) return;
    }
    if (g->pidCount < 256) {
        g->pids[g->pidCount++] = pid;
    }
}

static int groupCmp(const void *a, const void *b) {
    return _stricmp(((const AppGroup *)a)->exeName,
                    ((const AppGroup *)b)->exeName);
}

BOOL procmapEnumApps(AppGroup *out, size_t cap, size_t *outCount) {
    if (out == NULL || outCount == NULL || cap == 0) {
        return FALSE;
    }
    ProcEndpoint *eps = NULL;
    size_t epCount = 0;
    if (!procmapSnapshotEndpoints(&eps, &epCount)) {
        return FALSE;
    }

    size_t used = 0;
    for (size_t i = 0; i < epCount; i++) {
        UINT32 pid = eps[i].pid;
        if (pid == 0) continue;

        char name[64];
        wchar_t path[MAX_PATH];
        if (!resolveImage(pid, name, sizeof(name), path, MAX_PATH)) {
            continue;
        }
        AppGroup *g = findGroup(out, used, name);
        if (g == NULL) {
            if (used >= cap) {
                // Report the required count so the caller can grow.
                *outCount = used + 1;
                procmapFree(eps);
                return FALSE;
            }
            g = &out[used++];
            memset(g, 0, sizeof(*g));
            strncpy(g->exeName, name, sizeof(g->exeName) - 1);
            wcsncpy(g->exePath, path, MAX_PATH - 1);
        }
        addPid(g, pid);
    }
    procmapFree(eps);

    qsort(out, used, sizeof(AppGroup), groupCmp);
    *outCount = used;
    return TRUE;
}
