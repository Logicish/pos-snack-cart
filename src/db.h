#pragma once
#include <sqlite3.h>
#include <stddef.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- SD card mount + SQLite database layer -- schema creation, a generic
            config key/value table, and raw-file backup/restore/write-test
            primitives, all implemented in db.cpp.
*/

// SD mount + SQLite open + schema creation. Non-fatal on failure — caller decides
// whether to halt; the DB Check screen exists specifically to diagnose this on-device.
bool db_init();
sqlite3 *db_handle();  // nullptr if db_init() failed or hasn't run
void db_close();

// Reads one row from the config table. Returns false (out left untouched) if the key
// is missing, its value is empty, or the DB isn't available.
bool db_config_get(const char *key, char *out, size_t out_len);
// Writes/overwrites one row in the config table. `INSERT OR REPLACE` — the older,
// universally-supported conflict mechanism, not `ON CONFLICT...DO UPDATE` (see
// project memory/snack_cart_pos.md: that syntax silently no-ops on this SQLite build).
bool db_config_set(const char *key, const char *value);

// Backup/restore, 2026-09-14 — a raw file copy of pos.db to pos_backup.db, deliberately
// not a live SQLite-level backup mechanism. This SQLite build (siara-cc/Sqlite3Esp32) has
// already been caught twice silently not supporting a feature it should (UPSERT, PRAGMA
// integrity_check — see project memory feedback-no-upsert-syntax), so this sidesteps
// trusting any SQLite API for the backup itself and uses only plain SD.h file I/O instead.
// Safe to call db_backup_now() whenever there's no write in flight — TRUNCATE journal mode
// means pos.db is a complete, self-consistent snapshot the instant a COMMIT finishes and
// the journal resets, so a raw copy at that moment needs no live SQLite involvement to be
// correct. db_restore_from_backup() is meant to run BEFORE db_init() (or after db_close()),
// never while a handle is open on pos.db.
//
// 2026-10-02 rework: five rolling copies instead of one, every copy verified, and nothing
// copied or restored without passing the deep check below first.
//  - Slots: /pos_backup.db (newest -- also what Download database sends) then
//    /pos_backup1.db .. /pos_backup4.db, oldest last.
//  - db_backup_now() refuses to back up a live DB that fails the deep check (so damage
//    never rotates into the copies), writes to a temp file, compares it byte-for-byte
//    against pos.db, and only then rotates the slots and renames it into place.
//  - Restore picks the newest slot that passes the deep check (opened read-only, nothing
//    written just to look), copies it in the same verified way, and drops any stale
//    pos.db-journal so a leftover journal can't be replayed onto the restored file.
bool db_backup_now();                    // verified copy of /pos.db into the newest slot
int  db_find_good_backup(bool need_admin); // newest slot passing the deep check (and holding
                                           // an admin row, if asked), or -1. Read-only.
bool db_restore_backup_slot(int slot);   // verified copy of that slot over /pos.db; DB must be closed
bool db_restore_from_backup();           // db_find_good_backup(false) + db_restore_backup_slot()

// Deep check, 2026-10-02 (was a single COUNT(*) on users). PRAGMA integrity_check returns
// nothing on this build, so instead this reads every row and column of every table, and
// walks every index in order -- forcing SQLite to touch every page of the file, where a
// damaged page makes the read fail with an error. Also requires the core tables to exist.
// Well under a second at this cart's data size.
bool db_sanity_check();

// Data consistency check, 2026-10-02 -- the deep check above finds damaged pages; this
// finds rows that read fine but don't add up: a sale whose total isn't the sum of its
// lines, a sale with no lines, line items pointing at a missing sale or item, a barcode
// linked to a missing item, a sale for a missing person, a negative price. Report-only --
// nothing is changed. Writes "ok" or e.g. "total_mismatch=2,orphan_lines=1" to `summary`
// and returns how many kinds of problem were found.
int db_consistency_check(char *summary, size_t summary_len);

// SD-card-layer check, 2026-09-14 — distinct from db_sanity_check() above, which only
// proves the DB's own schema is queryable, not that the card underneath it is actually
// writable right now. Writes a small known payload to a throwaway file, reads it back,
// verifies it matches byte-for-byte, then deletes the file. Same technique proven during
// this project's original 2026-08-17 SD bring-up, just made callable again rather than
// living only in a since-removed dev screen. A failure here means something's genuinely
// wrong with the card itself (write-protected, failing, wrong card) — not something a DB
// restore can fix, since the restore path needs the same write access this test checks.
bool sd_write_read_test();

// Boot cleanup, 2026-10-02: deletes scratch files a power cut could have left behind (a
// half-written backup/restore copy, the SD write-test file, the report-download scratch).
// Never touches pos.db-journal -- after a power cut that journal is exactly what SQLite
// uses to repair pos.db on open. Returns how many files it removed.
int sd_cleanup_temp_files();
