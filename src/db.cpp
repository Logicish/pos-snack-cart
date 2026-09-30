#include "db.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <sqlite3.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- SD card mount + SQLite database layer: schema creation/migration, the
            config key/value table, and the raw-file backup/restore + write-test
            primitives declared in db.h.
*/

#define SD_CS_PIN 14

static sqlite3 *_db = nullptr;

// Returns the open DB handle, or nullptr if db_init() failed or hasn't run yet.
sqlite3 *db_handle() { return _db; }

// ── helpers ───────────────────────────────────────────────────────────────────

// Runs a raw SQL statement with no result set (CREATE TABLE, PRAGMA, INSERT/UPDATE),
// logging and returning false on failure.
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

// Mounts the SD card, opens (or creates) pos.db, and creates/migrates the schema.
// Non-fatal on failure -- returns false and leaves _db null for the caller to handle.
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
        // sqlite3_open() can still hand back a non-null (but unusable) handle on failure --
        // close/null it so db_handle() reliably reports "unavailable" here, matching what
        // boot_try_init_db()'s caller already assumes (system_alerts.cpp's _db_unavailable
        // check, screen_sd_error.cpp's diagnosis message).
        if (_db) { sqlite3_close(_db); _db = nullptr; }
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
    // Admin web-portal login used a per-admin salted SHA-256 password (see auth.cpp,
    // password_hash/password_salt/password_is_default columns) from 2026-08-25 until
    // 2026-09-14, when it was dropped for a single shared plaintext password (see
    // webserver.cpp's ADMIN_PASSWORD_CONFIG_KEY) -- this device's AP is off most of the
    // time, local-range-only, no real threat model to justify the complexity. Those three
    // columns are left as inert leftovers on any DB that already has them (SQLite on this
    // build has no reliable DROP COLUMN, see the items.upc comment below for the same
    // reasoning) rather than migrated -- nothing reads or writes them anymore.

    exec(
        "CREATE TABLE IF NOT EXISTS items ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  upc         TEXT    UNIQUE,"   // deprecated 2026-08-25, see item_upcs below — left in
                                           // place rather than dropped (DROP COLUMN support on
                                           // this SQLite build is unconfirmed and not worth
                                           // risking), never read or written anymore
        "  name        TEXT    NOT NULL,"
        "  price_cents INTEGER NOT NULL,"
        "  stocked     INTEGER NOT NULL DEFAULT 0,"
        // hidden is in CREATE TABLE as of 2026-09-29 -- a blank-card setup-wizard test
        // produced a fresh pos.db WITHOUT it: the guarded ALTER below failed silently on a
        // brand-new table, same as every other ALTER on this build. Fresh DBs no longer
        // depend on ALTER at all; the ALTER below stays only for pre-09-14 DBs.
        "  hidden      INTEGER NOT NULL DEFAULT 0"
        ");"
    );

    // hidden — 2026-09-14. Lets an item be pulled from customer-facing Browse/scanning
    // (checkout add-by-scan, Price Check, Restock/Add-Attach's scan-to-open all treat a
    // hidden item's UPC as unrecognized, via items_find_by_upc()) while its row -- and any
    // checkout_items history referencing it -- stays intact, unlike items_delete() which
    // is blocked outright once real history exists. Still reachable through Inventory's
    // full list and Add/Attach's existing-item picker, both deliberately unfiltered, so an
    // admin can restock/unhide/edit it without a live barcode in hand. Added after real
    // items already existed on-device, so it's an ALTER not a CREATE TABLE column, same
    // guarded pattern as users.admin/active above.
    if (!column_exists("items", "hidden")) exec("ALTER TABLE items ADD COLUMN hidden INTEGER NOT NULL DEFAULT 0;");
    // Verify it actually landed rather than trust it — this exact ALTER TABLE ADD COLUMN
    // pattern silently failed to persist on real hardware once before (see the users.active
    // history above), with no error surfaced anywhere at the time; that was only ever caught
    // by physically pulling the SD card. Logging it immediately here means a repeat shows up
    // in the Serial monitor on the very next boot instead of needing the same slow diagnosis.
    if (!column_exists("items", "hidden")) {
        Serial.println("[DB] WARNING: items.hidden column missing after ALTER -- Hide/Unhide"
                        " and any query selecting it will fail this boot.");
    }

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
    // The every-boot items.upc -> item_upcs copy (2026-08-25 migration) was removed
    // 2026-09-29. Every real DB was migrated long ago, and re-running it each boot
    // silently re-linked any barcode an admin had unlinked in Item Edit. The real card's
    // legacy items.upc values were cleared by hand the same day; items.upc stays in the
    // schema only because this build's DROP COLUMN support is unconfirmed.

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
    // Real display_name/handle deliberately NOT seeded here — real PII doesn't belong
    // hardcoded in firmware source. Set these once through the Admin web page's Payment
    // Settings form instead (writes straight to this table, no firmware rebuild needed).

    // Extras minigame high scores, 2026-09-30 — one row per user per game, best score
    // only (no history). `game` is a short fixed key ('laggy_fish', 'slide_free')
    // so every game shares this one table instead of each growing its own. Brand-new
    // table, so CREATE TABLE IF NOT EXISTS covers existing cards — no ALTER involved.
    exec(
        "CREATE TABLE IF NOT EXISTS game_scores ("
        "  user_id     INTEGER NOT NULL REFERENCES users(id),"
        "  game        TEXT NOT NULL,"
        "  best        INTEGER NOT NULL DEFAULT 0,"
        "  achieved_at INTEGER,"
        "  PRIMARY KEY (user_id, game)"
        ");"
    );

    Serial.println("[DB] schema ready");
    return true;
}

