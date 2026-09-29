#include "backlight.h"
#include "db.h"
#include <Arduino.h>
#include <stdlib.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the PWM backlight driver + persisted brightness declared in
            backlight.h.
  Notes---- See backlight.h for the full reasoning on the boot-order quirk.
*/

#define BL_PIN      18
#define BL_CHANNEL  0
#define BL_FREQ_HZ  5000
#define BL_RES_BITS 8

#define NORMAL_DEFAULT_PCT 100
#define NORMAL_MIN_PCT      10
#define NORMAL_MAX_PCT     100
#define NORMAL_CONFIG_KEY  "backlight_normal_pct"

static int _normalPct = NORMAL_DEFAULT_PCT;

// Sets up the PWM channel and lights the screen at the built-in default -- called
// before the DB is mounted, so it can't read a saved brightness yet.
void backlight_init() {
    ledcSetup(BL_CHANNEL, BL_FREQ_HZ, BL_RES_BITS);
    ledcAttachPin(BL_PIN, BL_CHANNEL);
    backlight_set(NORMAL_DEFAULT_PCT);  // DB isn't mounted yet -- see backlight_normal_load_from_config()
}

// Drives the backlight PWM duty cycle directly. No persistence -- the screensaver's
// dim level calls this directly rather than going through the "normal" get/set pair.
void backlight_set(uint8_t percent) {
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledcWrite(BL_CHANNEL, duty);
}

// Loads the saved normal brightness from config and applies it immediately.
void backlight_normal_load_from_config() {
    char buf[8];
    if (db_config_get(NORMAL_CONFIG_KEY, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= NORMAL_MIN_PCT && v <= NORMAL_MAX_PCT) _normalPct = v;
    }
    backlight_set((uint8_t)_normalPct);  // apply immediately, in case it differs from the boot default
}

// Returns the current normal-brightness percentage.
int backlight_normal_get_pct() {
    return _normalPct;
}

// Sets a new normal brightness (wrapping 10-100 by tens), applies it live, and persists it.
void backlight_normal_set_pct(int pct) {
    if (pct > NORMAL_MAX_PCT) pct = NORMAL_MIN_PCT;  // wrap both ends, matches the cycling-row UX
    if (pct < NORMAL_MIN_PCT) pct = NORMAL_MAX_PCT;
    _normalPct = pct;
    backlight_set((uint8_t)_normalPct);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", _normalPct);
    db_config_set(NORMAL_CONFIG_KEY, buf);
}
