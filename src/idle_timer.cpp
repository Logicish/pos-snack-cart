#include "idle_timer.h"
#include "screens.h"
#include "db.h"
#include <Arduino.h>
#include <stdlib.h>

// 2026-08-26 — was a hardcoded 30s (#define IDLE_TIMEOUT_MS 30000). Now a configurable
// 1-10 minutes, adjustable from Admin Menu -> Advanced Tools and persisted to the config
// table so it survives a reboot.
#define IDLE_TIMEOUT_DEFAULT_MIN 3
#define IDLE_TIMEOUT_MIN_MIN     1
#define IDLE_TIMEOUT_MAX_MIN     10
#define CONFIG_KEY "idle_timeout_min"

static bool     _armed;
static uint32_t _deadlineMs;
static int      _timeoutMin = IDLE_TIMEOUT_DEFAULT_MIN;

static uint32_t timeout_ms() { return (uint32_t)_timeoutMin * 60000UL; }

void idle_timer_reset() {
    if (_armed) _deadlineMs = millis() + timeout_ms();
}

void idle_timer_arm() {
    _armed      = true;
    _deadlineMs = millis() + timeout_ms();
}

void idle_timer_disarm() {
    _armed = false;
}

void idle_timer_check() {
    if (!_armed) return;
    if ((int32_t)(millis() - _deadlineMs) >= 0) {
        _armed = false;  // screen_screensaver_push() disarms us too via buttons_set_handlers(), but be explicit
        screen_screensaver_push();
    }
}

void idle_timer_load_from_config() {
    char buf[8];
    if (db_config_get(CONFIG_KEY, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= IDLE_TIMEOUT_MIN_MIN && v <= IDLE_TIMEOUT_MAX_MIN) _timeoutMin = v;
    }
}

int idle_timer_get_minutes() {
    return _timeoutMin;
}

void idle_timer_set_minutes(int minutes) {
    if (minutes > IDLE_TIMEOUT_MAX_MIN) minutes = IDLE_TIMEOUT_MIN_MIN;  // wrap both ends,
    if (minutes < IDLE_TIMEOUT_MIN_MIN) minutes = IDLE_TIMEOUT_MAX_MIN;  // matches the cycling-row UX
    _timeoutMin = minutes;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", _timeoutMin);
    db_config_set(CONFIG_KEY, buf);
}
