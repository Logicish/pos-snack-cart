#include "screen_screensaver.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "backlight.h"
#include <lvgl.h>
#include <Arduino.h>

extern HardwareSerial scanner;  // owned by main.cpp; this screen only ever writes to it

// GM65 scan-mode toggle, 2026-08-26 — locked in after real bench testing (see project
// memory: project_gm65_settings_lockin). 0x54 = Manual mode (no autonomous sensing loop
// at all, so nothing reacts to a passerby's hand) with light/aim/buzzer/LED untouched;
// 0x57 = the full locked config (Induction + Light Normal + Aim Normal + Buzzer on + LED
// off). Both are plain register writes, RAM-only (no EEPROM save) — meant to be sent
// every time, not a one-time provision. Real hand-wave testing already confirmed this
// mechanism works; a physical power switch was considered and rejected as unnecessary.
static void write_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t data) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x08, 0x01, addr_hi, addr_lo, data, 0xAB, 0xCD};
    scanner.write(cmd, 9);
}

// After the wake-time write_reg(0x57) that re-enables scanning, the GM65 answers with an
// ACK frame a few ms later. It MUST be consumed before we switch to IDLE — once IDLE is
// up, loop()'s screensaver/splash guard no longer applies and those bytes get parsed as
// a bogus badge ("badge not recognized"). A blocking delay() here does not help: it never
// reads the port, it just lets the reply accumulate for IDLE to trip over, and the
// turnaround sometimes outruns a short fixed wait anyway. So actively drain instead —
// read and discard until the module has gone quiet for ACK_QUIET_MS (reply fully
// received), or ACK_MAX_MS elapses regardless (a genuinely missing ACK must not freeze
// the wake). Normal path exits in well under 100ms; the ceiling is a pure fault-guard.
#define ACK_QUIET_MS    40
#define ACK_MAX_MS    2000   // you asked for ~10s; 2s is plenty as a fault-guard since the
                             // quiet-detect is what ends it normally — bump if you disagree
static void drain_gm65_ack() {
    uint32_t start  = millis();
    uint32_t last_rx = 0;
    bool     saw_any = false;

    while (millis() - start < ACK_MAX_MS) {
        if (scanner.available()) {
            while (scanner.available()) scanner.read();
            last_rx = millis();
            saw_any = true;
        } else if (saw_any && (millis() - last_rx >= ACK_QUIET_MS)) {
            break;  // reply received and the bus has been idle since — done
        }
        delay(1);   // yields to the RTOS / feeds the watchdog
    }
}

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

static void bubble_hide(Bubble &b) {
    lv_obj_add_flag(b.circle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(b.pop_label, LV_OBJ_FLAG_HIDDEN);
    b.active  = false;
    b.popping = false;
}

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
    backlight_set(100);
    header_set_visible(true);

    // Restore full scanning, then consume the module's ACK reply here — while THIS screen
    // is still the active one — so it can't leak into on_scan() as a bogus "badge not
    // recognized" the instant we switch to IDLE. See drain_gm65_ack() for why a plain
    // delay() wasn't enough. The final sweep catches anything that slipped in during the
    // few microseconds between the drain returning and the screen actually swapping.
    write_reg(0x00, 0x00, 0x57);
    drain_gm65_ack();
    while (scanner.available()) scanner.read();

    screen_idle_load();
}

bool screen_screensaver_is_active() {
    return _scr && lv_scr_act() == _scr;
}

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
        lv_obj_set_style_text_color(_press_key_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
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
    backlight_set(30);
    paint_frame();

    ButtonHandlers h;
    h.up    = cb_wake;
    h.down  = cb_wake;
    h.left  = cb_wake;
    h.right = cb_wake;
    h.enter = cb_wake;
    h.back  = cb_wake;
    buttons_set_handlers(h);

    lv_scr_load(_scr);

    // Sent after lv_scr_load() on purpose — screen_screensaver_is_active() is already
    // true by this point, so the ACK reply (arriving several ms later, over UART) gets
    // safely drained by main.cpp's loop() guard instead of reaching on_scan().
    write_reg(0x00, 0x00, 0x54);

    if (_timer) lv_timer_del(_timer);
    _timer = lv_timer_create(tick_cb, 1000, nullptr);
}
