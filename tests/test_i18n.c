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

// Count printf conversion specifiers, treating %% as a literal percent.
static size_t placeholderCount(const char *s) {
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '%') {
            if (*(p + 1) == '%') {
                p++;
            } else {
                n++;
            }
        }
    }
    return n;
}

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
    assert(count >= 63); // full Task 11 UI vocabulary
    for (size_t i = 0; i < count; i++) {
        const char *key, *zh, *en;
        i18nTableEntry(i, &key, &zh, &en);
        assert(key != NULL && key[0] != '\0');
        assert(zh != NULL && zh[0] != '\0');
        assert(en != NULL && en[0] != '\0');
        // Interpolated UI strings must take the same arguments per language.
        assert(placeholderCount(zh) == placeholderCount(en));
        for (size_t j = 0; j < i; j++) {
            const char *prev, *pz, *pe;
            i18nTableEntry(j, &prev, &pz, &pe);
            assert(strcmp(prev, key) != 0);
        }
    }

    // 3c. Beginner UI vocabulary resolves in both languages.
    i18nSetLang(LANG_ZH);
    assert(strcmp(tr("action.start"), "开始影响") == 0);
    assert(strcmp(tr("action.stop"), "停止影响") == 0);
    assert(strcmp(tr("preset.offline"), "完全断网 100%") == 0);
    assert(strcmp(tr("preset.severe"), "严重丢包 95%") == 0);
    assert(strcmp(tr("preset.normal"), "正常 0%") == 0);
    assert(strcmp(tr("app.all"), "不限制应用") == 0);
    assert(strcmp(tr("status.selectApp"), "请先选择一个应用") == 0);
    assert(strcmp(tr("advanced.toggle"), "高级功能") == 0);
    i18nSetLang(LANG_EN);
    assert(strcmp(tr("action.start"), "Start") == 0);
    assert(strcmp(tr("action.stop"), "Stop") == 0);
    assert(strcmp(tr("app.all"), "All applications") == 0);
    assert(strcmp(tr("direction.all"), "Both") == 0);

    // 3d. Advanced modules keep English names in EN and get Chinese
    //     display names in ZH.
    assert(strcmp(tr("mod.lag"), "Lag") == 0);
    assert(strcmp(tr("mod.bandwidth"), "Bandwidth") == 0);
    i18nSetLang(LANG_ZH);
    assert(strcmp(tr("mod.lag"), "延迟") == 0);
    assert(strcmp(tr("mod.drop"), "丢包") == 0);
    assert(strcmp(tr("mod.throttle"), "节流") == 0);
    assert(strcmp(tr("mod.ood"), "乱序") == 0);
    assert(strcmp(tr("mod.duplicate"), "重复") == 0);
    assert(strcmp(tr("mod.tamper"), "篡改") == 0);
    assert(strcmp(tr("mod.reset"), "RST 重置") == 0);
    assert(strcmp(tr("mod.bandwidth"), "限速") == 0);
    i18nSetLang(LANG_EN);

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
