#pragma once

// Admin Menu -> Add User: scan the new person's badge. Unknown badge -> drops straight into
// the existing screen_enroll.cpp wheel flow (unchanged) to collect their name and confirm.
// Already-enrolled badge -> a message, stays put so the admin can scan a different one.
// This is the replacement for self-service enrollment (owner's explicit call: no guest
// checkout, all users manually enrolled by an admin) -- screen_enroll_push() itself wasn't
// touched, just how it's reached.
void screen_add_user_push();
bool screen_add_user_on_scan(const char *badge_id);
