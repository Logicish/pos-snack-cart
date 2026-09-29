#include "screen_laggy_fish.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Laggy Fish minigame declared in screen_laggy_fish.h.
  Notes---- A splash/rules screen, horizontal-only D-pad fish movement (fixed vertical
            position, no up/down -- kept deliberately simple), drifting point/hazard
            glyphs rising from the bottom, and win/game-over end screens (WIN_SCORE
            points reached, or lives run out). Movement is still driven straight off
            button press/repeat events rather than its own timer -- only the drifting
            items need a timer, since they move on their own. See project memory,
            project-laggy-fish-design-notes, for why the point/hazard glyphs are plain
            fixed-width characters rather than art assets.
*/

LV_IMG_DECLARE(fish_left);
LV_IMG_DECLARE(fish_right);

// ── layout ───────────────────────────────────────────────────────────────────
#define FISH_W       120
#define FISH_H        80
#define SCORE_H       30   // strip below the main header for the Lives/Score readout
#define FOOTER_H      52
#define MOVE_STEP     16   // px per press/repeat tick -- chunky on purpose

#define PLAY_TOP     (HDR_H + SCORE_H)
#define PLAY_BOTTOM  (SCREEN_H - FOOTER_H)
#define X_MIN         0
#define X_MAX        (SCREEN_W - FISH_W)

// Fish sits fixed ~3/4 of the way toward the top of the play field and never moves
// vertically -- items drift UP past it (matches the screensaver's rising bubbles), so a
// high, fixed perch gives the player a consistent interception line to aim for.
#define FISH_Y_FRAC   0.25f

// Sparkle ring around the fish on the win screen -- plain "*" glyphs at fixed offsets
// from panel center, same cheap-fixed-width-glyph reasoning as the in-game item glyphs
// (see project memory, project-laggy-fish-design-notes) rather than a new art asset.
// Y offsets nudged down ~40px (about 2 lines) 2026-09-15 after a first look on hardware
// showed the ring sitting too high relative to the fish.
#define WIN_SPARKLE_COUNT 6
static const lv_point_t WIN_SPARKLE_OFFSETS[WIN_SPARKLE_COUNT] = {
    { -80, -20 }, { 80, -20 },
    { -95,  20 }, { 95,  20 },
    { -70,  75 }, { 70,  75 },
};

// ── items (drifting point/hazard glyphs) ────────────────────────────────────
#define MAX_ITEMS         6
#define ITEM_TICK_MS     250   // deliberately slow/chunky, matches the "lag is the game" feel
#define ITEM_SPAWN_PCT    40   // % chance per tick of spawning a new item, if a slot is free
#define ITEM_W             20  // rough glyph bounding box, used for spawn range + hit test
#define ITEM_H             28

// Difficulty ramp: items start slow and speed up the longer a run goes, rather than a
// fixed rise speed for the whole game. Step size only (not the tick interval) so the
// timer callback rate never changes -- one less moving part.
#define ITEM_STEP_START    10   // px risen per tick, start of a run
#define ITEM_STEP_MAX      30   // px risen per tick, ramp ceiling
#define ITEM_STEP_RAMP_MS 8000  // +ITEM_STEP_INC every this many ms of play
#define ITEM_STEP_INC       3

// Minimum horizontal separation enforced between any two currently-active items
// (regardless of their height) so there's always a fish-width-ish gap to dodge into --
// without this, two items spawned close together in x can arrive at the fish's row at
// the same time with no room to escape either side, per real hardware testing.
#define ITEM_MIN_GAP_X    FISH_W
#define ITEM_SPAWN_ATTEMPTS 6

enum ItemType { ITEM_O, ITEM_DOLLAR, ITEM_X, ITEM_STAR, ITEM_TYPE_COUNT };

struct FishItem {
    lv_obj_t *label;
    int16_t   x, y;
    ItemType  type;
    bool      active;
};

static const char *ITEM_GLYPH[ITEM_TYPE_COUNT] = { "O", "$", "X", "*" };
// Points for O/$/, life cost (as a negative) for X/*  -- looked up by type below.
static const int ITEM_POINTS[ITEM_TYPE_COUNT] = { 1, 5, 0, 0 };
static bool item_is_hazard(ItemType t) { return t == ITEM_X || t == ITEM_STAR; }

enum GameState { ST_SPLASH, ST_PLAYING, ST_GAMEOVER, ST_WIN };

static lv_obj_t *_scr;

static lv_obj_t *_splash_panel;

