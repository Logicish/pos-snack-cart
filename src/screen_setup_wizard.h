#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- First-boot / DB-recovery setup wizard -- runs instead of the normal
            boot flow when the DB has no admin, walking through badge-scan +
            name-entry until at least one admin exists.
*/

// First-boot / DB-recovery setup wizard, 2026-09-14 — replaces the SD-card seed_users.csv
// bulk-import path (removed the same day; real people no longer come from a file on the
// card at all). Runs instead of the normal splash -> IDLE boot flow whenever the DB comes
// up usable but users_has_admin() is false -- the one state this device can't escape any
// other way, since Admin Login (and everything behind it, including the normal Add User
// flow) requires an existing admin badge to scan in the first place. Covers a genuinely
// blank/fresh SD card (the "fried card, swapped for a new one" scenario this was built
// for) and the rarer case of an existing DB that's lost every admin row some other way --
// either way the fix is the same: create at least one admin before anything else can
// happen.
//
// Reuses screen_enroll.cpp's badge-scan -> name-wheel -> confirm -> re-scan-confirm flow
// as its own self-contained copy (same convention as screen_admin_password.cpp mirroring
// screen_wifi_password.cpp) rather than parameterizing the shared original, since this one
// always forces admin=true and always loops back into itself instead of
// screen_add_user_push(). Loops after each successful add ("scan another, or Back to
// finish") -- Back is intentionally inert until at least one admin exists, so there's no
// way to wander out of this screen and re-hit the same deadlock it exists to escape.
// Finishing hands off to the normal screen_splash_push() boot flow.
void screen_setup_wizard_push();
bool screen_setup_wizard_on_scan(const char *badge_id);

// Same ACK-misrouting guard as screen_splash_is_active()/screen_sd_error_is_active() --
// checked in main.cpp's loop() so the boot-time GM65 config writes' ACK replies (setup(),
// which runs unconditionally after whichever screen loads at boot) don't get read as a
// bogus badge scan while this screen is up.
bool screen_setup_wizard_is_active();
