#include "items.h"
#include "db.h"
#include <sqlite3.h>
#include <string.h>

static Item _cache;  // filled in by items_get(), overwritten on the next call

static void seed(const char *name, int price_cents, int stocked) {
    if (!db_handle()) return;

    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR IGNORE INTO items (name, price_cents, stocked) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, price_cents);
    sqlite3_bind_int(stmt, 3, stocked);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void items_init() {
    if (items_count() > 0) return;  // already seeded (or DB unavailable) — don't duplicate

    seed("Chips",        150, 20);
    seed("Candy Bar",    125, 20);
    seed("Soda",         150, 20);
    seed("Water",        100, 20);
    seed("Granola Bar",  125, 20);
    seed("Gum",           75, 20);
    seed("Cookies",      175, 20);
    seed("Energy Drink", 250, 20);
}

int items_count() {
    if (!db_handle()) return 0;

    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM items;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

const Item *items_get(int index) {
    if (!db_handle()) return nullptr;

    sqlite3_stmt *stmt;
    const Item *result = nullptr;
    // Alphabetical, case-insensitive — this feeds every on-device catalog list (Browse,
    // POS Manual Entry, Restock, Inventory Count, Add/Attach picker); insertion order (id)
    // is useless to navigate once the catalog is ~100 items. Callers only need index→row
    // to stay stable for the life of a screen, which it does (no concurrent writes).
    const char *sql = "SELECT id, name, price_cents, stocked FROM items ORDER BY name COLLATE NOCASE LIMIT 1 OFFSET ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, index);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            _cache.id = sqlite3_column_int(stmt, 0);
            strncpy(_cache.name, (const char *)sqlite3_column_text(stmt, 1), ITEM_NAME_LEN - 1);
            _cache.name[ITEM_NAME_LEN - 1] = '\0';
            _cache.price_cents = sqlite3_column_int(stmt, 2);
            _cache.stocked = sqlite3_column_int(stmt, 3);
            result = &_cache;
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

// Joins through item_upcs (2026-08-25) rather than items.upc directly — an item can be
// linked to several real barcodes (see db.cpp's item_upcs comment), any of which should
// resolve to the same shared row.
const Item *items_get_by_id(int item_id) {
    if (!db_handle()) return nullptr;

    sqlite3_stmt *stmt;
    const Item *result = nullptr;
    const char *sql = "SELECT id, name, price_cents, stocked FROM items WHERE id = ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, item_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            _cache.id = sqlite3_column_int(stmt, 0);
            strncpy(_cache.name, (const char *)sqlite3_column_text(stmt, 1), ITEM_NAME_LEN - 1);
            _cache.name[ITEM_NAME_LEN - 1] = '\0';
            _cache.price_cents = sqlite3_column_int(stmt, 2);
            _cache.stocked = sqlite3_column_int(stmt, 3);
            result = &_cache;
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

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

const Item *items_find_by_upc(const char *upc) {
    if (!db_handle() || !upc || upc[0] == '\0') return nullptr;

    sqlite3_stmt *stmt;
    const Item *result = nullptr;
    const char *sql =
        "SELECT i.id, i.name, i.price_cents, i.stocked "
        "FROM items i JOIN item_upcs u ON u.item_id = i.id "
        "WHERE u.upc = ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, upc, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            _cache.id = sqlite3_column_int(stmt, 0);
            strncpy(_cache.name, (const char *)sqlite3_column_text(stmt, 1), ITEM_NAME_LEN - 1);
            _cache.name[ITEM_NAME_LEN - 1] = '\0';
            _cache.price_cents = sqlite3_column_int(stmt, 2);
            _cache.stocked = sqlite3_column_int(stmt, 3);
            result = &_cache;
        }
        sqlite3_finalize(stmt);
    }
    return result;
}