static lv_obj_t *_play_panel;
static lv_obj_t *_fish_img;
static lv_obj_t *_lives_label;
static lv_obj_t *_score_label;
static FishItem   _items[MAX_ITEMS];

static lv_obj_t *_gameover_panel;
static lv_obj_t *_gameover_label;

static lv_obj_t *_win_panel;
static lv_obj_t *_win_label;
static lv_obj_t *_win_score_label;
static lv_obj_t *_win_fish_img;
static lv_obj_t *_win_sparkles[WIN_SPARKLE_COUNT];

static lv_timer_t *_timer;
static GameState    _state;

static int16_t _fish_x;
static int16_t _fish_y;   // fixed once computed; kept as a var only to avoid recomputing
static bool    _facing_right;
static int     _score;
static int     _lives;
static uint32_t _game_start_ms;

#define LIVES_START 3
#define WIN_SCORE   100   // first cut, easy to retune once actually played to it

static void enter_state(GameState s);

// ── fish ─────────────────────────────────────────────────────────────────────
static void redraw_fish() {
    lv_img_set_src(_fish_img, _facing_right ? &fish_right : &fish_left);
    lv_obj_set_pos(_fish_img, _fish_x, _fish_y);
}

static void cb_move_left() {
    _facing_right = false;
    _fish_x -= MOVE_STEP;
    if (_fish_x < X_MIN) _fish_x = X_MIN;
    redraw_fish();
}

static void cb_move_right() {
    _facing_right = true;
    _fish_x += MOVE_STEP;
    if (_fish_x > X_MAX) _fish_x = X_MAX;
    redraw_fish();
}

// ── items ────────────────────────────────────────────────────────────────────
static void item_hide(FishItem &it) {
    lv_obj_add_flag(it.label, LV_OBJ_FLAG_HIDDEN);
    it.active = false;
}

// True if candidate_x is at least ITEM_MIN_GAP_X away from every currently-active item's
// x, regardless of that item's height -- guarantees a dodge-able gap opens up on one side
// or the other by the time this item reaches the fish's row.
static bool x_clear_of_active_items(int16_t candidate_x) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (!_items[i].active) continue;
        if (abs((int)candidate_x - (int)_items[i].x) < ITEM_MIN_GAP_X) return false;
    }
    return true;
}

static void item_spawn() {
    int slot = -1;
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (!_items[i].active) { slot = i; break; }
    }
    if (slot < 0) return;  // pool full? skip spawning this tick, no big deal

    int16_t x = -1;
    for (int attempt = 0; attempt < ITEM_SPAWN_ATTEMPTS; attempt++) {
        int16_t candidate = random(0, SCREEN_W - ITEM_W);
        if (x_clear_of_active_items(candidate)) { x = candidate; break; }
    }
    if (x < 0) return;  // couldn't find a clear enough spot this tick -- try again next tick

    FishItem &it = _items[slot];
    it.type   = (ItemType)random(ITEM_TYPE_COUNT);
    it.x      = x;
    it.y      = PLAY_BOTTOM - ITEM_H;
    it.active = true;

    lv_label_set_text(it.label, ITEM_GLYPH[it.type]);
    // Both hazards render red now (was X=red/*=orange) -- one glance should be enough to
    // tell good from bad, per the owner's request, rather than needing to recognize two
    // different "danger" colors.
    lv_obj_set_style_text_color(it.label,
        lv_color_hex(item_is_hazard(it.type) ? C_RED :
                     it.type == ITEM_O ? C_YELLOW : C_GREEN),
        LV_PART_MAIN);
    lv_obj_set_pos(it.label, it.x, it.y);
    lv_obj_clear_flag(it.label, LV_OBJ_FLAG_HIDDEN);
}

// Current per-tick rise speed for this run -- ramps from ITEM_STEP_START up to
// ITEM_STEP_MAX the longer the run goes, rather than a single fixed-forever speed.
static int16_t current_item_step() {
    uint32_t elapsed = millis() - _game_start_ms;
    int32_t  step = ITEM_STEP_START + (elapsed / ITEM_STEP_RAMP_MS) * ITEM_STEP_INC;
    if (step > ITEM_STEP_MAX) step = ITEM_STEP_MAX;
    return (int16_t)step;
}

static void refresh_score_labels() {
    char buf[24];
    snprintf(buf, sizeof(buf), "Lives: %d", _lives);
    lv_label_set_text(_lives_label, buf);
    snprintf(buf, sizeof(buf), "Score: %d", _score);
    lv_label_set_text(_score_label, buf);
}

