// Unit tests for the 60-second per-app traffic ring:
// per-second byte rates split by direction, oldest data evicted, reset
// clears everything, and the start marker scrolls with the buckets.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <assert.h>

#include "traffic.h"

static void assertAllZero(const UINT64 *down, const UINT64 *up) {
    for (int i = 0; i < TRAFFIC_BUCKETS; i++) {
        assert(down[i] == 0);
        assert(up[i] == 0);
    }
}

int main(void) {
    Traffic *t = trafficNew();
    assert(t != NULL);
    UINT64 down[TRAFFIC_BUCKETS], up[TRAFFIC_BUCKETS];

    // Fresh ring: empty.
    trafficSeries(t, down, up);
    assertAllZero(down, up);

    // 1. Records before any tick land in the newest (live) bucket.
    trafficRecord(t, 1000, FALSE);
    trafficRecord(t, 400, TRUE);
    trafficSeries(t, down, up);
    assert(down[TRAFFIC_BUCKETS - 1] == 1000);
    assert(up[TRAFFIC_BUCKETS - 1] == 400);
    assert(down[TRAFFIC_BUCKETS - 2] == 0);
    assert(up[TRAFFIC_BUCKETS - 2] == 0);

    // 2. After a tick the live bucket is new and empty; data shifted one
    // column left; directions never mix.
    trafficTick(t);
    trafficSeries(t, down, up);
    assert(down[TRAFFIC_BUCKETS - 2] == 1000);
    assert(up[TRAFFIC_BUCKETS - 2] == 400);
    assert(down[TRAFFIC_BUCKETS - 1] == 0);
    assert(up[TRAFFIC_BUCKETS - 1] == 0);
    for (int i = 0; i < TRAFFIC_BUCKETS - 2; i++) {
        assert(down[i] == 0 && up[i] == 0);
    }

    // Records accumulate within one second.
    trafficRecord(t, 5, FALSE);
    trafficRecord(t, 7, FALSE);
    trafficSeries(t, down, up);
    assert(down[TRAFFIC_BUCKETS - 1] == 12);
    assert(up[TRAFFIC_BUCKETS - 1] == 0);

    // 3. Reset on app switch clears all buckets.
    trafficReset(t);
    trafficSeries(t, down, up);
    assertAllZero(down, up);

    // 4. The oldest second is evicted once it scrolls past 60 columns.
    trafficRecord(t, 77, FALSE);
    for (int i = 0; i < TRAFFIC_BUCKETS + 1; i++) {
        trafficTick(t);
    }
    trafficSeries(t, down, up);
    assertAllZero(down, up);

    // 5. Start marker: marked second is the newest column, then scrolls
    // left with each tick, then disappears off the edge.
    int col = -1;
    assert(!trafficStartColumn(t, &col)); // no marker yet
    trafficMarkStart(t);
    assert(trafficStartColumn(t, &col) && col == TRAFFIC_BUCKETS - 1);
    for (int i = 0; i < 3; i++) trafficTick(t);
    assert(trafficStartColumn(t, &col) && col == TRAFFIC_BUCKETS - 4);
    for (int i = 0; i < TRAFFIC_BUCKETS - 3; i++) trafficTick(t);
    assert(!trafficStartColumn(t, &col));

    // Reset clears the marker.
    trafficMarkStart(t);
    assert(trafficStartColumn(t, &col));
    trafficReset(t);
    assert(!trafficStartColumn(t, &col));

    trafficDestroy(t);
    printf("OK: traffic\n");
    return 0;
}
