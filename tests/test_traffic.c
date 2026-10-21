// Unit tests for the 60-second per-app traffic ring:
// per-second byte rates split by direction, oldest data evicted, reset
// clears everything, and the start marker scrolls with the buckets.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

    // 6. Rate formatting: automatic B/s, KB/s, MB/s with one decimal.
    char rate[32];
    trafficFormatRate(0, rate, sizeof(rate));
    assert(strcmp(rate, "0 B/s") == 0);
    trafficFormatRate(999, rate, sizeof(rate));
    assert(strcmp(rate, "999 B/s") == 0);
    trafficFormatRate(1024, rate, sizeof(rate));
    assert(strcmp(rate, "1.0 KB/s") == 0);
    trafficFormatRate(1536, rate, sizeof(rate));
    assert(strcmp(rate, "1.5 KB/s") == 0);
    trafficFormatRate(10u * 1024 * 1024, rate, sizeof(rate));
    assert(strcmp(rate, "10.0 MB/s") == 0);

    // 7. Recent average spans the N completed buckets before the live one
    // and is capped by elapsed ticks (empty history is zero, not an
    // ancient-zero average).
    t = trafficNew();
    UINT64 avgDown = 999, avgUp = 999;
    trafficRecentAverage(t, 10, &avgDown, &avgUp);
    assert(avgDown == 0 && avgUp == 0); // no ticks yet
    trafficRecord(t, 300, FALSE);
    trafficRecord(t, 100, TRUE);
    trafficTick(t);
    // live bucket excluded; its older neighbour holds 300/100
    trafficRecentAverage(t, 10, &avgDown, &avgUp);
    assert(avgDown == 300 && avgUp == 100);
    // three more identical completed seconds: data accumulates in the
    // live bucket, then the tick rolls it over (the production rhythm)
    trafficRecord(t, 300, FALSE); trafficRecord(t, 100, TRUE); trafficTick(t);
    trafficRecord(t, 300, FALSE); trafficRecord(t, 100, TRUE); trafficTick(t);
    trafficRecord(t, 300, FALSE); trafficRecord(t, 100, TRUE); trafficTick(t);
    trafficRecentAverage(t, 10, &avgDown, &avgUp);
    assert(avgDown == 300 && avgUp == 100);
    // the window clips to the requested number of seconds
    trafficRecentAverage(t, 2, &avgDown, &avgUp);
    assert(avgDown == 300 && avgUp == 100);
    trafficDestroy(t);

    // 8. Axis scale: a 1/2/5 ceiling in a single chosen unit (B/s, KB/s,
    // or MB/s), so all three gridline labels on one chart share one unit.
    UINT64 divisor = 0, ceiling = 0;
    const char *unit = NULL;
    trafficAxisScale(0, &divisor, &unit, &ceiling);
    assert(divisor == 1 && strcmp(unit, "B/s") == 0 && ceiling == 1);
    trafficAxisScale(300, &divisor, &unit, &ceiling);
    assert(divisor == 1 && strcmp(unit, "B/s") == 0 && ceiling == 500);
    trafficAxisScale(1500, &divisor, &unit, &ceiling);
    assert(divisor == 1024 && strcmp(unit, "KB/s") == 0 && ceiling == 1);
    trafficAxisScale(21504, &divisor, &unit, &ceiling); // 21 KB/s -> 1/2/5 -> 50
    assert(divisor == 1024 && strcmp(unit, "KB/s") == 0 && ceiling == 50);
    trafficAxisScale(10ull * 1024 * 1024, &divisor, &unit, &ceiling);
    assert(divisor == 1024ull * 1024 && strcmp(unit, "MB/s") == 0
           && ceiling == 10);

    // Axis numbers: one decimal under 10, integers from 10 up.
    char axis[24];
    trafficFormatAxisValue(0.0, axis, sizeof(axis));
    assert(strcmp(axis, "0") == 0);
    trafficFormatAxisValue(0.5, axis, sizeof(axis));
    assert(strcmp(axis, "0.5") == 0);
    trafficFormatAxisValue(1.0, axis, sizeof(axis));
    assert(strcmp(axis, "1.0") == 0);
    trafficFormatAxisValue(25.0, axis, sizeof(axis));
    assert(strcmp(axis, "25") == 0);

    // 9. Axis timestamps are local HH:MM:SS, eight characters with colons.
    char stamp[16];
    trafficTimeLabel((time_t)0, stamp, sizeof(stamp));
    assert(strlen(stamp) == 8 && stamp[2] == ':' && stamp[5] == ':');
    for (int i = 0; i < 8; i++) {
        if (i != 2 && i != 5) {
            assert(stamp[i] >= '0' && stamp[i] <= '9');
        }
    }

    printf("OK: traffic\n");
    return 0;
}
