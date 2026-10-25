#include "ocr_timeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSV_FIELD_CAP 4096

struct OcrTimeline {
    FILE *fp;
};

void ocrCsvEscape(const char *field, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!field) return;

    BOOL needsQuote = FALSE;
    for (const char *p = field; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\r' || *p == '\n') {
            needsQuote = TRUE;
            break;
        }
    }

    if (!needsQuote) {
        strncpy(out, field, cap - 1);
        out[cap - 1] = '\0';
        return;
    }

    size_t n = 0;
    if (n + 1 < cap) out[n++] = '"';
    for (const char *p = field; *p; p++) {
        if (*p == '"') {
            if (n + 1 < cap) out[n++] = '"';
        }
        if (n + 1 < cap) out[n++] = *p;
        if (n + 1 >= cap) break;
    }
    if (n + 1 < cap) out[n++] = '"';
    out[n] = '\0';
}

// Accept forward and back slashes; create every missing component.
static BOOL createDirsRecursive(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return FALSE;
    memcpy(tmp, path, len + 1);

    for (size_t i = 0; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            if (i == 0) continue; // leading backslash
            char saved = tmp[i];
            tmp[i] = '\0';
            if (!CreateDirectoryA(tmp, NULL)
                && GetLastError() != ERROR_ALREADY_EXISTS) {
                return FALSE;
            }
            tmp[i] = saved;
        }
    }
    if (!CreateDirectoryA(tmp, NULL)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return FALSE;
    }
    return TRUE;
}

static const char *CSV_HEADER =
    "time_ms,time_local,event,region,ocr_text,keyword,edge,"
    "loss_percent,source,note\r\n";

static void writeEscaped(FILE *fp, const char *field) {
    char buf[CSV_FIELD_CAP];
    ocrCsvEscape(field, buf, sizeof(buf));
    fputs(buf, fp);
}

OcrTimeline *ocrTimelineOpen(const char *dirHint, char *pathOut,
                             size_t pathCap, char err[MSG_BUFSIZE]) {
    if (!dirHint || !pathOut || pathCap == 0) {
        if (err) strncpy(err, "bad arguments", MSG_BUFSIZE - 1);
        return NULL;
    }
    pathOut[0] = '\0';
    if (err) err[0] = '\0';

    if (!createDirsRecursive(dirHint)) {
        if (err) {
            snprintf(err, MSG_BUFSIZE,
                     "cannot create timeline directory (win32 error %lu)",
                     GetLastError());
        }
        return NULL;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[128];
    snprintf(name, sizeof(name),
             "ocr-run-%04d%02d%02d-%02d%02d%02d-%03d.csv",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);

    char full[1200];
    snprintf(full, sizeof(full), "%s\\%s", dirHint, name);
    FILE *fp = NULL;
    int ferr = fopen_s(&fp, full, "wb");
    if (ferr != 0 || !fp) {
        snprintf(err, MSG_BUFSIZE,
                 "cannot create timeline file (errno %d)", ferr);
        return NULL;
    }

    fwrite("\xEF\xBB\xBF", 1, 3, fp); // UTF-8 BOM
    fputs(CSV_HEADER, fp);

    if (strlen(full) >= pathCap) {
        fclose(fp);
        DeleteFileA(full);
        strncpy(err, "timeline path longer than buffer",
                MSG_BUFSIZE - 1);
        return NULL;
    }
    strcpy(pathOut, full);

    OcrTimeline *tl = calloc(1, sizeof(*tl));
    if (!tl) {
        fclose(fp);
        DeleteFileA(full);
        strncpy(err, "out of memory", MSG_BUFSIZE - 1);
        return NULL;
    }
    tl->fp = fp;
    return tl;
}

static void localTimestamp(char *out, size_t cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

BOOL ocrTimelineEvent(OcrTimeline *tl, const OcrTimelineEvent *ev) {
    if (!tl || !tl->fp || !ev) return FALSE;

    char ts[32];
    localTimestamp(ts, sizeof(ts));

    if (fprintf(tl->fp, "%llu,", (unsigned long long)ev->timeMs) < 0)
        return FALSE;
    fputs(ts, tl->fp);
    fputc(',', tl->fp);

    writeEscaped(tl->fp, ev->event);
    fputc(',', tl->fp);
    writeEscaped(tl->fp, ev->region);
    fputc(',', tl->fp);
    writeEscaped(tl->fp, ev->ocrText);
    fputc(',', tl->fp);
    writeEscaped(tl->fp, ev->keyword);
    fputc(',', tl->fp);
    writeEscaped(tl->fp, ev->edge);
    fputc(',', tl->fp);
    if (ev->lossPercent >= 0) {
        fprintf(tl->fp, "%d", ev->lossPercent);
    }
    fputc(',', tl->fp);
    writeEscaped(tl->fp, ev->source);
    fputc(',', tl->fp);
    writeEscaped(tl->fp, ev->note);
    fputs("\r\n", tl->fp);

    fflush(tl->fp);
    return TRUE;
}

void ocrTimelineClose(OcrTimeline *tl) {
    if (!tl) return;
    if (tl->fp) fclose(tl->fp);
    free(tl);
}

static void exeDir(char *out, size_t cap) {
    out[0] = '\0';
    GetModuleFileNameA(NULL, out, (DWORD)cap);
    char *slash = strrchr(out, '\\');
    if (slash) slash[1] = '\0';
    else out[0] = '\0';
}

BOOL ocrTimelineDefaultDir(char *out, size_t cap, char *fallback,
                           size_t fbCap) {
    if (out && cap) out[0] = '\0';
    if (fallback && fbCap) {
        fallback[0] = '\0';
        exeDir(fallback, fbCap);
    }

    const char *appdata = getenv("LOCALAPPDATA");
    char candidate[1024];
    if (appdata && appdata[0]) {
        snprintf(candidate, sizeof(candidate),
                 "%s\\clumsy\\ocr-runs", appdata);
    } else {
        return FALSE;
    }

    if (strlen(candidate) + 1 > cap || !createDirsRecursive(candidate)) {
        return FALSE;
    }
    strcpy(out, candidate);
    return TRUE;
}
