// Manual admin harness for Task 12 acceptance item 9:
// Start/Stop (+ attach/detach) 20 times and watch the process handle count.
// Run elevated; no traffic needed — lifecycle handles must return to baseline.
#include <stdio.h>
// winsock2 (pulled by divert/flowmap) must precede windows.h
#include <winsock2.h>
#include <windows.h>
#include "common.h"
#include "divert.h"

// ! mirrors main.c pipeline order
Module* modules[MODULE_CNT] = {
    &lagModule,
    &dropModule,
    &throttleModule,
    &dupModule,
    &oodModule,
    &tamperModule,
    &resetModule,
    &bandwidthModule,
};
BOOL parameterized = FALSE;
volatile short sendState = SEND_STATUS_NONE;

static DWORD handleCount(void) {
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return count;
}

int main(void) {
    char err[MSG_BUFSIZE];
    DWORD baseline;
    int i;

    baseline = handleCount();
    printf("baseline handles: %lu\n", baseline);
    for (i = 0; i < 20; i++) {
        if (!divertAttach("true", err)) {
            printf("cycle %2d attach FAILED: %s\n", i, err);
            return 1;
        }
        if (!divertStartAffect("true", err)) {
            printf("cycle %2d start FAILED: %s\n", i, err);
            divertDetach();
            return 1;
        }
        Sleep(150);
        divertStopAffect();
        divertDetach();
        Sleep(50);
        if (i % 5 == 4) {
            printf("cycle %2d handles: %lu (delta %ld)\n", i + 1,
                   handleCount(), (long)(handleCount() - baseline));
        }
    }
    Sleep(500); // let thread/handle teardown settle
    DWORD finalCount = handleCount();
    printf("final handles: %lu (delta %ld from baseline)\n",
           finalCount, (long)(finalCount - baseline));
    if ((long)(finalCount - baseline) <= 2) {
        printf("PASS: no handle leak across 20 cycles\n");
        return 0;
    }
    printf("FAIL: handles grew by more than 2\n");
    return 1;
}
