// Unit tests for the string-table i18n mechanism: language switching,
// missing-key fallback, table integrity (non-empty, no duplicate keys),
// and control binding/retranslation.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "i18n.h"

int main(int argc, char **argv) {
    // 1. The same key resolves to different text per language.
    i18nSetLang(LANG_ZH);
    assert(i18nGetLang() == LANG_ZH);
    assert(strcmp(tr("start"), "开始") == 0);
    i18nSetLang(LANG_EN);
    assert(i18nGetLang() == LANG_EN);
    assert(strcmp(tr("start"), "Start") == 0);

    // 2. Missing key falls back to the key itself (English log stays
    //    intact; UI never shows a blank).
    assert(strcmp(tr("does.not.exist"), "does.not.exist") == 0);
    // Unknown key is language-independent.
    i18nSetLang(LANG_ZH);
    assert(strcmp(tr("does.not.exist"), "does.not.exist") == 0);
    i18nSetLang(LANG_EN);

    // 3. Detection returns a valid language.
    Lang detected = i18nDetect();
    assert(detected == LANG_ZH || detected == LANG_EN);

    // 3b. Whole-table integrity: every row has a key and both texts,
    //     and no key repeats.
    size_t count = i18nTableCount();
    assert(count >= 5);
    for (size_t i = 0; i < count; i++) {
        const char *key, *zh, *en;
        i18nTableEntry(i, &key, &zh, &en);
        assert(key != NULL && key[0] != '\0');
        assert(zh != NULL && zh[0] != '\0');
        assert(en != NULL && en[0] != '\0');
        for (size_t j = 0; j < i; j++) {
            const char *prev, *pz, *pe;
            i18nTableEntry(j, &prev, &pz, &pe);
            assert(strcmp(prev, key) != 0);
        }
    }

    // 4. Bind controls and retranslate live.
    IupOpen(&argc, &argv);
    Ihandle *label = IupLabel("");
    i18nBind(label, "TITLE", "stop");
    i18nSetLang(LANG_EN);
    i18nRetranslate();
    assert(strcmp(IupGetAttribute(label, "TITLE"), "Stop") == 0);
    i18nSetLang(LANG_ZH);
    i18nRetranslate();
    assert(strcmp(IupGetAttribute(label, "TITLE"), "停止") == 0);
    IupClose();

    printf("OK: i18n\n");
    return 0;
}
