#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Boot-time blocking error screen -- shown instead of the normal boot flow
            when the DB never came up usable, even after a backup attempt. Enter
            retries in place.
*/

// Boot-time blocking error screen, 2026-09-14 — shown instead of the normal
// splash -> IDLE flow when main.cpp's boot_try_init_db() couldn't get a usable database,
// even after trying the backup. This device does nothing useful without it (no items, no
// users, no checkouts), so rather than let a regular user wander into a Start screen that
// silently can't do anything, this is the terminal landing point until it's fixed or the
// device is power-cycled. Enter re-runs the same boot healing sequence in place — if it
// succeeds (card reseated, a different card inserted, whatever), falls through into the
// normal splash -> IDLE flow immediately, no reboot needed.
void screen_sd_error_push();

// Same guard shape as screen_splash_is_active()/screen_screensaver_is_active() -- checked
// in main.cpp's loop() so the boot-time GM65 config writes' ACK replies (setup(), which run
// unconditionally regardless of whether the DB is OK) don't get misrouted here as a bogus
// badge scan. This screen skips splash entirely when the DB is broken, so without this it
// had none of splash's existing protection for that exact same timing window.
bool screen_sd_error_is_active();
