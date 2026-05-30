#pragma once

/**
 * Math utilities, constants, enums, and shared types for the
 * Flipper Zero Generative Art Engine.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define PIXEL_COUNT   (SCREEN_WIDTH * SCREEN_HEIGHT) // 8192
#define BITMAP_SIZE   (PIXEL_COUNT / 8) // 1024 bytes for 1-bit packed
#define FRACTAL_ITER  20 // max escape-time iterations
#define FRAME_MS      33 // timer tick in ms (~30 FPS)
#define EVOLVE_FRAMES 30 // ticks between auto-evolve mutations (~1 sec)
#define FREQ_MIN      0.1f
#define FREQ_MAX      4.0f
#define FREQ_STEP     0.2f
#define FONT_CHAR_W   6 // approximate width of FontSecondary glyphs

// Reciprocal constants — multiply instead of divide in the per-pixel loop.
// Exact in IEEE 754 since 128 and 64 are powers of two.
#define INV_WIDTH  (1.0f / 128.0f) // 0.0078125f
#define INV_HEIGHT (1.0f / 64.0f) // 0.015625f

// -- Pattern IDs --
enum {
    PAT_HORIZONTAL, //  0: left-to-right gradient
    PAT_VERTICAL, //  1: top-to-bottom gradient
    PAT_RADIAL, //  2: center-outward gradient
    PAT_DIAGONAL, //  3: corner-to-corner gradient
    PAT_SINE, //  4: animated horizontal sine wave
    PAT_COSINE, //  5: animated vertical cosine wave
    PAT_INTERFERENCE, //  6: product of two animated sine waves
    PAT_CHECKERBOARD, //  7: XOR checkerboard grid
    PAT_NOISE, //  8: hash-based noise, shifts over time
    PAT_SPIRAL, //  9: rotating polar-coordinate spiral
    PAT_MANDELBROT, // 10: infinite zoom into Seahorse Valley
    PAT_JULIA, // 11: Julia set with orbiting c parameter
    PAT_PLASMA, // 12: classic demoscene sum-of-sines
    PAT_PERLIN, // 13: 4-octave layered noise
    PAT_MOIRE, // 14: two drifting concentric ring sets
    PAT_SIERPINSKI, // 15: XOR fractal with horizontal scroll
    PATTERN_COUNT, // 16: sentinel / "no layer" value
};

// -- Blend mode IDs --
enum {
    BLEND_NONE, // 0: passthrough
    BLEND_XOR, // 1: bitwise XOR
    BLEND_AND, // 2: bitwise AND
    BLEND_OR, // 3: bitwise OR
    BLEND_ADD, // 4: additive (clamped)
    BLEND_MULTIPLY, // 5: darken
    BLEND_DIFF, // 6: absolute difference
    BLEND_SCREEN, // 7: inverse multiply
    BLEND_COUNT,
};

// -- Dither algorithm IDs --
enum {
    DITHER_FLOYD_STEINBERG, // 0: error-diffusion
    DITHER_BAYER, // 1: ordered 4x4
    DITHER_ATKINSON, // 2: Mac-style 6/8 error
    DITHER_THRESHOLD, // 3: hard cutoff
    DITHER_COUNT,
};

// Rendering state: pixel buffer + all user-configurable parameters.
typedef struct {
    uint8_t pixels[PIXEL_COUNT]; // 8-bit grayscale, dithered to 0/255 in-place
    uint32_t seed; // RNG seed, re-rolled on OK press
    uint8_t pattern_a; // primary pattern (0–15)
    uint8_t pattern_b; // layer pattern (0–15 or PATTERN_COUNT=none)
    uint8_t blend_mode; // blend operation for combining a+b
    uint8_t dither_mode; // quantization algorithm
    float frequency; // scale/speed control (0.1–4.0)
    bool invert; // flip black↔white after blend
    uint32_t frame_count; // monotonic frame counter
} GenState;

// Per-frame precomputed values. Stack-allocated in generate_frame(),
// populated by frame_ctx_init(). Eliminates redundant per-pixel math
// for values that are constant across an entire frame.
typedef struct {
    // -- Common (used by most patterns) --
    float t; // animation time: frame_count * 0.05
    float freq; // cached state->frequency

    // -- Mandelbrot (precomputed zoom) --
    float m_x0; // left edge of viewport in complex plane
    float m_y0; // top edge of viewport
    float m_dx; // x step per pixel (scale / width)
    float m_dy; // y step per pixel (scale * aspect / height)

    // -- Julia (precomputed c parameter) --
    float j_cr; // real part of c
    float j_ci; // imaginary part of c

    // -- Moire (precomputed drift offsets) --
    float mo_ox; // horizontal center drift
    float mo_oy; // vertical center drift

    // -- Noise seeds (avoid recomputing in per-pixel loop) --
    uint32_t noise_seed; // state->seed + frame_count/3
    uint32_t perlin_seed; // state->seed + frame_count/4
    uint32_t frame_count; // cached for patterns that need raw count
} FrameCtx;

// xorshift32: fast PRNG for auto-evolve mutations. ~3 cycles on ARM.
static uint32_t xorshift32(uint32_t* s) {
    if(*s == 0) *s = 1; // prevent zero-lock
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

// 64-entry sine LUT, amplitude ±64. One full period = indices 0–63.
// Last 3 entries zero-padded for minor wrap smoothing.
static const int8_t sine_lut[64] = {
    0,   6,   12,  18,  24,  30,  36,  41, //  0– 7: rising
    46,  50,  54,  57,  60,  62,  63,  64, //  8–15: peak
    63,  62,  60,  57,  54,  50,  46,  41, // 16–23: falling
    36,  30,  24,  18,  12,  6,   0,   -6, // 24–31: zero crossing
    -12, -18, -24, -30, -36, -41, -46, -50, // 32–39: trough approach
    -54, -57, -60, -62, -63, -64, // 40–45: trough
    -63, -62, -60, -57, -54, -50, -46, -41, // 46–53: rising from trough
    -36, -30, -24, -18, -12, -6,  0,   0,   0, 0, // 54–63: zero crossing + pad
};

// Sine lookup by 6-bit angle. Single array access — ~1 cycle.
static inline int8_t fast_sin(uint8_t angle) {
    return sine_lut[angle & 63];
}

// Spatial hash noise: returns 0–255 for any (x, y, seed).
// Two multiply-XOR rounds for good avalanche. ~6 cycles.
static inline uint8_t hash_noise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = (x * 374761393u) + (y * 668265263u) + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 0xFF;
}

// Clamp int to 0–255. ARM compiles to two conditional moves (branchless).
static inline uint8_t clamp8(int v) {
    if(v < 0) return 0;
    if(v > 255) return 255;
    return (uint8_t)v;
}

// Fast atan2 approximation. ~10 cycles vs ~80 for libm atan2f.
// Max error ~0.07 radians (~4°) — invisible in generative art.
static float fast_atan2(float y, float x) {
    float abs_y = fabsf(y) + 1e-10f; // prevent 0/0
    float r;
    if(x >= 0) {
        r = (x - abs_y) / (x + abs_y); // range reduction to [-1,1]
        r = 0.7854f - 0.7854f * r; // linear approx in [0, π/2]
    } else {
        r = (x + abs_y) / (abs_y - x);
        r = 2.3562f - 0.7854f * r; // linear approx in [π/2, π]
    }
    return y < 0 ? -r : r; // sign correction
}

// Fractal iteration-to-brightness LUT. Avoids per-pixel multiply+divide.
// fractal_lut[i] = i * 255 / FRACTAL_ITER, with [FRACTAL_ITER] = 0 (inside set).
static const uint8_t fractal_lut[FRACTAL_ITER + 1] = {
    0, 12, 25, 38, 51, 63, 76, 89, 102, 114, 127, 140, 153, 165, 178, 191, 204, 216, 229, 242,
    0, // index 20: inside set → black
};
