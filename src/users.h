#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The enrolled-people table: badge lookup, creation/editing, the admin
            flag, and the "who scanned in on-device" tracking the web portal's login
            uses to auto-identify the current admin.
*/

#define BADGE_ID_LEN  32
#define NAME_FIELD_LEN 16

struct User {
    int  id;
    char badge_id[BADGE_ID_LEN];
    char first_name[NAME_FIELD_LEN];
    char last_name[NAME_FIELD_LEN];
    bool admin;   // not used to gate anything yet — no web login exists to check it against
    bool active;  // false = badge scans are turned away instead of starting a transaction
};

// Pointer is to a static internal buffer — valid until the next users_find_by_badge() call.
const User *users_find_by_badge(const char *badge_id);  // nullptr if not found
// Self-enrollment via badge scan never sets admin — only Add User and the setup wizard
// (screen_setup_wizard.cpp) create admin rows. Always creates as active — there's no
// scenario yet where a user should start out booted.
int users_create(const char *badge_id, const char *first_name, const char *last_name, bool admin = false);  // returns new id, -1 on failure
int users_count();  // total row count in the users table
bool users_set_active(int id, bool active);  // for the web Users page's active toggle

const User *users_get_by_id(int id);  // nullptr if not found

// On-device Edit Users (2026-08-28) — the badge/enrollment fields are otherwise
// immutable after users_create(). Both return false if the DB isn't available or the
// update failed.
bool users_set_name(int id, const char *first_name, const char *last_name);
// This DOES gate real access: screen_admin_login.cpp rejects a scan where u->admin is
// false ("That's not an admin badge."). The web Users page still shows it read-only; this
// is the first place that lets an admin toggle the flag directly.
bool users_set_admin(int id, bool admin);

// UI display cap for screen_edit_users.cpp's row pool, not a DB storage limit — same
// convention as items.h's MAX_ITEMS.
#define MAX_USERS_LISTED 128
// Fills out[] with up to `max` users, sorted first-then-last case-insensitively — same
// convention as every other people list in this codebase (see project memory
// feedback-lists-alphabetical). Returns how many were written.
int users_get_all(User *out, int max);

// True if at least one admin row exists — the invariant that keeps Admin Login (and
// everything behind it, including the normal Add User flow) reachable at all. main.cpp's
// setup() checks this at boot and runs screen_setup_wizard_push() instead of the normal
// splash -> IDLE flow when it's false, since there would otherwise be no way to ever reach
// Admin Login again (a blank/fresh SD card, or an existing DB that's lost every admin row).
bool users_has_admin();

// Admin web-portal login: per-admin hashed passwords (2026-08-25), then one shared
// plaintext password (2026-09-14), then removed entirely (2026-09-29, owner's call). The
// portal is gated by the cart's Admin Login plus the WiFi password.

// Tracks which admin most recently scanned in on-device this boot, so the web login can
// auto-populate identity instead of asking for a username (see snack_cart_pos.md's
// 2026-08-25 planning round for the reasoning — the physical badge scan already proves
// identity before the web portal is even reachable). -1 = no admin session yet this boot.
void users_set_current_admin(int user_id);  // called when a known admin badge routes into the Admin Menu
int  users_get_current_admin();             // -1 if no admin has scanned in yet this boot

// Self-service auto-enrollment toggle (2026-09-29, owner request) -- when on, an unknown
// badge scanned at IDLE opens the enroll name wheel instead of the "see an admin" screen.
// Persisted in config as "auto_enroll" ("1"/"0"), default OFF so a fresh card behaves
// exactly like the admin-only enrollment it had before. Toggled from Admin -> Users.
bool users_auto_enroll_enabled();
void users_set_auto_enroll(bool enabled);
