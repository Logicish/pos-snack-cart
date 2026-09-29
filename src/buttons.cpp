#include "buttons.h"
#include "idle_timer.h"
#include "session_timer.h"
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the 6-button driver declared in buttons.h -- per-pin debounce,
            press dispatch, and auto-repeat, plus arming/disarming the idle and
            session timers on every handler swap. Also owns the centralized GM65
            scanner on/off toggle, 2026-09-15 -- one place that decides whether the
            module should be actively sensing, driven by each screen's
            ButtonHandlers::wantsScanner instead of every screen managing the scanner
            itself. Was leaving it sensing (light + auto-buzz-ready) through every menu
            and the Laggy Fish game, which the owner flagged as just plain annoying --
            most screens genuinely never need it.
  Notes---- GPIO assignments confirmed against board silk 2026-08-16, see
            snack_cart_pos.md's pin table. Up/Down/Left/Right remapped 2026-09-09 to
            match how the D-pad was actually soldered (physical Right/Left ended up on
            the Up/Down GPIOs and vice versa) -- swap Up<->Right and Down<->Left from
            the original assignment rather than reflowing solder joints.
*/

extern HardwareSerial scanner;  // owned by main.cpp; only written to for the mode toggle below

// Same 9-byte register-write command frame used by every other GM65 command site in
// this codebase (main.cpp, screen_screensaver.cpp historically, screen_gm65_test.cpp) --
// duplicated here rather than shared, per this codebase's established per-file
// self-contained convention for GM65 command helpers (see main.cpp's gm65_write_reg()).
static void gm65_write_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t data) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x08, 0x01, addr_hi, addr_lo, data, 0xAB, 0xCD};
    scanner.write(cmd, 9);
}

// A register write like the one above gets an ACK reply from the module a few ms
// later, over the same UART -- it MUST be consumed here, synchronously, before this
// function returns, rather than left for main.cpp's loop() to stumble over on some
// later iteration once a totally different (and possibly scan-processing) screen is
// active. A blocking delay() doesn't help, since it never actually reads the port; this
// actively drains until the module's gone quiet for SCANNER_ACK_QUIET_MS, or
// SCANNER_ACK_MAX_MS elapses regardless as a fault-guard for a genuinely missing ACK.
// Ported from screen_screensaver.cpp's original drain_gm65_ack(), which this replaces.
#define SCANNER_ACK_QUIET_MS   40
#define SCANNER_ACK_MAX_MS   2000
static void drain_scanner_ack() {
    uint32_t start   = millis();
    uint32_t last_rx = 0;
    bool     saw_any = false;

    while (millis() - start < SCANNER_ACK_MAX_MS) {
        if (scanner.available()) {
            while (scanner.available()) scanner.read();
            last_rx = millis();
            saw_any = true;
        } else if (saw_any && (millis() - last_rx >= SCANNER_ACK_QUIET_MS)) {
            break;  // reply received and the bus has been idle since -- done
        }
        delay(1);   // yields to the RTOS / feeds the watchdog
    }
}

// Tracks the scanner's actual last-commanded hardware state so repeated screens that
// want the same thing (on or off) never re-send the command. _scannerReady stays false
// until buttons_scanner_ready() runs -- several screens (splash, the setup wizard, the
// SD/DB error screen) get their first buttons_set_handlers() call during setup(),
// before scanner.begin() has even run, so there's nothing safe to write yet.
static bool _scannerReady       = false;
static bool _scannerActive      = true;  // matches the always-on state before this system existed
static bool _scannerWantedLast  = true;

// Sends the Induction(0x57)/Manual(0x54) mode command and blocks until its ACK is fully
// drained -- see project memory, project_gm65_settings_lockin, for why this specific
// register write is the one already proven (on real hardware) to control autonomous
// sensing. No-ops if this already matches the last commanded state.
static void apply_scanner_state(bool active) {
    if (active == _scannerActive) return;
    gm65_write_reg(0x00, 0x00, active ? 0x57 : 0x54);
    drain_scanner_ack();
    while (scanner.available()) scanner.read();  // catch anything that slipped in right after
    _scannerActive = active;
}

#define PIN_UP    41
#define PIN_DOWN  42
#define PIN_LEFT  2
#define PIN_RIGHT 1
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

// Sets all 6 pins to input with internal pull-up (active-low buttons).
void buttons_init() {
    for (auto &b : _btn) pinMode(b.pin, INPUT_PULLUP);
}

// Swaps in a new set of button callbacks for whatever screen is now active, and resets
// the idle/session timers to their per-screen defaults (see idle_timer.h/session_timer.h).
void buttons_set_handlers(const ButtonHandlers &h) {
    _handlers = h;
    // Every screen transition goes through here, so this is where the idle timer defaults
    // back off — only screen_idle_load()/screen_browse_push() explicitly re-arm afterward.
    idle_timer_disarm();
    // Session (auto-logout) timer defaults the OPPOSITE way, 2026-09-14 — most screens
    // represent an active session that should time out if walked away from, so this arms
    // it here; the handful of screens with nothing to log out of (IDLE, Browse, the
    // screensaver, splash) explicitly disarm it right after their own buttons_set_handlers()
    // call. See session_timer.h for the full reasoning.
    session_timer_arm();

    // 2026-09-15 — centralized scanner on/off, driven by whatever this screen just
    // declared. _scannerReady stays false for the handful of buttons_set_handlers()
    // calls that happen during setup() before scanner.begin() has run (see comment at
    // _scannerReady above) -- buttons_scanner_ready() reconciles once it's safe to.
    _scannerWantedLast = h.wantsScanner;
    if (_scannerReady) apply_scanner_state(h.wantsScanner);
}

// Called once from setup(), right after scanner.begin() and the boot-time forced GM65
// config write (which leaves the module in Induction/sensing mode -- see main.cpp).
// Reconciles that known hardware state against whatever screen ended up being current
// during setup()'s earlier buttons_set_handlers() calls, which couldn't touch the
// scanner at all yet.
void buttons_scanner_ready() {
    _scannerReady  = true;
    _scannerActive = true;
    apply_scanner_state(_scannerWantedLast);
}

// Looks up which callback (if any) is registered for a given button id.
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

// Called every loop() iteration -- debounces all 6 pins, dispatches the initial press
// and any auto-repeats, and resets the idle/session timers on each dispatch.
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
                session_timer_reset();
                ButtonCb cb = handler_for(i);
                if (cb) cb();
                // 2026-09-29 — restart the hold clock AFTER the callback. A slow screen
                // build (e.g. Transaction's Item Lookup list) used to count toward the
                // repeat delay, so a normal tap fired a phantom repeat into the NEW
                // screen's handler (Right -> open Lookup -> Page Down).
                // `now` must move forward with it -- leaving it stale made
                // `now - pressedAtMs` underflow (uint32) and fire a repeat instantly.
                now = millis();
                b.pressedAtMs = now;
            }
        }

        // stableRaw, not just pressed: `pressed` lags a release by DEBOUNCE_MS, which let
        // a repeat fire after the finger was already off the button.
        if (b.pressed && b.stableRaw && b.repeats) {
            if (!b.repeating && now - b.pressedAtMs >= b.repeatDelayMs) {
                b.repeating   = true;
                b.nextRepeatMs = now;
            }
            if (b.repeating && now >= b.nextRepeatMs) {
                b.nextRepeatMs = now + REPEAT_INTERVAL_MS;
                idle_timer_reset();
                session_timer_reset();
                ButtonCb cb = handler_for(i);
                if (cb) cb();
            }
        }
    }
}
