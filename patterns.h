#pragma once

/**
 * Pattern generators for the Flipper Zero Generative Art Engine.
 *
 * FrameCtx precomputation + 16 pattern generators.
 */

#include "math_utils.h"

// ===========================================================================
// PER-FRAME PRECOMPUTATION
// ===========================================================================
//
// Called ONCE per frame. Hoists expensive math (expf, fmodf, trig, division)
// out of the per-pixel loop. For Mandelbrot, this eliminates ~8192 expf calls.

static void frame_ctx_init(FrameCtx* ctx, const GenState* state) {
    ctx->t = state->frame_count * 0.05f;
    ctx->freq = state->frequency;
    ctx->frame_count = state->frame_count;

    // -- Mandelbrot zoom (expf + fmodf + division: ~200 cycles, done once) --
    float zoom_t = fmodf(ctx->t * ctx->freq * 0.6f, 14.0f); // wraps to avoid precision loss
    float zoom = expf(zoom_t); // exponential zoom-in
    float scale = 2.5f / zoom; // viewport width in complex plane
    float scale_y = scale * 0.5f; // aspect correction for 128:64
    ctx->m_dx = scale * INV_WIDTH; // complex-plane step per pixel x
    ctx->m_dy = scale_y * INV_HEIGHT; // complex-plane step per pixel y
    ctx->m_x0 = -0.7435669f - 0.5f * scale; // left edge (Seahorse Valley)
    ctx->m_y0 = 0.1314023f - 0.5f * scale_y; // top edge

    // -- Julia c parameter (orbits through parameter space via sine LUT) --
    uint8_t ta = (uint8_t)(ctx->t * ctx->freq * 3.0f);
    ctx->j_cr = -0.7f + 0.35f * fast_sin(ta) / 64.0f;
    ctx->j_ci = 0.27f + 0.2f * fast_sin((uint8_t)(ta + 16)) / 64.0f;

    // -- Moire drift offsets --
    ctx->mo_ox = 0.1f * fast_sin((uint8_t)(ctx->t * 3)) / 64.0f;
    ctx->mo_oy = 0.1f * fast_sin((uint8_t)(ctx->t * 2 + 16)) / 64.0f;

    // -- Animated noise seeds (avoid addition per pixel) --
    ctx->noise_seed = state->seed + state->frame_count / 3;
    ctx->perlin_seed = state->seed + state->frame_count / 4;
}

// ===========================================================================
// PATTERN GENERATORS
// ===========================================================================
//
// Each pattern returns a 0–255 grayscale value for pixel (x, y).
// Uses FrameCtx for all per-frame constants — no redundant work.
// Uses reciprocal multiplication for coordinate normalization.

