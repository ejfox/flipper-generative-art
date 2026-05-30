#pragma once

/**
 * Frame pipeline for the Flipper Zero Generative Art Engine.
 *
 * Pipeline per frame:
 *   1. Precompute FrameCtx (once)
 *   2. Generate pattern A (and blend with B if active)
 *   3. Invert pass (if enabled) — separate loop, branch-free generation
 *   4. Dither 8-bit → 1-bit in-place
 *   5. Auto-evolve mutations (if enabled, every ~1 sec)
 */

#include "patterns.h"
#include "blend.h"
#include "dither.h"

static void generate_frame(GenState* state, bool auto_evolve) {
    // -- 1. Precompute all per-frame constants --
    FrameCtx ctx;
    frame_ctx_init(&ctx, state);

    uint8_t* px = state->pixels;

    // -- 2. Generate pixels --
    bool has_blend = (state->pattern_b < PATTERN_COUNT) && (state->blend_mode != BLEND_NONE);
    uint8_t pat_a = state->pattern_a;
    uint8_t pat_b = state->pattern_b;
    uint8_t bmode = state->blend_mode;

    if(has_blend) {
        // Two-pattern path: generate A, generate B, blend
        for(int y = 0; y < SCREEN_HEIGHT; y++) {
            for(int x = 0; x < SCREEN_WIDTH; x++) {
                uint8_t a = generate_pattern(x, y, pat_a, &ctx);
                uint8_t b = generate_pattern(x, y, pat_b, &ctx);
                *px++ = apply_blend(a, b, bmode);
            }
        }
    } else {
        // Single-pattern path: no blend overhead
        for(int y = 0; y < SCREEN_HEIGHT; y++) {
            for(int x = 0; x < SCREEN_WIDTH; x++) {
                *px++ = generate_pattern(x, y, pat_a, &ctx);
            }
        }
    }

    // -- 3. Invert pass (separate loop: keeps generation loop branch-free) --
    if(state->invert) {
        px = state->pixels;
        for(int i = 0; i < PIXEL_COUNT; i++) {
            px[i] = 255 - px[i];
        }
    }

    // -- 4. Dither --
    apply_dither(state);

    // -- 5. Advance frame counter --
    state->frame_count++;

    // -- 6. Auto-evolve mutations (~every 1 second) --
    if(!auto_evolve || (state->frame_count % EVOLVE_FRAMES != 0)) return;

    uint32_t rng = state->seed + state->frame_count;

    if((xorshift32(&rng) % 100) < 20) // 20%: switch pattern
        state->pattern_a = xorshift32(&rng) % PATTERN_COUNT;

    state->frequency = 0.5f + (float)(xorshift32(&rng) % 100) / 50.0f;

    if((xorshift32(&rng) % 100) < 10) // 10%: toggle invert
        state->invert = !state->invert;

    if((xorshift32(&rng) % 100) < 30) { // 30%: change layer B pattern
        state->pattern_b = xorshift32(&rng) % PATTERN_COUNT;
        // Ensure blend is active when we have a second layer
        if(state->blend_mode == BLEND_NONE) {
            state->blend_mode = 1 + xorshift32(&rng) % (BLEND_COUNT - 1);
        }
    }

    if((xorshift32(&rng) % 100) < 25) { // 25%: change blend mode
        state->blend_mode = 1 + xorshift32(&rng) % (BLEND_COUNT - 1);
        // Ensure we have a second layer to blend with
        if(state->pattern_b >= PATTERN_COUNT) {
            state->pattern_b = xorshift32(&rng) % PATTERN_COUNT;
        }
    }

    if((xorshift32(&rng) % 100) < 5) { // 5%: occasionally go single-layer
        state->pattern_b = PATTERN_COUNT;
        state->blend_mode = BLEND_NONE;
    }

    if((xorshift32(&rng) % 100) < 8) // 8%: switch dither
        state->dither_mode = xorshift32(&rng) % DITHER_COUNT;
}
