// Unit tests for the OCR rule engine: whitespace-normalized case-folded
// substring matching (comma-separated keyword OR) and the per-rule
// edge-triggered state machine with fail-closed arming.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "ocr_rules.h"

static OcrRule makeRule(OcrEdge edge, const char *keywords, int frames, int pct) {
    OcrRule rule;
    memset(&rule, 0, sizeof(rule));
    strncpy(rule.keywords, keywords, sizeof(rule.keywords) - 1);
    rule.edge = edge;
    rule.confirmFrames = frames;
    rule.lossPercent = pct;
    return rule;
}

int main(void) {
    char out[128];

    // --- normalize: trim ends, delete ALL internal whitespace, fold case ---

    // 1. Surrounding and internal ASCII spaces vanish; CJK bytes preserved.
    assert(ocrNormalizeText(" 重 连 ", out, sizeof(out)));
    assert(strcmp(out, "重连") == 0);

    // 2. ASCII lowercases, punctuation survives.
    assert(ocrNormalizeText("Reconnect!", out, sizeof(out)));
    assert(strcmp(out, "reconnect!") == 0);

    // 3. Tabs/newlines and the U+3000 ideographic space are whitespace.
    assert(ocrNormalizeText("  Online\t仓\n库　", out, sizeof(out)));
    assert(strcmp(out, "online仓库") == 0);

    // 4. Empty and whitespace-only inputs report no content.
    assert(!ocrNormalizeText("", out, sizeof(out)));
    assert(out[0] == '\0');
    assert(!ocrNormalizeText("  　 ", out, sizeof(out)));
    assert(out[0] == '\0');

    // --- matches: normalized substring OR over comma-separated keywords ---

    OcrRule zh = makeRule(OCR_EDGE_DISAPPEAR, "重连", 2, 95);
    OcrRule en = makeRule(OCR_EDGE_APPEAR, "reconnect", 2, 50);
    OcrRule cang = makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95);

    // 5. Substring inside a longer OCR line.
    assert(ocrRuleMatches(&zh, "正在重连，请稍候"));
    // 6. Case-insensitive ASCII.
    assert(ocrRuleMatches(&en, "RECONNECT"));
    // 7. Prefix of a longer token still matches.
    assert(ocrRuleMatches(&cang, "仓库2级"));
    // 8. OCR-inserted spaces do not break matching.
    assert(ocrRuleMatches(&cang, "Online 仓 库 12:30"));

    // 9. Empty keyword set never matches.
    OcrRule empty = makeRule(OCR_EDGE_APPEAR, "", 2, 50);
    assert(!ocrRuleMatches(&empty, "anything"));
    OcrRule sepOnly = makeRule(OCR_EDGE_APPEAR, " , ，", 2, 50);
    assert(!ocrRuleMatches(&sepOnly, "anything"));

    // 10. Multi-keyword OR, ASCII and Chinese commas, surrounding spaces.
    OcrRule multi = makeRule(OCR_EDGE_APPEAR, " 重连 , 仓库，reconnect ", 2, 50);
    assert(ocrRuleMatches(&multi, "状态：仓库"));
    assert(ocrRuleMatches(&multi, "重连中"));
    assert(ocrRuleMatches(&multi, "RECONNECTING"));
    assert(!ocrRuleMatches(&multi, "在线 12:30"));

    // 11. A keyword that is simply absent misses.
    assert(!ocrRuleMatches(&zh, "Online 仓库 12:30"));

    // --- state machine: unarmed fails closed ---

    OcrRule disappear = makeRule(OCR_EDGE_DISAPPEAR, "仓库", 2, 95);
    OcrRule appear = makeRule(OCR_EDGE_APPEAR, "重连", 2, 50);
    OcrRuleState st;
    int target = -1;

    // 12. Fresh state is unarmed; no poll sequence triggers.
    ocrRuleStateInit(&st);
    assert(!st.armed);
    assert(st.stable == OCR_STABLE_ABSENT);
    assert(st.run == 0);
    for (int i = 0; i < 5; i++) {
        assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
        assert(!ocrRuleStep(&st, &appear, OCR_POLL_MATCHED, &target));
        assert(!ocrRuleStep(&st, &disappear, OCR_POLL_UNAVAILABLE, &target));
    }
    assert(target == -1);

    // --- DISAPPEAR, armed while present: two consecutive misses trigger ---

    // 13. First miss just counts; second miss fires once with rule value.
    ocrRuleStateInit(&st);
    ocrRuleArm(&st, TRUE);
    assert(st.armed && st.stable == OCR_STABLE_PRESENT);
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(target == -1);
    assert(ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(target == 95);
    assert(st.stable == OCR_STABLE_ABSENT);

    // 14. Further misses do not re-trigger.
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));

    // 15. Text coming back settles present again (no trigger); next
    //     disappearance needs a fresh confirm run.
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MATCHED, &target));
    assert(st.stable == OCR_STABLE_PRESENT);
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(target == 95);

    // --- APPEAR, armed while absent: two consecutive matches trigger ---

    // 16.
    ocrRuleStateInit(&st);
    ocrRuleArm(&st, FALSE);
    assert(st.stable == OCR_STABLE_ABSENT);
    assert(!ocrRuleStep(&st, &appear, OCR_POLL_MATCHED, &target));
    assert(ocrRuleStep(&st, &appear, OCR_POLL_MATCHED, &target));
    assert(target == 50);
    assert(st.stable == OCR_STABLE_PRESENT);
    assert(!ocrRuleStep(&st, &appear, OCR_POLL_MATCHED, &target));

    // --- UNAVAILABLE neither counts nor flips polarity ---

    // 17. Miss, unavailable, miss still totals two misses.
    ocrRuleStateInit(&st);
    ocrRuleArm(&st, TRUE);
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_UNAVAILABLE, &target));
    assert(st.run == 1 && st.stable == OCR_STABLE_PRESENT);
    assert(ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(target == 95);

    // --- Opposite-direction poll interrupts the confirm run ---

    // 18. Miss, match (resets), then two misses are needed to trigger.
    ocrRuleStateInit(&st);
    ocrRuleArm(&st, TRUE);
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MATCHED, &target));
    assert(st.run == 0 && st.stable == OCR_STABLE_PRESENT);
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(ocrRuleStep(&st, &disappear, OCR_POLL_MISSED, &target));
    assert(target == 95);

    // 19. Same interrupt behavior on the APPEAR edge.
    ocrRuleStateInit(&st);
    ocrRuleArm(&st, FALSE);
    assert(!ocrRuleStep(&st, &appear, OCR_POLL_MATCHED, &target));
    assert(!ocrRuleStep(&st, &appear, OCR_POLL_MISSED, &target));
    assert(st.run == 0 && st.stable == OCR_STABLE_ABSENT);
    assert(!ocrRuleStep(&st, &appear, OCR_POLL_MATCHED, &target));
    assert(ocrRuleStep(&st, &appear, OCR_POLL_MATCHED, &target));
    assert(target == 50);

    // --- confirmFrames == 1 triggers on the first qualifying poll ---

    // 20.
    OcrRule immediate = makeRule(OCR_EDGE_DISAPPEAR, "仓库", 1, 95);
    ocrRuleStateInit(&st);
    ocrRuleArm(&st, TRUE);
    assert(ocrRuleStep(&st, &immediate, OCR_POLL_MISSED, &target));
    assert(target == 95);

    // --- target stays untouched on a non-triggering step ---

    // 21.
    ocrRuleStateInit(&st);
    ocrRuleArm(&st, TRUE);
    target = 12345;
    assert(!ocrRuleStep(&st, &disappear, OCR_POLL_UNAVAILABLE, &target));
    assert(target == 12345);

    printf("OK: ocr_rules\n");
    return 0;
}
