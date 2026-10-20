// Filtering frame, filter presets and the 8 classic function modules,
// moved out of main.c unchanged in behavior. All visible text goes
// through i18n keys.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <Windows.h>

#include "iup.h"
#include "common.h"
#include "i18n.h"
#include "ui_advanced.h"

// Provided by main.c; order is the module pipeline order.
extern Module *modules[MODULE_CNT];

// --------------------------------------------------------------------------
// config.txt filter presets (custom serialization, unchanged from main.c)
// --------------------------------------------------------------------------

#define CONFIG_FILE "config.txt"
#define CONFIG_MAX_RECORDS 64
#define CONFIG_BUF_SIZE 4096

typedef struct {
    char *filterName;
    char *filterValue;
} filterRecord;

static UINT filtersSize;
static filterRecord filters[CONFIG_MAX_RECORDS] = {0};
static char configBuf[CONFIG_BUF_SIZE + 2]; // room for the patched '\n'

void uiAdvancedLoadConfig(void) {
    char path[MSG_BUFSIZE];
    char *p;
    FILE *f;
    GetModuleFileName(NULL, path, MSG_BUFSIZE);
    LOG("Executable path: %s", path);
    p = strrchr(path, '\\');
    if (p == NULL) p = strrchr(path, '/'); // holy shit
    strcpy(p + 1, CONFIG_FILE);
    LOG("Config path: %s", path);
    f = fopen(path, "r");
    if (f) {
        size_t len;
        char *current, *last;
        len = fread(configBuf, sizeof(char), CONFIG_BUF_SIZE, f);
        if (len == CONFIG_BUF_SIZE) {
            LOG("Config file is larger than %d bytes, get truncated.",
                CONFIG_BUF_SIZE);
        }
        // always patch in a newline at the end to ease parsing
        configBuf[len] = '\n';
        configBuf[len + 1] = '\0';

        // parse out the kv pairs. isn't quite safe
        filtersSize = 0;
        last = current = configBuf;
        do {
            // eat up empty lines
EAT_SPACE:
            while (isspace((unsigned char)*current)) { ++current; }
            if (*current == '#') {
                current = strchr(current, '\n');
                if (!current) break;
                current = current + 1;
                goto EAT_SPACE;
            }

            // now we can start
            last = current;
            current = strchr(last, ':');
            if (!current) break;
            *current = '\0';
            filters[filtersSize].filterName = last;
            current += 1;
            while (isspace((unsigned char)*current)) { ++current; }
            last = current;
            current = strchr(last, '\n');
            if (!current) break;
            filters[filtersSize].filterValue = last;
            *current = '\0';
            if (*(current - 1) == '\r') *(current - 1) = 0;
            last = current = current + 1;
            ++filtersSize;
        } while (last && last - configBuf < CONFIG_BUF_SIZE);
        LOG("Loaded %u records.", filtersSize);
    }

    if (!f || filtersSize == 0) {
        LOG("Failed to load from config. Fill in a simple one.");
        // config is missing or ill-formed. fill in some simple ones
        filters[filtersSize].filterName = "loopback packets";
        filters[filtersSize].filterValue =
            "outbound and ip.DstAddr >= 127.0.0.1 and "
            "ip.DstAddr <= 127.255.255.255";
        filtersSize = 1;
    }
}

// --------------------------------------------------------------------------
// UI
// --------------------------------------------------------------------------

static Ihandle *filterText, *filterSelectList,
                *stateIcon, *errorLabel;

static int uiListSelectCb(Ihandle *ih, char *text, int item, int state) {
    UNREFERENCED_PARAMETER(ih);
    UNREFERENCED_PARAMETER(text);
    if (state == 1) {
        IupSetAttribute(filterText, "VALUE", filters[item - 1].filterValue);
    }
    return IUP_DEFAULT;
}

static int uiFilterTextCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    // unselect list
    IupSetAttribute(filterSelectList, "VALUE", "0");
    // any manual edit clears the last compile error
    uiAdvancedSetFilterError(NULL);
    return IUP_DEFAULT;
}

