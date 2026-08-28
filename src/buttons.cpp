#include "buttons.h"
#include "idle_timer.h"
#include <Arduino.h>

// GPIO assignments — confirmed against board silk 2026-08-16, see snack_cart_pos.md pin table.
#define PIN_UP    1
#define PIN_DOWN  2
#define PIN_LEFT  42
#define PIN_RIGHT 41
#define PIN_ENTER 40
#define PIN_BACK  39

#define DEBOUNCE_MS         20
// 2026-08-26 — split into two delays, was one global REPEAT_DELAY_MS. Up/Down/Left/Right
// are pure navigation/data-entry (letter wheel, list scrolling, qty adjust) — worst case
// on an over-fast repeat there is an extra cursor step, trivially correctable, so these
// can start repeating almost immediately. Enter/Back trigger real actions (payment
// confirm, logout, destructive confirms) and MUST keep a comfortable margin above the
// measured real press duration on this hardware (~780-1080ms) — dropping this one would
// risk a normal tap double-firing a confirm/logout.
#define REPEAT_DELAY_NAV_MS    1400  // 150ms was too aggressive in practice — a normal tap on
                                      // this hardware can run up to the measured ~1080ms ceiling
                                      // above, so anything much below that reads a real single
                                      // click as a hold and fires an extra repeat
#define REPEAT_DELAY_ACTION_MS 2000
#define REPEAT_INTERVAL_MS       60  // 2026-08-26: 80->50->30 chasing letter-wheel speed, but
                                      // that screen was doing ~100 redundant LVGL calls per
                                      // keypress (see screen_enroll.cpp) which was actually
                                      // throttling the real rate below the 30ms nominal value.
                                      // Fixing that made 30ms feel too fast — 60 is the real
                                      // "2x slower than 30" now that rendering isn't the bottleneck.

enum ButtonId { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_ENTER, BTN_BACK, BTN_COUNT };

struct ButtonState {
    uint8_t  pin;
    bool     repeats;       // all 6 auto-repeat as of 2026-08-25 (was Up/Down only)
    uint32_t repeatDelayMs; // per-button initial hold-before-repeat delay, see above
    bool     stableRaw;     // debounced raw level, true = pressed
    uint32_t lastChangeMs;
    bool     pressed;       // committed debounced state
    uint32_t pressedAtMs;
    bool     repeating;
    uint32_t nextRepeatMs;
};

static ButtonState _btn[BTN_COUNT] = {
    { PIN_UP,    true, REPEAT_DELAY_NAV_MS,    false, 0, false, 0, false, 0 },
    { PIN_DOWN,  true, REPEAT_DELAY_NAV_MS,    false, 0, false, 0, false, 0 },
    { PIN_LEFT,  true, REPEAT_DELAY_NAV_MS,    false, 0, false, 0, false, 0 },
    { PIN_RIGHT, true, REPEAT_DELAY_NAV_MS,    false, 0, false, 0, false, 0 },
    { PIN_ENTER, true, REPEAT_DELAY_ACTION_MS, false, 0, false, 0, false, 0 },
    { PIN_BACK,  true, REPEAT_DELAY_ACTION_MS, false, 0, false, 0, false, 0 },
};

static ButtonHandlers _handlers;

void buttons_init() {
    for (auto &b : _btn) pinMode(b.pin, INPUT_PULLUP);
}

void buttons_set_handlers(const ButtonHandlers &h) {
    _handlers = h;
    // Every screen transition goes through here, so this is where the idle timer defaults
    // back off — only screen_idle_load()/screen_browse_push() explicitly re-arm afterward.
    idle_timer_disarm();
}

static ButtonCb handler_for(int id) {
    switch (id) {
        case BTN_UP:    return _handlers.up;
        case BTN_DOWN:  return _handlers.down;
        case BTN_LEFT:  return _handlers.left;
        case BTN_RIGHT: return _handlers.right;
        case BTN_ENTER: return _handlers.enter;
        case BTN_BACK:  return _handlers.back;
        default:        return nullptr;
    }
}

void buttons_poll() {
    uint32_t now = millis();

    for (int i = 0; i < BTN_COUNT; i++) {
        ButtonState &b = _btn[i];
        bool raw = (digitalRead(b.pin) == LOW);  // active-low, internal pullup

        if (raw != b.stableRaw) {
            b.stableRaw    = raw;
            b.lastChangeMs = now;
        }

        if (now - b.lastChangeMs >= DEBOUNCE_MS && b.stableRaw != b.pressed) {
            b.pressed = b.stableRaw;
            if (b.pressed) {
                b.pressedAtMs = now;
                b.repeating   = false;
                idle_timer_reset();
                ButtonCb cb = handler_for(i);
                if (cb) cb();
            }
        }

        if (b.pressed && b.repeats) {
            if (!b.repeating && now - b.pressedAtMs >= b.repeatDelayMs) {
                b.repeating   = true;
                b.nextRepeatMs = now;
            }
            if (b.repeating && now >= b.nextRepeatMs) {
                b.nextRepeatMs = now + REPEAT_INTERVAL_MS;
                idle_timer_reset();
                ButtonCb cb = handler_for(i);
                if (cb) cb();
            }
        }
    }
}
