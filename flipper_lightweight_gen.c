/**
 * Flipper Zero Generative Art Engine v2.1
 *
 * Performance-optimized for the ARM Cortex-M4 @ 64MHz with hardware FPU.
 *
 * Key optimizations over v2.0:
 *   - FrameCtx precomputation: expf/fmodf/atan2 computed ONCE per frame,
 *     not per pixel (saves ~500K cycles/frame on Mandelbrot)
 *   - Bitmap blit: pack_bitmap() + canvas_draw_xbm() replaces 4096+
 *     individual canvas_draw_dot() calls
 *   - Pointer-based dither iteration: eliminates y*WIDTH multiply per pixel
 *   - fast_atan2() approximation: ~5x faster than libm atan2f
 *   - Reciprocal multiply: x * INV_WIDTH instead of x / SCREEN_WIDTH
 *   - Fractal LUT: single byte lookup instead of multiply+divide per pixel
 *   - Cached zx²/zy² in fractal loops: saves 2 multiplies per iteration
 *   - Separate invert pass: branch-free main generation loop
 *
 * Architecture:
 *   ViewDispatcher manages two views:
 *     - Config (VariableItemList): pattern/blend/dither selection
 *     - Canvas (custom View):     live rendering + controls
 *
 *   A periodic FuriTimer drives animation at ~30 FPS, running only while
 *   the canvas view is active.
 *
 * Modules:
 *   - math_utils.h:      constants, enums, types, math primitives
 *   - patterns.h:         FrameCtx + 16 pattern generators
 *   - blend.h:            8 blend modes
 *   - dither.h:           4 dither algorithms
 *   - frame_pipeline.h:   generate_frame() orchestrator
 */

// ===========================================================================
// INCLUDES
// ===========================================================================

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification_messages.h>

#include "frame_pipeline.h"

// ===========================================================================
// VIEW IDS
// ===========================================================================

// -- View IDs --
typedef enum {
    ViewIdConfig,
    ViewIdCanvas,
} ViewId;

// ===========================================================================
// NAME TABLES
// ===========================================================================

// Full names for the config screen.
static const char* const pattern_names[] = {
    "Horizontal",
    "Vertical",
    "Radial",
    "Diagonal",
    "Sine",
    "Cosine",
    "Interfer.",
    "Checker",
    "Noise",
    "Spiral",
    "Mandelbrot",
    "Julia Set",
    "Plasma",
    "Perlin",
    "Moire",
    "Sierpinski",
};

// Short names for the canvas overlay.
static const char* const pattern_short[] = {
    "horiz",
    "vert",
    "radial",
    "diag",
    "sine",
    "cos",
    "interf",
    "check",
    "noise",
    "spiral",
    "mandel",
    "julia",
    "plasma",
    "perlin",
    "moire",
    "sierp",
};

static const char* const blend_names[] = {
    "None",
    "XOR",
    "AND",
    "OR",
    "Add",
    "Multiply",
    "Diff",
    "Screen",
};
static const char* const dither_names[] = {
    "Floyd-S.",
    "Bayer 4x4",
    "Atkinson",
    "Threshold",
};

// ===========================================================================
// DATA STRUCTURES
// ===========================================================================

// Top-level app context.
typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    VariableItemList* config_list;
    View* canvas_view;
    FuriTimer* timer;
    GenState* state;
    NotificationApp* notifications;
    bool auto_evolve;
    bool show_overlay;        // toggle text overlay on canvas view
    uint32_t show_name_until; // frame counter: flash pattern name briefly on change
    bool down_was_held;       // tracks if DOWN repeat fired (long-hold detection)
    bool request_config;      // deferred: switch to config on next timer tick

    // Config item handles for syncing display after canvas/auto-evolve changes.
    VariableItem* item_pat_a;
    VariableItem* item_pat_b;
    VariableItem* item_blend;
    VariableItem* item_dither;
    VariableItem* item_auto;
} GenApp;

// ===========================================================================
// CANVAS VIEW
// ===========================================================================

