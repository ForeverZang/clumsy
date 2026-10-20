#include <stdlib.h>
#include <memory.h>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include "windivert.h"
#include "common.h"
#include "divert.h"
#include "flowwatch.h"
#include "targeting.h"

#define DIVERT_PRIORITY 0
// SNIFF shares the NETWORK layer but never reinjects; a distinct priority
// is required because two handles cannot share one priority.
#define SNIFF_PRIORITY  10
#define MAX_PACKETSIZE 0xFFFF
// FIXME does this need to be larger then the time to process the list?
#define CLOCK_WAITMS 40
#define QUEUE_LEN (2 << 10)
#define QUEUE_TIME (2 << 9)

/*
 * Persistent session: FLOW watcher + flow ownership map + a SNIFF network
 * handle feeding the chart observer. Lives across Start/Stop cycles.
 */
typedef struct {
    FlowMap *map;
    FlowWatch *watch;
    HANDLE sniffHandle;
    HANDLE sniffThread;
    volatile SniffObserver observer;
} PersistentSession;

/*
 * Affect session: the intercepting handle plus the recv/clock pipeline.
 * Created per Start, fully torn down per Stop.
 */
typedef struct {
    HANDLE handle;
    HANDLE mutex;
    HANDLE recvThread;
    HANDLE clockThread;
    volatile short stopLooping;
} AffectSession;

static PersistentSession *g_persist = NULL;
static AffectSession *g_affect = NULL;

static DWORD divertReadLoop(LPVOID arg);
static DWORD divertClockLoop(LPVOID arg);
static DWORD divertSniffLoop(LPVOID arg);

// not to put these in common.h since modules shouldn't see these
extern PacketNode * const head;
extern PacketNode * const tail;

#ifdef _DEBUG
PWINDIVERT_IPHDR dbg_ip_header;
PWINDIVERT_IPV6HDR dbg_ipv6_header;
PWINDIVERT_TCPHDR dbg_tcp_header;
PWINDIVERT_UDPHDR dbg_udp_header;
PWINDIVERT_ICMPHDR dbg_icmp_header;
PWINDIVERT_ICMPV6HDR dbg_icmpv6_header;
UINT payload_len;
void dumpPacket(char *buf, int len, PWINDIVERT_ADDRESS paddr) {
    char *protocol;
    UINT16 srcPort = 0, dstPort = 0;

    WinDivertHelperParsePacket(buf, len, &dbg_ip_header, &dbg_ipv6_header, NULL,
        &dbg_icmp_header, &dbg_icmpv6_header, &dbg_tcp_header, &dbg_udp_header,
        NULL, &payload_len, NULL, NULL);
    // need to cast byte order on port numbers
    if (dbg_tcp_header != NULL) {
        protocol = "TCP ";
        srcPort = ntohs(dbg_tcp_header->SrcPort);
        dstPort = ntohs(dbg_tcp_header->DstPort);
    } else if (dbg_udp_header != NULL) {
        protocol = "UDP ";
        srcPort = ntohs(dbg_udp_header->SrcPort);
        dstPort = ntohs(dbg_udp_header->DstPort);
    } else if (dbg_icmp_header || dbg_icmpv6_header) {
        protocol = "ICMP";
    } else {
        protocol = "???";
    }

    if (dbg_ip_header != NULL) {
        UINT8 *src_addr = (UINT8*)&dbg_ip_header->SrcAddr;
        UINT8 *dst_addr = (UINT8*)&dbg_ip_header->DstAddr;
        LOG("%s.%s: %u.%u.%u.%u:%d->%u.%u.%u.%u:%d",
            protocol,
            paddr->Outbound ? "OUT " : "IN  ",
            src_addr[0], src_addr[1], src_addr[2], src_addr[3], srcPort,
            dst_addr[0], dst_addr[1], dst_addr[2], dst_addr[3], dstPort);
    } else if (dbg_ipv6_header != NULL) {
        UINT16 *src_addr6 = (UINT16*)&dbg_ipv6_header->SrcAddr;
        UINT16 *dst_addr6 = (UINT16*)&dbg_ipv6_header->DstAddr;
        LOG("%s.%s: %x:%x:%x:%x:%x:%x:%x:%x:%d->%x:%x:%x:%x:%x:%x:%x:%x:%d",
            protocol,
            paddr->Outbound ? "OUT " : "IN  ",
            src_addr6[0], src_addr6[1], src_addr6[2], src_addr6[3],
            src_addr6[4], src_addr6[5], src_addr6[6], src_addr6[7], srcPort,
            dst_addr6[0], dst_addr6[1], dst_addr6[2], dst_addr6[3],
            dst_addr6[4], dst_addr6[5], dst_addr6[6], dst_addr6[7], dstPort);
    }
}
#else
#define dumpPacket(x, y, z)
#endif

