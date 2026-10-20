#ifndef CLUMSY_PROCMAP_H
#define CLUMSY_PROCMAP_H

#include <winsock2.h>
#include <windows.h>

// One TCP/UDP endpoint owned by a process, as reported by the IP Helper
// tables. Address words and ports stay in NETWORK byte order (WinDivert
// convention); IPv4 uses [0] with the other words zero.
typedef struct {
    UINT8  protocol;          // IPPROTO_TCP / IPPROTO_UDP
    UINT32 localAddr[4];
    UINT16 localPort;
    UINT32 remoteAddr[4];
    UINT16 remotePort;
    UINT32 pid;
} ProcEndpoint;

// All processes sharing one executable image, e.g. 12 chrome.exe PIDs.
typedef struct {
    char    exeName[64];      // e.g. "chrome.exe" (UTF-8, ASCII basename)
    wchar_t exePath[MAX_PATH];
    UINT32  pids[256];
    UINT    pidCount;
} AppGroup;

// Enumerate processes that currently own TCP/UDP endpoints, grouped by
// image name. Fills at most `cap` groups; on success *outCount is the
// group count. Returns FALSE (and sets *outCount to the required count)
// when the buffer is too small, or when IP Helper fails.
BOOL procmapEnumApps(AppGroup *out, size_t cap, size_t *outCount);

// Snapshot all established/listening TCP and all UDP endpoints.
// On success *out is a malloc'd array released with procmapFree.
BOOL procmapSnapshotEndpoints(ProcEndpoint **out, size_t *outCount);

void procmapFree(ProcEndpoint *endpoints);

#endif // CLUMSY_PROCMAP_H
