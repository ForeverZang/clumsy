// Beginner loss step: random per-packet drops below 100%, unplugged-cable
// semantics (TCP RST, other protocols dropped) at 100%.
#include <stdlib.h>

#include "common.h"
#include "targeting.h"
#include "simple.h"

#define LOSS_FULL 10000

static volatile short simpleEnabled = 0;
static volatile short loss = 0;

void simpleSetLoss(short permyriad) {
    if (permyriad < 0) {
        permyriad = 0;
    } else if (permyriad > LOSS_FULL) {
        permyriad = LOSS_FULL;
    }
    InterlockedExchange16(&loss, permyriad);
}

void simpleSetEnabled(BOOL enabled) {
    InterlockedExchange16(&simpleEnabled, (short)(enabled ? 1 : 0));
}

static void simpleStartUp(void) {
    LOG("simple loss enabled at %.1f%%", loss / 100.0);
}

static void simpleCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("simple loss disabled");
}

// Turn one in-flight TCP packet into a valid RST+ACK (its real sequence
// number is inside the peer's window because the packet was real) and
// leave it in the list so it gets reinjected. Both ends abort at once.
static BOOL simpleRstTcp(PacketNode *pac) {
    PWINDIVERT_TCPHDR tcp = NULL;
    WinDivertHelperParsePacket(pac->packet, pac->packetLen, NULL, NULL,
                               NULL, NULL, NULL, &tcp, NULL,
                               NULL, NULL, NULL, NULL);
    if (tcp == NULL) {
        return FALSE;
    }
    tcp->Rst = 1;
    WinDivertHelperCalcChecksums(pac->packet, pac->packetLen, NULL, 0);
    return TRUE;
}

static short simpleProcess(PacketNode *head, PacketNode *tail) {
    short triggered = FALSE;
    short level = loss;
    PacketNode *pac = head->next;

    while (pac != tail) {
        PacketNode *next = pac->next;
        if (!shouldAffect(pac, 1, 1)) {
            // Untargeted / unattributable: always pass untouched.
            pac = next;
            continue;
        }

        if (level >= LOSS_FULL) {
            if (simpleRstTcp(pac)) {
                LOG("100%%: injected TCP RST");
                triggered = TRUE; // RST packet stays in the list
            } else {
                LOG("100%%: dropped non-TCP packet");
                freeNode(popNode(pac));
                triggered = TRUE;
            }
        } else if (calcChance(level)) {
            // Per-packet, per-volume random drop independent of session.
            freeNode(popNode(pac));
            triggered = TRUE;
        }
        pac = next;
    }
    return triggered;
}

// setupUIFunc stays NULL: the beginner UI drives this step directly.
Module simpleModule = {
    "Simple loss",
    "simple",
    (short *)&simpleEnabled,
    NULL,
    simpleStartUp,
    simpleCloseDown,
    simpleProcess,
    // runtime fields
    0, 0, NULL
};