static void adjust_frequency(GenState* state, float delta) {
    state->frequency = fmaxf(FREQ_MIN, fminf(FREQ_MAX, state->frequency + delta));
}

// Pack dithered pixel buffer (0x00/0xFF values) into 1-bit XBM bitmap.
// Branchless: extracts MSB of each pixel with shifts.
// 1024 iterations instead of ~4096 canvas_draw_dot calls.
static void pack_bitmap(const uint8_t* pixels, uint8_t* bitmap) {
    for(int i = 0; i < BITMAP_SIZE; i++) {
        const uint8_t* p = &pixels[i << 3]; // p = &pixels[i * 8]
        // XBM format: LSB = leftmost pixel of the 8.
        // After dithering pixels are 0x00 or 0xFF; MSB (bit 7) is 0 or 1.
        bitmap[i] = ((p[0] & 0x80) >> 7) // bit 0: pixel 0
                    | ((p[1] & 0x80) >> 6) // bit 1: pixel 1
                    | ((p[2] & 0x80) >> 5) // bit 2: pixel 2
                    | ((p[3] & 0x80) >> 4) // bit 3: pixel 3
                    | ((p[4] & 0x80) >> 3) // bit 4: pixel 4
                    | ((p[5] & 0x80) >> 2) // bit 5: pixel 5
                    | ((p[6] & 0x80) >> 1) // bit 6: pixel 6
                    | (p[7] & 0x80); // bit 7: pixel 7
    }
}

// Draw callback: pack pixels → blit XBM → overlay text.
static void canvas_draw_cb(Canvas* canvas, void* _model) {
    GenApp* app = *(GenApp**)_model;
    GenState* s = app->state;

    // Pack 8KB grayscale → 1KB bitmap, then blit in one call
    uint8_t bitmap[BITMAP_SIZE]; // 1024 bytes on stack (within 4KB budget)
    pack_bitmap(s->pixels, bitmap);
    canvas_draw_xbm(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, bitmap);

    // -- Brief pattern name flash on change (zen mode only, not during auto-evolve) --
    if(!app->show_overlay && !app->auto_evolve && s->frame_count < app->show_name_until) {
        canvas_set_font(canvas, FontPrimary);
        const char* name = pattern_names[s->pattern_a];
        // Center the name on screen
        uint16_t tw = strlen(name) * 7; // FontPrimary is ~7px wide
        int16_t tx = (SCREEN_WIDTH - tw) / 2;
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, tx - 2, 24, tw + 4, 16);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_str(canvas, tx, 37, name);
    }

    // -- Status overlay --
    if(app->show_overlay) {
        canvas_set_font(canvas, FontSecondary);
        char line[48];

        snprintf(
            line,
            sizeof(line),
            "%s %.1f%s",
            pattern_short[s->pattern_a],
            (double)s->frequency,
            s->invert ? " inv" : "");
        uint16_t w = strlen(line) * FONT_CHAR_W;
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, 0, w + 2, 10);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_str(canvas, 1, 8, line);

        if(s->pattern_b < PATTERN_COUNT && s->blend_mode != BLEND_NONE) {
            snprintf(
                line,
                sizeof(line),
                "+%s %s  %s",
                pattern_short[s->pattern_b],
                blend_names[s->blend_mode],
                dither_names[s->dither_mode]);
            w = strlen(line) * FONT_CHAR_W;
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_box(canvas, 0, 10, w + 2, 10);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_str(canvas, 1, 18, line);
        }

    }
}

