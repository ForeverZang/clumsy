// Unit tests for the OCR CSV timeline: RFC4180 escaping, header/BOM,
// row shape for a session_start and a rule_trigger event, and default
// directory resolution under LOCALAPPDATA.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "ocr_timeline.h"

static const char *HEADER =
    "time_ms,time_local,event,region,ocr_text,keyword,edge,"
    "loss_percent,source,note";

static void ensureDir(const char *path) {
    CreateDirectoryA(path, NULL); // ERROR_ALREADY_EXISTS is fine
}

// Read the whole file (<= 64 KiB here).
static char *readAll(const char *path) {
    FILE *fp = NULL;
    fopen_s(&fp, path, "rb");
    assert(fp);
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    assert(n > 0 && n < 65536);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)n, fp);
    assert(got == (size_t)n);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}

// Split one CSV line into unparsed columns (no quoted fields are used
// in the rows examined here); strips a trailing CR.
static int splitLine(char *line, char *cols[], int max) {
    if (line[0] == '\0') return 0;
    int n = 0;
    cols[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == ',') {
            *p = '\0';
            cols[n++] = p + 1;
        }
    }
    size_t len = strlen(cols[n - 1]);
    if (len && cols[n - 1][len - 1] == '\r') cols[n - 1][len - 1] = '\0';
    return n;
}

