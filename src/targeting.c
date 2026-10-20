#include <stdlib.h>
#include <string.h>

// procmap.h pulls in winsock2; include before targeting.h/windows.h
#include "procmap.h"
#include "targeting.h"
#include "swfilter.h"

// Beginner mode never tracks more than a handful of picked images.
#define TARGET_MAX_IMAGES 32
#define IMAGE_NAME_LEN 64
#define NAME_REFRESH_MS 2000
// procmapEnumApps cap; enlarged dynamically if the table reports more.
#define APP_ENUM_CAP 128

typedef struct {
    char (*names)[IMAGE_NAME_LEN];
    size_t count;
} ImageSet;

typedef struct {
    UINT32 pid;
    char name[IMAGE_NAME_LEN];
} PidNameEntry;

typedef struct {
    PidNameEntry *items;
    size_t count;
} PidNameCache;

static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_imageLock; // guards g_images + refresh thread state
static CRITICAL_SECTION g_cacheLock; // guards g_pidNameCache swaps/reads
static ImageSet *volatile g_images = NULL;
static PidNameCache *volatile g_pidNameCache = NULL;

static volatile short g_dirIn = 1;
static volatile short g_dirOut = 1;
static volatile TargetingOwnerFn g_ownerFn = NULL;
static volatile TargetingPidNameFn g_pidNameFn = NULL;

static HANDLE g_refreshThread = NULL;
static HANDLE g_refreshStop = NULL;
static volatile short g_refreshRunning = 0;

// Direction predicate kept private here; modules go through shouldAffect.
static INLINE_FUNCTION
BOOL checkDirection(BOOL outboundPacket, short handleInbound,
                    short handleOutbound) {
    return (handleInbound && !outboundPacket)
           || (handleOutbound && outboundPacket);
}

static BOOL WINAPI targetingInit(INIT_ONCE *once, PVOID param,
                                 PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_imageLock);
    InitializeCriticalSection(&g_cacheLock);
    return TRUE;
}

static void targetingEnsureInit(void) {
    InitOnceExecuteOnce(&g_initOnce, targetingInit, NULL, NULL);
}

static void freeImageSet(ImageSet *set) {
    if (set == NULL) {
        return;
    }
    free(set->names);
    free(set);
}

static int pidNameCmp(const void *a, const void *b) {
    const PidNameEntry *ea = (const PidNameEntry *)a;
    const PidNameEntry *eb = (const PidNameEntry *)b;
    if (ea->pid < eb->pid) return -1;
    if (ea->pid > eb->pid) return 1;
    return 0;
}

// Built-in pid -> image name resolver, backed by the periodic procmap
// cache. Holds g_cacheLock for the lookup (swap latency is microseconds).
static BOOL builtinPidName(UINT32 pid, char *outName, size_t cap) {
    PidNameEntry key = { pid, {0} };
    const PidNameEntry *found;
    BOOL result = FALSE;
    if (cap == 0) {
        return FALSE;
    }
    EnterCriticalSection(&g_cacheLock);
    if (g_pidNameCache != NULL && g_pidNameCache->items != NULL) {
        found = (const PidNameEntry *)bsearch(
            &key, g_pidNameCache->items, g_pidNameCache->count,
            sizeof(PidNameEntry), pidNameCmp);
        if (found != NULL) {
            strncpy(outName, found->name, cap - 1);
            outName[cap - 1] = '\0';
            result = TRUE;
        }
    }
    LeaveCriticalSection(&g_cacheLock);
    return result;
}

