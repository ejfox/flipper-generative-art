#pragma once

/**
 * Blend modes for the Flipper Zero Generative Art Engine.
 *
 * 8 blend operations for combining two pattern layers.
 */

#include "math_utils.h"

static inline uint8_t apply_blend(uint8_t a, uint8_t b, uint8_t mode) {
    switch(mode) {
    case BLEND_XOR:
        return a ^ b;
    case BLEND_AND:
        return a & b;
    case BLEND_OR:
        return a | b;
    case BLEND_ADD:
        return clamp8((int)a + b);
    case BLEND_MULTIPLY:
        return (uint8_t)((a * b) / 255);
    case BLEND_DIFF:
        return (uint8_t)abs((int)a - (int)b);
    case BLEND_SCREEN:
        return 255 - (uint8_t)(((255 - a) * (255 - b)) / 255);
    default:
        return a;
    }
}
