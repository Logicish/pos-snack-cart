#include "idle_timer.h"
#include "screens.h"
#include "db.h"
#include <Arduino.h>
#include <stdlib.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the idle-to-screensaver countdown declared in idle_timer.h --
            a simple armed/disarmed deadline checked once per loop().
  Notes---- Was a hardcoded 30s (#define IDLE_TIMEOUT_MS 30000) until 2026-08-26, when
            it became a configurable 1-10 minutes, adjustable from Settings and
            persisted to the config table so it survives a reboot.
*/

#define IDLE_TIMEOUT_DEFAULT_MIN 3
#define IDLE_TIMEOUT_MIN_MIN     1
#define IDLE_TIMEOUT_MAX_MIN     10
#define CONFIG_KEY "idle_timeout_min"

static bool     _armed;
static uint32_t _deadlineMs;
static int      _timeoutMin = IDLE_TIMEOUT_DEFAULT_MIN;

// Converts the current timeout (minutes) to milliseconds for the deadline math below.
static uint32_t timeout_ms() { return (uint32_t)_timeoutMin * 60000UL; }

// Pushes the deadline back out to a fresh full timeout. No-op while disarmed.
void idle_timer_reset() {
    if (_armed) _deadlineMs = millis() + timeout_ms();
}

// Arms the timer and starts a fresh countdown.
void idle_timer_arm() {
    _armed      = true;
    _deadlineMs = millis() + timeout_ms();
}

// Disarms the timer -- it can't fire again until something re-arms it.
void idle_timer_disarm() {
    _armed = false;
}

// Checked every loop() iteration -- fires the screensaver once the deadline passes.
void idle_timer_check() {
    if (!_armed) return;
    if ((int32_t)(millis() - _deadlineMs) >= 0) {
        _armed = false;  // screen_screensaver_push() disarms us too via buttons_set_handlers(), but be explicit
        screen_screensaver_push();
    }
}

// Loads the saved timeout from config at boot; falls back to the built-in default if
// nothing's saved yet or the saved value is out of range.
void idle_timer_load_from_config() {
    char buf[8];
    if (db_config_get(CONFIG_KEY, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= IDLE_TIMEOUT_MIN_MIN && v <= IDLE_TIMEOUT_MAX_MIN) _timeoutMin = v;
    }
}

// Returns the current timeout in minutes.
int idle_timer_get_minutes() {
    return _timeoutMin;
}

// Sets a new timeout (wrapping 1-10) and persists it to config.
void idle_timer_set_minutes(int minutes) {
    if (minutes > IDLE_TIMEOUT_MAX_MIN) minutes = IDLE_TIMEOUT_MIN_MIN;  // wrap both ends,
    if (minutes < IDLE_TIMEOUT_MIN_MIN) minutes = IDLE_TIMEOUT_MAX_MIN;  // matches the cycling-row UX
    _timeoutMin = minutes;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", _timeoutMin);
    db_config_set(CONFIG_KEY, buf);
}
