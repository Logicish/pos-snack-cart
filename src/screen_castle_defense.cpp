#include "screen_castle_defense.h"
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
  Function- Implements Castle Defense, declared in screen_castle_defense.h. First real
            pass -- built, not yet hardware-tested or polished (layout constants below
            are first-guess placements, not measured against the real panel).
  Notes---- Fully event-driven, no lv_timer at all: unlike Laggy Fish, nothing here moves
            on its own between button presses, so there's nothing to tick.
*/

// ── layout (all panel-local coordinates -- panel already sits at screen y=HDR_H) ──
// Second pass, 2026-09-15 -- first look on hardware found the wall overlapping the
// footer legend and too short to visually reach the archer. Blocks shrunk (50->46) to
// buy back the room needed for a taller wall + a clean gap above the legend, rather
// than fighting for a couple of leftover pixels.
#define GRID_COLS   6
#define GRID_ROWS   6
#define CELL        46
// Left-aligned, not centered -- the panel sits slightly skewed left in the enclosure
// (see theme.h's SCREEN_W/SCREEN_H comment), so mathematically centering content in the
// 315px canvas doesn't actually look centered on the real physical mount. Left-aligning
// with a plain margin (matching the ~8px padding convention used elsewhere) reads
// straight; a centered grid read visibly off.
#define GRID_LEFT   8
#define STAGE_H     26
#define GRID_TOP    STAGE_H
#define TRACK_GAP   10   // breathing room between the grid's bottom row and the wall
#define TRACK_H     56
#define TRACK_TOP   (GRID_TOP + GRID_ROWS * CELL + TRACK_GAP)
#define MERLON_TALL   38
#define MERLON_SHORT  22
#define LEGEND_H    52
#define WALL_COLOR  0x2E2E2E  // darker than theme.h's C_DIM (0x4A4A4A) -- that's tuned
                              // for dimmed text, not a stone structure; kept local to
                              // this file rather than added to theme.h since nothing
                              // else uses it

// ── colors / difficulty ─────────────────────────────────────────────────────────
#define START_COLORS 4
#define MAX_COLORS   7   // cyan/green/orange/red/yellow/blue/purple -- retune once seen
                         // on hardware if any two read too close together
static const uint32_t BLOCK_COLORS[MAX_COLORS] = {
    C_CYAN, C_GREEN, C_ORANGE, C_RED, C_YELLOW, C_BLUE, C_PURPLE,
};

#define MAX_FLOOD (GRID_ROWS * GRID_COLS)
#define MAX_MISSES 3

enum GameState { ST_SPLASH, ST_PLAYING, ST_STAGE_CLEAR, ST_GAMEOVER };
static GameState _state;

static lv_obj_t *_scr;

static lv_obj_t *_splash_panel;

static lv_obj_t *_play_panel;
static lv_obj_t *_stage_label;
static lv_obj_t *_miss_label;
static lv_obj_t *_bullet_swatch[2];  // [0] = about to fire, [1] = next up (preview only)
static lv_obj_t *_archer_label;
static lv_obj_t *_cell_objs[GRID_ROWS][GRID_COLS];

static lv_obj_t *_stage_clear_panel;
static lv_obj_t *_stage_clear_label;

static lv_obj_t *_gameover_panel;
static lv_obj_t *_gameover_label;

// -1 = empty, else an index into BLOCK_COLORS.
static int8_t _grid[GRID_ROWS][GRID_COLS];
static int8_t _bullet_color[2];  // [0] fires next, [1] is the preview of what follows
static int    _shooter_col;
static int    _stage;
static int    _active_colors;
static int    _misses;

static void enter_state(GameState s);
static void cb_start_or_continue();
static void cb_exit();
static void cb_move_left();
static void cb_move_right();
static void fire();

// ── board helpers ────────────────────────────────────────────────────────────────
static bool grid_is_empty() {
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
            if (_grid[r][c] >= 0) return false;
    return true;
}

// Collects every cell 4-directionally connected to (r,c) sharing its color -- a plain
// flood fill. GRID_ROWS/GRID_COLS are compile-time constants, so this is an ordinary
// fixed-size array parameter, not a VLA.
static void flood_collect(int r, int c, int8_t color, bool visited[GRID_ROWS][GRID_COLS],
                           int *out_r, int *out_c, int *count) {
    if (r < 0 || r >= GRID_ROWS || c < 0 || c >= GRID_COLS) return;
    if (visited[r][c]) return;
    if (_grid[r][c] != color) return;
    visited[r][c] = true;
    out_r[*count] = r;
    out_c[*count] = c;
    (*count)++;
    flood_collect(r - 1, c, color, visited, out_r, out_c, count);
    flood_collect(r + 1, c, color, visited, out_r, out_c, count);
    flood_collect(r, c - 1, color, visited, out_r, out_c, count);
    flood_collect(r, c + 1, color, visited, out_r, out_c, count);
}