// One item vs. the fish's current bounding box.
static bool item_hits_fish(const FishItem &it) {
    return it.x + ITEM_W >= _fish_x && it.x <= _fish_x + FISH_W &&
           it.y + ITEM_H >= _fish_y && it.y <= _fish_y + FISH_H;
}

// Advances every active item one tick: rises, checks for a catch, or clears off the top.
static void items_tick() {
    int16_t step = current_item_step();

    for (int i = 0; i < MAX_ITEMS; i++) {
        FishItem &it = _items[i];
        if (!it.active) continue;

        it.y -= step;

        if (item_hits_fish(it)) {
            if (item_is_hazard(it.type)) {
                _lives--;
            } else {
                _score += ITEM_POINTS[it.type];
            }
            refresh_score_labels();
            item_hide(it);
            if (_lives <= 0) {
                enter_state(ST_GAMEOVER);
                return;  // panel just got torn down/hidden -- stop touching this tick's items
            }
            if (_score >= WIN_SCORE) {
                enter_state(ST_WIN);
                return;  // same reasoning as the game-over return above
            }
            continue;
        }

        if (it.y + ITEM_H < PLAY_TOP) {
            item_hide(it);  // drifted past the fish without being caught -- no penalty
            continue;
        }

        lv_obj_set_pos(it.label, it.x, it.y);
    }

    if (random(100) < ITEM_SPAWN_PCT) item_spawn();
}

static void tick_cb(lv_timer_t *) {
    items_tick();
}

// ── state transitions ────────────────────────────────────────────────────────
static void stop_timer() {
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
}

static void start_game() {
    _score = 0;
    _lives = LIVES_START;
    _game_start_ms = millis();
    _facing_right = true;
    _fish_x = (SCREEN_W - FISH_W) / 2;
    _fish_y = PLAY_TOP + (int)((PLAY_BOTTOM - PLAY_TOP - FISH_H) * FISH_Y_FRAC);
    if (_fish_y < PLAY_TOP) _fish_y = PLAY_TOP;
    redraw_fish();
    refresh_score_labels();
    for (int i = 0; i < MAX_ITEMS; i++) item_hide(_items[i]);

    enter_state(ST_PLAYING);

    stop_timer();
    _timer = lv_timer_create(tick_cb, ITEM_TICK_MS, nullptr);
}

// Back from the splash or game-over screens: leaves Laggy Fish entirely.
static void cb_exit() {
    stop_timer();
    screen_extras_return_to_list();  // 2026-09-15 -- was screen_extras_push(), which now
                                      // forces a fresh badge scan; this just returns to
                                      // the already-identified menu instead
}

// Back while actually playing: bail out immediately, same as everywhere else in the app.
static void cb_abandon() {
    stop_timer();
    screen_extras_return_to_list();
}

// Any non-Back button on the splash or game-over screens (re)starts the game.
static void cb_start_or_retry() {
    start_game();
}

static void enter_state(GameState s) {
    _state = s;

    lv_obj_add_flag(_splash_panel,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_play_panel,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_gameover_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_win_panel,      LV_OBJ_FLAG_HIDDEN);

    ButtonHandlers h;
    switch (s) {
        case ST_SPLASH:
            stop_timer();
            lv_obj_clear_flag(_splash_panel, LV_OBJ_FLAG_HIDDEN);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_retry;
            h.back = cb_exit;
            break;

        case ST_PLAYING:
            lv_obj_clear_flag(_play_panel, LV_OBJ_FLAG_HIDDEN);
            h.left  = cb_move_left;
            h.right = cb_move_right;
            h.back  = cb_abandon;
            break;

        case ST_GAMEOVER: {
            stop_timer();
            lv_obj_clear_flag(_gameover_panel, LV_OBJ_FLAG_HIDDEN);
            char buf[64];
            snprintf(buf, sizeof(buf), "GAME OVER\n\nScore: %d\n\nPress any button to play again.", _score);
            lv_label_set_text(_gameover_label, buf);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_retry;
            h.back = cb_exit;
            break;
        }

        case ST_WIN: {
            stop_timer();
            lv_obj_clear_flag(_win_panel, LV_OBJ_FLAG_HIDDEN);
            char buf[24];
            snprintf(buf, sizeof(buf), "Score: %d", _score);
            lv_label_set_text(_win_score_label, buf);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_retry;
            h.back = cb_exit;
            break;
        }
    }
    buttons_set_handlers(h);
}

