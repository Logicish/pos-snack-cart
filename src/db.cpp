#include "db.h"
#include "report_export.h"  // REPORT_TMP_PATH, for sd_cleanup_temp_files()
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
    // re-running ALTER on an already-existing column). `admin` gates Admin Login on the
    // cart (and with it the Web Portal). `active` (2026-08-24): a badge scan for an
    // inactive user gets turned away with a message instead of starting a transaction —
    // a non-destructive way to "boot" someone (e.g. an outstanding balance) without
    // touching their checkout history or deleting the row.
    if (!column_exists("users", "admin"))  exec("ALTER TABLE users ADD COLUMN admin INTEGER NOT NULL DEFAULT 0;");
    if (!column_exists("users", "active")) exec("ALTER TABLE users ADD COLUMN active INTEGER NOT NULL DEFAULT 1;");
    // Admin web-portal login used a per-admin salted SHA-256 password (see auth.cpp,
    // password_hash/password_salt/password_is_default columns) from 2026-08-25 until
    // 2026-09-14, when it was dropped for a single shared plaintext password, itself
    // removed 2026-09-29 -- the web portal has no login now (cart Admin Login + the WiFi
    // password are the gates, see webserver.cpp's header). Those three
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
#define DB_JOURNAL_SD   "/pos.db-journal"
#define BACKUP_TMP      "/pos_backup.tmp"
#define RESTORE_TMP     "/pos_restore.tmp"
#define BACKUP_SLOTS    5

// Slot 0 keeps the original single-backup name, so Download database and existing cards
// carry straight over.
static const char *BACKUP_PATHS[BACKUP_SLOTS] = {
    "/pos_backup.db", "/pos_backup1.db", "/pos_backup2.db", "/pos_backup3.db", "/pos_backup4.db",
};

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

// True if both files exist and match byte-for-byte -- reads the copy back off the card,
// so a write the card silently dropped or mangled shows up here, not at restore time.
static bool files_equal(const char *a, const char *b) {
    File fa = SD.open(a, FILE_READ);
    File fb = SD.open(b, FILE_READ);
    bool same = fa && fb && fa.size() == fb.size();
    uint8_t ba[512], bb[512];
    while (same) {
        int na = fa.read(ba, sizeof(ba));
        int nb = fb.read(bb, sizeof(bb));
        if (na != nb) { same = false; break; }
        if (na <= 0) break;
        if (memcmp(ba, bb, na) != 0) same = false;
    }
    if (fa) fa.close();
    if (fb) fb.close();
    return same;
}

// Copies src to dst through a temp file: copy, compare, and only then replace dst. dst is
// never left half-written -- a failure anywhere before the final swap leaves it as it was.
static bool copy_verified(const char *src, const char *tmp, const char *dst) {
    if (!copy_file(src, tmp) || !files_equal(src, tmp)) {
        SD.remove(tmp);
        return false;
    }
    SD.remove(dst);
    return SD.rename(tmp, dst);
}

// Steps through every row of one query, touching every column -- column_bytes() makes
// SQLite load the full value, including any overflow pages. False on any read error.
static bool read_all_rows(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int cols = sqlite3_column_count(stmt);
        for (int c = 0; c < cols; c++) sqlite3_column_bytes(stmt, c);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// The deep check behind db_sanity_check(), on any open handle (the live DB or a backup
// opened read-only). See db.h for why it reads everything instead of using a PRAGMA.
static bool deep_check(sqlite3 *db) {
    if (!db) return false;

    // Gather every table and index name first, then read each one.
    struct Obj { char type; char name[48]; char tbl[48]; };
    static Obj objs[32];
    int n = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT type, name, tbl_name FROM sqlite_master WHERE type IN ('table','index');",
                           -1, &stmt, nullptr) != SQLITE_OK) return false;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *ty = (const char *)sqlite3_column_text(stmt, 0);
        const char *nm = (const char *)sqlite3_column_text(stmt, 1);
        const char *tb = (const char *)sqlite3_column_text(stmt, 2);
        if (!ty || !nm || !tb || n >= 32) continue;
        objs[n].type = ty[0];
        snprintf(objs[n].name, sizeof(objs[n].name), "%s", nm);
        snprintf(objs[n].tbl, sizeof(objs[n].tbl), "%s", tb);
        n++;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        Serial.printf("[DB] deep check: reading the schema failed: %s\n", sqlite3_errmsg(db));
        return false;
    }

    // The cart can't run without these -- a file missing any of them isn't a usable DB.
    static const char *REQUIRED[] = { "users", "items", "item_upcs", "checkouts",
                                      "checkout_items", "config", "payment_methods" };
    for (const char *req : REQUIRED) {
        bool found = false;
        for (int i = 0; i < n && !found; i++) found = objs[i].type == 't' && strcmp(objs[i].name, req) == 0;
        if (!found) {
            Serial.printf("[DB] deep check: table %s missing\n", req);
            return false;
        }
    }

    char sql[200];
    for (int i = 0; i < n; i++) {
        if (objs[i].type == 't') {
            snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\";", objs[i].name);
            if (!read_all_rows(db, sql)) {
                Serial.printf("[DB] deep check: reading table %s failed: %s\n", objs[i].name, sqlite3_errmsg(db));
                return false;
            }
            continue;
        }
        // Index: walk it in order on its first column, which reads the index's own pages
        // (a table scan never touches them). If this build won't report the column or plan
        // the query, skip that index rather than fail a healthy DB -- only a read error counts.
        char col[48] = "";
        snprintf(sql, sizeof(sql), "PRAGMA index_info(\"%s\");", objs[i].name);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *c = (const char *)sqlite3_column_text(stmt, 2);
                if (c) snprintf(col, sizeof(col), "%s", c);
            }
            sqlite3_finalize(stmt);
        }
        if (!col[0]) continue;
        snprintf(sql, sizeof(sql), "SELECT \"%s\" FROM \"%s\" INDEXED BY \"%s\" ORDER BY \"%s\";",
                 col, objs[i].tbl, objs[i].name, col);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {}
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            Serial.printf("[DB] deep check: reading index %s failed: %s\n", objs[i].name, sqlite3_errmsg(db));
            return false;
        }
    }
    return true;
}

