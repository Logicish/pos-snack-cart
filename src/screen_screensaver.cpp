#include "screen_screensaver.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "backlight.h"
#include "session_timer.h"
#include "db.h"
#include <lvgl.h>
#include <Arduino.h>
#include <stdlib.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the screensaver declared in screen_screensaver.h -- the
            dancing/wandering fish animation, bubble particles, and the backlight dim
            that goes with entering/leaving it. The GM65 scan-mode toggle this screen
            used to own directly was centralized into buttons.cpp's wantsScanner system
            2026-09-15 -- see ButtonHandlers::wantsScanner in buttons.h.
*/

// Backlight dim level while the screensaver is up -- adjustable from Settings ->
// Screensaver, 2026-09-14 (was a hardcoded 30%). Same config-table persistence pattern as
// idle_timer.cpp's timeout. Steps of 10 (10 discrete positions, same button-press feel as
// idle_timer's 1-10 minute cycle) -- 0% would leave the screen fully black, so the floor
// is 10, dim enough for real power/burn-in-adjacent savings but still visible.
#define DIM_DEFAULT_PCT 30
#define DIM_MIN_PCT     10
#define DIM_MAX_PCT     100
#define DIM_STEP        10
#define DIM_CONFIG_KEY  "screensaver_dim_pct"

static int _dimPct = DIM_DEFAULT_PCT;

// Loads the saved dim level from config; falls back to the built-in default if
// nothing's saved yet or the saved value is out of range.
void screensaver_dim_load_from_config() {
    char buf[8];
    if (db_config_get(DIM_CONFIG_KEY, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= DIM_MIN_PCT && v <= DIM_MAX_PCT) _dimPct = v;
    }
}

// Returns the current dim-level percentage.
int screensaver_dim_get_pct() {
    return _dimPct;
}

// Sets a new dim level (wrapping 10-100 by tens) and persists it to config.
void screensaver_dim_set_pct(int pct) {
    if (pct > DIM_MAX_PCT) pct = DIM_MIN_PCT;  // wrap both ends, matches the cycling-row UX
    if (pct < DIM_MIN_PCT) pct = DIM_MAX_PCT;
    _dimPct = pct;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", _dimPct);
    db_config_set(DIM_CONFIG_KEY, buf);
}

// GM65 scan-mode toggle -- locked in 2026-08-26 after real bench testing (see project
// memory: project_gm65_settings_lockin), CENTRALIZED 2026-09-15 into buttons.cpp's
// wantsScanner system (this screen just wants it off, like most non-transaction
// screens now do) rather than this file managing the register write + ACK drain
// itself. Real hand-wave testing already confirmed the underlying mechanism works; a
// physical power switch was considered and rejected as unnecessary.

LV_IMG_DECLARE(fish_left);
LV_IMG_DECLARE(fish_right);

static lv_obj_t   *_scr;
static lv_obj_t   *_fish_img;
static lv_obj_t   *_wordmark;
static lv_obj_t   *_press_key_lbl;
static lv_timer_t *_timer;
static uint32_t    _tick;

// Fish random-walk offsets from its resting spot (LV_ALIGN_CENTER, 0, 20). Updated once
// per beat in fish_wander(), clamped to an invisible box so it never leaves frame.
static int16_t     _fish_dx;
static int16_t     _fish_dy;

#define WORDMARK_Y_HIGH   -20
#define WORDMARK_Y_LOW    -12
#define PRESS_KEY_Y_HIGH  -44  // sits one line above the wordmark, bobs the same way so
#define PRESS_KEY_Y_LOW   -36  // it doesn't sit static on the same pixels for hours on end

// Dance sequence: right2,left2,right2,left2,right4,left1,right1,left2 (16s cycle, then repeat)
static const bool DANCE_RIGHT[16] = {
    true, true, false, false, true, true, false, false,
    true, true, true,  true,  false, true, false, false,
};

// Random walk layered on top of the flip "dance", 2026-08-27. Each beat: 50% chance to
// step FORWARD (whichever way the fish is currently facing) a few px, and independently
// 33/33/33 up / level / down a few px. Both axes are clamped to an invisible box, so
// pressed against the forward wall a "step forward" roll just clamps back to the edge —
// net zero advance — which is the intended "no chance of leaving frame" behaviour, and
// it works the same on either side.
#define FISH_BOX_DX          90   // max horizontal offset from center, each side
#define FISH_BOX_UP         100   // max upward offset from the resting baseline
#define FISH_BOX_DOWN       100   // max downward offset from the resting baseline
#define FISH_STEP_FWD_MIN     2
#define FISH_STEP_FWD_MAX     6
#define FISH_STEP_VERT_MIN    2
#define FISH_STEP_VERT_MAX    5

