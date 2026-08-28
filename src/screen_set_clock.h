#pragma once

// "Set Clock" — Settings submenu, 2026-08-28. Manually sets the ESP32's system clock via
// settimeofday() so time(nullptr) (already used everywhere real timestamps matter —
// checkouts.cpp, webserver.cpp, screen_balances.cpp) reads real wall-clock time instead
// of a small since-boot counter. Interim stopgap until the DS3231 RTC is wired (see
// snack_cart_pos.md's RTC section) — without battery backup, whatever's set here is lost
// on the next power cycle. Back cancels without setting.
void screen_set_clock_push();