// Verified copy of the live DB into slot 0, rotating the older copies down one slot.
bool db_backup_now() {
    // Never back up a DB that fails the deep check -- that's what keeps damage from
    // rotating into (and eventually through) every copy. The existing copies stay as-is.
    if (!deep_check(_db)) {
        Serial.println("[DB] backup SKIPPED -- live DB failed the deep check, existing copies kept");
        return false;
    }
    // Copy + compare into a temp file before touching any slot.
    if (!copy_file(DB_PATH_SD, BACKUP_TMP) || !files_equal(DB_PATH_SD, BACKUP_TMP)) {
        SD.remove(BACKUP_TMP);
        Serial.println("[DB] backup FAILED -- copy didn't verify, existing copies kept");
        return false;
    }
    SD.remove(BACKUP_PATHS[BACKUP_SLOTS - 1]);
    for (int i = BACKUP_SLOTS - 2; i >= 0; i--) {
        if (SD.exists(BACKUP_PATHS[i])) SD.rename(BACKUP_PATHS[i], BACKUP_PATHS[i + 1]);
    }
    bool ok = SD.rename(BACKUP_TMP, BACKUP_PATHS[0]);
    Serial.printf("[DB] backup %s\n", ok ? "OK (verified)" : "FAILED at final rename");
    return ok;
}