static int uiToggleControls(Ihandle *ih, int state) {
    Ihandle *controls = (Ihandle *)IupGetAttribute(ih, CONTROLS_HANDLE);
    short *target = (short *)IupGetAttribute(ih, SYNCED_VALUE);
    int controlsActive = IupGetInt(controls, "ACTIVE");
    if (controlsActive && !state) {
        IupSetAttribute(controls, "ACTIVE", "NO");
        InterlockedExchange16(target, I2S(state));
    } else if (!controlsActive && state) {
        IupSetAttribute(controls, "ACTIVE", "YES");
        InterlockedExchange16(target, I2S(state));
    }
    return IUP_DEFAULT;
}

static const char *moduleNameKey(const Module *module) {
    if (strcmp(module->shortName, "lag") == 0)       return "mod.lag";
    if (strcmp(module->shortName, "drop") == 0)      return "mod.drop";
    if (strcmp(module->shortName, "throttle") == 0)  return "mod.throttle";
    if (strcmp(module->shortName, "duplicate") == 0) return "mod.duplicate";
    if (strcmp(module->shortName, "ood") == 0)       return "mod.ood";
    if (strcmp(module->shortName, "tamper") == 0)    return "mod.tamper";
    if (strcmp(module->shortName, "reset") == 0)     return "mod.reset";
    if (strcmp(module->shortName, "bandwidth") == 0) return "mod.bandwidth";
    return NULL;
}

