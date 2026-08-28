#pragma once

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
// Self-enrollment via badge scan never sets admin — only users_import_from_sd() below does.
// Always creates as active — there's no scenario yet where a user should start out booted.
int users_create(const char *badge_id, const char *first_name, const char *last_name, bool admin = false);  // returns new id, -1 on failure
int users_count();
bool users_set_active(int id, bool active);  // for the web Users page's active toggle

// Bulk-enrolls users from /seed_users.csv on the SD card (if present) — "badge,first,last,admin"
// per line, admin is 1/0. Real names/badge numbers belong on the SD card, not hardcoded in
// firmware source. Idempotent (already-enrolled badges are skipped), safe to call every boot —
// see users.cpp for the full format/behavior notes.
void users_import_from_sd();

const User *users_get_by_id(int id);  // nullptr if not found

// On-device Edit Users (2026-08-28) — the badge/enrollment fields are otherwise
// immutable after users_create(). Both return false if the DB isn't available or the
// update failed.
bool users_set_name(int id, const char *first_name, const char *last_name);
// This DOES gate real access: screen_admin_login.cpp rejects a scan where u->admin is
// false ("That's not an admin badge."). Promoting someone (admin: false -> true) also
// arms the shared default password + forced-first-login-change flow, same as
// users_create() does for a brand-new admin -- see users.cpp. This is the first place
// that lets an admin toggle the flag directly; the web Users page still shows it
// read-only.
bool users_set_admin(int id, bool admin);

// UI display cap for screen_edit_users.cpp's row pool, not a DB storage limit — same
// convention as items.h's MAX_ITEMS.
#define MAX_USERS_LISTED 128
// Fills out[] with up to `max` users, sorted first-then-last case-insensitively — same
// convention as every other people list in this codebase (see project memory
// feedback-lists-alphabetical). Returns how many were written.
int users_get_all(User *out, int max);

// Admin web-portal login (2026-08-25) — every admin row (users_create(..., admin=true))
// gets a shared, known default password automatically, forcing a change on first login
// rather than needing a separate manual setup step. Not real PII, deliberately discoverable
// — the point is the forced-change flow, not secrecy of the default itself.
bool users_verify_password(int user_id, const char *password);
bool users_is_password_default(int user_id);
bool users_set_password(int user_id, const char *new_password);  // clears the default flag
bool users_reset_password(int user_id);  // any admin can do this to any admin — back to the shared default, re-arms the forced-change flag

// Tracks which admin most recently scanned in on-device this boot, so the web login can
// auto-populate identity instead of asking for a username (see snack_cart_pos.md's
// 2026-08-25 planning round for the reasoning — the physical badge scan already proves
// identity before the web portal is even reachable). -1 = no admin session yet this boot.
void users_set_current_admin(int user_id);
int  users_get_current_admin();

// One-time migration: any admin row that predates the password columns (password_hash
// still NULL) gets the shared default password set, same as a brand-new admin would via
// users_create(). Without this, an admin created before 2026-08-25 could never log in at
// all — Reset Password is only reachable *from* a logged-in session, so a NULL-password
// admin with no other admin around would be permanently locked out. Idempotent, safe to
// call every boot (see main.cpp's setup()).
void users_backfill_admin_passwords();
