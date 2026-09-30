#include "game_scores.h"
#include "db.h"
#include <sqlite3.h>
#include <time.h>
#include <stdio.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the minigame high-score table declared in game_scores.h.
  Notes---- No db_backup_now() after a new best, unlike checkouts -- a high score is
            low-stakes, and the next checkout/boot backup picks it up anyway. Keeps the
            game-over screen from stalling on a full DB file copy.
*/

// Returns this user's best score for `game`, 0 if none.
int game_scores_get_best(int user_id, const char *game) {
    sqlite3 *db = db_handle();
    if (!db || user_id < 0) return 0;

    int best = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT best FROM game_scores WHERE user_id = ? AND game = ?;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, user_id);
        sqlite3_bind_text(stmt, 2, game, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) best = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return best;
}

// Saves `score` if it beats the stored best. Returns true on a saved new best.
bool game_scores_submit(int user_id, const char *game, int score) {
    sqlite3 *db = db_handle();
    if (!db || user_id < 0 || score <= 0) return false;

    // Check-then-branch: does a row exist, and is this score higher?
    bool exists = false;
    int  best   = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT best FROM game_scores WHERE user_id = ? AND game = ?;",
                           -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, game, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) { exists = true; best = sqlite3_column_int(stmt, 0); }
    sqlite3_finalize(stmt);

    if (exists && score <= best) return false;

    const char *sql = exists
        ? "UPDATE game_scores SET best = ?, achieved_at = ? WHERE user_id = ? AND game = ?;"
        : "INSERT INTO game_scores (best, achieved_at, user_id, game) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, score);
    sqlite3_bind_int(stmt, 2, (int)time(nullptr));
    sqlite3_bind_int(stmt, 3, user_id);
    sqlite3_bind_text(stmt, 4, game, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        Serial.printf("[SCORES] save failed: %s\n", sqlite3_errmsg(db));
        return false;
    }
    return true;
}

// Top score for `game` across every user, with "FIRST L." as the holder's name.
bool game_scores_get_top(const char *game, int *score, char *name, size_t name_len) {
    sqlite3 *db = db_handle();
    if (!db) return false;

    bool found = false;
    sqlite3_stmt *stmt;
    // Oldest achieved_at wins a tie -- whoever got there first keeps the crown.
    if (sqlite3_prepare_v2(db,
            "SELECT g.best, u.first_name, u.last_name FROM game_scores g "
            "JOIN users u ON u.id = g.user_id WHERE g.game = ? "
            "ORDER BY g.best DESC, g.achieved_at ASC LIMIT 1;",
            -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, game, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *first = (const char *)sqlite3_column_text(stmt, 1);
            const char *last  = (const char *)sqlite3_column_text(stmt, 2);
            *score = sqlite3_column_int(stmt, 0);
            if (last && last[0]) snprintf(name, name_len, "%s %c.", first ? first : "", last[0]);
            else                 snprintf(name, name_len, "%s", first ? first : "");
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}