static void divertFormatOpenError(DWORD lastError, char buf[MSG_BUFSIZE]) {
    if (lastError == ERROR_INVALID_PARAMETER) {
        snprintf(buf, MSG_BUFSIZE,
            "Failed to start filtering : filter syntax error.");
    } else {
        snprintf(buf, MSG_BUFSIZE,
            "Failed to start filtering : failed to open device (code:%lu).\n"
            "Make sure you run clumsy as Administrator.", lastError);
    }
}

// ---------------------------------------------------------------------------
// Module pipeline (intercept session only)
// ---------------------------------------------------------------------------

static int sendAllListPackets(HANDLE handle) {
    // send packet from tail to head and remove sent ones
    int sendCount = 0;
    UINT sendLen;
    PacketNode *pnode;
#ifdef _DEBUG
    // check the list is good
    // might go into dead loop but it's better for debugging
    PacketNode *p = head;
    do {
        p = p->next;
    } while (p->next);
    assert(p == tail);
#endif

    while (!isListEmpty()) {
        pnode = popNode(tail->prev);
        sendLen = 0;
        assert(pnode != head);
        // FIXME inbound injection on any kind of packet is failing with a very high percentage
        //       need to contact windivert auther and wait for next release
        if (!WinDivertSend(handle, pnode->packet, pnode->packetLen, &sendLen, &(pnode->addr))) {
            PWINDIVERT_ICMPHDR icmp_header;
            PWINDIVERT_ICMPV6HDR icmpv6_header;
            PWINDIVERT_IPHDR ip_header;
            PWINDIVERT_IPV6HDR ipv6_header;
            LOG("Failed to send a packet. (%lu)", GetLastError());
            dumpPacket(pnode->packet, pnode->packetLen, &(pnode->addr));
            // as noted in windivert help, reinject inbound icmp packets some times would fail
            // workaround this by resend them as outbound
            // TODO not sure is this even working as can't find a way to test
            //      need to document about this
            WinDivertHelperParsePacket(pnode->packet, pnode->packetLen, &ip_header, &ipv6_header, NULL,
                &icmp_header, &icmpv6_header, NULL, NULL, NULL, NULL, NULL, NULL);
            if ((icmp_header || icmpv6_header) && !pnode->addr.Outbound) {
                BOOL resent;
                pnode->addr.Outbound = TRUE;
                if (ip_header) {
                    UINT32 tmp = ip_header->SrcAddr;
                    ip_header->SrcAddr = ip_header->DstAddr;
                    ip_header->DstAddr = tmp;
                } else if (ipv6_header) {
                    UINT32 tmpArr[4];
                    memcpy(tmpArr, ipv6_header->SrcAddr, sizeof(tmpArr));
                    memcpy(ipv6_header->SrcAddr, ipv6_header->DstAddr, sizeof(tmpArr));
                    memcpy(ipv6_header->DstAddr, tmpArr, sizeof(tmpArr));
                }
                resent = WinDivertSend(handle, pnode->packet, pnode->packetLen, &sendLen, &(pnode->addr));
                LOG("Resend failed inbound ICMP packets as outbound: %s", resent ? "SUCCESS" : "FAIL");
                InterlockedExchange16(&sendState, SEND_STATUS_SEND);
            } else {
                InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
            }
        } else {
            if (sendLen < pnode->packetLen) {
                // TODO don't know how this can happen, or it needs to be resent like good old UDP packet
                LOG("Internal Error: DivertSend truncated send packet.");
                InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
            } else {
                InterlockedExchange16(&sendState, SEND_STATUS_SEND);
            }
        }


        freeNode(pnode);
        ++sendCount;
    }
    assert(isListEmpty()); // all packets should be sent by now

    return sendCount;
}

