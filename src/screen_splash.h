#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Boot splash screen -- shows briefly, then falls through to IDLE.
*/
#include <stdint.h>

void screen_splash_push();                    // shows it, IDLE after SPLASH_DURATION_MS
void screen_splash_hold();                    // boot: shows it with no timer at all
void screen_splash_release(uint32_t hold_ms); // boot is done -- IDLE hold_ms from now
// Same pattern as screen_screensaver_is_active() — used by main.cpp's loop() to ignore
// scanner UART traffic while splash is up, so the boot-time GM65 config writes' ACK
// replies don't get misrouted into on_scan() as a bogus scan.
bool screen_splash_is_active();
