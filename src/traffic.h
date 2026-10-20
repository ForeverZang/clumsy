#ifndef CLUMSY_TRAFFIC_H
#define CLUMSY_TRAFFIC_H

#include "common.h"

#define TRAFFIC_BUCKETS 60

/*
 * 60 one-second buckets of byte counts per direction. The newest bucket
 * is the live (still accumulating) second. The owner UI calls
 * trafficTick once per second; SNIFF/intercept paths call trafficRecord.
 */
typedef struct Traffic Traffic;

Traffic* trafficNew(void);
void     trafficDestroy(Traffic *t);
// Clear every bucket and the start marker (called on app switch).
void     trafficReset(Traffic *t);
void     trafficRecord(Traffic *t, UINT bytes, BOOL outbound);
void     trafficTick(Traffic *t);
// Chronological bytes-per-second series: index 0 is the oldest retained
// second, index TRAFFIC_BUCKETS-1 is the live second. Caller-owned arrays.
void     trafficSeries(const Traffic *t, UINT64 down[TRAFFIC_BUCKETS],
                       UINT64 up[TRAFFIC_BUCKETS]);
// Stamp the live second as the "Start" moment.
void     trafficMarkStart(Traffic *t);
// Marker column in series coordinates (0 = oldest, 59 = newest); FALSE
// when no marker exists or it has scrolled past the left edge.
BOOL     trafficStartColumn(const Traffic *t, int *outColumn);

// Self-drawn IupCanvas: blue download / green upload polylines and a red
// dashed start marker. Repaint with IupUpdate() after each tick.
Ihandle* trafficCreateCanvas(Traffic *t);

#endif // CLUMSY_TRAFFIC_H