// Compacts every column downward after a clear -- remaining blocks fall to fill the
// gap, empty cells end up at the top. Nothing auto-clears as a result of this; only a
// direct shot ever pops a group (owner's explicit call -- the strategy is setting up a
// bigger group for your NEXT shot, not chain reactions).
static void apply_gravity() {
    for (int c = 0; c < GRID_COLS; c++) {
        int write_row = GRID_ROWS - 1;
        for (int r = GRID_ROWS - 1; r >= 0; r--) {
            if (_grid[r][c] >= 0) {
                _grid[write_row][c] = _grid[r][c];
                if (write_row != r) _grid[r][c] = -1;
                write_row--;
            }
        }
        for (int r = write_row; r >= 0; r--) _grid[r][c] = -1;
    }
}

// ── redraw ───────────────────────────────────────────────────────────────────────
static void redraw_grid() {
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            lv_obj_t *cell = _cell_objs[r][c];
            if (_grid[r][c] < 0) {
                lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_style_bg_color(cell, lv_color_hex(BLOCK_COLORS[_grid[r][c]]), LV_PART_MAIN);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void redraw_bullets() {
    lv_obj_set_style_bg_color(_bullet_swatch[0], lv_color_hex(BLOCK_COLORS[_bullet_color[0]]), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_bullet_swatch[1], lv_color_hex(BLOCK_COLORS[_bullet_color[1]]), LV_PART_MAIN);
}

static void redraw_misses() {
    char buf[16];
    snprintf(buf, sizeof(buf), "Misses: %d/%d", _misses, MAX_MISSES);
    lv_label_set_text(_miss_label, buf);
}

// The archer is the only thing that actually moves between shots -- the ammo swatches
// live in the footer legend (see build_play_panel()) and only ever change color, not
// position. Parked low in the track, close to its bottom edge, so the wall's tall
// merlons cover most of the glyph -- 2026-09-15, was TRACK_TOP+8 (too high, read as
// standing in front of the wall instead of behind it).
static void redraw_shooter() {
    int x = GRID_LEFT + _shooter_col * CELL + CELL / 2 - 12;  // 12 ~= half the glyph's width
    lv_obj_set_pos(_archer_label, x, TRACK_TOP + TRACK_H - 34);
}

// ── gameplay ─────────────────────────────────────────────────────────────────────
// Misses reset to 0 every stage, not just once per run -- keeps each individual stage
// forgiving on its own even as the color count climbs, rather than stacking a
// never-recovering miss budget on top of the difficulty ramp. (Judgment call, not
// explicitly specified -- easy to change to a run-wide budget if that reads wrong.)
static void generate_board() {
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
            _grid[r][c] = (int8_t)random(_active_colors);

    _bullet_color[0] = (int8_t)random(_active_colors);
    _bullet_color[1] = (int8_t)random(_active_colors);
    _shooter_col = GRID_COLS / 2;
    _misses = 0;

    redraw_grid();
    redraw_bullets();
    redraw_shooter();
    redraw_misses();

    char buf[16];
    snprintf(buf, sizeof(buf), "Stage: %d", _stage);
    lv_label_set_text(_stage_label, buf);
}

static void cb_move_left() {
    if (_shooter_col > 0) _shooter_col--;
    redraw_shooter();
}

static void cb_move_right() {
    if (_shooter_col < GRID_COLS - 1) _shooter_col++;
    redraw_shooter();
}

// Fires the front of the two-deep bullet queue at the block sitting at the bottom of
// the current column -- gravity guarantees that's the only cell a column can ever have
// occupied without everything below it also being occupied, so it's always the target.
// Simplified 2026-09-15 from an earlier two-button (Up/Down, independently choosable)
// loadout the owner found "weird" -- now just one Fire button, and the second queue slot
// is a pure lookahead preview rather than a second choice. A miss still spends the shot
// and advances the queue -- same cost as a hit -- but now costs a strike toward
// MAX_MISSES instead of risking an unwinnable "both colors useless" stall.
static void fire() {
    int8_t color = _bullet_color[0];
    int target_row = -1;
    for (int r = GRID_ROWS - 1; r >= 0; r--) {
        if (_grid[r][_shooter_col] >= 0) { target_row = r; break; }
    }

    if (target_row >= 0 && _grid[target_row][_shooter_col] == color) {
        bool visited[GRID_ROWS][GRID_COLS] = {};
        int out_r[MAX_FLOOD], out_c[MAX_FLOOD], count = 0;
        flood_collect(target_row, _shooter_col, color, visited, out_r, out_c, &count);
        for (int i = 0; i < count; i++) _grid[out_r[i]][out_c[i]] = -1;
        apply_gravity();
    } else {
        _misses++;
    }

    _bullet_color[0] = _bullet_color[1];
    _bullet_color[1] = (int8_t)random(_active_colors);

    redraw_grid();
    redraw_bullets();
    redraw_misses();

    if (grid_is_empty()) {
        enter_state(ST_STAGE_CLEAR);
        return;
    }
    if (_misses >= MAX_MISSES) {
        enter_state(ST_GAMEOVER);
        return;
    }
}

static void cb_exit() {
    screen_extras_return_to_list();
}

// Shared by all three "any button" entry points -- splash and game-over both start a
// fresh run at Stage 1; stage-clear advances instead. _state still holds whichever of
// those three screens was up when this fires (enter_state() below is what changes it).
static void cb_start_or_continue() {
    if (_state == ST_STAGE_CLEAR) {
        _stage++;
    } else {
        _stage = 1;
    }
    _active_colors = START_COLORS + (_stage - 1);
    if (_active_colors > MAX_COLORS) _active_colors = MAX_COLORS;

    generate_board();
    enter_state(ST_PLAYING);
}

static void enter_state(GameState s) {
    _state = s;

    lv_obj_add_flag(_splash_panel,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_play_panel,        LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_stage_clear_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_gameover_panel,    LV_OBJ_FLAG_HIDDEN);

    ButtonHandlers h;
    switch (s) {
        case ST_SPLASH:
            lv_obj_clear_flag(_splash_panel, LV_OBJ_FLAG_HIDDEN);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_continue;
            h.back = cb_exit;
            break;

        case ST_PLAYING:
            lv_obj_clear_flag(_play_panel, LV_OBJ_FLAG_HIDDEN);
            h.left  = cb_move_left;
            h.right = cb_move_right;
            h.up    = fire;
            h.back  = cb_exit;
            break;

        case ST_STAGE_CLEAR: {
            lv_obj_clear_flag(_stage_clear_panel, LV_OBJ_FLAG_HIDDEN);
            char buf[64];
            snprintf(buf, sizeof(buf), "STAGE %d COMPLETE!\n\nPress any button\nfor Stage %d.", _stage, _stage + 1);
            lv_label_set_text(_stage_clear_label, buf);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_continue;
            h.back = cb_exit;
            break;
        }

        case ST_GAMEOVER: {
            lv_obj_clear_flag(_gameover_panel, LV_OBJ_FLAG_HIDDEN);
            char buf[64];
            snprintf(buf, sizeof(buf), "GAME OVER\n\nReached Stage %d\n\nPress any button to play again.", _stage);
            lv_label_set_text(_gameover_label, buf);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_continue;
            h.back = cb_exit;
            break;
        }
    }
    buttons_set_handlers(h);
}

// ── build ────────────────────────────────────────────────────────────────────────
static void build_splash_panel() {
    _splash_panel = lv_obj_create(_scr);
    lv_obj_set_size(_splash_panel, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(_splash_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_splash_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_splash_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_splash_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(_splash_panel);
    lv_label_set_text(lbl,
        "CASTLE DEFENSE\n\n"
        "Fire your loaded\ncolor at the blocks\nbelow to clear them.\n\n"
        "3 misses and it's\ngame over -- clear the\nboard to win the stage!");
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

    _stage_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_stage_label, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_stage_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_stage_label, LV_ALIGN_TOP_LEFT, 4, 4);

    _miss_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_miss_label, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_miss_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_miss_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    // Ammo readout lives in the footer legend's Fire row (see below) so it sits right
    // next to the control it describes, instead of a separate readout up here.

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(_play_panel);
            lv_obj_set_size(cell, CELL - 4, CELL - 4);
            lv_obj_set_style_radius(cell, 4, LV_PART_MAIN);
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(cell, GRID_LEFT + c * CELL + 2, GRID_TOP + r * CELL + 2);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
            _cell_objs[r][c] = cell;
        }
    }

    // Castle-wall crenellated track, one merlon per column, alternating tall/short --
    // static, doesn't move or change color; the archer sliding across the top of it is
    // what shows which column is currently aimed.
    for (int c = 0; c < GRID_COLS; c++) {
        int h = (c % 2 == 0) ? MERLON_TALL : MERLON_SHORT;
        lv_obj_t *seg = lv_obj_create(_play_panel);
        lv_obj_set_size(seg, CELL - 6, h);
        lv_obj_set_style_bg_color(seg, lv_color_hex(WALL_COLOR), LV_PART_MAIN);
        lv_obj_set_style_radius(seg, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(seg, GRID_LEFT + c * CELL + 3, TRACK_TOP + TRACK_H - h);
    }

    _archer_label = lv_label_create(_play_panel);
    lv_label_set_text(_archer_label, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(_archer_label, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_archer_label, &lv_font_montserrat_28, LV_PART_MAIN);

    lv_obj_t *legend = ui_legend(_play_panel);
    lv_obj_set_width(legend, SCREEN_W - 28);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);

    // Move / Fire row -- hand-built instead of ui_legend_row() (which only takes a
    // single string per side) since Fire needs two inline colored swatches: the color
    // about to fire, and a preview of the one after it, not just a generic label.
    lv_obj_t *row1 = lv_obj_create(legend);
    lv_obj_set_size(row1, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row1, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row1, 0, LV_PART_MAIN);
    lv_obj_set_layout(row1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *move_lbl = lv_label_create(row1);
    char move_txt[24];
    snprintf(move_txt, sizeof(move_txt), "%s%s Move", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    lv_label_set_text(move_lbl, move_txt);
    lv_obj_set_style_text_color(move_lbl, lv_color_hex(C_YELLOW), LV_PART_MAIN);
    lv_obj_set_style_text_font(move_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_t *fire_group = lv_obj_create(row1);
    lv_obj_set_size(fire_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(fire_group, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(fire_group, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(fire_group, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(fire_group, 3, LV_PART_MAIN);
    lv_obj_set_layout(fire_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(fire_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fire_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(fire_group, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fire_lbl = lv_label_create(fire_group);
    char fire_txt[16];
    snprintf(fire_txt, sizeof(fire_txt), "Fire %s", LV_SYMBOL_UP);
    lv_label_set_text(fire_lbl, fire_txt);
    lv_obj_set_style_text_color(fire_lbl, lv_color_hex(C_YELLOW), LV_PART_MAIN);
    lv_obj_set_style_text_font(fire_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    _bullet_swatch[0] = lv_obj_create(fire_group);
    lv_obj_set_size(_bullet_swatch[0], 14, 14);
    lv_obj_set_style_radius(_bullet_swatch[0], 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(_bullet_swatch[0], 0, LV_PART_MAIN);
    lv_obj_clear_flag(_bullet_swatch[0], LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *next_lbl = lv_label_create(fire_group);
    lv_label_set_text(next_lbl, ">");  // plain ASCII -- see feedback-lvgl-ascii-only-ui-text
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(next_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    _bullet_swatch[1] = lv_obj_create(fire_group);
    lv_obj_set_size(_bullet_swatch[1], 14, 14);
    lv_obj_set_style_radius(_bullet_swatch[1], 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(_bullet_swatch[1], 0, LV_PART_MAIN);
    lv_obj_clear_flag(_bullet_swatch[1], LV_OBJ_FLAG_SCROLLABLE);

    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

static void build_stage_clear_panel() {
    _stage_clear_panel = lv_obj_create(_scr);
    lv_obj_set_size(_stage_clear_panel, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(_stage_clear_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_stage_clear_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_stage_clear_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_stage_clear_panel, LV_OBJ_FLAG_SCROLLABLE);

    _stage_clear_label = lv_label_create(_stage_clear_panel);
    lv_label_set_long_mode(_stage_clear_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_stage_clear_label, 260);
    lv_obj_set_style_text_color(_stage_clear_label, lv_color_hex(C_GREEN), LV_PART_MAIN);
    lv_obj_set_style_text_font(_stage_clear_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(_stage_clear_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(_stage_clear_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *legend = ui_legend(_stage_clear_panel);
    lv_obj_set_width(legend, SCREEN_W - 28);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Any Button = Continue", lv_color_hex(C_YELLOW));
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

// Loads Castle Defense, always starting back at the splash/rules screen.
void screen_castle_defense_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        build_splash_panel();
        build_play_panel();
        build_stage_clear_panel();
        build_gameover_panel();
    }

    header_set_visible(true);
    header_set_title("CASTLE DEFENSE");
    enter_state(ST_SPLASH);

    lv_scr_load(_scr);
}
