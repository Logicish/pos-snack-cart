#include "session_timer.h"
#include "screens.h"
#include "db.h"
#include <Arduino.h>
#include <stdlib.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the auto-logout countdown declared in session_timer.h -- an
            armed-by-default deadline checked once per loop(), mirroring
            idle_timer.cpp's structure with the arm/disarm default flipped.
  Notes---- See session_timer.h for the full reasoning on why this is armed by default
            instead of disarmed.
*/

#define SESSION_TIMEOUT_DEFAULT_MIN 5
#define SESSION_TIMEOUT_MIN_MIN     1
#define SESSION_TIMEOUT_MAX_MIN    10
#define CONFIG_KEY "session_timeout_min"

static bool     _armed;
static uint32_t _deadlineMs;
static int      _timeoutMin = SESSION_TIMEOUT_DEFAULT_MIN;

// Converts the current timeout (minutes) to milliseconds for the deadline math below.
static uint32_t timeout_ms() { return (uint32_t)_timeoutMin * 60000UL; }

// Pushes the deadline back out to a fresh full timeout. No-op while disarmed.
void session_timer_reset() {
    if (_armed) _deadlineMs = millis() + timeout_ms();
}

// Arms the timer and starts a fresh countdown.
void session_timer_arm() {
    _armed      = true;
    _deadlineMs = millis() + timeout_ms();
}

// Disarms the timer -- it can't fire again until something re-arms it.
void session_timer_disarm() {
    _armed = false;
}

// Checked every loop() iteration -- forces a full logout once the deadline passes.
void session_timer_check() {
    if (!_armed) return;
    if ((int32_t)(millis() - _deadlineMs) >= 0) {
        _armed = false;  // screen_idle_load() disarms us too via buttons_set_handlers(), but be explicit
        screen_idle_load();
    }
}

// Loads the saved timeout from config at boot; falls back to the built-in default if
// nothing's saved yet or the saved value is out of range.
void session_timer_load_from_config() {
    char buf[8];
    if (db_config_get(CONFIG_KEY, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= SESSION_TIMEOUT_MIN_MIN && v <= SESSION_TIMEOUT_MAX_MIN) _timeoutMin = v;
    }
}

// Returns the current timeout in minutes.
int session_timer_get_minutes() {
    return _timeoutMin;
}

// Sets a new timeout (wrapping 1-10) and persists it to config.
void session_timer_set_minutes(int minutes) {
    if (minutes > SESSION_TIMEOUT_MAX_MIN) minutes = SESSION_TIMEOUT_MIN_MIN;  // wrap both ends,
    if (minutes < SESSION_TIMEOUT_MIN_MIN) minutes = SESSION_TIMEOUT_MAX_MIN;  // matches the cycling-row UX
    _timeoutMin = minutes;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", _timeoutMin);
    db_config_set(CONFIG_KEY, buf);
}
