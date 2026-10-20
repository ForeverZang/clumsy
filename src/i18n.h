#ifndef CLUMSY_I18N_H
#define CLUMSY_I18N_H

#include "common.h"

typedef enum {
    LANG_ZH = 0,
    LANG_EN = 1
} Lang;

// Follow the OS user UI language: Chinese variants -> ZH, else English.
Lang        i18nDetect(void);
void        i18nSetLang(Lang lang);
Lang        i18nGetLang(void);

// Translate one key. A missing key returns the key itself (logged once in
// English) so the interface never goes blank.
const char *tr(const char *key);

// Register a control attribute (TITLE/VALUE/...) to be refreshed from the
// string table on every i18nRetranslate() call (live language switch).
void        i18nBind(Ihandle *control, const char *attr, const char *key);
void        i18nRetranslate(void);

// Table inspection for the integrity test.
size_t      i18nTableCount(void);
void        i18nTableEntry(size_t index, const char **key,
                           const char **zh, const char **en);

#endif // CLUMSY_I18N_H
