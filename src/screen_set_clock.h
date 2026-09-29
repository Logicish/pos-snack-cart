#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Set Clock" -- Settings submenu. Manually sets the ESP32's system clock
            via settimeofday() and, if the DS3231 is wired, writes the correction
            through to it too (see rtc_sync_from_system()) so it survives a power
            cycle. Back cancels without setting.
*/
void screen_set_clock_push();