// Input callback.
static bool canvas_input_cb(InputEvent* event, void* context) {
    GenApp* app = context;
    GenState* s = app->state;

    // OK → request switch to config (deferred to timer to avoid reentrancy)
    if(event->key == InputKeyOk && event->type == InputTypePress) {
        app->request_config = true;
        return true;
    }

    // DOWN: short tap = prev pattern, long hold = toggle overlay
    if(event->key == InputKeyDown) {
        if(event->type == InputTypeRepeat) {
            if(!app->down_was_held) {
                app->show_overlay = !app->show_overlay;
                app->down_was_held = true;
            }
            return true;
        }
        if(event->type == InputTypeShort) {
            if(!app->down_was_held) {
                s->pattern_a = (s->pattern_a + PATTERN_COUNT - 1) % PATTERN_COUNT;
                app->show_name_until = s->frame_count + 30;
            }
            app->down_was_held = false;
            return true;
        }
        if(event->type == InputTypeRelease) {
            app->down_was_held = false;
        }
        return false;
    }

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        switch(event->key) {
        case InputKeyUp:
            s->pattern_a = (s->pattern_a + 1) % PATTERN_COUNT;
            app->show_name_until = s->frame_count + 30;
            return true;
        case InputKeyLeft:
            adjust_frequency(s, -FREQ_STEP);
            return true;
        case InputKeyRight:
            adjust_frequency(s, FREQ_STEP);
            return true;
        default:
            return false;
        }
    }

    return false;
}

// Canvas lifecycle: start/stop timer.
static void canvas_enter_cb(void* context) {
    GenApp* app = context;
    notification_message(app->notifications, &sequence_display_backlight_on);
    furi_timer_start(app->timer, FRAME_MS);
}

static void canvas_exit_cb(void* context) {
    GenApp* app = context;
    furi_timer_stop(app->timer);

    // Sync config UI with state (may have been changed by canvas controls or auto-evolve)
    GenState* s = app->state;
    variable_item_set_current_value_index(app->item_pat_a, s->pattern_a);
    variable_item_set_current_value_text(app->item_pat_a, pattern_names[s->pattern_a]);

    uint8_t pb_idx = (s->pattern_b >= PATTERN_COUNT) ? 0 : s->pattern_b + 1;
    variable_item_set_current_value_index(app->item_pat_b, pb_idx);
    variable_item_set_current_value_text(
        app->item_pat_b, pb_idx == 0 ? "None" : pattern_names[s->pattern_b]);

    variable_item_set_current_value_index(app->item_blend, s->blend_mode);
    variable_item_set_current_value_text(app->item_blend, blend_names[s->blend_mode]);

    variable_item_set_current_value_index(app->item_dither, s->dither_mode);
    variable_item_set_current_value_text(app->item_dither, dither_names[s->dither_mode]);

    variable_item_set_current_value_index(app->item_auto, app->auto_evolve ? 1 : 0);
    variable_item_set_current_value_text(app->item_auto, app->auto_evolve ? "On" : "Off");
}

static uint32_t canvas_prev_cb(void* context) {
    UNUSED(context);
    return ViewIdConfig;
}

// Timer: generate frame + trigger redraw.
static void timer_cb(void* context) {
    GenApp* app = context;
    // Deferred view switch (safe: not inside input callback)
    if(app->request_config) {
        app->request_config = false;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdConfig);
        return;
    }
    generate_frame(app->state, app->auto_evolve);
    with_view_model(app->canvas_view, GenApp * *m, { UNUSED(m); }, true);
}

// ===========================================================================
// CONFIG VIEW
// ===========================================================================

static void on_pattern_a(VariableItem* item) {
    GenApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->state->pattern_a = idx;
    variable_item_set_current_value_text(item, pattern_names[idx]);
}

static void on_pattern_b(VariableItem* item) {
    GenApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx == 0) {
        app->state->pattern_b = PATTERN_COUNT;
        variable_item_set_current_value_text(item, "None");
    } else {
        app->state->pattern_b = idx - 1;
        variable_item_set_current_value_text(item, pattern_names[idx - 1]);
    }
}

static void on_blend(VariableItem* item) {
    GenApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->state->blend_mode = idx;
    variable_item_set_current_value_text(item, blend_names[idx]);
}

static void on_dither(VariableItem* item) {
    GenApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->state->dither_mode = idx;
    variable_item_set_current_value_text(item, dither_names[idx]);
}

static void on_auto(VariableItem* item) {
    GenApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->auto_evolve = (idx == 1);
    variable_item_set_current_value_text(item, idx ? "On" : "Off");
}

