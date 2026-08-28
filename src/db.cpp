#include "db.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <sqlite3.h>
#include <string.h>

#define SD_CS_PIN 14

static sqlite3 *_db = nullptr;

sqlite3 *db_handle() { return _db; }

// ── helpers ───────────────────────────────────────────────────────────────────

static bool exec(const char *sql) {
    char *err = nullptr;
    int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        Serial.printf("[DB] %s\n", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

// 2026-08-24: `ALTER TABLE users ADD COLUMN active ...` was found (via pulling the SD
// card and inspecting pos.db directly) to have silently failed to actually add the
// column on real hardware, despite running unconditionally right after an `admin`
// ALTER that fails with "duplicate column" every boot once `admin` already exists.
// Checking existence first — so a column that's already present is never ALTERed
// again — sidesteps whatever this SQLite build's ALTER TABLE / error-recovery quirk is,
// rather than trying to root-cause a third-party library's internals further.
static bool column_exists(const char *table, const char *column) {
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);

    sqlite3_stmt *stmt;
    bool found = false;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 1);
            if (name && strcmp(name, column) == 0) {
                found = true;
                break;
            }
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

// ── public ────────────────────────────────────────────────────────────────────

bool db_init() {
    // Shared HSPI bus with the display — SCLK=12/MISO=13/MOSI=11, SD gets its own CS.
    // Confirmed working (no bus contention with the display) via the earlier SD test screen.
    SPI.begin(12, 13, 11, SD_CS_PIN);

    if (!SD.begin(SD_CS_PIN, SPI, 20000000)) {
        Serial.println("[DB] SD.begin() failed");
        return false;
    }
    if (SD.cardType() == CARD_NONE) {
        Serial.println("[DB] No SD card detected");
        return false;
    }

    sqlite3_initialize();

    if (sqlite3_open("/sd/pos.db", &_db) != SQLITE_OK) {
        Serial.printf("[DB] open failed: %s\n", sqlite3_errmsg(_db));
        return false;
    }
    Serial.println("[DB] opened /sd/pos.db");

    // Deliberately NOT WAL — WAL on a filesystem with iffy locking semantics was the root
    // cause of the original LittleFS DB crashes. Rollback-journal mode is slower but
    // crash-safe, which matters since this device gets unplugged periodically.
    //
    // TRUNCATE not DELETE — 2026-08-24, after real hardware kept showing a stale
    // pos.db-journal (one page, 4096B) even right after a totally clean boot with
    // correct data (items/users counts matched exactly, no crash involved). DELETE mode
    // deletes the journal file after each commit; TRUNCATE instead zeroes its header and
    // leaves the file allocated for reuse — same rollback-journal crash-safety guarantee,
    // just without the file delete/rename step, which SD/FAT filesystems and their VFS
    // wrappers are known to handle unreliably. Should make the leftover file either
    // disappear or become a harmless, always-present, always-inert placeholder.
    exec("PRAGMA journal_mode=TRUNCATE;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA foreign_keys=ON;");

    exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  badge_barcode TEXT    UNIQUE NOT NULL,"
        "  first_name    TEXT    NOT NULL,"
        "  last_name     TEXT    NOT NULL,"
        "  created_at    INTEGER,"
        "  admin         INTEGER NOT NULL DEFAULT 0,"
        "  active        INTEGER NOT NULL DEFAULT 1"
        ");"
    );
    // `admin`/`active` added after some real users already existed on-device.
    // column_exists() guards each ALTER so it only ever runs once per column, not every
    // boot — see that helper's comment for why (a real on-device failure was traced to
    // re-running ALTER on an already-existing column). `admin` isn't used to gate
    // anything yet (no web login exists). `active` (2026-08-24) is: a badge scan for an
    // inactive user gets turned away with a message instead of starting a transaction —
    // a non-destructive way to "boot" someone (e.g. an outstanding balance) without
    // touching their checkout history or deleting the row.
    if (!column_exists("users", "admin"))  exec("ALTER TABLE users ADD COLUMN admin INTEGER NOT NULL DEFAULT 0;");
    if (!column_exists("users", "active")) exec("ALTER TABLE users ADD COLUMN active INTEGER NOT NULL DEFAULT 1;");
    // Admin web-portal login, 2026-08-25 — salted SHA-256 (see auth.cpp), NULL for
    // non-admin rows (they never log into the web portal). password_is_default drives the
    // forced change-password redirect after first login.
    if (!column_exists("users", "password_hash"))       exec("ALTER TABLE users ADD COLUMN password_hash TEXT;");
    if (!column_exists("users", "password_salt"))        exec("ALTER TABLE users ADD COLUMN password_salt TEXT;");
    if (!column_exists("users", "password_is_default"))  exec("ALTER TABLE users ADD COLUMN password_is_default INTEGER NOT NULL DEFAULT 0;");

    exec(
        "CREATE TABLE IF NOT EXISTS items ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  upc         TEXT    UNIQUE,"   // deprecated 2026-08-25, see item_upcs below — left in
                                           // place rather than dropped (DROP COLUMN support on
                                           // this SQLite build is unconfirmed and not worth
                                           // risking), just no longer read or written by new code
        "  name        TEXT    NOT NULL,"
        "  price_cents INTEGER NOT NULL,"
        "  stocked     INTEGER NOT NULL DEFAULT 0"
        ");"
    );

    // item_upcs — 2026-08-25, replaces items.upc. One item can be sold under several real
    // barcodes (e.g. a variety pack where the owner restocks by package, not by flavor, so
    // she wants one shared name/price/stock count regardless of which flavor's barcode gets
    // scanned — see snack_cart_pos.md's 2026-08-25 planning round for the full reasoning).
    // A same-price different-product item (most energy drink brands, say) needs none of
    // this — price_cents was never unique, two items rows can already share a price.
    exec(
        "CREATE TABLE IF NOT EXISTS item_upcs ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  item_id INTEGER NOT NULL REFERENCES items(id),"
        "  upc     TEXT    UNIQUE NOT NULL"
        ");"
    );
    // One-time (but safe to re-run every boot — UNIQUE on upc makes it a no-op once migrated)
    // copy of any legacy items.upc value into the new table. Deliberately INSERT OR IGNORE,
    // not UPSERT — see the 2026-08-24 note above on why ON CONFLICT...DO UPDATE silently
    // no-ops on this SQLite build; OR IGNORE is the older, universally-supported mechanism.
    exec(
        "INSERT OR IGNORE INTO item_upcs (item_id, upc) "
        "SELECT id, upc FROM items WHERE upc IS NOT NULL AND upc != '';"
    );

    // One row per checkout ("Finish" press), NOT per item scan — Venmo pays per-checkout.
    // id IS the transaction number (free from AUTOINCREMENT). cleared_at is UPDATE-only,
    // never delete — this is what makes lifetime totals and clearable balances the same
    // data, just filtered differently, instead of two things that can drift apart.
    exec(
        "CREATE TABLE IF NOT EXISTS checkouts ("
        "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id           INTEGER NOT NULL REFERENCES users(id),"
        "  total_price_cents INTEGER NOT NULL,"
        "  created_at        INTEGER NOT NULL,"
        "  cleared_at        INTEGER"
        ");"
    );

    exec(
        "CREATE TABLE IF NOT EXISTS checkout_items ("
        "  checkout_id INTEGER NOT NULL REFERENCES checkouts(id),"
        "  item_id     INTEGER NOT NULL REFERENCES items(id),"
        "  price_cents INTEGER NOT NULL,"
        "  quantity    INTEGER NOT NULL DEFAULT 1"
        ");"
    );

    exec(
        "CREATE TABLE IF NOT EXISTS config ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT"
        ");"
    );

    exec(
        "INSERT OR IGNORE INTO config (key, value) VALUES"
        "  ('wifi_password_hash', '');"
    );

    // Payment recipient info — deliberately its OWN table, not a couple of `config` rows
    // and not columns on `users`. Reasoning (2026-08-24 design discussion):
    //  - Not on `users`: a Venmo/Zelle handle is only ever relevant for whoever currently
    //    receives payment (one person), not a per-enrollee attribute — putting it on every
    //    user row would mean stale handles lingering on old/former owners indefinitely, and
    //    wasted columns on the ~50-100 rows that will never use it.
    //  - Not tied to `users` by a foreign key either — ownership is a whole-cart concept,
    //    not a data relationship, and ownership transfer should be a deliberate action
    //    (wipe this table, re-enter fresh) rather than something a schema constraint decides.
    //  - One row per payment method (not per person) so adding Zelle etc. later is just
    //    another row, no schema change.
    exec(
        "CREATE TABLE IF NOT EXISTS payment_methods ("
        "  method       TEXT PRIMARY KEY,"   // 'venmo', 'zelle', ...
        "  display_name TEXT,"               // shown as "Payment to: ___"
        "  handle       TEXT,"
        "  enabled      INTEGER NOT NULL DEFAULT 1"
        ");"
    );
    exec(
        "INSERT OR IGNORE INTO payment_methods (method, display_name, handle, enabled) VALUES"
        "  ('venmo', '', '', 1);"
    );
    // Real display_name/handle deliberately NOT seeded here — same reasoning as
    // users_import_from_sd() in users.cpp: real PII doesn't belong hardcoded in firmware
    // source. Set these once through the Admin web page's Payment Settings form instead
    // (writes straight to this table, no firmware rebuild needed).

    Serial.println("[DB] schema ready");
    return true;
}

void db_close() {
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

bool db_config_get(const char *key, char *out, size_t out_len) {
    if (!_db) return false;

    sqlite3_stmt *stmt;
    bool found = false;
    if (sqlite3_prepare_v2(_db, "SELECT value FROM config WHERE key = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *val = sqlite3_column_text(stmt, 0);
            if (val && val[0] != '\0') {
                strncpy(out, (const char *)val, out_len - 1);
                out[out_len - 1] = '\0';
                found = true;
            }
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

bool db_config_set(const char *key, const char *value) {
    if (!_db) return false;

    sqlite3_stmt *stmt;
    bool ok = false;
    if (sqlite3_prepare_v2(_db, "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?);", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    return ok;
}
