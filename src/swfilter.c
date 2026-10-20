// Software eval path for the advanced filter text box.
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "swfilter.h"

// WinDivert compiles to a normalized string a little longer than the
// input; 8 KiB comfortably covers anything the one-line text box holds.
#define SWFILTER_COMPILED_MAX 8192

static char *g_compiled = NULL; // NULL = gate disabled

void swfilterClear(void) {
    free(g_compiled);
    g_compiled = NULL;
}

BOOL swfilterActive(void) {
    return g_compiled != NULL;
}

static void setError(char err[MSG_BUFSIZE], const char *detail, UINT pos) {
    if (err == NULL) {
        return;
    }
    if (detail == NULL || detail[0] == '\0') {
        detail = "invalid filter";
    }
    snprintf(err, MSG_BUFSIZE, "%s (column %u)", detail, pos);
}

// Compile into a stack temp first, so a failed recompile leaves the
// previously active filter untouched.
BOOL swfilterSet(const char *text, char err[MSG_BUFSIZE]) {
    if (err != NULL) {
        err[0] = '\0';
    }
    if (text == NULL) {
        swfilterClear();
        return TRUE;
    }
    const char *p = text;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0') {
        swfilterClear();
        return TRUE;
    }

    char temp[SWFILTER_COMPILED_MAX];
    const char *errorStr = NULL;
    UINT errorPos = 0;
    if (!WinDivertHelperCompileFilter(text, WINDIVERT_LAYER_NETWORK,
                                      temp, sizeof(temp),
                                      &errorStr, &errorPos)) {
        setError(err, errorStr, errorPos);
        return FALSE;
    }

    char *buf = malloc(SWFILTER_COMPILED_MAX);
    if (buf == NULL) {
        if (err != NULL) {
            strncpy(err, "out of memory", MSG_BUFSIZE - 1);
            err[MSG_BUFSIZE - 1] = '\0';
        }
        return FALSE;
    }
    strcpy(buf, temp);
    char *old = g_compiled;
    g_compiled = buf;
    free(old);
    return TRUE;
}

BOOL swfilterMatch(const char *packet, UINT packetLen,
                   const WINDIVERT_ADDRESS *addr) {
    if (g_compiled == NULL) {
        return TRUE; // no advanced filter: this AND leg always passes
    }
    if (packet == NULL || addr == NULL) {
        return FALSE;
    }
    return WinDivertHelperEvalFilter(g_compiled, packet, packetLen, addr);
}
