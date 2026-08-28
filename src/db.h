#pragma once
#include <sqlite3.h>
#include <stddef.h>

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
