#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The idle-timeout screensaver and its persisted backlight dim level.
*/

// Screensaver: dancing fish (fixed 16-beat left/right flip pattern) that also random-
// walks within an invisible box, bubble particle stream from its mouth, bobbing
// "LogicishDesigns.com" wordmark + "press any key" prompt, backlight dim.
void screen_screensaver_push();

// True while the screensaver is the active screen — used to ignore scanner input while
// it's up (idle-timeout only arms from IDLE/Browse, so waking is deliberately button-only,
// not scan-triggered; see snack_cart_pos.md's 2026-08-25 planning round for the reasoning).
bool screen_screensaver_is_active();

// Screensaver backlight dim level, 2026-09-14 — same load/get/set/persist pattern as
// idle_timer.h's timeout (config table, survives a reboot). Call
// screensaver_dim_load_from_config() once at boot, before the screensaver can first fire.
void screensaver_dim_load_from_config();
int  screensaver_dim_get_pct();
void screensaver_dim_set_pct(int pct);  // clamped to [10,100], snapped to the nearest 10