// step function to let module process and consume all packets on the list
static void divertConsumeStep(HANDLE handle) {
#ifdef _DEBUG
    DWORD startTick = GetTickCount(), dt;
#endif
    int ix, cnt;
    // use lastEnabled to keep track of module starting up and closing down
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        Module *module = modules[ix];
        if (*(module->enabledFlag)) {
            if (!module->lastEnabled) {
                module->startUp();
                module->lastEnabled = 1;
            }
            if (module->process(head, tail)) {
                InterlockedIncrement16(&(module->processTriggered));
            }
        } else {
            if (module->lastEnabled) {
                module->closeDown(head, tail);
                module->lastEnabled = 0;
            }
        }
    }
    cnt = sendAllListPackets(handle);
#ifdef _DEBUG
    dt =  GetTickCount() - startTick;
    if (dt > CLOCK_WAITMS / 2) {
        LOG("Costy consume step: %lu ms, sent %d packets", GetTickCount() - startTick, cnt);
    }
#endif
}

// close every enabled module and reinject everything still queued.
// must be called with the session mutex held (or after the recv thread
// has exited), and while WinDivertSend still works (recv-only shutdown).
static void divertFinalFlush(AffectSession *a) {
    int ix, lastSendCount;
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        Module *module = modules[ix];
        if (*(module->enabledFlag)) {
            module->closeDown(head, tail);
        }
    }
    LOG("Send all packets upon closing");
    lastSendCount = sendAllListPackets(a->handle);
    LOG("Lastly sent %d packets. Closing...", lastSendCount);
}

// ---------------------------------------------------------------------------
// Intercept session threads
// ---------------------------------------------------------------------------

// fail-open reinject of a single raw packet outside the node list
static void divertReinjectRaw(HANDLE handle, char *buf, UINT len,
                              WINDIVERT_ADDRESS *addr) {
    UINT sendLen = 0;
    if (!WinDivertSend(handle, buf, len, &sendLen, addr)) {
        LOG("Failed to reinject raw packet on stop/error. (%lu)", GetLastError());
    }
}

static void divertHandleReceived(AffectSession *a, char *packetBuf,
                                 UINT readLen, WINDIVERT_ADDRESS *addrBuf) {
    PacketNode *pnode;
    DWORD waitResult = WaitForSingleObject(a->mutex, INFINITE);
    switch (waitResult) {
    case WAIT_OBJECT_0:
        /***************** enter critical region ************************/
        if (a->stopLooping) {
            LOG("User stopped; reinjecting last recved packet untouched.");
            if (!ReleaseMutex(a->mutex)) {
                LOG("Fatal: Failed to release mutex on stopping (%lu). Will stop anyway.", GetLastError());
            }
            divertReinjectRaw(a->handle, packetBuf, readLen, addrBuf);
            return;
        }
        // create node and put it into the list. On allocation failure the
        // security rule is fail-open: reinject the packet untouched.
        pnode = createNode(packetBuf, readLen, addrBuf);
        if (pnode == NULL) {
            if (!ReleaseMutex(a->mutex)) {
                LOG("Fatal: Failed to release mutex (%lu).", GetLastError());
            }
            divertReinjectRaw(a->handle, packetBuf, readLen, addrBuf);
            return;
        }
        appendNode(pnode);
        divertConsumeStep(a->handle);
        /***************** leave critical region ************************/
        if (!ReleaseMutex(a->mutex)) {
            LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
            ABORT();
        }
        break;
    case WAIT_ABANDONED:
        LOG("Acquire abandoned, reinjecting one packet untouched.");
        divertReinjectRaw(a->handle, packetBuf, readLen, addrBuf);
        break;
    case WAIT_FAILED:
        LOG("Acquire failed, reinjecting one packet untouched.");
        divertReinjectRaw(a->handle, packetBuf, readLen, addrBuf);
        break;
    }
}

static DWORD divertReadLoop(LPVOID arg) {
    AffectSession *a = (AffectSession *)arg;
    char packetBuf[MAX_PACKETSIZE];
    WINDIVERT_ADDRESS addrBuf;
    UINT readLen;

    for(;;) {
        // each step must fully consume the list
        assert(isListEmpty()); // FIXME has failed this assert before. don't know why
        if (!WinDivertRecv(a->handle, packetBuf, MAX_PACKETSIZE, &readLen, &addrBuf)) {
            DWORD lastError = GetLastError();
            if (lastError == ERROR_NO_DATA
                || lastError == ERROR_INVALID_HANDLE
                || lastError == ERROR_OPERATION_ABORTED) {
                // shutdown()/close() unblocked the recv: quit.
                LOG("Recv shutdown (code %lu). Exit loop.", lastError);
                return 0;
            }
            LOG("Failed to recv a packet. (%lu)", lastError);
            continue;
        }
        if (readLen > MAX_PACKETSIZE) {
            // don't know how this can happen
            LOG("Internal Error: DivertRecv truncated recv packet.");
        }
        divertHandleReceived(a, packetBuf, readLen, &addrBuf);
    }
}

