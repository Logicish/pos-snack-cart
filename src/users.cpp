#include "users.h"
#include "db.h"
#include <sqlite3.h>
#include <string.h>
#include <ctype.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the enrolled-people table declared in users.h -- badge
            lookup/creation/editing, the admin flag, and the current-admin tracker.
*/

// Names are stored all-caps everywhere, not just from the on-device character wheel (which
// only offers A-Z anyway) — the web Users page's inline edit form accepts free-typed text,
// and a mixed-case name from it next to an all-caps wheel-entered one was flagged 2026-08-28
// as a real data-consistency risk (nothing compares names case-sensitively today — lookups
// key off badge_barcode, sorting already uses COLLATE NOCASE — but better to not leave two
// casing conventions sitting in the same column). Applied at every write path (users_create(),
// users_set_name(); the web handler uses String's own toUpperCase()) rather than just the
// wheel's input, so it's consistent regardless of source.
static void upper_copy(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    for (; src[i] != '\0' && i < dst_size - 1; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static User _cache;  // filled in by users_find_by_badge(), overwritten on the next call
static int  _current_admin_id = -1;

// Looks up one user by badge barcode.
const User *users_find_by_badge(const char *badge_id) {
    if (!db_handle()) return nullptr;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, badge_barcode, first_name, last_name, admin, active FROM users WHERE badge_barcode = ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Serial.printf("[USERS] find_by_badge prepare failed: %s\n", sqlite3_errmsg(db_handle()));
        return nullptr;
    }
    sqlite3_bind_text(stmt, 1, badge_id, -1, SQLITE_STATIC);

    const User *result = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        _cache.id = sqlite3_column_int(stmt, 0);
        strncpy(_cache.badge_id, (const char *)sqlite3_column_text(stmt, 1), BADGE_ID_LEN - 1);
        _cache.badge_id[BADGE_ID_LEN - 1] = '\0';
        strncpy(_cache.first_name, (const char *)sqlite3_column_text(stmt, 2), NAME_FIELD_LEN - 1);
        _cache.first_name[NAME_FIELD_LEN - 1] = '\0';
        strncpy(_cache.last_name, (const char *)sqlite3_column_text(stmt, 3), NAME_FIELD_LEN - 1);
        _cache.last_name[NAME_FIELD_LEN - 1] = '\0';
        _cache.admin = sqlite3_column_int(stmt, 4) != 0;
        _cache.active = sqlite3_column_int(stmt, 5) != 0;
        result = &_cache;
    }
    sqlite3_finalize(stmt);
    return result;
}

// Enrolls a new user (name uppercased before storing). Returns the new id, -1 on failure.
int users_create(const char *badge_id, const char *first_name, const char *last_name, bool admin) {
    if (!db_handle()) return -1;

    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT INTO users (badge_barcode, first_name, last_name, created_at, admin, active) "
        "VALUES (?, ?, ?, strftime('%s','now'), ?, 1);";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Serial.printf("[USERS] create prepare failed: %s\n", sqlite3_errmsg(db_handle()));
        return -1;
    }
    char first_upper[NAME_FIELD_LEN], last_upper[NAME_FIELD_LEN];
    upper_copy(first_upper, sizeof(first_upper), first_name);
    upper_copy(last_upper, sizeof(last_upper), last_name);

    sqlite3_bind_text(stmt, 1, badge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, first_upper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, last_upper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, admin ? 1 : 0);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        Serial.printf("[USERS] create step failed for badge %s: %s\n", badge_id, sqlite3_errmsg(db_handle()));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    int new_id = (int)sqlite3_last_insert_rowid(db_handle());
    return new_id;
}

// Toggles a user active/inactive (turned away at the device without deleting them).
bool users_set_active(int id, bool active) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "UPDATE users SET active = ? WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, active ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// Returns the total number of rows in the users table.
int users_count() {
    if (!db_handle()) return 0;

    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

// Looks up one user by database id.
const User *users_get_by_id(int id) {
    if (!db_handle()) return nullptr;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, badge_barcode, first_name, last_name, admin, active FROM users WHERE id = ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) return nullptr;
    sqlite3_bind_int(stmt, 1, id);

    const User *result = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        _cache.id = sqlite3_column_int(stmt, 0);
        strncpy(_cache.badge_id, (const char *)sqlite3_column_text(stmt, 1), BADGE_ID_LEN - 1);
        _cache.badge_id[BADGE_ID_LEN - 1] = '\0';
        strncpy(_cache.first_name, (const char *)sqlite3_column_text(stmt, 2), NAME_FIELD_LEN - 1);
        _cache.first_name[NAME_FIELD_LEN - 1] = '\0';
        strncpy(_cache.last_name, (const char *)sqlite3_column_text(stmt, 3), NAME_FIELD_LEN - 1);
        _cache.last_name[NAME_FIELD_LEN - 1] = '\0';
        _cache.admin = sqlite3_column_int(stmt, 4) != 0;
        _cache.active = sqlite3_column_int(stmt, 5) != 0;
        result = &_cache;
    }
    sqlite3_finalize(stmt);
    return result;
}

// Updates a user's name (uppercased before storing).
bool users_set_name(int id, const char *first_name, const char *last_name) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "UPDATE users SET first_name=?, last_name=? WHERE id=?;", -1, &stmt, nullptr) != SQLITE_OK) return false;

    char first_upper[NAME_FIELD_LEN], last_upper[NAME_FIELD_LEN];
    upper_copy(first_upper, sizeof(first_upper), first_name);
    upper_copy(last_upper, sizeof(last_upper), last_name);

    sqlite3_bind_text(stmt, 1, first_upper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, last_upper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// Toggles a user's admin flag.
bool users_set_admin(int id, bool admin) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "UPDATE users SET admin = ? WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, admin ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// Fills out[] with every user, sorted first-then-last-name alphabetically.
int users_get_all(User *out, int max) {
    if (!db_handle() || max <= 0) return 0;

    sqlite3_stmt *stmt;
    int n = 0;
    const char *sql =
        "SELECT id, badge_barcode, first_name, last_name, admin, active FROM users "
        "ORDER BY first_name COLLATE NOCASE, last_name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
            out[n].id = sqlite3_column_int(stmt, 0);
            strncpy(out[n].badge_id, (const char *)sqlite3_column_text(stmt, 1), BADGE_ID_LEN - 1);
            out[n].badge_id[BADGE_ID_LEN - 1] = '\0';
            strncpy(out[n].first_name, (const char *)sqlite3_column_text(stmt, 2), NAME_FIELD_LEN - 1);
            out[n].first_name[NAME_FIELD_LEN - 1] = '\0';
            strncpy(out[n].last_name, (const char *)sqlite3_column_text(stmt, 3), NAME_FIELD_LEN - 1);
            out[n].last_name[NAME_FIELD_LEN - 1] = '\0';
            out[n].admin = sqlite3_column_int(stmt, 4) != 0;
            out[n].active = sqlite3_column_int(stmt, 5) != 0;
            n++;
        }
        sqlite3_finalize(stmt);
    }
    return n;
}

// Records which admin most recently scanned in on-device this boot.
void users_set_current_admin(int user_id) { _current_admin_id = user_id; }
// Returns that admin's id, or -1 if none has scanned in yet this boot.
int  users_get_current_admin()            { return _current_admin_id;  }

// True if at least one admin row exists in the users table.
bool users_has_admin() {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    bool found = false;
    if (sqlite3_prepare_v2(db_handle(), "SELECT 1 FROM users WHERE admin = 1 LIMIT 1;", -1, &stmt, nullptr) == SQLITE_OK) {
        found = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    return found;
}