// ── build ────────────────────────────────────────────────────────────────────
static void build_splash_panel() {
    _splash_panel = lv_obj_create(_scr);
    lv_obj_set_size(_splash_panel, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(_splash_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_splash_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_splash_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_splash_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(_splash_panel);
    lv_label_set_text(lbl,
        "LAGGY FISH\n\n"
        "O = 1 point\n"
        "$ = 5 points\n\n"
        "X and * cost a life\n\n"
        "Reach 100 points to win!");
    // ("Catch what you can, dodge the rest!" flavor line dropped 2026-09-15 to make room
    // for the win-condition line -- the point/hazard rows above already say the same
    // thing more concretely.)
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 260);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *legend = ui_legend(_splash_panel);
    lv_obj_set_width(legend, SCREEN_W - 28);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Any Button = Start", lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

static void build_play_panel() {
    _play_panel = lv_obj_create(_scr);
    lv_obj_set_size(_play_panel, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(_play_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_play_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_play_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_play_panel, LV_OBJ_FLAG_SCROLLABLE);

    _lives_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_lives_label, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_lives_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_lives_label, LV_ALIGN_TOP_LEFT, 4, 4);

    _score_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_score_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    _fish_img = lv_img_create(_play_panel);

    for (int i = 0; i < MAX_ITEMS; i++) {
        FishItem &it = _items[i];
        it.label = lv_label_create(_play_panel);
        lv_obj_set_style_text_font(it.label, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_add_flag(it.label, LV_OBJ_FLAG_HIDDEN);
        it.active = false;
    }

    lv_obj_t *legend = ui_legend(_play_panel);
    lv_obj_set_width(legend, SCREEN_W - 28);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    char move_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

static void build_gameover_panel() {
    _gameover_panel = lv_obj_create(_scr);
    lv_obj_set_size(_gameover_panel, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(_gameover_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_gameover_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_gameover_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_gameover_panel, LV_OBJ_FLAG_SCROLLABLE);

    _gameover_label = lv_label_create(_gameover_panel);
    lv_label_set_long_mode(_gameover_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_gameover_label, 260);
    lv_obj_set_style_text_color(_gameover_label, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(_gameover_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(_gameover_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(_gameover_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *legend = ui_legend(_gameover_panel);
    lv_obj_set_width(legend, SCREEN_W - 28);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Any Button = Retry", lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

static void build_win_panel() {
    _win_panel = lv_obj_create(_scr);
    lv_obj_set_size(_win_panel, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(_win_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_win_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_win_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_win_panel, LV_OBJ_FLAG_SCROLLABLE);

    _win_label = lv_label_create(_win_panel);
    lv_label_set_text(_win_label, "Congrats!\nYou Won Laggy Fish!!!");
    lv_label_set_long_mode(_win_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_win_label, 260);
    lv_obj_set_style_text_color(_win_label, lv_color_hex(C_YELLOW), LV_PART_MAIN);
    lv_obj_set_style_text_font(_win_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(_win_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(_win_label, LV_ALIGN_CENTER, 0, -130);

    // Sparkle ring first, fish image on top -- draw order matters so the fish isn't
    // hidden behind a sparkle that happens to overlap its bounding box.
    for (int i = 0; i < WIN_SPARKLE_COUNT; i++) {
        lv_obj_t *lbl = lv_label_create(_win_panel);
        lv_label_set_text(lbl, "*");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_YELLOW), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_CENTER, WIN_SPARKLE_OFFSETS[i].x, WIN_SPARKLE_OFFSETS[i].y - 20);
        _win_sparkles[i] = lbl;
    }

    _win_fish_img = lv_img_create(_win_panel);
    lv_img_set_src(_win_fish_img, &fish_right);
    lv_obj_align(_win_fish_img, LV_ALIGN_CENTER, 0, -20);

    _win_score_label = lv_label_create(_win_panel);
    lv_obj_set_style_text_color(_win_score_label, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_win_score_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_win_score_label, LV_ALIGN_CENTER, 0, 90);

    lv_obj_t *legend = ui_legend(_win_panel);
    lv_obj_set_width(legend, SCREEN_W - 28);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Any Button = Retry", lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

// Loads the Laggy Fish screen, always starting back at the splash/rules screen.
void screen_laggy_fish_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        build_splash_panel();
        build_play_panel();
        build_gameover_panel();
        build_win_panel();
    }

    stop_timer();
    header_set_visible(true);
    header_set_title("LAGGY FISH");
    enter_state(ST_SPLASH);

    lv_scr_load(_scr);
}