int main(void) {
    char out[512];

    // --- ocrCsvEscape ------------------------------------------------

    // 1. Plain text passes through unchanged.
    ocrCsvEscape("session_start", out, sizeof(out));
    assert(strcmp(out, "session_start") == 0);

    // 2. Comma triggers quoting.
    ocrCsvEscape("a,b", out, sizeof(out));
    assert(strcmp(out, "\"a,b\"") == 0);

    // 3. Quotes are doubled inside a quoted field.
    ocrCsvEscape("a\"b", out, sizeof(out));
    assert(strcmp(out, "\"a\"\"b\"") == 0);

    // 4. Newline (LF or CRLF) triggers quoting.
    ocrCsvEscape("a\nb", out, sizeof(out));
    assert(strcmp(out, "\"a\nb\"") == 0);
    ocrCsvEscape("a\r\nb", out, sizeof(out));
    assert(strcmp(out, "\"a\r\nb\"") == 0);

    // 5. NULL and empty both render as an empty field.
    ocrCsvEscape(NULL, out, sizeof(out));
    assert(strcmp(out, "") == 0);
    ocrCsvEscape("", out, sizeof(out));
    assert(strcmp(out, "") == 0);

    // 6. CJK UTF-8 bytes are untouched and a plain field stays unquoted.
    ocrCsvEscape("重连", out, sizeof(out));
    assert(strcmp(out, "重连") == 0);

    // --- open / write / close round trip -----------------------------

    const char *dir = "build/tests/tl_tmp";
    ensureDir(dir);
    char path[512];
    char err[MSG_BUFSIZE];
    OcrTimeline *tl = ocrTimelineOpen(dir, path, sizeof(path), err);
    assert(tl);
    assert(path[0] != '\0');
    assert(strstr(path, dir) == path);
    assert(strstr(path, ".csv") != NULL);

    OcrTimelineEvent start = {0};
    start.timeMs      = 0;
    start.event       = "session_start";
    start.lossPercent = -1;
    start.source      = "initial_slider";
    assert(ocrTimelineEvent(tl, &start));

    OcrTimelineEvent trig = {0};
    trig.timeMs      = 250;
    trig.event       = "rule_trigger";
    trig.region      = "center";
    trig.ocrText     = "正在重连";
    trig.keyword     = "重连";
    trig.edge        = "appear";
    trig.lossPercent = 50;
    trig.source      = "rule:reconnect";
    trig.note        = "confirm=2";
    assert(ocrTimelineEvent(tl, &trig));

    ocrTimelineClose(tl);

    char *raw = readAll(path);

    // 7. File begins with the UTF-8 BOM.
    assert((unsigned char)raw[0] == 0xEF
           && (unsigned char)raw[1] == 0xBB
           && (unsigned char)raw[2] == 0xBF);

    // 8. First line is exactly the fixed header.
    char *line1 = raw + 3;
    char *eol1 = strstr(line1, "\r\n");
    assert(eol1);
    *eol1 = '\0';
    assert(strcmp(line1, HEADER) == 0);

    // 9. Row 1: session_start, empty optionals, -1 renders empty.
    char *line2 = eol1 + 2;
    char *eol2 = strstr(line2, "\r\n");
    assert(eol2);
    *eol2 = '\0';
    char *c1[10];
    assert(splitLine(line2, c1, 10) == 10);
    assert(strcmp(c1[0], "0") == 0);
    assert(c1[1][0] != '\0');                  // time_local present
    assert(strcmp(c1[2], "session_start") == 0);
    assert(c1[3][0] == '\0');                  // region
    assert(c1[4][0] == '\0');                  // ocr_text
    assert(c1[5][0] == '\0');                  // keyword
    assert(c1[6][0] == '\0');                  // edge
    assert(c1[7][0] == '\0');                  // loss_percent for -1
    assert(strcmp(c1[8], "initial_slider") == 0);
    assert(c1[9][0] == '\0');                  // note

    // 10. Row 2: rule_trigger carries every field.
    char *line3 = eol2 + 2;
    char *eol3 = strstr(line3, "\r\n");
    assert(eol3);
    *eol3 = '\0';
    char *c2[10];
    assert(splitLine(line3, c2, 10) == 10);
    assert(strcmp(c2[0], "250") == 0);
    assert(c2[1][0] != '\0');
    assert(strcmp(c2[2], "rule_trigger") == 0);
    assert(strcmp(c2[3], "center") == 0);
    assert(strcmp(c2[4], "正在重连") == 0);
    assert(strcmp(c2[5], "重连") == 0);
    assert(strcmp(c2[6], "appear") == 0);
    assert(strcmp(c2[7], "50") == 0);
    assert(strcmp(c2[8], "rule:reconnect") == 0);
    assert(strcmp(c2[9], "confirm=2") == 0);
    assert(eol3[2] == '\0'); // exactly three lines total
    free(raw);

    // 11. NULL-safe close and event are no-ops.
    ocrTimelineClose(NULL);
    assert(!ocrTimelineEvent(NULL, &start));

    // 12. A directory whose parent is a regular file cannot be created;
    //     open fails cleanly with an error message (drive-letter free).
    const char *blocker = "build/tests/tl_blocker";
    FILE *bf = NULL;
    fopen_s(&bf, blocker, "wb");
    assert(bf);
    fputs("not a directory", bf);
    fclose(bf);
    char badErr[MSG_BUFSIZE];
    char badPath[256];
    OcrTimeline *bad = ocrTimelineOpen(
        "build/tests/tl_blocker/sub/ocr-runs", badPath, sizeof(badPath),
        badErr);
    assert(!bad);
    assert(badErr[0] != '\0');
    assert(badPath[0] == '\0');
    DeleteFileA(blocker);

    // --- default directory resolution --------------------------------

    const char *saved = getenv("LOCALAPPDATA");
    char savedBuf[1024];
    if (saved) {
        strncpy(savedBuf, saved, sizeof(savedBuf) - 1);
        savedBuf[sizeof(savedBuf) - 1] = '\0';
    }

    // 13. LOCALAPPDATA pointed at a fresh temp base: path ends in
    //     ocr-runs and the directory is created.
    char appdata[512];
    GetFullPathNameA("build/tests/tl_appdata", sizeof(appdata), appdata,
                     NULL);
    _putenv_s("LOCALAPPDATA", appdata);
    char dirOut[512];
    char fallback[512];
    assert(ocrTimelineDefaultDir(dirOut, sizeof(dirOut), fallback,
                                 sizeof(fallback)));
    size_t dl = strlen(dirOut);
    assert(dl >= 8 && strcmp(dirOut + dl - 8, "ocr-runs") == 0);
    assert(GetFileAttributesA(dirOut)
           != INVALID_FILE_ATTRIBUTES);

    // 14. A too-small output buffer fails and fills the fallback hint.
    char tiny[8];
    fallback[0] = '\0';
    assert(!ocrTimelineDefaultDir(tiny, sizeof(tiny), fallback,
                                  sizeof(fallback)));
    assert(fallback[0] != '\0');

    if (saved) _putenv_s("LOCALAPPDATA", savedBuf);
    else _putenv_s("LOCALAPPDATA", "");

    printf("OK: ocr_timeline\n");
    return 0;
}