static void fish_wander(bool facing_right) {
    if (random(2) == 0) {  // 50/50: advance the way we're facing
        int16_t step = random(FISH_STEP_FWD_MIN, FISH_STEP_FWD_MAX + 1);
        _fish_dx += facing_right ? step : -step;
        if (_fish_dx >  FISH_BOX_DX) _fish_dx =  FISH_BOX_DX;
        if (_fish_dx < -FISH_BOX_DX) _fish_dx = -FISH_BOX_DX;
    }

    int roll = random(3);  // 33/33/33: up / level / down
    if (roll != 1) {
        int16_t step = random(FISH_STEP_VERT_MIN, FISH_STEP_VERT_MAX + 1);
        _fish_dy += (roll == 0) ? -step : step;   // 0 = up, 2 = down
        if (_fish_dy >  FISH_BOX_DOWN) _fish_dy =  FISH_BOX_DOWN;
        if (_fish_dy < -FISH_BOX_UP)   _fish_dy = -FISH_BOX_UP;
    }
}

// ── bubbles ──────────────────────────────────────────────────────────────────
#define MAX_BUBBLES      10
#define BUBBLE_RISE_PX   40
#define BUBBLE_TOP_Y     50   // pop once risen at/above this y
#define BUBBLE_RADIUS     6
#define BUBBLE_X_JITTER  10   // +/- random spread off the spawn anchor
#define BUBBLE_X_OFFSET  60   // spawn anchor offset toward whichever way the fish faces

#define FISH_CENTER_X (SCREEN_W / 2)
#define FISH_CENTER_Y (SCREEN_H / 2 + 20)   // matches fish's LV_ALIGN_CENTER, 0, 20
#define BUBBLE_SPAWN_Y (FISH_CENTER_Y - 40 - 10)  // just above the fish sprite's top edge

struct Bubble {
    lv_obj_t *circle;
    lv_obj_t *pop_label;
    int16_t   x, y;
    bool      active;
    bool      popping;  // true = showing "*" this tick, removed next tick
};

static Bubble _bubbles[MAX_BUBBLES];

