#include "fruit_rule.h"

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
    if (!zeroed(b.reserved, sizeof(b.reserved))) return false;

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
        if (b.override_class >= FRM_NUM_CLASSES) return false;
        if (b.override_op1 > DISC_OP_LT || b.override_op2 > DISC_OP_LT)
            return false;
        if (b.override_slot1 >= DISC_COUNT || b.override_slot2 >= DISC_COUNT)
            return false;
        if (!((b.slot_mask >> b.override_slot1) & 1U)) return false;
        if (!((b.slot_mask >> b.override_slot2) & 1U)) return false;
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
