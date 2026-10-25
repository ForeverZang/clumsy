#ifndef CLUMSY_OCR_CAPTURE_H
#define CLUMSY_OCR_CAPTURE_H

#include <windows.h>

#include "common.h"

/*
 * Screen capture support for OCR: target window enumeration by process
 * image name, normalized client-area rectangles, tiny grayscale
 * thumbnails for change detection, and PrintWindow-based BGRA frames.
 */

typedef struct {
    double x, y, w, h; // client-area fractions, clamped to 0..1
} NormRect;

typedef struct {
    HWND hwnd;
    DWORD pid;
    char  title[256]; // UTF-8
    int   width;      // client size
    int   height;
} CaptureWindow;

// Top-level visible windows owned by any PID in the named app group.
// Returns the number written (0 when the app owns no endpoints/windows).
int captureEnumWindowsForApp(const char *exeName, CaptureWindow *out,
                             int cap);

// Saved-title vs current-title match: 0 none, 1 normalized substring,
// 2 trim + case-insensitive exact.
int captureTitleMatchLevel(const char *saved, const char *candidate);

// Pick the unique best match for savedTitle among candidates.
// *matchCount is 0 (gone), 1 (unique: exact beats substring) or >1
// (ambiguous; out left untouched).
BOOL captureResolveWindow(const CaptureWindow *cands, int candCount,
                          const char *savedTitle, CaptureWindow *out,
                          int *matchCount);

// Map a normalized rect to physical client pixels, clamped on all sides.
RECT captureMapNorm(int clientW, int clientH, const NormRect *r);

// Downscale a BGRA frame to a gray thumbnail of fixed width (height
// proportional, at least 1), block-averaged. outTh receives thumbW,
// outThumbH the computed height; caller sizes grayOut for that.
void captureMakeThumbnail(const unsigned char *bgra, int w, int h,
                          int thumbW, unsigned char *grayOut, int *outTh,
                          int *outThumbH);

// Mean absolute gray difference strictly greater than madThreshold.
BOOL captureChanged(const unsigned char *grayA, const unsigned char *grayB,
                    int pixels, int madThreshold);

// PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT) of hwnd's client area
// (or the sub rectangle when non-NULL). On success *outBgra is a malloc'd
// top-down BGRA buffer released with captureFreeBitmap.
BOOL captureClientRegion(HWND hwnd, const RECT *sub,
                         unsigned char **outBgra, int *outW, int *outH,
                         char err[MSG_BUFSIZE]);
void captureFreeBitmap(unsigned char *bgra);

// Window still exists, is visible and not minimized.
BOOL captureIsCapturable(HWND hwnd);

#endif // CLUMSY_OCR_CAPTURE_H
