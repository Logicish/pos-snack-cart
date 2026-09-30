#include "items.h"
#include "db.h"
#include <Arduino.h>
#include <sqlite3.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the product catalog declared in items.h -- CRUD on the items
            table plus the item_upcs many-UPCs-to-one-item linking table.
*/

static Item _cache;  // filled in by items_get(), overwritten on the next call

// Logs a query-prepare failure with its real SQLite error text -- items.cpp used to fail
// these silently, which made a real on-device query failure (e.g. a column genuinely
// missing after a migration, see db.cpp's items.hidden verification) indistinguishable
// from "no rows" until someone pulled the SD card and inspected pos.db directly. Every
// caller of this still returns nullptr/0/false as before -- this only adds visibility.
static void log_prepare_fail(const char *fn) {
    Serial.printf("[ITEMS] %s: query prepare failed: %s\n", fn, sqlite3_errmsg(db_handle()));
}

// items_init()'s hardcoded 8-item starter catalog (Chips/Soda/etc.) removed 2026-09-29 --
// useful for early testing, but a blank card set up in the field would have shown fake
// items for sale. A fresh DB now starts with an empty catalog; the admin adds real items.

// Returns the total number of rows in the items table (or just the visible ones -- see items.h).
int items_count(bool include_hidden) {
    if (!db_handle()) return 0;

    sqlite3_stmt *stmt;
    int count = 0;
    const char *sql = include_hidden
        ? "SELECT COUNT(*) FROM items;"
        : "SELECT COUNT(*) FROM items WHERE hidden=0;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    } else {
        log_prepare_fail("items_count");
    }
    return count;
}

// Returns the item at alphabetical position `index` (0-based) among either all items or
// just the visible ones (see items.h), or nullptr if out of range.
const Item *items_get(int index, bool include_hidden) {
    if (!db_handle()) return nullptr;

    sqlite3_stmt *stmt;
    const Item *result = nullptr;
    // Alphabetical, case-insensitive — this feeds every on-device catalog list (Browse,
    // POS Manual Entry, Restock, Inventory, Add/Attach picker); insertion order (id)
    // is useless to navigate once the catalog is ~100 items. Callers only need index→row
    // to stay stable for the life of a screen, which it does (no concurrent writes).
    const char *sql = include_hidden
        ? "SELECT id, name, price_cents, stocked, hidden FROM items ORDER BY name COLLATE NOCASE LIMIT 1 OFFSET ?;"
        : "SELECT id, name, price_cents, stocked, hidden FROM items WHERE hidden=0 ORDER BY name COLLATE NOCASE LIMIT 1 OFFSET ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, index);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            _cache.id = sqlite3_column_int(stmt, 0);
            strncpy(_cache.name, (const char *)sqlite3_column_text(stmt, 1), ITEM_NAME_LEN - 1);
            _cache.name[ITEM_NAME_LEN - 1] = '\0';
            _cache.price_cents = sqlite3_column_int(stmt, 2);
            _cache.stocked = sqlite3_column_int(stmt, 3);
            _cache.hidden = sqlite3_column_int(stmt, 4) != 0;
            result = &_cache;
        }
        sqlite3_finalize(stmt);
    } else {
        log_prepare_fail("items_get");
    }
    return result;
}

// Looks up one item by its database id.
//
// Joins through item_upcs (2026-08-25) rather than items.upc directly — an item can be
// linked to several real barcodes (see db.cpp's item_upcs comment), any of which should
// resolve to the same shared row.
const Item *items_get_by_id(int item_id) {
    if (!db_handle()) return nullptr;

    sqlite3_stmt *stmt;
    const Item *result = nullptr;
    const char *sql = "SELECT id, name, price_cents, stocked, hidden FROM items WHERE id = ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, item_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            _cache.id = sqlite3_column_int(stmt, 0);
            strncpy(_cache.name, (const char *)sqlite3_column_text(stmt, 1), ITEM_NAME_LEN - 1);
            _cache.name[ITEM_NAME_LEN - 1] = '\0';
            _cache.price_cents = sqlite3_column_int(stmt, 2);
            _cache.stocked = sqlite3_column_int(stmt, 3);
            _cache.hidden = sqlite3_column_int(stmt, 4) != 0;
            result = &_cache;
        }
        sqlite3_finalize(stmt);
    } else {
        log_prepare_fail("items_get_by_id");
    }
    return result;
}

// Updates an item's price and stock count (used by Restock/Inventory/the web Items page).
bool items_update(int item_id, int price_cents, int stocked) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    bool ok = false;
    const char *sql = "UPDATE items SET price_cents=?, stocked=? WHERE id=?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, price_cents);
        sqlite3_bind_int(stmt, 2, stocked);
        sqlite3_bind_int(stmt, 3, item_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    return ok;
}

// Sets (or clears) an item's hidden flag -- see items.h/db.cpp for what this does and doesn't
// affect. Reversible, unlike items_delete() -- no confirm needed at the call site.
bool items_set_hidden(int item_id, bool hidden) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    bool ok = false;
    const char *sql = "UPDATE items SET hidden=? WHERE id=?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, hidden ? 1 : 0);
        sqlite3_bind_int(stmt, 2, item_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    return ok;
}

// Creates a brand-new item row. Returns the new id, or -1 on failure/blank name.
int items_create(const char *name, int price_cents, int stocked) {
    if (!db_handle() || !name || name[0] == '\0') return -1;

    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO items (name, price_cents, stocked) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, price_cents);
    sqlite3_bind_int(stmt, 3, stocked);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) return -1;

    return (int)sqlite3_last_insert_rowid(db_handle());
}

