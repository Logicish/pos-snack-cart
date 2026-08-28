#include "users.h"
#include "auth.h"
#include "db.h"
#include <sqlite3.h>
#include <string.h>
#include <SD.h>
#include <Arduino.h>

// Deliberately dumb/discoverable — the point is the forced password_is_default change flow
// on first login, not secrecy of this string itself. See auth.h. "logicish" per explicit
// request 2026-08-28 (was "changeme") — simple enough to remember, not so obvious it's
// the first thing anyone would guess; this is a low-security, small trust-based system,
// not something meant to resist a real attacker.
#define DEFAULT_ADMIN_PASSWORD "logicish"

static User _cache;  // filled in by users_find_by_badge(), overwritten on the next call
static int  _current_admin_id = -1;

bool users_reset_password(int user_id);  // forward decl -- users_create() below calls this

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
    sqlite3_bind_text(stmt, 1, badge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, first_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, last_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, admin ? 1 : 0);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        Serial.printf("[USERS] create step failed for badge %s: %s\n", badge_id, sqlite3_errmsg(db_handle()));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    int new_id = (int)sqlite3_last_insert_rowid(db_handle());
    // Every admin gets the shared default password set automatically -- no separate manual
    // step, and password_is_default forces a change on their first login.
    if (admin) users_reset_password(new_id);
    return new_id;
}

// Bulk-enrollment from the SD card, not firmware source — real names/badge numbers have
// no business being hardcoded into compiled firmware (they'd ship in every .bin and sit
// in source history forever). Format: one user per line, "badge,first,last,admin" —
// admin is 1 or 0 (or absent/anything else, treated as 0). Blank lines and lines starting
// with '#' are skipped. Safe to leave this file on the card permanently and re-run every
// boot — already-enrolled badges are skipped, so onboarding someone new later is just
// "add a line, reboot," no firmware change needed. Missing file is not an error; this is
// opportunistic, not required to boot.
void users_import_from_sd() {
    if (!db_handle() || !SD.exists("/seed_users.csv")) return;

    File f = SD.open("/seed_users.csv", FILE_READ);
    if (!f) return;

    int imported = 0, skipped = 0, malformed = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;

        int c1 = line.indexOf(',');
        int c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
        if (c1 < 0 || c2 < 0) {
            Serial.printf("[USERS] skipping malformed seed line: %s\n", line.c_str());
            malformed++;
            continue;
        }
        int c3 = line.indexOf(',', c2 + 1);  // admin field is optional — c3 may be -1

        String badge = line.substring(0, c1);            badge.trim();
        String first = line.substring(c1 + 1, c2);        first.trim();
        String last  = (c3 >= 0) ? line.substring(c2 + 1, c3) : line.substring(c2 + 1);
        last.trim();
        bool   admin = (c3 >= 0) && line.substring(c3 + 1).toInt() != 0;

        if (badge.length() == 0 || first.length() == 0) {
            Serial.printf("[USERS] skipping malformed seed line: %s\n", line.c_str());
            malformed++;
            continue;
        }

        if (users_find_by_badge(badge.c_str())) {
            skipped++;
            continue;
        }
        if (users_create(badge.c_str(), first.c_str(), last.c_str(), admin) >= 0) imported++;
    }
    f.close();
    Serial.printf("[USERS] SD seed import: %d new, %d already enrolled, %d malformed\n",
                  imported, skipped, malformed);
}

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

bool users_set_name(int id, const char *first_name, const char *last_name) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "UPDATE users SET first_name=?, last_name=? WHERE id=?;", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, first_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, last_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool users_set_admin(int id, bool admin) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "UPDATE users SET admin = ? WHERE id = ?;", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, admin ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    // Promoting to admin needs a real password to log into the web portal with -- same
    // shared-default + forced-change flow users_create() already arms for a brand-new
    // admin (2026-08-28: this toggle used to just flip the column, leaving a promoted
    // user with no password until the next boot's users_backfill_admin_passwords() catch-
    // all happened to run). Demoting doesn't need the reverse -- a stale password_hash on
    // a former admin is inert, nothing ever checks it once admin=0.
    if (ok && admin) users_reset_password(id);

    return ok;
}

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

bool users_verify_password(int user_id, const char *password) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT password_hash, password_salt FROM users WHERE id = ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, user_id);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        const char *salt = (const char *)sqlite3_column_text(stmt, 1);
        if (hash && salt) ok = auth_verify_password(password, hash, salt);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool users_is_password_default(int user_id) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    bool is_default = false;
    if (sqlite3_prepare_v2(db_handle(), "SELECT password_is_default FROM users WHERE id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, user_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) is_default = sqlite3_column_int(stmt, 0) != 0;
        sqlite3_finalize(stmt);
    }
    return is_default;
}

static bool set_password_internal(int user_id, const char *password, bool is_default) {
    if (!db_handle()) return false;

    char hash[AUTH_HASH_HEX_LEN], salt[AUTH_SALT_HEX_LEN];
    auth_hash_password(password, hash, salt);

    sqlite3_stmt *stmt;
    const char *sql = "UPDATE users SET password_hash=?, password_salt=?, password_is_default=? WHERE id=?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, salt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, is_default ? 1 : 0);
    sqlite3_bind_int(stmt, 4, user_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool users_set_password(int user_id, const char *new_password) {
    return set_password_internal(user_id, new_password, false);
}

bool users_reset_password(int user_id) {
    return set_password_internal(user_id, DEFAULT_ADMIN_PASSWORD, true);
}

void users_set_current_admin(int user_id) { _current_admin_id = user_id; }
int  users_get_current_admin()            { return _current_admin_id;  }

void users_backfill_admin_passwords() {
    if (!db_handle()) return;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(),
            "SELECT id FROM users WHERE admin = 1 AND password_hash IS NULL;",
            -1, &stmt, nullptr) != SQLITE_OK) return;

    // Collect ids first, then update -- avoids running UPDATE statements on this connection
    // while a SELECT statement is still mid-iteration. Small device, few admins realistically.
    int ids[16];
    int n = 0;
    while (n < 16 && sqlite3_step(stmt) == SQLITE_ROW) ids[n++] = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    for (int i = 0; i < n; i++) {
        users_reset_password(ids[i]);
        Serial.printf("[USERS] backfilled default password for admin id %d\n", ids[i]);
    }
}
