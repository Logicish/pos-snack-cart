#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Generic read-only table browser -- shared column-metadata rendering for
            every DB table, reached from the DB submenu.
*/

// Table identifiers for screen_db_view.cpp — shared with screen_db_menu.cpp so its menu
// order and screen_db_view.cpp's per-table SQL/title stay in sync (same index into both).
enum DbTable {
    DB_TABLE_USERS,
    DB_TABLE_ITEMS,
    DB_TABLE_CHECKOUTS,
    DB_TABLE_PAYMENT_METHODS,
    DB_TABLE_CONFIG,
    DB_TABLE_COUNT,
};

// Generic read-only, scrollable dump of every column in every row of the given table
// (SELECT * under the hood -- column names/values rendered generically via SQLite's own
// column metadata, not hand-written per table, so this works for any table without
// per-table display code). Diagnostic only, "for me mainly" -- raw column values are
// shown as-is, no special-casing. Back returns to screen_db_menu.cpp.
void screen_db_view_push(DbTable table);
