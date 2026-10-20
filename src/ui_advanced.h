#ifndef CLUMSY_UI_ADVANCED_H
#define CLUMSY_UI_ADVANCED_H

#include "common.h"

/*
 * The folded "advanced" surface: the WinDivert filter text box with its
 * presets, the activity/state icons, and the 8 classic function modules.
 * Call uiAdvancedLoadConfig() before IupOpen, uiAdvancedCreate() after.
 */

// Read config.txt filter presets into memory (was loadConfig in main).
void uiAdvancedLoadConfig(void);

// Builds the two frames (Filtering + Functions) in one vbox. The
// beginner main window owns Start/Stop; the filter box is seeded
// permissive ("true") and its text feeds the software-side AND leg.
Ihandle *uiAdvancedCreate(void);

// Filter text box accessors.
const char *uiAdvancedFilterValue(void);
void uiAdvancedSetFilterValue(const char *value);
void uiAdvancedSetFilterEditable(BOOL editable);

// Red syntax-error line inside the advanced area; NULL/"" hides it.
void uiAdvancedSetFilterError(const char *msg);

// Periodic (200 ms) icon refresh: per-module activity + global send icon.
void uiAdvancedTimerTick(void);
// Reset every icon to the idle state (runs when stopping).
void uiAdvancedResetActivity(void);

// Command-line --filter value for the filter text box.
void uiAdvancedApplyFilterParameter(void);

#endif // CLUMSY_UI_ADVANCED_H
