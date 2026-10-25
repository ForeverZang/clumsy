#include <winsock2.h>
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "ocr_worker.h"

#define WORKER_MIN_INTERVAL_MS 200
#define WORKER_EVENT_CAP       64

typedef struct QNode {
    OcrSessionEvent ev;
    char text[OCR_EVENT_TEXT];
    struct QNode *next;
} QNode;

static struct {
    OcrSession *session;
    HANDLE thread;
    HANDLE stopEvent;
    volatile LONG busy;
    CRITICAL_SECTION lock;
    BOOL lockInit;
    QNode *head;
    QNode *tail;
} g_worker;

static void enqueue(const OcrSessionEvent *src) {
    QNode *node = calloc(1, sizeof(*node));
    if (!node) return;
    node->ev = *src;
    if (src->text) {
        strncpy(node->text, src->text, sizeof(node->text) - 1);
        node->ev.text = node->text;
    } else {
        node->ev.text = NULL;
    }
    EnterCriticalSection(&g_worker.lock);
    if (g_worker.tail) g_worker.tail->next = node;
    else g_worker.head = node;
    g_worker.tail = node;
    LeaveCriticalSection(&g_worker.lock);
}

static void freeQueueLocked(void) {
    QNode *node = g_worker.head;
    while (node) {
        QNode *next = node->next;
        free(node);
        node = next;
    }
    g_worker.head = NULL;
    g_worker.tail = NULL;
}

static DWORD WINAPI workerThread(LPVOID arg) {
    (void)arg;
    OcrSession *s = g_worker.session;
    DWORD interval = (DWORD)(s->intervalMs >= WORKER_MIN_INTERVAL_MS
                             ? s->intervalMs : WORKER_MIN_INTERVAL_MS);
    UINT64 nextDue = GetTickCount64() + interval;

    for (;;) {
        UINT64 now = GetTickCount64();
        DWORD delay = nextDue > now ? (DWORD)(nextDue - now) : 1;
        if (WaitForSingleObject(g_worker.stopEvent, delay) == WAIT_OBJECT_0) {
            break;
        }

        InterlockedExchange(&g_worker.busy, 1);
        OcrSessionEvent evs[WORKER_EVENT_CAP];
        int n = 0;
        if (ocrSessionPump(s, evs, &n, WORKER_EVENT_CAP)) {
            for (int i = 0; i < n; i++) enqueue(&evs[i]);
        }
        InterlockedExchange(&g_worker.busy, 0);

        // Anchor beats to absolute time. A long pump pushes nextDue past
        // the present, so every beat it overran is skipped (not buffered).
        UINT64 end = GetTickCount64();
        do {
            nextDue += interval;
        } while (nextDue <= end);
    }
    return 0;
}

BOOL ocrWorkerStart(OcrSession *session, OcrEventSink sink,
                    char err[MSG_BUFSIZE]) {
    (void)sink; // events are collected via ocrWorkerDrain on the GUI thread
    if (err) err[0] = '\0';
    if (!session || !session->regions || session->regionCount == 0
        || !session->captureFn || !session->readFn) {
        if (err) strncpy(err, "invalid OCR worker session", MSG_BUFSIZE - 1);
        return FALSE;
    }
    if (g_worker.thread) {
        if (err) strncpy(err, "OCR worker already running", MSG_BUFSIZE - 1);
        return FALSE;
    }

    if (!g_worker.lockInit) {
        InitializeCriticalSection(&g_worker.lock);
        g_worker.lockInit = TRUE;
    }
    EnterCriticalSection(&g_worker.lock);
    freeQueueLocked(); // events left undelivered from a previous run
    LeaveCriticalSection(&g_worker.lock);
    g_worker.busy = 0;
    g_worker.session = session;

    g_worker.stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_worker.stopEvent) {
        if (err) {
            snprintf(err, MSG_BUFSIZE, "CreateEvent failed (%lu)",
                     GetLastError());
        }
        g_worker.session = NULL;
        return FALSE;
    }

    g_worker.thread = CreateThread(NULL, 0, workerThread, NULL, 0, NULL);
    if (!g_worker.thread) {
        if (err) {
            snprintf(err, MSG_BUFSIZE, "CreateThread failed (%lu)",
                     GetLastError());
        }
        CloseHandle(g_worker.stopEvent);
        g_worker.stopEvent = NULL;
        g_worker.session = NULL;
        return FALSE;
    }
    return TRUE;
}

void ocrWorkerStop(void) {
    if (!g_worker.thread) return;
    SetEvent(g_worker.stopEvent);
    WaitForSingleObject(g_worker.thread, INFINITE);
    CloseHandle(g_worker.thread);
    CloseHandle(g_worker.stopEvent);
    g_worker.thread = NULL;
    g_worker.stopEvent = NULL;
    g_worker.busy = 0;
    g_worker.session = NULL;
    // The queue is preserved so the GUI thread can drain final events
    // after stop; the next Start clears anything left behind.
}

BOOL ocrWorkerBusy(void) {
    return g_worker.busy != 0;
}

void ocrWorkerDrain(OcrEventSink sink) {
    if (!sink) return;
    EnterCriticalSection(&g_worker.lock);
    QNode *head = g_worker.head;
    g_worker.head = NULL;
    g_worker.tail = NULL;
    LeaveCriticalSection(&g_worker.lock);

    while (head) {
        QNode *next = head->next;
        sink(&head->ev);
        free(head);
        head = next;
    }
}