static void config_enter_cb(void* context, uint32_t index) {
    UNUSED(index);
    GenApp* app = context;
    app->state->frame_count = 0;
    app->state->seed = furi_get_tick();
    generate_frame(app->state, app->auto_evolve);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdCanvas);
}

static uint32_t config_prev_cb(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

// ===========================================================================
// APP LIFECYCLE
// ===========================================================================

static GenApp* app_alloc(void) {
    GenApp* app = malloc(sizeof(GenApp));
    furi_check(app);

    app->state = malloc(sizeof(GenState));
    furi_check(app->state);
    memset(app->state, 0, sizeof(GenState));
    app->state->seed = furi_get_tick();
    app->state->pattern_a = PAT_MANDELBROT;
    app->state->pattern_b = PATTERN_COUNT;
    app->state->blend_mode = BLEND_NONE;
    app->state->dither_mode = DITHER_FLOYD_STEINBERG;
    app->state->frequency = 1.0f;
    app->auto_evolve = false;
    app->show_overlay = false; // zen mode by default — hold DOWN to show info
    app->down_was_held = false;
    app->request_config = false;

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Config list
    app->config_list = variable_item_list_alloc();

    app->item_pat_a =
        variable_item_list_add(app->config_list, "Pattern", PATTERN_COUNT, on_pattern_a, app);
    variable_item_set_current_value_index(app->item_pat_a, PAT_MANDELBROT);
    variable_item_set_current_value_text(app->item_pat_a, pattern_names[PAT_MANDELBROT]);

    app->item_pat_b =
        variable_item_list_add(app->config_list, "Layer", PATTERN_COUNT + 1, on_pattern_b, app);
    variable_item_set_current_value_index(app->item_pat_b, 0);
    variable_item_set_current_value_text(app->item_pat_b, "None");

    app->item_blend =
        variable_item_list_add(app->config_list, "Blend", BLEND_COUNT, on_blend, app);
    variable_item_set_current_value_index(app->item_blend, BLEND_NONE);
    variable_item_set_current_value_text(app->item_blend, blend_names[BLEND_NONE]);

    app->item_dither =
        variable_item_list_add(app->config_list, "Dither", DITHER_COUNT, on_dither, app);
    variable_item_set_current_value_index(app->item_dither, DITHER_FLOYD_STEINBERG);
    variable_item_set_current_value_text(app->item_dither, dither_names[DITHER_FLOYD_STEINBERG]);

    app->item_auto = variable_item_list_add(app->config_list, "Auto-evolve", 2, on_auto, app);
    variable_item_set_current_value_index(app->item_auto, 0);
    variable_item_set_current_value_text(app->item_auto, "Off");

    variable_item_list_set_enter_callback(app->config_list, config_enter_cb, app);
    view_set_previous_callback(variable_item_list_get_view(app->config_list), config_prev_cb);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdConfig, variable_item_list_get_view(app->config_list));

    // Canvas view
    app->canvas_view = view_alloc();
    view_allocate_model(app->canvas_view, ViewModelTypeLockFree, sizeof(GenApp*));
    with_view_model(app->canvas_view, GenApp * *model, { *model = app; }, false);
    view_set_context(app->canvas_view, app);
    view_set_draw_callback(app->canvas_view, canvas_draw_cb);
    view_set_input_callback(app->canvas_view, canvas_input_cb);
    view_set_enter_callback(app->canvas_view, canvas_enter_cb);
    view_set_exit_callback(app->canvas_view, canvas_exit_cb);
    view_set_previous_callback(app->canvas_view, canvas_prev_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdCanvas, app->canvas_view);

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);

    return app;
}

static void app_free(GenApp* app) {
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdCanvas);
    view_free(app->canvas_view);

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdConfig);
    variable_item_list_free(app->config_list);

    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    free(app->state);
    free(app);
}

int32_t flipper_gen_app(void* p) {
    UNUSED(p);
    GenApp* app = app_alloc();

    // Boot straight into art — Back goes to config
    generate_frame(app->state, app->auto_evolve);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdCanvas);
    view_dispatcher_run(app->view_dispatcher);

    app_free(app);
    return 0;
}