// Links a UPC to an item. Returns which of the four ItemUpcLinkResult outcomes happened
// (see items.h) -- checks for an existing link first rather than trusting an UPSERT
// (this SQLite build silently no-ops on ON CONFLICT...DO UPDATE, see project memory
// feedback-no-upsert-syntax).
ItemUpcLinkResult item_upcs_link(int item_id, const char *upc) {
    if (!db_handle() || !upc || upc[0] == '\0') return ITEM_UPC_ERROR;

    sqlite3_stmt *stmt;
    int existing_item = -1;
    if (sqlite3_prepare_v2(db_handle(), "SELECT item_id FROM item_upcs WHERE upc = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, upc, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) existing_item = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (existing_item == item_id) return ITEM_UPC_ALREADY_HERE;
    if (existing_item >= 0)       return ITEM_UPC_COLLISION;

    // Not an UPSERT — plain INSERT is fine here, we've already confirmed above this upc
    // isn't linked to anything (this is check-then-branch, same discipline as the rest of
    // this codebase's DB writes; see feedback-no-upsert-syntax).
    if (sqlite3_prepare_v2(db_handle(), "INSERT INTO item_upcs (item_id, upc) VALUES (?, ?);", -1, &stmt, nullptr) != SQLITE_OK) {
        return ITEM_UPC_ERROR;
    }
    sqlite3_bind_int(stmt, 1, item_id);
    sqlite3_bind_text(stmt, 2, upc, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? ITEM_UPC_LINKED : ITEM_UPC_ERROR;
}

// Fills out[] with up to `max` UPCs linked to item_id, returns how many were written.
int items_get_upcs(int item_id, char out[][UPC_LEN], int max) {
    if (!db_handle()) return 0;

    sqlite3_stmt *stmt;
    int n = 0;
    const char *sql = "SELECT upc FROM item_upcs WHERE item_id = ? ORDER BY id;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, item_id);
        while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *upc = (const char *)sqlite3_column_text(stmt, 0);
            strncpy(out[n], upc ? upc : "", UPC_LEN - 1);
            out[n][UPC_LEN - 1] = '\0';
            n++;
        }
        sqlite3_finalize(stmt);
    }
    return n;
}

// Removes one UPC's link to an item. Returns true if a row was actually removed.
bool item_upcs_unlink(int item_id, const char *upc) {
    if (!db_handle() || !upc || upc[0] == '\0') return false;

    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM item_upcs WHERE item_id = ? AND upc = ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, item_id);
    sqlite3_bind_text(stmt, 2, upc, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    int changed = sqlite3_changes(db_handle());
    sqlite3_finalize(stmt);
    return ok && changed > 0;
}

// Deletes an item's UPC links, then the item itself. See items.h for the
// checkout_items-history guard reasoning.
bool items_delete(int item_id) {
    if (!db_handle()) return false;

    sqlite3_stmt *stmt;
    // Refuse up front if the item has sale history -- checked BEFORE clearing its UPCs.
    // Until 2026-09-29 the UPCs were cleared first and the item delete then failed on the
    // foreign key, so a refused delete still silently stripped the item's barcodes.
    int history = 0;
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM checkout_items WHERE item_id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, item_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) history = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (history > 0) return false;

    // UPC links are just barcode metadata, not historical data -- safe to clear before
    // touching the item row itself.
    if (sqlite3_prepare_v2(db_handle(), "DELETE FROM item_upcs WHERE item_id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, item_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // checkout_items.item_id REFERENCES items(id), foreign_keys=ON, no CASCADE -- deleting
    // an item with real sale history fails here (step() != SQLITE_DONE) rather than
    // silently orphaning that history.
    if (sqlite3_prepare_v2(db_handle(), "DELETE FROM items WHERE id=?;", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, item_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// Looks up one item by any of its linked UPCs. ALWAYS excludes hidden items (see items.h)
// -- this is the one function every customer-facing scan (POS checkout add, Price Check)
// and every admin scan-to-open (Restock, Add/Attach) funnels through, so filtering here
// once is what makes a hidden item's barcode read as "not recognized" everywhere without
// needing each call site to remember to check it separately.
const Item *items_find_by_upc(const char *upc) {
    if (!db_handle() || !upc || upc[0] == '\0') return nullptr;

    sqlite3_stmt *stmt;
    const Item *result = nullptr;
    const char *sql =
        "SELECT i.id, i.name, i.price_cents, i.stocked, i.hidden "
        "FROM items i JOIN item_upcs u ON u.item_id = i.id "
        "WHERE u.upc = ? AND i.hidden = 0;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, upc, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            _cache.id = sqlite3_column_int(stmt, 0);
            strncpy(_cache.name, (const char *)sqlite3_column_text(stmt, 1), ITEM_NAME_LEN - 1);
            _cache.name[ITEM_NAME_LEN - 1] = '\0';
            _cache.price_cents = sqlite3_column_int(stmt, 2);
            _cache.stocked = sqlite3_column_int(stmt, 3);
            _cache.hidden = sqlite3_column_int(stmt, 4) != 0;
            result = &_cache;
        }
        sqlite3_finalize(stmt);
    } else {
        log_prepare_fail("items_find_by_upc");
    }
    return result;
}
