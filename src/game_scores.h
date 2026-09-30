#pragma once
#include <stddef.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Per-user best scores for the Extras minigames, shared by every game via a
            short game key (GAME_KEY_* below). Identity comes from Extras' badge gate
            (screen_extras_current_user_id()).
*/

#define GAME_KEY_LAGGY_FISH "laggy_fish"

// This user's best for `game`, 0 if they've never finished a run (or the DB is down).
int game_scores_get_best(int user_id, const char *game);

// Records a finished run. Only writes when `score` beats the user's stored best (check-
// then-branch, not UPSERT -- see feedback-no-upsert-syntax). Returns true if this was a
// new personal best AND it saved.
bool game_scores_submit(int user_id, const char *game, int score);

// The cart-wide top score for `game`, with the holder's display name ("FIRST L.").
// Returns false (outputs untouched) if nobody has a score yet.
bool game_scores_get_top(const char *game, int *score, char *name, size_t name_len);