// Hides and deactivates one bubble slot.
static void bubble_hide(Bubble &b) {
    lv_obj_add_flag(b.circle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(b.pop_label, LV_OBJ_FLAG_HIDDEN);
    b.active  = false;
    b.popping = false;
}

// Activates the first free bubble slot near the fish's mouth, or no-ops if the pool is full.
static void bubble_spawn(bool facing_right) {
    for (int i = 0; i < MAX_BUBBLES; i++) {
        Bubble &b = _bubbles[i];
        if (b.active) continue;

        int jitter = random(-BUBBLE_X_JITTER, BUBBLE_X_JITTER + 1);
        b.x = FISH_CENTER_X + _fish_dx + (facing_right ? BUBBLE_X_OFFSET : -BUBBLE_X_OFFSET) + jitter;
        b.y = BUBBLE_SPAWN_Y + _fish_dy;
        b.active  = true;
        b.popping = false;

        lv_obj_set_pos(b.circle, b.x - BUBBLE_RADIUS, b.y - BUBBLE_RADIUS);
        lv_obj_clear_flag(b.circle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(b.pop_label, LV_OBJ_FLAG_HIDDEN);
        return;  // pool full? just skip spawning this tick, no big deal
    }
}

// Advances every active bubble one tick: rises, pops at the top, or gets hidden after popping.
static void bubbles_tick() {
    for (int i = 0; i < MAX_BUBBLES; i++) {
        Bubble &b = _bubbles[i];
        if (!b.active) continue;

        if (b.popping) {
            bubble_hide(b);
            continue;
        }

        b.y -= BUBBLE_RISE_PX;
        if (b.y <= BUBBLE_TOP_Y) {
            b.popping = true;
            lv_obj_add_flag(b.circle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(b.pop_label, b.x - 6, b.y - 8);
            lv_obj_clear_flag(b.pop_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_pos(b.circle, b.x - BUBBLE_RADIUS, b.y - BUBBLE_RADIUS);
        }
    }
}

// ── frame paint ──────────────────────────────────────────────────────────────
// Draws one animation frame: fish sprite/position, a bubble spawn, and the bobbing text.
static void paint_frame() {
    bool right = DANCE_RIGHT[_tick % 16];
    lv_img_set_src(_fish_img, right ? &fish_right : &fish_left);

    if (_tick != 0) fish_wander(right);  // _tick==0 is the pre-timer paint — start centered
    lv_obj_align(_fish_img, LV_ALIGN_CENTER, _fish_dx, 20 + _fish_dy);

    bubble_spawn(right);

    bool bob_high = ((_tick / 4) % 2) == 0;
    lv_obj_align(_wordmark, LV_ALIGN_BOTTOM_MID, 0, bob_high ? WORDMARK_Y_HIGH : WORDMARK_Y_LOW);
    lv_obj_align(_press_key_lbl, LV_ALIGN_BOTTOM_MID, 0, bob_high ? PRESS_KEY_Y_HIGH : PRESS_KEY_Y_LOW);
}

// Runs once per second while the screensaver is up.
static void tick_cb(lv_timer_t *) {
    _tick++;
    bubbles_tick();
    paint_frame();
}

// Wakes on any button — deliberately not on a badge scan (see screen_screensaver_is_active(),
// checked in main.cpp's loop() to skip on_scan() entirely while this screen is up). Always
// returns to IDLE, regardless of which of IDLE/Browse originally armed the idle timeout.
static void cb_wake() {
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    for (int i = 0; i < MAX_BUBBLES; i++) bubble_hide(_bubbles[i]);
    backlight_set((uint8_t)backlight_normal_get_pct());
    header_set_visible(true);

    // screen_idle_load() below calls buttons_set_handlers() with wantsScanner=true,
    // which turns the module back on (and drains its ACK) before IDLE's own screen
    // actually loads — see buttons.cpp. Used to do that here directly; centralized
    // 2026-09-15.
    screen_idle_load();
}

// True if this screen is the one currently on screen.
bool screen_screensaver_is_active() {
    return _scr && lv_scr_act() == _scr;
}

// Loads the screensaver: dims the backlight, puts the scanner in Manual mode, and starts
// the animation timer.
void screen_screensaver_push() {
    _tick = 0;
    _fish_dx = 0;
    _fish_dy = 0;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);
        // A drifting fish (or a bubble spawned at its nose) can momentarily poke a pixel
        // past the screen edge; without this LVGL treats that as scrollable content and
        // paints a thin grey scrollbar along that edge. Nothing here is meant to scroll.
        lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(_scr, LV_SCROLLBAR_MODE_OFF);

        _fish_img = lv_img_create(_scr);
        lv_obj_align(_fish_img, LV_ALIGN_CENTER, 0, 20);

        _wordmark = lv_label_create(_scr);
        lv_label_set_text(_wordmark, "LogicishDesigns.com");
        lv_obj_set_style_text_color(_wordmark, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_wordmark, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(_wordmark, LV_ALIGN_BOTTOM_MID, 0, WORDMARK_Y_HIGH);

        _press_key_lbl = lv_label_create(_scr);
        lv_label_set_text(_press_key_lbl, "Press any key to start.");
        lv_obj_set_style_text_color(_press_key_lbl, lv_color_hex(C_YELLOW), LV_PART_MAIN);
        lv_obj_set_style_text_font(_press_key_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(_press_key_lbl, LV_ALIGN_BOTTOM_MID, 0, PRESS_KEY_Y_HIGH);

        // pre-allocate the whole bubble pool up front — avoids heap churn/fragmentation
        // from creating and destroying LVGL objects every tick
        for (int i = 0; i < MAX_BUBBLES; i++) {
            Bubble &b = _bubbles[i];

            b.circle = lv_obj_create(_scr);
            lv_obj_set_size(b.circle, BUBBLE_RADIUS * 2, BUBBLE_RADIUS * 2);
            lv_obj_set_style_radius(b.circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(b.circle, lv_color_hex(C_CYAN), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(b.circle, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(b.circle, 0, LV_PART_MAIN);
            lv_obj_clear_flag(b.circle, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b.circle, LV_OBJ_FLAG_HIDDEN);

            b.pop_label = lv_label_create(_scr);
            lv_label_set_text(b.pop_label, "*");
            lv_obj_set_style_text_color(b.pop_label, lv_color_hex(C_CYAN), LV_PART_MAIN);
            lv_obj_set_style_text_font(b.pop_label, &lv_font_montserrat_16, LV_PART_MAIN);
            lv_obj_add_flag(b.pop_label, LV_OBJ_FLAG_HIDDEN);

            b.active  = false;
            b.popping = false;
        }
    }

    for (int i = 0; i < MAX_BUBBLES; i++) bubble_hide(_bubbles[i]);

    header_set_visible(false);
    backlight_set((uint8_t)_dimPct);
    paint_frame();

    ButtonHandlers h;
    h.up    = cb_wake;
    h.down  = cb_wake;
    h.left  = cb_wake;
    h.right = cb_wake;
    h.enter = cb_wake;
    h.back  = cb_wake;
    h.wantsScanner = false;  // default already, listed explicitly -- this is the screen the
                              // whole centralized on/off system exists to cover
    buttons_set_handlers(h);
    session_timer_disarm();  // nothing logged in while the screensaver is up

    lv_scr_load(_scr);

    if (_timer) lv_timer_del(_timer);
    _timer = lv_timer_create(tick_cb, 1000, nullptr);
}
