// Central UI string table with live language switching. Log lines stay
// English by design; only UI text goes through tr().
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

typedef struct {
    const char *key;
    const char *zh;
    const char *en;
} I18nEntry;

// Seed rows for the mechanism; Task 11 expands the table and replaces the
// literals in the UI. All text is UTF-8 (T11 enables IUP UTF8MODE).
static const I18nEntry g_table[] = {
    {"app.title",       "游戏流量控制器", "Game Traffic Controller"},
    {"start",           "开始",           "Start"},
    {"stop",            "停止",           "Stop"},
    {"direction.all",   "全部",           "All"},
    {"direction.in",    "流入（下载）",   "Inbound (download)"},
    {"direction.out",   "流出（上传）",   "Outbound (upload)"},
};

static Lang g_lang = LANG_EN;

typedef struct {
    Ihandle *control;
    char    *attr;
    char    *key;
} Binding;

static Binding *g_bindings = NULL;
static size_t   g_bindingCount = 0;

// Keys already reported missing, so each is LOG'd exactly once.
static char   **g_missing = NULL;
static size_t   g_missingCount = 0;

Lang i18nDetect(void) {
    LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_CHINESE ? LANG_ZH : LANG_EN;
}

void i18nSetLang(Lang lang) {
    g_lang = lang;
}

Lang i18nGetLang(void) {
    return g_lang;
}

static const I18nEntry *findEntry(const char *key) {
    if (key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++) {
        if (strcmp(g_table[i].key, key) == 0) {
            return &g_table[i];
        }
    }
    return NULL;
}

static BOOL missingAlreadyLogged(const char *key) {
    for (size_t i = 0; i < g_missingCount; i++) {
        if (strcmp(g_missing[i], key) == 0) {
            return TRUE;
        }
    }
    char *copy = _strdup(key);
    if (copy == NULL) {
        return TRUE; // fail quiet: logging must never break the UI
    }
    char **grown = realloc(g_missing, sizeof(char *) * (g_missingCount + 1));
    if (grown == NULL) {
        free(copy);
        return TRUE;
    }
    g_missing = grown;
    g_missing[g_missingCount++] = copy;
    return FALSE;
}

const char *tr(const char *key) {
    if (key == NULL) {
        return "";
    }
    const I18nEntry *entry = findEntry(key);
    if (entry != NULL) {
        return g_lang == LANG_ZH ? entry->zh : entry->en;
    }
    if (!missingAlreadyLogged(key)) {
        LOG("i18n: missing key '%s'", key);
    }
    return key;
}

static char *dupString(const char *s) {
    char *copy = _strdup(s != NULL ? s : "");
    if (copy == NULL) {
        LOG("i18n: out of memory binding attribute");
    }
    return copy;
}

void i18nBind(Ihandle *control, const char *attr, const char *key) {
    if (control == NULL || attr == NULL || key == NULL) {
        return;
    }
    Binding *grown = realloc(g_bindings,
                             sizeof(Binding) * (g_bindingCount + 1));
    if (grown == NULL) {
        LOG("i18n: out of memory binding control");
        return;
    }
    g_bindings = grown;
    g_bindings[g_bindingCount].control = control;
    g_bindings[g_bindingCount].attr = dupString(attr);
    g_bindings[g_bindingCount].key = dupString(key);
    if (g_bindings[g_bindingCount].attr == NULL
        || g_bindings[g_bindingCount].key == NULL) {
        free(g_bindings[g_bindingCount].attr);
        free(g_bindings[g_bindingCount].key);
        return;
    }
    g_bindingCount++;
}

void i18nRetranslate(void) {
    for (size_t i = 0; i < g_bindingCount; i++) {
        IupSetAttribute(g_bindings[i].control, g_bindings[i].attr,
                        tr(g_bindings[i].key));
    }
}

size_t i18nTableCount(void) {
    return sizeof(g_table) / sizeof(g_table[0]);
}

void i18nTableEntry(size_t index, const char **key,
                    const char **zh, const char **en) {
    if (index >= i18nTableCount()) {
        *key = *zh = *en = "";
        return;
    }
    *key = g_table[index].key;
    *zh = g_table[index].zh;
    *en = g_table[index].en;
}