static void uiSetupModule(Module *module, Ihandle *parent) {
    Ihandle *groupBox, *toggle, *controls, *icon;
    const char *nameKey = moduleNameKey(module);
    groupBox = IupHbox(
        icon = IupLabel(NULL),
        toggle = IupToggle(nameKey != NULL ? tr(nameKey)
                                          : module->displayName,
                           NULL),
        IupFill(),
        controls = module->setupUIFunc(),
        NULL
    );
    if (nameKey != NULL) {
        i18nBind(toggle, "TITLE", nameKey);
    }
    IupSetAttribute(groupBox, "EXPAND", "HORIZONTAL");
    IupSetAttribute(groupBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(controls, "ALIGNMENT", "ACENTER");
    IupAppend(parent, groupBox);

    // set controls as attribute to toggle and enable toggle callback
    IupSetCallback(toggle, "ACTION", (Icallback)uiToggleControls);
    IupSetAttribute(toggle, CONTROLS_HANDLE, (char *)controls);
    IupSetAttribute(toggle, SYNCED_VALUE, (char *)module->enabledFlag);
    IupSetAttribute(controls, "ACTIVE", "NO"); // startup as inactive
    IupSetAttribute(controls, "NCGAP", "4");

    // set default icon
    IupSetAttribute(icon, "IMAGE", "none_icon");
    IupSetAttribute(icon, "PADDING", "4x");
    module->iconHandle = icon;

    // parameterize toggle
    if (parameterized) {
        setFromParameter(toggle, "VALUE", module->shortName);
    }
}

Ihandle *uiAdvancedCreate(void) {
    UINT ix;
    Ihandle *topFrame, *topVbox, *controlHbox, *presetsLabel;
    Ihandle *bottomFrame, *bottomVbox, *loopbackLabel;
    Ihandle *noneIcon, *doingIcon, *errorIcon;

    topFrame = IupFrame(
        topVbox = IupVbox(
            filterText = IupText(NULL),
            controlHbox = IupHbox(
                stateIcon = IupLabel(NULL),
                IupFill(),
                presetsLabel = IupLabel(tr("advanced.presets")),
                filterSelectList = IupList(NULL),
                NULL
            ),
            errorLabel = IupLabel(""),
            loopbackLabel = IupFlatLabel(tr("advanced.loopbackNotice")),
            NULL
        )
    );
    i18nBind(topFrame, "TITLE", "advanced.filtering");
    i18nBind(presetsLabel, "TITLE", "advanced.presets");
    i18nBind(loopbackLabel, "TITLE", "advanced.loopbackNotice");
    IupSetAttribute(loopbackLabel, "FGCOLOR", "110 110 110");
    IupSetAttribute(loopbackLabel, "WRAP", "YES");
    IupSetAttribute(loopbackLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(topFrame, "EXPAND", "HORIZONTAL");
    IupSetAttribute(filterText, "EXPAND", "HORIZONTAL");
    IupSetCallback(filterText, "VALUECHANGED_CB", (Icallback)uiFilterTextCb);
    IupSetAttribute(topVbox, "NCMARGIN", "4x4");
    IupSetAttribute(topVbox, "NCGAP", "4x2");
    IupSetAttribute(controlHbox, "ALIGNMENT", "ACENTER");

    // setup state icon
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");
    IupSetAttribute(stateIcon, "PADDING", "4x");

    // red compile-error line, hidden while empty
    IupSetAttribute(errorLabel, "FGCOLOR", "208 70 72");
    IupSetAttribute(errorLabel, "EXPAND", "HORIZONTAL");

    // fill in options and setup callback
    IupSetAttribute(filterSelectList, "VISIBLECOLUMNS", "24");
    IupSetAttribute(filterSelectList, "DROPDOWN", "YES");
    for (ix = 0; ix < filtersSize; ++ix) {
        char ixBuf[4];
        sprintf(ixBuf, "%d", ix + 1); // ! starting from 1, lua indexing
        IupStoreAttribute(filterSelectList, ixBuf, filters[ix].filterName);
    }
    // No preset selected initially: the beginner default must pass every
    // packet (kernel filter stays "true"); choosing a preset opts in.
    IupSetAttribute(filterSelectList, "VALUE", "0");
    IupSetCallback(filterSelectList, "ACTION", (Icallback)uiListSelectCb);
    IupSetAttribute(filterText, "VALUE", "true");

    // functions frame
    bottomFrame = IupFrame(bottomVbox = IupVbox(NULL));
    i18nBind(bottomFrame, "TITLE", "advanced.functions");
    IupSetAttribute(bottomVbox, "NCMARGIN", "4x4");
    IupSetAttribute(bottomVbox, "NCGAP", "4x2");

    // create icons
    noneIcon = IupImage(8, 8, icon8x8);
    doingIcon = IupImage(8, 8, icon8x8);
    errorIcon = IupImage(8, 8, icon8x8);
    IupSetAttribute(noneIcon, "0", "BGCOLOR");
    IupSetAttribute(noneIcon, "1", "224 224 224");
    IupSetAttribute(doingIcon, "0", "BGCOLOR");
    IupSetAttribute(doingIcon, "1", "109 170 44");
    IupSetAttribute(errorIcon, "0", "BGCOLOR");
    IupSetAttribute(errorIcon, "1", "208 70 72");
    IupSetHandle("none_icon", noneIcon);
    IupSetHandle("doing_icon", doingIcon);
    IupSetHandle("error_icon", errorIcon);

    // setup module uis
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        uiSetupModule(modules[ix], bottomVbox);
    }

    return IupVbox(topFrame, bottomFrame, NULL);
}

const char *uiAdvancedFilterValue(void) {
    return IupGetAttribute(filterText, "VALUE");
}

void uiAdvancedSetFilterValue(const char *value) {
    IupSetAttribute(filterText, "VALUE", value);
}

void uiAdvancedSetFilterEditable(BOOL editable) {
    IupSetAttribute(filterText, "ACTIVE", editable ? "YES" : "NO");
}

void uiAdvancedSetFilterError(const char *msg) {
    IupStoreAttribute(errorLabel, "TITLE",
                      (msg != NULL && msg[0] != '\0') ? msg : "");
}

void uiAdvancedTimerTick(void) {
    int ix;
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (modules[ix]->processTriggered) {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "doing_icon");
            InterlockedAnd16(&(modules[ix]->processTriggered), 0);
        } else {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
        }
    }

    // update global send status icon
    switch (sendState) {
    case SEND_STATUS_NONE:
        IupSetAttribute(stateIcon, "IMAGE", "none_icon");
        break;
    case SEND_STATUS_SEND:
        IupSetAttribute(stateIcon, "IMAGE", "doing_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    case SEND_STATUS_FAIL:
        IupSetAttribute(stateIcon, "IMAGE", "error_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    }
}

void uiAdvancedResetActivity(void) {
    int ix;
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        modules[ix]->processTriggered = 0; // threads already stopped
        IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
    }
    sendState = SEND_STATUS_NONE;
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");
}

void uiAdvancedApplyFilterParameter(void) {
    setFromParameter(filterText, "VALUE", "filter");
}