static uint8_t generate_pattern(uint8_t x, uint8_t y, uint8_t pattern, const FrameCtx* ctx) {
    // Reciprocal multiply: ~1 cycle each vs ~12 for division
    float nx = x * INV_WIDTH;
    float ny = y * INV_HEIGHT;

    float freq = ctx->freq;
    float t = ctx->t;
    float value = 0.0f;

    switch(pattern) {
        // -- Static gradients --

    case PAT_HORIZONTAL:
        value = nx;
        break;

    case PAT_VERTICAL:
        value = ny;
        break;

    case PAT_RADIAL: {
        float dx = nx - 0.5f;
        float dy = ny - 0.5f;
        // sqrtf is ~14 cycles on Cortex-M4 FPU (hardware VSQRT)
        value = sqrtf(dx * dx + dy * dy) * 1.414f;
    } break;

    case PAT_DIAGONAL:
        value = (nx + ny) * 0.5f; // multiply by 0.5 instead of divide by 2
        break;

        // -- Animated wave patterns --

    case PAT_SINE:
        value = (fast_sin((uint8_t)(nx * 64 * freq + t * 10)) + 64) * (1.0f / 128.0f);
        break;

    case PAT_COSINE:
        value = (fast_sin((uint8_t)(ny * 64 * freq + t * 8 + 16)) + 64) * (1.0f / 128.0f);
        break;

    case PAT_INTERFERENCE: {
        int8_t w1 = fast_sin((uint8_t)(nx * 32 * freq + t * 6));
        int8_t w2 = fast_sin((uint8_t)(ny * 32 * freq + t * 4));
        // w1*w2 range: -4096..+4096; /64 → -64..+64; +64 → 0..128; /128 → 0..1
        value = ((w1 * w2) / 64 + 64) * (1.0f / 128.0f);
    } break;

    case PAT_CHECKERBOARD: {
        uint8_t cx = (uint8_t)(nx * 8 * freq) & 1;
        uint8_t cy = (uint8_t)(ny * 8 * freq) & 1;
        return (cx ^ cy) ? 255 : 0; // skip float→int conversion entirely
    }

    case PAT_NOISE:
        return hash_noise(x, y, ctx->noise_seed); // already 0–255, skip float

    case PAT_SPIRAL: {
        float dx = nx - 0.5f;
        float dy = ny - 0.5f;
        // fast_atan2: ~10 cycles vs ~80 for atan2f
        float angle = fast_atan2(dy, dx) + t * 0.5f;
        float dist = sqrtf(dx * dx + dy * dy);
        value = fmodf(fabsf(angle + dist * 10.0f * freq), 6.28f) * (1.0f / 6.28f);
    } break;

        // -- Fractal patterns (use precomputed FrameCtx) --

    case PAT_MANDELBROT: {
        // Per-pixel: just 2 multiplies + additions. All zoom math is in FrameCtx.
        float cx = ctx->m_x0 + x * ctx->m_dx;
        float cy = ctx->m_y0 + y * ctx->m_dy;
        float zx = 0, zy = 0;
        float zx2 = 0, zy2 = 0; // cache squares: saves 2 muls/iter
        int iter;
        for(iter = 0; iter < FRACTAL_ITER; iter++) {
            if(zx2 + zy2 > 4.0f) break;
            zy = 2.0f * zx * zy + cy;
            zx = zx2 - zy2 + cx;
            zx2 = zx * zx;
            zy2 = zy * zy;
        }
        return fractal_lut[iter]; // LUT: 1 byte load vs multiply+divide
    }

    case PAT_JULIA: {
        // c is precomputed in FrameCtx (constant per frame)
        float zx = (nx - 0.5f) * 3.0f;
        float zy = (ny - 0.5f) * 2.0f;
        float zx2 = zx * zx;
        float zy2 = zy * zy;
        int iter;
        for(iter = 0; iter < FRACTAL_ITER; iter++) {
            if(zx2 + zy2 > 4.0f) break;
            zy = 2.0f * zx * zy + ctx->j_ci;
            zx = zx2 - zy2 + ctx->j_cr;
            zx2 = zx * zx;
            zy2 = zy * zy;
        }
        return fractal_lut[iter];
    }

    case PAT_PLASMA: {
        // Four sine waves at different scales + speeds
        float v1 = fast_sin((uint8_t)(nx * 32 * freq + t * 10)) * (1.0f / 64.0f);
        float v2 = fast_sin((uint8_t)(ny * 24 * freq + t * 8)) * (1.0f / 64.0f);
        float v3 = fast_sin((uint8_t)((nx + ny) * 16 * freq + t * 6)) * (1.0f / 64.0f);
        float dx = nx - 0.5f;
        float dy = ny - 0.5f;
        float v4 =
            fast_sin((uint8_t)(sqrtf(dx * dx + dy * dy) * 40 * freq + t * 4)) * (1.0f / 64.0f);
        value = (v1 + v2 + v3 + v4 + 4.0f) * (1.0f / 8.0f); // 0..1
    } break;

    case PAT_PERLIN: {
        // 4-octave fBm with precomputed animated seed
        float total = 0.0f;
        float amp = 1.0f;
        float f = freq * 4;
        float max_amp = 0.0f;
        for(int oct = 0; oct < 4; oct++) {
            total += hash_noise(
                         (uint32_t)(nx * f * 16),
                         (uint32_t)(ny * f * 16),
                         ctx->perlin_seed + oct * 7919u) *
                     (1.0f / 255.0f) * amp;
            max_amp += amp;
            amp *= 0.5f;
            f *= 2.0f;
        }
        value = total / max_amp;
    } break;

    case PAT_MOIRE: {
        // Drift offsets precomputed in FrameCtx
        float d1x = nx - 0.3f - ctx->mo_ox;
        float d1y = ny - 0.5f - ctx->mo_oy;
        float d2x = nx - 0.7f + ctx->mo_ox;
        float d2y = ny - 0.5f + ctx->mo_oy;
        float d1 = sqrtf(d1x * d1x + d1y * d1y);
        float d2 = sqrtf(d2x * d2x + d2y * d2y);
        float v1 = (fast_sin((uint8_t)(d1 * 64 * freq)) + 64) * (1.0f / 128.0f);
        float v2 = (fast_sin((uint8_t)(d2 * 64 * freq)) + 64) * (1.0f / 128.0f);
        value = fabsf(v1 - v2);
    } break;

    case PAT_SIERPINSKI: {
        // Pure integer: bitwise AND trick + horizontal scroll
        uint32_t sx = (uint32_t)(nx * 128 * freq) + ctx->frame_count;
        uint32_t sy = (uint32_t)(ny * 64 * freq);
        return (sx & sy) ? 255 : 0; // skip float entirely
    }

    default:
        value = nx;
    }

    // Clamp and convert to 0–255
    if(value < 0.0f) value = 0.0f;
    if(value > 1.0f) value = 1.0f;
    return (uint8_t)(value * 255);
}
