#include "checkouts.h"
#include "db.h"
#include <sqlite3.h>
#include <time.h>
#include <Arduino.h>

// No RTC/NTP wired yet — time(nullptr) is a placeholder that will read real wall-clock
// time automatically once RTC/NTP lands, same interim convention webserver.cpp already
// uses for checkouts.cleared_at.

static bool exec(sqlite3 *db, const char *sql) {
    char *err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        Serial.printf("[CHECKOUT] %s\n", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

int checkout_save(int user_id, const CheckoutLine *lines, int line_count, int total_cents) {
    sqlite3 *db = db_handle();
    if (!db || line_count <= 0) return -1;

    if (!exec(db, "BEGIN;")) return -1;

    sqlite3_stmt *stmt;
    const char *insert_checkout_sql =
        "INSERT INTO checkouts (user_id, total_price_cents, created_at) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db, insert_checkout_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        exec(db, "ROLLBACK;");
        return -1;
    }
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, total_cents);
    sqlite3_bind_int(stmt, 3, (int)time(nullptr));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        exec(db, "ROLLBACK;");
        return -1;
    }
    int checkout_id = (int)sqlite3_last_insert_rowid(db);

    const char *insert_item_sql =
        "INSERT INTO checkout_items (checkout_id, item_id, price_cents, quantity) VALUES (?, ?, ?, ?);";
    // Decrementing items.stocked here (not clamped at 0, deliberately — see items.stocked's
    // schema comment) is what lets the owner eventually pull a restock report without
    // physically counting inventory. Same transaction as the checkout itself, so a failure
    // anywhere rolls back the stock change along with everything else — no drift between
    // "what sold" and "what's left."
    const char *decrement_stock_sql = "UPDATE items SET stocked = stocked - ? WHERE id = ?;";
    for (int i = 0; i < line_count; i++) {
        if (sqlite3_prepare_v2(db, insert_item_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            exec(db, "ROLLBACK;");
            return -1;
        }
        sqlite3_bind_int(stmt, 1, checkout_id);
        sqlite3_bind_int(stmt, 2, lines[i].item_id);
        sqlite3_bind_int(stmt, 3, lines[i].price_cents);
        sqlite3_bind_int(stmt, 4, lines[i].quantity);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            exec(db, "ROLLBACK;");
            return -1;
        }

        if (sqlite3_prepare_v2(db, decrement_stock_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            exec(db, "ROLLBACK;");
            return -1;
        }
        sqlite3_bind_int(stmt, 1, lines[i].quantity);
        sqlite3_bind_int(stmt, 2, lines[i].item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            exec(db, "ROLLBACK;");
            return -1;
        }
    }

    if (!exec(db, "COMMIT;")) return -1;

    Serial.printf("[CHECKOUT] Saved checkout %d for user %d: %d lines, $%d.%02d\n",
                  checkout_id, user_id, line_count, total_cents / 100, total_cents % 100);
    return checkout_id;
}

int checkouts_get_outstanding(OutstandingCheckout *out, int max) {
    sqlite3 *db = db_handle();
    if (!db || max <= 0) return 0;

    sqlite3_stmt *stmt;
    int n = 0;
    const char *sql =
        "SELECT id, user_id, total_price_cents, created_at FROM checkouts "
        "WHERE cleared_at IS NULL ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
            out[n].id                = sqlite3_column_int(stmt, 0);
            out[n].user_id           = sqlite3_column_int(stmt, 1);
            out[n].total_price_cents = sqlite3_column_int(stmt, 2);
            out[n].created_at        = sqlite3_column_int64(stmt, 3);
            n++;
        }
        sqlite3_finalize(stmt);
    }
    return n;
}

bool checkouts_clear(int checkout_id) {
    sqlite3 *db = db_handle();
    if (!db) return false;

    sqlite3_stmt *stmt;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "UPDATE checkouts SET cleared_at=? WHERE id=? AND cleared_at IS NULL;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(nullptr));
        sqlite3_bind_int(stmt, 2, checkout_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(stmt);
    }
    return ok;
}

int checkouts_get_balance_cents(int user_id) {
    sqlite3 *db = db_handle();
    if (!db) return 0;

    sqlite3_stmt *stmt;
    int total = 0;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(total_price_cents),0) FROM checkouts WHERE user_id=? AND cleared_at IS NULL;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, user_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return total;
}
