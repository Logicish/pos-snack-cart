#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "DS3231 Test" -- chip-found/lost-power status, its own clock reading vs.
            system time, and manual Sync To/From RTC actions.
*/

// "DS3231 Test" — sub-menu under Advanced Tools, 2026-08-28. Shows whether the chip was
// found on I2C, whether it reports lost power, its own clock reading (direct from the
// chip, independent of whatever's already synced into the system clock), and the current
// system time for comparison. Two live actions: pull the chip's time into the system
// clock (same thing rtc_init() does at boot), or push the system clock's time into the
// chip. Back returns to Advanced Tools.
void screen_ds3231_test_push();
