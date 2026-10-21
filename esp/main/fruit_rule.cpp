#include "fruit_rule.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>

// Structural validation + decode for the v2 fruit-rule block. The block is
// the only part of the 852-byte wire that is not "classic Dual-Latent", so
// everything here is about making a host-packed blob impossible to corrupt
// the decision path.

namespace {

bool finite_f(float v) { return std::isfinite(v); }

bool zeroed(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] != 0) return false;
    return true;
}

}  // namespace

bool fruit_rule_block_valid(const FruitRuleBlock& b) {
    if (b.block_version != FRM_BLOCK_VERSION) return false;

    // Slot mask: at least one active slot, no bits outside the slot array.
    if (b.slot_mask == 0) return false;
    if (b.slot_mask & ~(uint32_t)((1u << FRM_MAX_SLOTS) - 1)) return false;

    // Global knobs.
    if (!finite_f(b.temperature) || b.temperature <= 0.0f) return false;
    if (!finite_f(b.margin) || b.margin < 0.0f) return false;
    if (!finite_f(b.anomaly_threshold) || b.anomaly_threshold < 0.0f ||
        b.anomaly_threshold > 1.0f)
        return false;
    if (!finite_f(b.ripeness_gain) || b.ripeness_gain < 0.0f) return false;
    if (b.default_class != FRM_NO_CLASS && b.default_class >= FRM_NUM_CLASSES)
        return false;

    // Padding must stay canonical so the blob round-trips byte-for-byte.
    if (!zeroed(b.pad0, sizeof(b.pad0))) return false;
    // The 156-byte run is EITHER the legacy all-zero padding OR a valid
    // per-side anchor table.  A model without a table keeps the legacy
    // behaviour byte for byte.
    {
        const FruitSideTable t = *reinterpret_cast<const FruitSideTable*>(b.reserved);
        const bool all_zero = zeroed(b.reserved, sizeof(b.reserved));
        if (!all_zero) {
            if (t.table_version != FRM_SIDE_TABLE_VERSION) return false;
            if (t.side_count > FRM_MAX_SIDES) return false;
            if (!zeroed(t.reserved, sizeof(t.reserved))) return false;
            for (uint8_t i = 0; i < t.side_count; i++) {
                if (!finite_f(t.sides[i].anchor)) return false;
                if (!finite_f(t.sides[i].softness) || t.sides[i].softness <= 0.0f)
                    return false;
            }
            for (uint8_t i = t.side_count; i < FRM_MAX_SIDES; i++) {
                if (t.sides[i].side_id != 0 || t.sides[i].flags != 0 ||
                    t.sides[i].anchor != 0.0f || t.sides[i].softness != 0.0f)
                    return false;
            }
            if (!zeroed(t.tail, sizeof(t.tail))) return false;
        }
    }

    // Per-slot feature id + weight. Inactive slots are canonical zeros.
    for (uint8_t i = 0; i < FRM_MAX_SLOTS; i++) {
        const bool active = (b.slot_mask >> i) & 1U;
        if (!active) {
            if (b.slot_feature[i] != 0) return false;
            if (b.slot_weight[i] != 0.0f) return false;
            continue;
        }
        if (b.slot_feature[i] >= DISC_COUNT) return false;
        if (!finite_f(b.slot_weight[i]) || b.slot_weight[i] < 0.0f) return false;
    }

    // Per-class slot masks + bands. A class that does not use a slot must
    // leave that slot's band canonical.
    for (uint8_t c = 0; c < FRM_NUM_CLASSES; c++) {
        if (b.class_slot_mask[c] & ~(uint8_t)b.slot_mask) return false;
        for (uint8_t i = 0; i < FRM_MAX_SLOTS; i++) {
            const bool used = (b.class_slot_mask[c] >> i) & 1U;
            if (!used) {
                if (b.class_lo[c][i] != 0.0f) return false;
                if (b.class_hi[c][i] != 0.0f) return false;
                if (b.class_soft[c][i] != 0.0f) return false;
                continue;
            }
            if (!finite_f(b.class_lo[c][i]) || !finite_f(b.class_hi[c][i]))
                return false;
            if (!finite_f(b.class_soft[c][i]) || b.class_soft[c][i] <= 0.0f)
                return false;
            if (b.class_hi[c][i] < b.class_lo[c][i]) return false;
        }
    }

    // Hard override: optional, but when present both conditions must be
    // well-formed and reference ACTIVE slots.
    if (b.override_class != FRM_NO_CLASS) {
        // FRM_ANOMALY_ONLY flags an anomaly without naming a class.
        if (b.override_class != FRM_ANOMALY_ONLY &&
            b.override_class >= FRM_NUM_CLASSES)
            return false;
        if (b.override_op1 > DISC_OP_LT || b.override_op2 > DISC_OP_LT)
            return false;
        if (b.override_slot1 >= DISC_COUNT || b.override_slot2 >= DISC_COUNT)
            return false;
        // The override names a FEATURE id (the host packs the feature, not
        // the slot index - see fruit_rules.py), so it must name a feature
        // that one of the ACTIVE slots carries.  Checking it against the
        // slot mask as if it were a slot index rejected every override that
        // used a feature id above 7 - the black-spot discriminator is 17.
        auto names_active_slot = [&b](uint8_t feature_id) {
            for (uint8_t i = 0; i < FRM_MAX_SLOTS; i++) {
                if (((b.slot_mask >> i) & 1U) && b.slot_feature[i] == feature_id)
                    return true;
            }
            return false;
        };
        if (!names_active_slot(b.override_slot1)) return false;
        if (!names_active_slot(b.override_slot2)) return false;
        if (!finite_f(b.override_val1) || !finite_f(b.override_val2)) return false;
    } else {
        if (b.override_slot1 != 0 || b.override_slot2 != 0) return false;
        if (b.override_op1 != 0 || b.override_op2 != 0) return false;
        if (b.override_val1 != 0.0f || b.override_val2 != 0.0f) return false;
    }

    return true;
}