// Newest backup slot that opens read-only and passes the deep check (plus holds an admin
// row, if need_admin), or -1 if none does.
int db_find_good_backup(bool need_admin) {
    sqlite3_initialize();  // idempotent; boot may get here before db_init() ever succeeded
    for (int i = 0; i < BACKUP_SLOTS; i++) {
        if (!SD.exists(BACKUP_PATHS[i])) continue;

        char vfs_path[32];
        snprintf(vfs_path, sizeof(vfs_path), "/sd%s", BACKUP_PATHS[i]);
        sqlite3 *bak = nullptr;
        bool good = false;
        if (sqlite3_open_v2(vfs_path, &bak, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK && deep_check(bak)) {
            good = true;
            if (need_admin) {
                sqlite3_stmt *stmt;
                good = false;
                if (sqlite3_prepare_v2(bak, "SELECT 1 FROM users WHERE admin = 1 LIMIT 1;", -1, &stmt, nullptr) == SQLITE_OK) {
                    good = sqlite3_step(stmt) == SQLITE_ROW;
                    sqlite3_finalize(stmt);
                }
            }
        }
        if (bak) sqlite3_close(bak);
        Serial.printf("[DB] backup slot %d (%s): %s\n", i, BACKUP_PATHS[i], good ? "good" : "skipped");
        if (good) return i;
    }
    return -1;
}

// Verified copy of one backup slot over the live DB. The DB handle must be closed.
bool db_restore_backup_slot(int slot) {
    if (slot < 0 || slot >= BACKUP_SLOTS || !SD.exists(BACKUP_PATHS[slot])) return false;
    // A journal left by the old pos.db belongs to that file, not this one -- if it were
    // replayed onto the restored copy it would corrupt it.
    SD.remove(DB_JOURNAL_SD);
    bool ok = copy_verified(BACKUP_PATHS[slot], RESTORE_TMP, DB_PATH_SD);
    Serial.printf("[DB] restore from slot %d (%s) %s\n", slot, BACKUP_PATHS[slot], ok ? "OK (verified)" : "FAILED");
    return ok;
}

// Restores the newest backup that passes the deep check.
bool db_restore_from_backup() {
    int slot = db_find_good_backup(false);
    if (slot < 0) {
        Serial.println("[DB] no backup passed the deep check, nothing to restore from");
        return false;
    }
    return db_restore_backup_slot(slot);
}

// Runs the deep check on the live DB.
bool db_sanity_check() {
    return deep_check(_db);
}

// Runs one COUNT(*) query, or -1 if it couldn't run.
static int count_rows(const char *sql) {
    sqlite3_stmt *stmt;
    int n = -1;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return n;
}

// See db.h. Each rule is one COUNT(*) of offending rows.
int db_consistency_check(char *summary, size_t summary_len) {
    struct Rule { const char *name; const char *sql; };
    static const Rule RULES[] = {
        { "empty_sales",
          "SELECT COUNT(*) FROM checkouts c WHERE NOT EXISTS "
          "(SELECT 1 FROM checkout_items ci WHERE ci.checkout_id = c.id);" },
        { "orphan_lines",
          "SELECT COUNT(*) FROM checkout_items ci WHERE NOT EXISTS "
          "(SELECT 1 FROM checkouts c WHERE c.id = ci.checkout_id);" },
        { "lines_missing_item",
          "SELECT COUNT(*) FROM checkout_items ci WHERE NOT EXISTS "
          "(SELECT 1 FROM items i WHERE i.id = ci.item_id);" },
        { "barcodes_missing_item",
          "SELECT COUNT(*) FROM item_upcs u WHERE NOT EXISTS "
          "(SELECT 1 FROM items i WHERE i.id = u.item_id);" },
        { "sales_missing_user",
          "SELECT COUNT(*) FROM checkouts c WHERE NOT EXISTS "
          "(SELECT 1 FROM users u WHERE u.id = c.user_id);" },
        { "negative_prices",
          "SELECT COUNT(*) FROM items WHERE price_cents < 0;" },
    };

    summary[0] = '\0';
    if (!_db) {
        snprintf(summary, summary_len, "no_db");
        return 1;
    }
    int problems = 0;

    // total_mismatch, done in C: this build refuses a correlated scalar subquery like
    // "WHERE total <> (SELECT SUM(...) WHERE checkout_id = c.id)" at prepare time (found
    // 2026-10-02 on the real card -- 4th silently-unsupported feature, see
    // feedback-no-upsert-syntax). LEFT JOIN + GROUP BY is the shape the web Report page
    // already runs on hardware.
    {
        sqlite3_stmt *stmt;
        int mismatched = -1;
        if (sqlite3_prepare_v2(_db,
                "SELECT c.total_price_cents, COALESCE(SUM(ci.price_cents * ci.quantity), 0) "
                "FROM checkouts c LEFT JOIN checkout_items ci ON ci.checkout_id = c.id GROUP BY c.id;",
                -1, &stmt, nullptr) == SQLITE_OK) {
            mismatched = 0;
            int rc;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                if (sqlite3_column_int(stmt, 0) != sqlite3_column_int(stmt, 1)) mismatched++;
            }
            if (rc != SQLITE_DONE) mismatched = -1;
            sqlite3_finalize(stmt);
        }
        if (mismatched < 0) Serial.printf("[DB] consistency total_mismatch failed: %s\n", sqlite3_errmsg(_db));
        if (mismatched != 0) {
            problems++;
            if (mismatched < 0) snprintf(summary, summary_len, "total_mismatch=query_failed");
            else                snprintf(summary, summary_len, "total_mismatch=%d", mismatched);
        }
    }

    for (const Rule &r : RULES) {
        int n = count_rows(r.sql);
        if (n < 0) Serial.printf("[DB] consistency %s failed: %s\n", r.name, sqlite3_errmsg(_db));
        if (n == 0) continue;
        problems++;
        size_t len = strlen(summary);
        if (n < 0) snprintf(summary + len, summary_len - len, "%s%s=query_failed", len ? "," : "", r.name);
        else       snprintf(summary + len, summary_len - len, "%s%s=%d", len ? "," : "", r.name, n);
    }
    if (problems == 0) snprintf(summary, summary_len, "ok");
    Serial.printf("[DB] consistency check: %s\n", summary);
    return problems;
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

// ── boot cleanup ─────────────────────────────────────────────────────────────

// Removes leftover scratch files (see db.h). Safe at boot only -- nothing is mid-use yet.
int sd_cleanup_temp_files() {
    static const char *TEMP_FILES[] = { BACKUP_TMP, RESTORE_TMP, SD_TEST_PATH, REPORT_TMP_PATH };
    int removed = 0;
    for (const char *path : TEMP_FILES) {
        if (SD.exists(path) && SD.remove(path)) {
            Serial.printf("[SD] removed leftover %s\n", path);
            removed++;
        }
    }
    return removed;
}