// periodically try to consume packets to keep the network responsive and not blocked by recv
static DWORD divertClockLoop(LPVOID arg) {
    AffectSession *a = (AffectSession *)arg;
    DWORD startTick, stepTick, waitResult;

    for(;;) {
        if (a->stopLooping) {
            // the recv thread is already exiting (recv side was shut down
            // before the join), so no new nodes can appear after this flush.
            waitResult = WaitForSingleObject(a->mutex, INFINITE);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
                LOG("Read stopLooping, stopping...");
                divertFinalFlush(a);
                if (waitResult == WAIT_OBJECT_0 && !ReleaseMutex(a->mutex)) {
                    LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                    ABORT();
                }
            } else {
                LOG("Acquire failed (%lu), exiting without final flush", GetLastError());
            }
            return 0;
        }

        // use acquire as wait for yielding thread
        startTick = GetTickCount();
        waitResult = WaitForSingleObject(a->mutex, CLOCK_WAITMS);
        if (waitResult == WAIT_OBJECT_0) {
            /***************** enter critical region ************************/
            divertConsumeStep(a->handle);
            /***************** leave critical region ************************/
            if (!ReleaseMutex(a->mutex)) {
                InterlockedIncrement16(&a->stopLooping);
                LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                ABORT();
            }
            // if didn't spent enough time, we sleep on it
            stepTick = GetTickCount() - startTick;
            if (stepTick < CLOCK_WAITMS) {
                Sleep(CLOCK_WAITMS - stepTick);
            }
        } else {
            // WAIT_TIMEOUT: read loop is processing, skip this run
            // WAIT_ABANDONED/FAILED: recv thread died holding the lock, keep
            // clock alive so an explicit Stop can still join/flush us.
            LOG("Clock mutex wait result: %lu. Skipping one run.", waitResult);
            Sleep(CLOCK_WAITMS);
        }
    }
}

// ---------------------------------------------------------------------------
// Persistent SNIFF thread
// ---------------------------------------------------------------------------

static DWORD divertSniffLoop(LPVOID arg) {
    PersistentSession *p = (PersistentSession *)arg;
    char packetBuf[MAX_PACKETSIZE];
    WINDIVERT_ADDRESS addrBuf;
    UINT readLen;

    for (;;) {
        if (!WinDivertRecv(p->sniffHandle, packetBuf, MAX_PACKETSIZE, &readLen, &addrBuf)) {
            DWORD lastError = GetLastError();
            if (lastError == ERROR_NO_DATA) {
                LOG("Sniff shutdown. Exit loop.");
                return 0;
            }
            LOG("Sniff recv failed. (%lu)", lastError);
            Sleep(10);
            continue;
        }
        SniffObserver cb = (SniffObserver)InterlockedCompareExchangePointer(
            (PVOID volatile *)&p->observer, NULL, NULL);
        if (cb != NULL) {
            cb(packetBuf, readLen, &addrBuf);
        }
    }
}