// Rebuild the flat sorted pid -> name table from a procmap app snapshot.
static void refreshPidNameCache(void) {
    size_t groupCount = 0;
    AppGroup *groups;
    PidNameCache *next;
    PidNameEntry *items;
    size_t cap = APP_ENUM_CAP, total = 0, i, j;

    groups = (AppGroup *)calloc(cap, sizeof(AppGroup));
    if (groups == NULL) {
        return;
    }
    if (!procmapEnumApps(groups, cap, &groupCount) && groupCount > cap) {
        AppGroup *bigger = (AppGroup *)realloc(groups,
                                               groupCount * sizeof(AppGroup));
        if (bigger == NULL) {
            free(groups);
            return;
        }
        groups = bigger;
        cap = groupCount;
        if (!procmapEnumApps(groups, cap, &groupCount)) {
            free(groups);
            return;
        }
    }

    for (i = 0; i < groupCount; i++) {
        total += groups[i].pidCount;
    }
    items = (PidNameEntry *)calloc(total ? total : 1,
                                   sizeof(PidNameEntry));
    next = (PidNameCache *)calloc(1, sizeof(*next));
    if (items == NULL || next == NULL) {
        free(items);
        free(next);
        free(groups);
        return;
    }
    {
        size_t put = 0;
        for (i = 0; i < groupCount; i++) {
            for (j = 0; j < groups[i].pidCount; j++) {
                items[put].pid = groups[i].pids[j];
                strncpy(items[put].name, groups[i].exeName,
                        IMAGE_NAME_LEN - 1);
                items[put].name[IMAGE_NAME_LEN - 1] = '\0';
                put++;
            }
        }
        next->items = items;
        next->count = total;
    }
    qsort(next->items, next->count, sizeof(PidNameEntry), pidNameCmp);

    EnterCriticalSection(&g_cacheLock);
    {
        PidNameCache *old = (PidNameCache *)g_pidNameCache;
        g_pidNameCache = next;
        next = old;
    }
    LeaveCriticalSection(&g_cacheLock);
    if (next != NULL) {
        free(next->items);
        free(next);
    }
    free(groups);
}

static DWORD WINAPI refreshLoop(LPVOID arg) {
    (void)arg;
    refreshPidNameCache(); // populate immediately so targeting is prompt
    while (WaitForSingleObject(g_refreshStop, NAME_REFRESH_MS)
           == WAIT_TIMEOUT) {
        refreshPidNameCache();
    }
    return 0;
}

// Caller must hold g_imageLock.
static void startRefreshLocked(void) {
    if (g_refreshRunning) {
        return;
    }
    g_refreshStop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_refreshStop == NULL) {
        return;
    }
    g_refreshThread = CreateThread(NULL, 0, refreshLoop, NULL, 0, NULL);
    if (g_refreshThread == NULL) {
        CloseHandle(g_refreshStop);
        g_refreshStop = NULL;
        return;
    }
    g_refreshRunning = 1;
}

