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
bool db_backup_now();          // copies /pos.db -> /pos_backup.db
bool db_restore_from_backup(); // copies /pos_backup.db -> /pos.db; false if no backup exists

// A quick, proven-primitive sanity check (plain SELECT, no PRAGMA — see the backup note
// above for why PRAGMAs aren't trusted blind on this build). Confirms the schema is
// actually queryable, not just that the file opened. Not real corruption detection (this
// SQLite build has none available) — only catches "the DB is unreadable," used at boot to
// decide whether to fall back to the backup.
bool db_sanity_check();

// SD-card-layer check, 2026-09-14 — distinct from db_sanity_check() above, which only
// proves the DB's own schema is queryable, not that the card underneath it is actually
// writable right now. Writes a small known payload to a throwaway file, reads it back,
// verifies it matches byte-for-byte, then deletes the file. Same technique proven during
// this project's original 2026-08-17 SD bring-up, just made callable again rather than
// living only in a since-removed dev screen. A failure here means something's genuinely
// wrong with the card itself (write-protected, failing, wrong card) — not something a DB
// restore can fix, since the restore path needs the same write access this test checks.
bool sd_write_read_test();
