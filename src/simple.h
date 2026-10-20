#ifndef CLUMSY_SIMPLE_H
#define CLUMSY_SIMPLE_H

#include "common.h"

/*
 * Beginner one-slider loss step. Not registered in modules[] (so the
 * advanced UI never shows it); divertConsumeStep invokes it explicitly
 * after lag and before drop. Direction is the global beginner gate only,
 * so shouldAffect is called with both module directions enabled.
 */
extern Module simpleModule;

// Despite the brief's "permille" parameter name, the value is permyriad
// (basis points) to match calcChance: 0..10000, 10000 == 100%.
void simpleSetLoss(short permyriad);

// Flip the pipeline flag the beginner Start/Stop action controls. The
// step stays registered in the processing chain at all times; while
// disabled every packet passes it untouched.
void simpleSetEnabled(BOOL enabled);

#endif // CLUMSY_SIMPLE_H
