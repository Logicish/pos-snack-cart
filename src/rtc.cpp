#include "rtc.h"
#include <RTClib.h>
#include <Wire.h>
#include <Arduino.h>
#include <sys/time.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the DS3231 RTC integration declared in rtc.h, via Adafruit's
            RTClib.
*/

// SDA=GPIO5/SCL=GPIO4 — reserved for the DS3231. Originally GPIO21/22 (reserved
// 2026-08-17), moved 2026-08-28 to SDA=5/SCL=6 (this specific physical board doesn't
// break out GPIO22, and GPIO48 — the next candidate — is committed to the board's
// onboard WS2812 RGB LED), then shifted again 2026-09-13 to SDA=5/SCL=4 — see
// snack_cart_pos.md's pin table for the full "verified safe pool" this was picked from.
// Passed explicitly to Wire.begin() rather than relying on the board's default I2C pins,
// which may not match.
#define RTC_SDA_PIN 5
#define RTC_SCL_PIN 4

static RTC_DS3231 _rtc;
static bool        _available = false;

// Converts an RTClib DateTime into the ESP32's system clock via settimeofday().
static void set_system_time_from(const DateTime &dt) {
    struct tm tmval = {};
    tmval.tm_year = dt.year() - 1900;
    tmval.tm_mon  = dt.month() - 1;
    tmval.tm_mday = dt.day();
    tmval.tm_hour = dt.hour();
    tmval.tm_min  = dt.minute();
    tmval.tm_sec  = dt.second();

    // No timezone handling anywhere in this codebase (see webserver.cpp's format_epoch,
    // checkouts.cpp, screen_set_clock.cpp) — mktime() reads the struct as local time under
    // the C library's TZ setting, UTC by default on this build since no TZ env var is ever
    // set, so this stores exactly what the DS3231 itself reports, same convention as
    // everywhere else.
    time_t t = mktime(&tmval);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
}

// Starts I2C, probes for the DS3231, and syncs the system clock from it if found and trustworthy.
void rtc_init() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    _available = _rtc.begin();
    if (!_available) {
        Serial.println("[RTC] DS3231 not found on I2C -- Set Clock stays the only time source this boot");
        return;
    }

    if (_rtc.lostPower()) {
        // Real power-loss (dead/missing battery) or a brand-new, never-set chip -- its
        // stored time is garbage (typically 2000-01-01) either way. Deliberately NOT
        // syncing from it in this case, same as checkouts.cpp's "0 = never set" convention
        // for a placeholder time -- leave the system clock alone (it'll read near-zero, the
        // existing "--" display convention on Balances/etc. already handles that) rather
        // than pushing a misleadingly precise-looking wrong date onto every timestamp.
        Serial.println("[RTC] DS3231 reports lost power -- not trusting its stored time. Use Set Clock, which will also correct the chip.");
        return;
    }

    DateTime now = _rtc.now();
    set_system_time_from(now);
    Serial.printf("[RTC] Synced system clock from DS3231: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
}

// True if the DS3231 responded on I2C at boot.
bool rtc_available() { return _available; }

// True if the chip reports losing backup power since it was last set.
bool rtc_lost_power() {
    return _available && _rtc.lostPower();
}

// Writes the current system time into the DS3231, so a manual correction survives a power cycle.
void rtc_sync_from_system() {
    if (!_available) return;

    time_t t = time(nullptr);
    struct tm tmval;
    gmtime_r(&t, &tmval);
    _rtc.adjust(DateTime(tmval.tm_year + 1900, tmval.tm_mon + 1, tmval.tm_mday,
                          tmval.tm_hour, tmval.tm_min, tmval.tm_sec));
    Serial.println("[RTC] Wrote current system time to DS3231");
}

// Reads the DS3231's own clock directly, bypassing the system clock.
bool rtc_read(struct tm *out) {
    if (!_available) return false;

    DateTime now = _rtc.now();
    out->tm_year = now.year() - 1900;
    out->tm_mon  = now.month() - 1;
    out->tm_mday = now.day();
    out->tm_hour = now.hour();
    out->tm_min  = now.minute();
    out->tm_sec  = now.second();
    return true;
}
