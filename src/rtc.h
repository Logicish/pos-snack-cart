#pragma once
#include <time.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- DS3231 real-time-clock integration -- syncs the ESP32 system clock from
            the chip at boot and lets a manual time correction (screen_set_clock.cpp)
            or the DS3231 Test screen write back to it.
  Notes---- See snack_cart_pos.md's RTC section for the hardware side (I2C SDA=GPIO5,
            SCL=GPIO4, no level shifter, VCC=3.3V). Deliberately no NTP half of the
            original 2026-08-17 plan: this device is AP-only (broadcasts its own WiFi
            network, never joins an upstream one — see webserver.cpp), so there's no
            network path to NTP at all. The DS3231 is the only source of continuity
            across power loss; screen_set_clock.cpp's manual entry is the only way to
            correct it.
*/

// Call once from setup(), after Serial is up. Starts I2C on the reserved pins, looks for
// the DS3231, and — if found and it reports real (non-power-lost) time — syncs the ESP32
// system clock from it via settimeofday(), the same call screen_set_clock.cpp uses. Safe
// to call even if the chip isn't wired yet: rtc_available() just comes back false and
// nothing else in the app changes behavior (time(nullptr) keeps counting from boot).
void rtc_init();

bool rtc_available();    // true if the DS3231 responded on I2C at boot
bool rtc_lost_power();   // true if it reports losing backup power since it was last set
                          // (its stored time isn't trustworthy) — only meaningful when
                          // rtc_available() is true; re-queries the chip live, not cached

// Writes the ESP32's current system time (time(nullptr)) into the DS3231. Call this after
// any manual time-set (see screen_set_clock.cpp's cb_set()) so the correction survives a
// power cycle, not just the current boot — with no NTP path, this is the only way the
// DS3231 ever gets updated after its first setting. No-op if !rtc_available().
void rtc_sync_from_system();

// Reads the DS3231's own clock directly (bypassing whatever rtc_init() already synced
// into the system clock) — for the Advanced Tools DS3231 Test screen, so it can show what
// the chip itself reports independent of time(nullptr). Returns false if unavailable.
bool rtc_read(struct tm *out);
