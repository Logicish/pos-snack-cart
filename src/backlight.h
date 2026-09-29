#pragma once
#include <stdint.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Drives the display's PWM backlight pin and tracks the "normal" (non-
            screensaver) operating brightness as a persisted percentage.
  Notes---- backlight_init() runs before the DB is mounted (the screen needs to light
            up before anything else, including a DB-mount failure message), so it
            always starts at the built-in 100% default -- call
            backlight_normal_load_from_config() once after db_init() to pick up and
            immediately apply a saved value. Same load/get/set/persist pattern as
            idle_timer.h's timeout. Added 2026-09-14.
*/

void backlight_init();              // call once in setup(), after tft.init()
void backlight_set(uint8_t percent); // sets the backlight directly, 0-100, no persistence

void backlight_normal_load_from_config();  // call once after db_init() -- loads and applies the saved brightness
int  backlight_normal_get_pct();           // returns the current normal-brightness percentage
void backlight_normal_set_pct(int pct);    // clamped to [10,100], snapped to the nearest 10, applies live + persists
