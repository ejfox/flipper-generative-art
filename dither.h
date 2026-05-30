#pragma once

/**
 * Dither algorithms for the Flipper Zero Generative Art Engine.
 *
 * All dithers operate on the pixel buffer in-place, converting 8-bit
 * grayscale to binary (0 or 255). Uses pointer arithmetic to avoid
 * y*SCREEN_WIDTH index multiplication in the inner loop.
 */

#include "math_utils.h"

// Bayer 4×4 threshold matrix, pre-scaled to 0–255.
static const uint8_t bayer4x4[16] = {
    0,
    128,
    32,
    160,
    192,
    64,
    224,
    96,
    48,
    176,
    16,
    144,
    240,
    112,
    208,
    80,
};

// Floyd-Steinberg error diffusion.
// Error weights: → 7/16, ↙ 3/16, ↓ 5/16, ↘ 1/16.
static void dither_floyd_steinberg(GenState* state) {
    uint8_t* px = state->pixels; // running pointer avoids idx = y*W+x
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        for(int x = 0; x < SCREEN_WIDTH; x++, px++) {
            uint8_t old = *px;
            uint8_t quantized = old > 127 ? 255 : 0;
            *px = quantized;
            int err = old - quantized;

            if(x + 1 < SCREEN_WIDTH) // →  7/16
                px[1] = clamp8(px[1] + (err * 7) / 16);
            if(y + 1 < SCREEN_HEIGHT) {
                if(x > 0) // ↙  3/16
                    px[SCREEN_WIDTH - 1] = clamp8(px[SCREEN_WIDTH - 1] + (err * 3) / 16);
                // ↓  5/16
                px[SCREEN_WIDTH] = clamp8(px[SCREEN_WIDTH] + (err * 5) / 16);
                if(x + 1 < SCREEN_WIDTH) // ↘  1/16
                    px[SCREEN_WIDTH + 1] = clamp8(px[SCREEN_WIDTH + 1] + (err * 1) / 16);
            }
        }
    }
}

// Bayer ordered dither. Pointer-based with precomputed row offset.
static void dither_bayer(GenState* state) {
    uint8_t* px = state->pixels;
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        const uint8_t* brow = &bayer4x4[(y & 3) << 2]; // row of 4 thresholds
        for(int x = 0; x < SCREEN_WIDTH; x++, px++) {
            *px = (*px > brow[x & 3]) ? 255 : 0;
        }
    }
}

// Atkinson dither: 6 neighbors × 1/8 error each. 2/8 lost → high contrast.
//   [*]  →  →→
//    ↙   ↓   ↘
//        ↓↓
static void dither_atkinson(GenState* state) {
    uint8_t* px = state->pixels;
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        for(int x = 0; x < SCREEN_WIDTH; x++, px++) {
            uint8_t old = *px;
            uint8_t quantized = old > 127 ? 255 : 0;
            *px = quantized;
            int err = (old - quantized) / 8; // each neighbor gets 1/8

            if(x + 1 < SCREEN_WIDTH) // →
                px[1] = clamp8(px[1] + err);
            if(x + 2 < SCREEN_WIDTH) // →→
                px[2] = clamp8(px[2] + err);
            if(y + 1 < SCREEN_HEIGHT) {
                if(x > 0) // ↙
                    px[SCREEN_WIDTH - 1] = clamp8(px[SCREEN_WIDTH - 1] + err);
                // ↓
                px[SCREEN_WIDTH] = clamp8(px[SCREEN_WIDTH] + err);
                if(x + 1 < SCREEN_WIDTH) // ↘
                    px[SCREEN_WIDTH + 1] = clamp8(px[SCREEN_WIDTH + 1] + err);
            }
            if(y + 2 < SCREEN_HEIGHT) // ↓↓
                px[2 * SCREEN_WIDTH] = clamp8(px[2 * SCREEN_WIDTH] + err);
        }
    }
}

// Hard threshold. No error diffusion.
static void dither_threshold(GenState* state) {
    uint8_t* px = state->pixels;
    for(int i = 0; i < PIXEL_COUNT; i++, px++) {
        *px = (*px > 127) ? 255 : 0;
    }
}

static void apply_dither(GenState* state) {
    switch(state->dither_mode) {
    case DITHER_FLOYD_STEINBERG:
        dither_floyd_steinberg(state);
        break;
    case DITHER_BAYER:
        dither_bayer(state);
        break;
    case DITHER_ATKINSON:
        dither_atkinson(state);
        break;
    case DITHER_THRESHOLD:
        dither_threshold(state);
        break;
    }
}
