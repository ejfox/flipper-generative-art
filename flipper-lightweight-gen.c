#include <furi.h>
#include <gui/gui.h>
#include <notification/notification_messages.h>
#include <dolphin/dolphin.h>
#include <stdlib.h>
#include <math.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

typedef struct {
    uint8_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];
    uint32_t seed;
    uint8_t mode;
    uint8_t gradient_type;
    float frequency;
    float noise_scale;
    bool invert;
    uint8_t frame_count;
} GenerativeState;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriTimer* timer;
    GenerativeState* state;
    NotificationApp* notifications;
} FlipperGenApp;

// Lightweight pseudo-random number generator
static uint32_t xorshift32(uint32_t* state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

// Fast sine approximation using lookup table
static const int8_t sine_table[64] = {
    0, 6, 12, 18, 24, 30, 36, 41, 46, 50, 54, 57, 60, 62, 63, 64,
    63, 62, 60, 57, 54, 50, 46, 41, 36, 30, 24, 18, 12, 6, 0, -6,
    -12, -18, -24, -30, -36, -41, -46, -50, -54, -57, -60, -62, -63, -64,
    -63, -62, -60, -57, -54, -50, -46, -41, -36, -30, -24, -18, -12, -6, 0, 0, 0, 0
};

static int8_t fast_sin(uint8_t angle) {
    return sine_table[angle & 63];
}

// Simple Perlin-like noise using bit manipulation
static uint8_t simple_noise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t hash = (x * 374761393) + (y * 668265263) + seed;
    hash = (hash ^ (hash >> 13)) * 1274126177;
    return (hash ^ (hash >> 16)) & 0xFF;
}

// Gradient generators
static uint8_t generate_gradient(uint8_t x, uint8_t y, GenerativeState* state) {
    float nx = (float)x / SCREEN_WIDTH;
    float ny = (float)y / SCREEN_HEIGHT;
    float value = 0.0f;
    
    switch(state->gradient_type) {
        case 0: // horizontal
            value = nx;
            break;
        case 1: // vertical
            value = ny;
            break;
        case 2: // radial
            {
                float dx = nx - 0.5f;
                float dy = ny - 0.5f;
                value = sqrtf(dx*dx + dy*dy) * 1.414f; // normalize
            }
            break;
        case 3: // diagonal
            value = (nx + ny) / 2.0f;
            break;
        case 4: // sine wave
            value = (fast_sin((uint8_t)(nx * 64 * state->frequency)) + 64) / 128.0f;
            break;
        case 5: // cosine wave
            value = (fast_sin((uint8_t)(ny * 64 * state->frequency + 16)) + 64) / 128.0f;
            break;
        case 6: // interference
            {
                int8_t wave1 = fast_sin((uint8_t)(nx * 32 * state->frequency));
                int8_t wave2 = fast_sin((uint8_t)(ny * 32 * state->frequency));
                value = ((wave1 * wave2) / 64 + 64) / 128.0f;
            }
            break;
        case 7: // checkerboard
            {
                uint8_t check_x = (uint8_t)(nx * 8 * state->frequency) & 1;
                uint8_t check_y = (uint8_t)(ny * 8 * state->frequency) & 1;
                value = (check_x ^ check_y) ? 1.0f : 0.0f;
            }
            break;
        case 8: // noise
            value = simple_noise(x, y, state->seed) / 255.0f;
            break;
        case 9: // spiral
            {
                float dx = nx - 0.5f;
                float dy = ny - 0.5f;
                float angle = atan2f(dy, dx);
                float dist = sqrtf(dx*dx + dy*dy);
                value = fmodf((angle + dist * 10.0f), 6.28f) / 6.28f;
            }
            break;
        default:
            value = nx;
    }
    
    // Apply noise overlay
    if(state->noise_scale > 0) {
        float noise = simple_noise(
            (uint32_t)(x * state->noise_scale),
            (uint32_t)(y * state->noise_scale),
            state->seed
        ) / 255.0f;
        value = value * 0.7f + noise * 0.3f;
    }
    
    // Clamp and invert if needed
    if(value < 0) value = 0;
    if(value > 1) value = 1;
    if(state->invert) value = 1.0f - value;
    
    return (uint8_t)(value * 255);
}

// Floyd-Steinberg dithering
static void apply_dither(GenerativeState* state) {
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        for(int x = 0; x < SCREEN_WIDTH; x++) {
            int idx = y * SCREEN_WIDTH + x;
            uint8_t old_pixel = state->pixels[idx];
            uint8_t new_pixel = old_pixel > 127 ? 255 : 0;
            state->pixels[idx] = new_pixel;
            
            int error = old_pixel - new_pixel;
            
            // Distribute error to neighbors
            if(x + 1 < SCREEN_WIDTH) {
                int right_idx = y * SCREEN_WIDTH + (x + 1);
                int new_val = state->pixels[right_idx] + (error * 7) / 16;
                state->pixels[right_idx] = (new_val < 0) ? 0 : (new_val > 255) ? 255 : new_val;
            }
            
            if(y + 1 < SCREEN_HEIGHT) {
                if(x > 0) {
                    int bl_idx = (y + 1) * SCREEN_WIDTH + (x - 1);
                    int new_val = state->pixels[bl_idx] + (error * 3) / 16;
                    state->pixels[bl_idx] = (new_val < 0) ? 0 : (new_val > 255) ? 255 : new_val;
                }
                
                int bottom_idx = (y + 1) * SCREEN_WIDTH + x;
                int new_val = state->pixels[bottom_idx] + (error * 5) / 16;
                state->pixels[bottom_idx] = (new_val < 0) ? 0 : (new_val > 255) ? 255 : new_val;
                
                if(x + 1 < SCREEN_WIDTH) {
                    int br_idx = (y + 1) * SCREEN_WIDTH + (x + 1);
                    int new_val = state->pixels[br_idx] + (error * 1) / 16;
                    state->pixels[br_idx] = (new_val < 0) ? 0 : (new_val > 255) ? 255 : new_val;
                }
            }
        }
    }
}