// Production owner lookup: packet tuple -> normalized FlowKey -> pid.
// Tuple words/ports are network order; loopback is therefore tested
// byte-wise (127/8 first byte, ::1 low word 0x01000000 network order).
static BOOL divertTargetingOwner(const PacketNode *node, UINT32 *outPid) {
    PersistentSession *p = g_persist;
    FlowKey key;
    BOOL loopback;

    if (p == NULL || !node->tupleValid) {
        return FALSE;
    }
    if (node->tuple.ipv6) {
        loopback = (node->tuple.aAddr[3] == htonl(1)
                    && node->tuple.aAddr[0] == 0
                    && node->tuple.aAddr[1] == 0
                    && node->tuple.aAddr[2] == 0)
                   || (node->tuple.bAddr[3] == htonl(1)
                       && node->tuple.bAddr[0] == 0
                       && node->tuple.bAddr[1] == 0
                       && node->tuple.bAddr[2] == 0);
    } else {
        loopback = ((ntohl(node->tuple.aAddr[0]) & 0xFF000000u)
                        == 0x7F000000u
                    || (ntohl(node->tuple.bAddr[0]) & 0xFF000000u)
                           == 0x7F000000u);
    }
    flowmapKeyFromPacket(node->tuple.protocol,
                         node->addr.Outbound ? TRUE : FALSE,
                         loopback,
                         node->tuple.aAddr, node->tuple.aPort,
                         node->tuple.bAddr, node->tuple.bPort,
                         node->tuple.ipv6, &key);
    return flowmapLookup(p->map, &key, outPid);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

BOOL divertAttach(const char *kernelFilter, char err[MSG_BUFSIZE]) {
    PersistentSession *p;
    HANDLE opened;

    if (g_persist != NULL) {
        return TRUE; // idempotent
    }
    if (kernelFilter == NULL) {
        kernelFilter = "true";
    }

    p = (PersistentSession *)calloc(1, sizeof(*p));
    if (p == NULL) {
        snprintf(err, MSG_BUFSIZE, "Failed to start filtering : out of memory.");
        return FALSE;
    }

    p->map = flowmapCreate();
    if (p->map == NULL) {
        snprintf(err, MSG_BUFSIZE, "Failed to start filtering : out of memory.");
        free(p);
        return FALSE;
    }
    p->watch = flowwatchStart(p->map);
    if (p->watch == NULL) {
        DWORD code = GetLastError();
        snprintf(err, MSG_BUFSIZE,
            "Failed to start flow watcher (code:%lu).\n"
            "Make sure you run clumsy as Administrator.", code);
        flowmapDestroy(p->map);
        free(p);
        return FALSE;
    }

    opened = WinDivertOpen(kernelFilter, WINDIVERT_LAYER_NETWORK,
                           SNIFF_PRIORITY,
                           WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    if (opened == INVALID_HANDLE_VALUE) {
        divertFormatOpenError(GetLastError(), err);
        flowwatchStop(p->watch);
        flowmapDestroy(p->map);
        free(p);
        return FALSE;
    }
    p->sniffHandle = opened;

    p->sniffThread = CreateThread(NULL, 1,
        (LPTHREAD_START_ROUTINE)divertSniffLoop, p, 0, NULL);
    if (p->sniffThread == NULL) {
        DWORD code = GetLastError();
        snprintf(err, MSG_BUFSIZE, "Failed to create sniff thread (%lu).", code);
        WinDivertClose(p->sniffHandle);
        flowwatchStop(p->watch);
        flowmapDestroy(p->map);
        free(p);
        return FALSE;
    }

    g_persist = p;
    targetingSetOwnerLookup(divertTargetingOwner);
    LOG("Attached: FLOW watcher + SNIFF handle (filter: %s).", kernelFilter);
    return TRUE;
}

void divertDetach(void) {
    PersistentSession *p = g_persist;
    if (p == NULL) {
        return;
    }
    divertStopAffect();
    g_persist = NULL;
    InterlockedExchangePointer((PVOID volatile *)&p->observer, NULL);

    WinDivertShutdown(p->sniffHandle, WINDIVERT_SHUTDOWN_BOTH);
    WaitForSingleObject(p->sniffThread, INFINITE);
    WinDivertClose(p->sniffHandle);
    CloseHandle(p->sniffThread);

    flowwatchStop(p->watch);
    flowmapDestroy(p->map);
    free(p);
    LOG("Detached.");
}

BOOL divertIsAttached(void) {
    return g_persist != NULL;
}

FlowMap *divertFlowMap(void) {
    return g_persist != NULL ? g_persist->map : NULL;
}

void divertSetSniffObserver(SniffObserver observer) {
    if (g_persist != NULL) {
        InterlockedExchangePointer((PVOID volatile *)&g_persist->observer,
                                   (PVOID)observer);
    }
}

// rollback after the intercept handle is open but startAffect failed.
// recvStarted means its thread exists and must be shut down / joined.
static void affectStartRollback(AffectSession *a, BOOL recvStarted) {
    InterlockedExchange16(&a->stopLooping, TRUE);
    if (recvStarted) {
        WinDivertShutdown(a->handle, WINDIVERT_SHUTDOWN_RECV);
        WaitForSingleObject(a->recvThread, INFINITE);
        CloseHandle(a->recvThread);
    }
    WinDivertClose(a->handle);
    if (a->mutex != NULL) {
        CloseHandle(a->mutex);
    }
    free(a);
}

BOOL divertStartAffect(const char *kernelFilter, char err[MSG_BUFSIZE]) {
    AffectSession *a;

    if (g_affect != NULL) {
        snprintf(err, MSG_BUFSIZE, "Filtering is already running.");
        return FALSE;
    }

    a = (AffectSession *)calloc(1, sizeof(*a));
    if (a == NULL) {
        snprintf(err, MSG_BUFSIZE, "Failed to start filtering : out of memory.");
        return FALSE;
    }

    a->handle = WinDivertOpen(kernelFilter, WINDIVERT_LAYER_NETWORK,
                              DIVERT_PRIORITY, 0);
    if (a->handle == INVALID_HANDLE_VALUE) {
        divertFormatOpenError(GetLastError(), err);
        free(a);
        return FALSE;
    }
    LOG("Divert opened handle.");

    WinDivertSetParam(a->handle, WINDIVERT_PARAM_QUEUE_LENGTH, QUEUE_LEN);
    WinDivertSetParam(a->handle, WINDIVERT_PARAM_QUEUE_TIME, QUEUE_TIME);
    LOG("WinDivert internal queue Len: %d, queue time: %d", QUEUE_LEN, QUEUE_TIME);

    // init package link list and reset per-run module state
    initPacketNodeList();
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        modules[ix]->lastEnabled = 0;
    }

    LOG("Creating threads and mutex...");
    a->mutex = CreateMutex(NULL, FALSE, NULL);
    if (a->mutex == NULL) {
        snprintf(err, MSG_BUFSIZE, "Failed to create mutex (%lu)", GetLastError());
        WinDivertClose(a->handle);
        free(a);
        return FALSE;
    }

    a->recvThread = CreateThread(NULL, 1,
        (LPTHREAD_START_ROUTINE)divertReadLoop, a, 0, NULL);
    if (a->recvThread == NULL) {
        snprintf(err, MSG_BUFSIZE, "Failed to create recv loop thread (%lu)", GetLastError());
        affectStartRollback(a, FALSE);
        return FALSE;
    }
    a->clockThread = CreateThread(NULL, 1,
        (LPTHREAD_START_ROUTINE)divertClockLoop, a, 0, NULL);
    if (a->clockThread == NULL) {
        snprintf(err, MSG_BUFSIZE, "Failed to create clock loop thread (%lu)", GetLastError());
        affectStartRollback(a, TRUE);
        return FALSE;
    }

    g_affect = a;
    LOG("Threads created");
    return TRUE;
}

void divertStopAffect(void) {
    AffectSession *a = g_affect;
    HANDLE threads[2];
    if (a == NULL) {
        return;
    }
    g_affect = NULL;

    LOG("Stopping...");
    InterlockedExchange16(&a->stopLooping, TRUE);
    // recv-only shutdown unblocks WinDivertRecv while keeping Send alive,
    // so the clock thread's final flush can still reinject. Full BOTH
    // shutdown happens only after both threads have joined.
    WinDivertShutdown(a->handle, WINDIVERT_SHUTDOWN_RECV);
    threads[0] = a->recvThread;
    threads[1] = a->clockThread;
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    WinDivertShutdown(a->handle, WINDIVERT_SHUTDOWN_BOTH);
    WinDivertClose(a->handle);
    CloseHandle(a->recvThread);
    CloseHandle(a->clockThread);
    CloseHandle(a->mutex);
    free(a);
    LOG("Successfully stopped intercept session.");
}

BOOL divertIsAffecting(void) {
    return g_affect != NULL;
}

// ---------------------------------------------------------------------------
// Interim adapters for the pre-Task-11 UI contract.
// Removed when the beginner UI lands.
// ---------------------------------------------------------------------------

int divertStart(const char *filter, char buf[]) {
    if (!divertAttach("true", buf)) {
        return FALSE;
    }
    if (!divertStartAffect(filter, buf)) {
        divertDetach();
        return FALSE;
    }
    return TRUE;
}

void divertStop(void) {
    divertStopAffect();
    divertDetach();
}