bool fruit_rule_block_empty(const FruitRuleBlock& b) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&b);
    for (size_t i = 0; i < sizeof(FruitRuleBlock); i++) if (p[i] != 0) return false;
    return true;
}

FruitRuleParams fruit_rule_decode(const FruitRuleBlock& b) {    FruitRuleParams p{};
    p.enabled      = true;
    p.slot_mask    = (uint8_t)b.slot_mask;
    p.temperature  = b.temperature;
    p.margin       = b.margin;
    p.default_class = (b.default_class == FRM_NO_CLASS) ? FRM_NO_CLASS
                                                        : (uint8_t)b.default_class;
    p.anomaly_threshold = b.anomaly_threshold;
    p.ripeness_gain     = b.ripeness_gain;
    for (uint8_t i = 0; i < FRM_MAX_SLOTS; i++) {
        p.slot_feature[i] = b.slot_feature[i];
        p.slot_weight[i]  = b.slot_weight[i];
    }
    // The per-side anchor table, when the blob carries one.
    {
        if (p.side_count > 0) {
            Serial.printf("[Rules] per-site anchors: %u site(s):", p.side_count);
            for (uint8_t i = 0; i < p.side_count; i++) {
                Serial.printf(" %u=%.6f", p.side_id[i], p.side_anchor[i]);
            }
            Serial.println();
        }
        const FruitSideTable t = *reinterpret_cast<const FruitSideTable*>(b.reserved);
        p.side_count = t.side_count;
        for (uint8_t i = 0; i < FRM_MAX_SIDES; i++) {
            p.side_id[i] = t.sides[i].side_id;
            p.side_anchor[i] = t.sides[i].anchor;
            p.side_soft[i] = t.sides[i].softness;
        }
    }
    for (uint8_t c = 0; c < FRM_NUM_CLASSES; c++) {
        p.class_slot_mask[c] = b.class_slot_mask[c];
        for (uint8_t i = 0; i < FRM_MAX_SLOTS; i++) {
            p.class_lo[c][i]   = b.class_lo[c][i];
            p.class_hi[c][i]   = b.class_hi[c][i];
            p.class_soft[c][i] = b.class_soft[c][i];
        }
    }
    p.override_class = (uint8_t)b.override_class;
    p.override_slot1 = b.override_slot1;
    p.override_op1   = b.override_op1;
    p.override_slot2 = b.override_slot2;
    p.override_op2   = b.override_op2;
    p.override_val1  = b.override_val1;
    p.override_val2  = b.override_val2;
    return p;
}
