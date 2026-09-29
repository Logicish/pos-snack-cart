#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Extras" -- Start screen's Left arrow. A short list of less-common, non-POS
            entries (Check Balance, Laggy Fish). Gated behind a badge scan, 2026-09-15 --
            not for access control (this device has none beyond physical badge
            possession anywhere else either) but so every entry behind it already knows
            who's using it: Check Balance needs to know whose balance to show, and a
            future Laggy Fish high-score feature will want the same identity.
*/

// Start screen -> Extras: scans a badge first (ST_SCAN), then a plain cursor-highlighted
// list (ST_LIST) once identified. Always starts over at ST_SCAN -- entering fresh from
// IDLE never trusts a stale identity from a previous visit. Back from ST_SCAN returns to
// IDLE, not a submenu parent, since Extras is reached directly from the Start screen.
void screen_extras_push();

// Re-shows the already-identified ST_LIST directly, skipping the scan -- the Back target
// for Extras' own sub-screens (Check Balance, Laggy Fish), which should return to the
// list without forcing a re-scan mid-session.
void screen_extras_return_to_list();

// Consumes a scan while ST_SCAN is active -- looks up the badge and either proceeds into
// ST_LIST (known, active user) or turns it away via screen_blocked_push() (unknown or
// locked), same as every other badge-scan gate in this codebase.
bool screen_extras_on_scan(const char *badge_id);

// The currently-identified Extras user's id, valid from a successful scan until Extras is
// exited back to IDLE. -1 if nothing has been scanned in yet this Extras session -- Check
// Balance (and later, Laggy Fish's high score) should treat that as "nothing to show"
// rather than assume it's always valid.
int screen_extras_current_user_id();