// Closes the DB handle, if open.
void db_close() {
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

// Reads one value out of the config table by key. False if missing/empty/unavailable.
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

// Writes/overwrites one value in the config table by key.
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

// ── backup / restore ─────────────────────────────────────────────────────────
// Plain SD.h paths (no "/sd" VFS prefix) — that prefix is specific to sqlite3_open()'s
// ESP-IDF VFS mount, a separate path namespace from Arduino's SD.h, which already
// addresses the same physical FAT root directly (confirmed by SD Info's own file listing,
// which shows "pos.db" with no "/sd/" prefix using this same SD.h API).
#define DB_PATH_SD      "/pos.db"
#define DB_BACKUP_PATH  "/pos_backup.db"

// Copies one file on the SD card byte-for-byte, overwriting any existing destination.
static bool copy_file(const char *src, const char *dst) {
    File in = SD.open(src, FILE_READ);
    if (!in) return false;

    SD.remove(dst);  // start clean -- FILE_WRITE would append onto any leftover partial file
    File out = SD.open(dst, FILE_WRITE);
    if (!out) {
        in.close();
        return false;
    }

    uint8_t buf[512];
    bool ok = true;
    while (true) {
        int n = in.read(buf, sizeof(buf));
        if (n <= 0) break;
        if (out.write(buf, n) != (size_t)n) { ok = false; break; }
    }
    in.close();
    out.close();
    return ok;
}

// Copies the live DB to the backup file.
bool db_backup_now() {
    bool ok = copy_file(DB_PATH_SD, DB_BACKUP_PATH);
    Serial.printf("[DB] backup %s\n", ok ? "OK" : "FAILED");
    return ok;
}

// Copies the backup file over the live DB, if a backup exists.
bool db_restore_from_backup() {
    if (!SD.exists(DB_BACKUP_PATH)) {
        Serial.println("[DB] no backup file present, nothing to restore from");
        return false;
    }
    bool ok = copy_file(DB_BACKUP_PATH, DB_PATH_SD);
    Serial.printf("[DB] restore from backup %s\n", ok ? "OK" : "FAILED");
    return ok;
}

// Runs a quick, proven-primitive query to confirm the schema is actually queryable.
bool db_sanity_check() {
    if (!_db) return false;

    sqlite3_stmt *stmt;
    bool ok = false;
    if (sqlite3_prepare_v2(_db, "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
        ok = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    return ok;
}

// ── SD-layer check ───────────────────────────────────────────────────────────
#define SD_TEST_PATH "/sd_write_test.tmp"
#define SD_TEST_PAYLOAD "snack-cart-write-test"

// Writes a throwaway file, reads it back, and verifies it round-tripped correctly --
// confirms the SD card itself is actually writable right now.
bool sd_write_read_test() {
    SD.remove(SD_TEST_PATH);  // clear any prior leftover first, same as copy_file()'s approach

    File out = SD.open(SD_TEST_PATH, FILE_WRITE);
    if (!out) return false;
    size_t written = out.print(SD_TEST_PAYLOAD);
    out.close();
    if (written != strlen(SD_TEST_PAYLOAD)) {
        SD.remove(SD_TEST_PATH);
        return false;
    }

    File in = SD.open(SD_TEST_PATH, FILE_READ);
    if (!in) return false;
    char buf[sizeof(SD_TEST_PAYLOAD)] = {0};
    size_t read = in.read((uint8_t *)buf, sizeof(buf) - 1);
    in.close();
    SD.remove(SD_TEST_PATH);  // don't leave the throwaway file sitting on the card

    return read == strlen(SD_TEST_PAYLOAD) && strcmp(buf, SD_TEST_PAYLOAD) == 0;
}