// Generate new frame
static void generate_frame(GenerativeState* state) {
    // Generate gradient
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        for(int x = 0; x < SCREEN_WIDTH; x++) {
            state->pixels[y * SCREEN_WIDTH + x] = generate_gradient(x, y, state);
        }
    }
    
    // Apply dithering
    apply_dither(state);
    
    // Evolve parameters for next frame
    state->frame_count++;
    if(state->frame_count % 30 == 0) { // Change every 30 frames (~1 second)
        uint32_t rng_state = state->seed + state->frame_count;
        
        // Sometimes change gradient type
        if((xorshift32(&rng_state) % 100) < 20) {
            state->gradient_type = xorshift32(&rng_state) % 10;
        }
        
        // Vary frequency
        state->frequency = 0.5f + (float)(xorshift32(&rng_state) % 100) / 50.0f;
        
        // Vary noise
        state->noise_scale = (float)(xorshift32(&rng_state) % 50) / 1000.0f;
        
        // Sometimes invert
        if((xorshift32(&rng_state) % 100) < 10) {
            state->invert = !state->invert;
        }
    }
}

// Draw callback
static void draw_callback(Canvas* canvas, void* context) {
    GenerativeState* state = (GenerativeState*)context;
    
    // Draw pixels
    for(int y = 0; y < SCREEN_HEIGHT; y++) {
        for(int x = 0; x < SCREEN_WIDTH; x++) {
            if(state->pixels[y * SCREEN_WIDTH + x] > 127) {
                canvas_draw_dot(canvas, x, y);
            }
        }
    }
    
    // Draw minimal UI
    canvas_set_font(canvas, FontSecondary);
    char info[32];
    snprintf(info, sizeof(info), "G:%d F:%.1f", state->gradient_type, (double)state->frequency);
    canvas_draw_str(canvas, 1, 8, info);
}

// Input callback
static void input_callback(InputEvent* input_event, void* context) {
    FlipperGenApp* app = (FlipperGenApp*)context;
    
    if(input_event->type == InputTypePress) {
        switch(input_event->key) {
            case InputKeyOk:
                // Generate new pattern
                app->state->seed = furi_get_tick();
                app->state->gradient_type = app->state->seed % 10;
                app->state->frequency = 0.5f + (float)(app->state->seed % 100) / 50.0f;
                break;
            case InputKeyUp:
                app->state->gradient_type = (app->state->gradient_type + 1) % 10;
                break;
            case InputKeyDown:
                app->state->gradient_type = (app->state->gradient_type + 9) % 10;
                break;
            case InputKeyLeft:
                app->state->frequency = fmaxf(0.1f, app->state->frequency - 0.1f);
                break;
            case InputKeyRight:
                app->state->frequency = fminf(4.0f, app->state->frequency + 0.1f);
                break;
            case InputKeyBack:
                // Exit handled by scene manager
                break;
            case InputKeyMAX:
                // Handle enum max value - do nothing
                break;
        }
    }
}

// Timer callback for animation
static void timer_callback(void* context) {
    FlipperGenApp* app = (FlipperGenApp*)context;
    generate_frame(app->state);
    view_port_update(app->view_port);
}

// App lifecycle
FlipperGenApp* flipper_gen_app_alloc() {
    FlipperGenApp* app = malloc(sizeof(FlipperGenApp));
    
    app->state = malloc(sizeof(GenerativeState));
    app->state->seed = furi_get_tick();
    app->state->mode = 0;
    app->state->gradient_type = 0;
    app->state->frequency = 1.0f;
    app->state->noise_scale = 0.05f;
    app->state->invert = false;
    app->state->frame_count = 0;
    
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app->state);
    view_port_input_callback_set(app->view_port, input_callback, app);
    
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    
    app->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, 33); // ~30 FPS
    
    // Generate initial frame
    generate_frame(app->state);
    
    return app;
}

void flipper_gen_app_free(FlipperGenApp* app) {
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    
    free(app->state);
    free(app);
}

int32_t flipper_gen_app(void* p) {
    UNUSED(p);
    
    FlipperGenApp* app = flipper_gen_app_alloc();
    
    notification_message(app->notifications, &sequence_display_backlight_on);
    
    view_port_update(app->view_port);
    
    // Wait for user to exit
    while(true) {
        furi_delay_ms(100);
        // In a real app, you'd check for exit conditions here
    }
    
    flipper_gen_app_free(app);
    
    return 0;
}