// Joins, so caller must NOT hold g_imageLock (refresh takes it briefly).
static void stopRefresh(void) {
    HANDLE thread;
    HANDLE stop;
    EnterCriticalSection(&g_imageLock);
    if (!g_refreshRunning) {
        LeaveCriticalSection(&g_imageLock);
        return;
    }
    g_refreshRunning = 0;
    thread = g_refreshThread;
    stop = g_refreshStop;
    g_refreshThread = NULL;
    g_refreshStop = NULL;
    LeaveCriticalSection(&g_imageLock);

    SetEvent(stop);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(stop);
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

void targetingSetOwnerLookup(TargetingOwnerFn fn) {
    targetingEnsureInit();
    InterlockedExchangePointer((PVOID volatile *)&g_ownerFn, (PVOID)fn);
}

void targetingSetPidNameResolver(TargetingPidNameFn fn) {
    targetingEnsureInit();
    InterlockedExchangePointer((PVOID volatile *)&g_pidNameFn, (PVOID)fn);
}

void targetingClear(void) {
    ImageSet *old;
    targetingEnsureInit();
    EnterCriticalSection(&g_imageLock);
    old = (ImageSet *)g_images;
    g_images = NULL;
    LeaveCriticalSection(&g_imageLock);
    freeImageSet(old);
    stopRefresh();
    InterlockedExchange16(&g_dirIn, 1);
    InterlockedExchange16(&g_dirOut, 1);
}

void targetingSetImages(const char *const *names, size_t count) {
    ImageSet *next, *old;
    size_t i;
    BOOL needThread;

    targetingEnsureInit();
    if (count > TARGET_MAX_IMAGES) {
        count = TARGET_MAX_IMAGES;
    }

    next = (ImageSet *)calloc(1, sizeof(*next));
    if (next == NULL) {
        return;
    }
    if (count > 0) {
        next->count = count;
        next->names = (char(*)[IMAGE_NAME_LEN])calloc(count,
                                                      IMAGE_NAME_LEN);
        if (next->names == NULL) {
            free(next);
            return;
        }
        for (i = 0; i < count; i++) {
            strncpy(next->names[i], names[i] ? names[i] : "",
                    IMAGE_NAME_LEN - 1);
        }
    }

    EnterCriticalSection(&g_imageLock);
    old = (ImageSet *)g_images;
    g_images = next;
    needThread = count > 0 && !g_refreshRunning;
    if (needThread) {
        startRefreshLocked();
    }
    LeaveCriticalSection(&g_imageLock);

    freeImageSet(old);
    if (count == 0) {
        // Empty selection is the same as no restriction.
        stopRefresh();
    }
}

void targetingSetDirection(short in, short out) {
    targetingEnsureInit();
    InterlockedExchange16(&g_dirIn, in);
    InterlockedExchange16(&g_dirOut, out);
}

// Ownership-only leg shared by shouldAffect (intercept gate) and
// targetingOwnsPacket (chart membership). No direction check, no software
// filter. No target selected short-circuits TRUE; anything unparseable,
// unowned, or owned by a different image returns FALSE.
static BOOL ownsNode(const PacketNode *node) {
    TargetingOwnerFn ownerFn;
    TargetingPidNameFn nameFn;
    const ImageSet *images;
    UINT32 pid = 0;
    char imageName[IMAGE_NAME_LEN];
    BOOL gotName;
    size_t i;
    BOOL matched = FALSE;

    if (node == NULL) {
        return FALSE;
    }

    targetingEnsureInit();
    EnterCriticalSection(&g_imageLock);
    images = (const ImageSet *)g_images;
    if (images == NULL || images->count == 0) {
        // No app targeted: short-circuit, ownership is never consulted.
        LeaveCriticalSection(&g_imageLock);
        return TRUE;
    }

    // Security rule: anything without a usable 5-tuple cannot be claimed.
    if (!node->tupleValid) {
        LeaveCriticalSection(&g_imageLock);
        return FALSE;
    }
    ownerFn = (TargetingOwnerFn)g_ownerFn;
    if (ownerFn == NULL || !ownerFn(node, &pid)) {
        LeaveCriticalSection(&g_imageLock);
        return FALSE;
    }

    nameFn = (TargetingPidNameFn)g_pidNameFn;
    gotName = nameFn != NULL
                  ? nameFn(pid, imageName, sizeof(imageName))
                  : builtinPidName(pid, imageName, sizeof(imageName));
    if (gotName) {
        for (i = 0; i < images->count; i++) {
            if (_stricmp(imageName, images->names[i]) == 0) {
                matched = TRUE;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_imageLock);
    return matched;
}

BOOL shouldAffect(const PacketNode *node, short modIn, short modOut) {
    if (node == NULL) {
        return FALSE;
    }
    // Global direction intersects the module's own direction checkboxes.
    if (!checkDirection(node->addr.Outbound,
                        (short)(g_dirIn && modIn),
                        (short)(g_dirOut && modOut))) {
        return FALSE;
    }

    // Advanced software filter is the final AND leg (spec 4.4): ownership
    // gate AND text filter, both evaluated before any module runs. The
    // filter is evaluated outside g_imageLock (it has its own state).
    return ownsNode(node)
           && swfilterMatch(node->packet, node->packetLen, &node->addr);
}

BOOL targetingOwnsPacket(const char *packet, UINT packetLen,
                         const WINDIVERT_ADDRESS *addr) {
    PacketNode node;

    if (packet == NULL || addr == NULL || packetLen == 0) {
        return FALSE;
    }
    memset(&node, 0, sizeof(node));
    node.packet = (char *)packet;
    node.packetLen = packetLen;
    node.addr = *addr;
    if (!pktparseTuple(packet, packetLen, &node.tuple)) {
        return FALSE;
    }
    node.tupleValid = TRUE;
    return ownsNode(&node);
}
