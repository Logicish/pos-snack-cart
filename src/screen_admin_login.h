#pragma once

// "Admin Login" — Start screen's Green/Enter button, 2026-08-26. Makes reaching the
// Admin Menu a deliberate two-step action (press this, then scan) instead of an automatic
// side effect of a badge happening to be an admin's. Side effect worth knowing: a PLAIN
// badge scan (this screen not armed) no longer routes admins to the Admin Menu at all —
// see main.cpp's on_scan(), which now sends any known active user, admin or not, straight
// to their own transaction. Admin Menu is reachable only through here.
void screen_admin_login_push();
bool screen_admin_login_on_scan(const char *badge_id);
