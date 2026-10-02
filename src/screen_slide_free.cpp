#include "screen_slide_free.h"
#include "screens.h"
#include "screen_extras.h"
#include "game_scores.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <SD.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Slide Free minigame declared in screen_slide_free.h.
  Notes---- Board files: /slidefree/tier1.txt, tier2.txt, ... (as many as exist, up to
            MAX_TIERS). One board per line, the format tools/tilt_lab.py exports:
              <min_moves> <exit> <row0>|<row1>|...      e.g.  7 R2 ....a.|.bb.a.|RR#...|...
            exit = side U/D/L/R + where the gap starts; '.' empty, '#' wall, 'R' red,
            other letters = one block each (a filled rectangle). min_moves is ignored
            here. ';' lines are comments. Bad lines are skipped with a Serial note --
            proving a board is solvable is the lab's job, far too heavy for this chip.
            Each board in a run is a random pick from the tier the run has reached
            (TIER_LEN below), never repeating within a run. The tilt rules match
            tilt_lab.py's Board.tilt() exactly: repeated passes, each block steps one
            cell if its way is clear, until nothing moves. Each pass is drawn over
            SLIDE_SUBSTEPS frames (half-cell steps, ~24fps), so blocks visibly slide
            instead of teleporting.
            Score is submitted after every solve (not on exit), so walking away or an
            auto-logout never loses a best.
*/

// ── board limits ─────────────────────────────────────────────────────────────
#define SF_DIR       "/slidefree"
#define MAX_TIERS     9
#define MAX_N         8
#define MAX_BLOCKS   16
#define MAX_WALLS    24
#define MAX_LINE    160
#define MAX_USED     64    // boards remembered per run, for no-repeats

// Boards spent in each tier before moving up; the last tier found on the card is played
// forever. Owner-agreed ramp (2026-09-30): boards 1-3 / 4-6 / 7-10 / 11-15 / 16+.
static const int TIER_LEN[MAX_TIERS - 1] = { 3, 3, 4, 5, 5, 5, 5, 5 };

// ── layout ───────────────────────────────────────────────────────────────────
#define PANEL_H      (SCREEN_H - HDR_H)
#define SCORE_H       30
#define FOOTER_H      84    // 3 legend rows
#define BOARD_AREA_H (PANEL_H - SCORE_H - FOOTER_H)
#define FRAME_W        6    // border thickness
#define EXIT_STICKOUT  5    // exit notch pokes this far outside the frame, so it reads at a glance
#define BOARD_X       16    // left-aligned, not centered -- see feedback-left-align-not-center
#define BOARD_Y      (SCORE_H + 22)  // room above for a top-exit arrow
#define BLOCK_INSET    2

// Slide animation: each one-cell move is drawn in SLIDE_SUBSTEPS frames (half-cell steps
// by default) at SLIDE_FRAME_MS each -- ~24fps, still deliberately a bit chunky. A full
// 5-cell slide takes 5 * 2 * 42 = ~0.4s. Owner's call, 2026-09-30.
#define SLIDE_SUBSTEPS  2
#define SLIDE_FRAME_MS 42
#define SOLVED_LOCKOUT_MS 500

// Muted, low-saturation tones on purpose (owner, 2026-09-30): red is the only bright
// thing on the board, so the eye goes straight to it and the exit. Different enough from
// each other to tell blocks apart, lighter than the dark-gray walls.
static const uint32_t BLOCK_COLORS[] = { 0x8A94A6, 0x9C9483, 0x8C9C8E, 0x9E8E98, 0x8497A3, 0xA39584 };
#define BLOCK_COLOR_COUNT (sizeof(BLOCK_COLORS) / sizeof(BLOCK_COLORS[0]))

struct SfBlock {
    int8_t r, c, h, w;
};

struct SfBoard {
    int8_t  n;
    char    exit_side;   // 'U' 'D' 'L' 'R'
    int8_t  exit_pos;
    int8_t  wall_count;
    int8_t  walls[MAX_WALLS][2];
    int8_t  block_count; // [0] is red
    SfBlock blocks[MAX_BLOCKS];
};

enum SfState { ST_SPLASH, ST_PLAY, ST_SOLVED, ST_NO_BOARDS };

static lv_obj_t *_scr;
static lv_obj_t *_splash_panel, *_splash_scores;
static lv_obj_t *_play_panel, *_solved_label, *_best_label;
static lv_obj_t *_frame, *_field, *_gap, *_exit_arrow;
static lv_obj_t *_wall_objs[MAX_WALLS];
static lv_obj_t *_block_objs[MAX_BLOCKS];
static lv_obj_t *_legend_play, *_legend_quit, *_legend_solved;
static lv_obj_t *_banner;
static lv_obj_t *_error_panel;

static SfState  _state;
static SfBoard  _start;          // board as loaded, for Reset
static SfBoard  _cur;
static int8_t   _occ[MAX_N][MAX_N];   // block index, -1 empty, -2 wall
static int      _cell;

static int      _tier_count;
static int      _tier_boards[MAX_TIERS];   // board lines per tier file
static uint16_t _used[MAX_USED];            // (tier << 12) | line index
static int      _used_count;

static int      _solved;         // boards solved this run
static int      _best_before;
static bool     _confirm_quit;
static uint32_t _solved_ms;

static lv_timer_t *_timer;
static char     _slide_dir;      // 0 = not sliding
static char     _pending_dir;    // one press queued while a slide is still running
static uint8_t  _substep;        // frames drawn of the current one-cell move
static uint16_t _moved_mask;     // blocks that moved this cell (bit i = block i)
static bool     _won_pending;    // red reached the exit -- finish drawing, then solve

// Play log for difficulty calibration (tools/tilt_lab.py calibrate), one line per board:
// tier,presses,resets,seconds,solved,<board line>. No user identity -- only the board and
// how it went. Presses count only tilts that moved something, matching the lab's model.
#define SF_LOG SF_DIR "/log.csv"
static char     _cur_line[MAX_LINE];
static int      _cur_tier;
static int      _presses, _resets;
static uint32_t _board_ms;
static bool     _press_counted;  // this tilt already counted (a slide spans many frames)

static void enter_state(SfState s);

// ── parsing ──────────────────────────────────────────────────────────────────
static bool is_board_line(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return *s && *s != ';' && *s != '\r';
}

// Parses one board line into `b`. False (with a Serial note) on anything malformed.
static bool parse_board(const char *line, SfBoard &b) {
    char buf[MAX_LINE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *tok[3];
    int nt = 0;
    for (char *p = strtok(buf, " \t\r\n"); p && nt < 3; p = strtok(nullptr, " \t\r\n")) tok[nt++] = p;
    if (nt < 2) return false;
    const char *ex   = tok[nt - 2];
    const char *grid = tok[nt - 1];

    b.exit_side = toupper(ex[0]);
    if (!strchr("UDLR", b.exit_side) || !isdigit(ex[1])) return false;
    b.exit_pos = atoi(ex + 1);

    // Rows: count them and check they're all n wide
    int n = 1;
    for (const char *p = grid; *p; p++) if (*p == '|') n++;
    if (n < 3 || n > MAX_N) return false;
    b.n = n;

    int8_t rmin[128], rmax[128], cmin[128], cmax[128];
    uint8_t cnt[128] = {0};
    b.wall_count = 0;
    int r = 0, c = 0;
    for (const char *p = grid; ; p++) {
        if (*p == '|' || *p == 0) {
            if (c != n) return false;
            r++; c = 0;
            if (*p == 0) break;
            continue;
        }
        if (c >= n) return false;
        unsigned char ch = *p;
        if (ch >= 128) return false;
        if (ch == '#') {
            if (b.wall_count >= MAX_WALLS) return false;
            b.walls[b.wall_count][0] = r;
            b.walls[b.wall_count][1] = c;
            b.wall_count++;
        } else if (ch != '.') {
            if (!cnt[ch]) { rmin[ch] = rmax[ch] = r; cmin[ch] = cmax[ch] = c; }
            rmin[ch] = min(rmin[ch], (int8_t)r); rmax[ch] = max(rmax[ch], (int8_t)r);
            cmin[ch] = min(cmin[ch], (int8_t)c); cmax[ch] = max(cmax[ch], (int8_t)c);
            cnt[ch]++;
        }
        c++;
    }
    if (!cnt['R']) return false;

    // Red first, then every other letter in character order (matches the lab)
    b.block_count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int ch = 0; ch < 128; ch++) {
            if (!cnt[ch] || (pass == 0) != (ch == 'R')) continue;
            int h = rmax[ch] - rmin[ch] + 1, w = cmax[ch] - cmin[ch] + 1;
            if (cnt[ch] != h * w) return false;   // not a filled rectangle
            if (b.block_count >= MAX_BLOCKS) return false;
            b.blocks[b.block_count++] = { rmin[ch], cmin[ch], (int8_t)h, (int8_t)w };
        }
    }
    int span = (b.exit_side == 'L' || b.exit_side == 'R') ? b.blocks[0].h : b.blocks[0].w;
    return b.exit_pos >= 0 && b.exit_pos + span <= n;
}

// ── SD loading ───────────────────────────────────────────────────────────────
static void tier_path(int t, char *out, size_t len) {
    snprintf(out, len, SF_DIR "/tier%d.txt", t + 1);
}

// Counts tier files (tier1.txt, tier2.txt, ... stopping at the first gap) and the
// board lines in each.
static void scan_tiers() {
    _tier_count = 0;
    char path[32], line[MAX_LINE];
    for (int t = 0; t < MAX_TIERS; t++) {
        tier_path(t, path, sizeof(path));
        File f = SD.open(path, FILE_READ);
        if (!f) break;
        int count = 0;
        while (f.available()) {
            size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1);
            line[len] = 0;
            if (is_board_line(line)) count++;
        }
        f.close();
        if (count == 0) break;
        _tier_boards[t] = count;
        _tier_count = t + 1;
    }
    Serial.printf("[SLIDE] %d tier file(s) found\n", _tier_count);
}

// Reads the idx-th board line of tier t into `b`.
static bool load_board(int t, int idx, SfBoard &b) {
    char path[32], line[MAX_LINE];
    tier_path(t, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    bool ok = false;
    int k = 0;
    while (f.available()) {
        size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = 0;
        if (!is_board_line(line)) continue;
        if (k++ == idx) {
            ok = parse_board(line, b);
            if (ok) {
                strncpy(_cur_line, line, sizeof(_cur_line) - 1);
                _cur_line[sizeof(_cur_line) - 1] = 0;
                char *cr = strchr(_cur_line, '\r');
                if (cr) *cr = 0;
                _cur_tier = t;
            }
            if (!ok) Serial.printf("[SLIDE] %s board %d is malformed, skipped: %s\n", path, idx, line);
            break;
        }
    }
    f.close();
    return ok;
}

static int tier_for_board(int board_no) {  // board_no is 1-based
    for (int t = 0; t < _tier_count - 1; t++) {
        if (board_no <= TIER_LEN[t]) return t;
        board_no -= TIER_LEN[t];
    }
    return _tier_count - 1;
}

static bool was_used(uint16_t key) {
    for (int i = 0; i < _used_count; i++) if (_used[i] == key) return true;
    return false;
}

// Picks a random, not-yet-played board from the tier this run has reached.
static bool pick_board(SfBoard &b) {
    int t = tier_for_board(_solved + 1);
    for (int attempt = 0; attempt < 12; attempt++) {
        int idx = random(_tier_boards[t]);
        uint16_t key = (t << 12) | idx;
        if (was_used(key) && attempt < 8) continue;  // after 8 misses, allow a repeat
        if (!load_board(t, idx, b)) continue;
        if (_used_count < MAX_USED) _used[_used_count++] = key;
        return true;
    }
    return false;
}

// ── tilt engine (mirrors tools/tilt_lab.py Board.tilt) ───────────────────────
static void rebuild_occ() {
    memset(_occ, -1, sizeof(_occ));
    for (int i = 0; i < _cur.wall_count; i++) _occ[_cur.walls[i][0]][_cur.walls[i][1]] = -2;
    for (int i = 0; i < _cur.block_count; i++) {
        const SfBlock &k = _cur.blocks[i];
        for (int y = 0; y < k.h; y++)
            for (int x = 0; x < k.w; x++) _occ[k.r + y][k.c + x] = i;
    }
}

static void dir_delta(char d, int &dr, int &dc) {
    dr = (d == 'D') - (d == 'U');
    dc = (d == 'R') - (d == 'L');
}

// Is (r, c) outside the box but inside the exit gap?
static bool in_exit_gap(int r, int c) {
    int n = _cur.n;
    const SfBlock &red = _cur.blocks[0];
    switch (_cur.exit_side) {
        case 'R': return c == n  && r >= _cur.exit_pos && r < _cur.exit_pos + red.h;
        case 'L': return c == -1 && r >= _cur.exit_pos && r < _cur.exit_pos + red.h;
        case 'D': return r == n  && c >= _cur.exit_pos && c < _cur.exit_pos + red.w;
        case 'U': return r == -1 && c >= _cur.exit_pos && c < _cur.exit_pos + red.w;
    }
    return false;
}

// One pass: every block that can step one cell toward `d` does. Returns a bitmask of the
// blocks that moved (bit i = block i); sets `won` if red stepped into the exit.
static uint16_t slide_pass(char d, bool &won) {
    int dr, dc;
    dir_delta(d, dr, dc);
    int n = _cur.n;
    uint16_t moved = 0;
    won = false;
    for (int i = 0; i < _cur.block_count; i++) {
        SfBlock &k = _cur.blocks[i];
        bool ok = true, out = false;
        for (int y = 0; y < k.h && ok; y++) {
            for (int x = 0; x < k.w && ok; x++) {
                int r = k.r + y + dr, c = k.c + x + dc;
                if (r >= 0 && r < n && c >= 0 && c < n) {
                    int o = _occ[r][c];
                    if (o != -1 && o != i) ok = false;
                } else if (i == 0 && in_exit_gap(r, c)) {
                    out = true;
                } else {
                    ok = false;
                }
            }
        }
        if (!ok) continue;
        k.r += dr;
        k.c += dc;
        moved |= 1u << i;
        if (out) { won = true; return moved; }
        rebuild_occ();
    }
    return moved;
}

// ── drawing ──────────────────────────────────────────────────────────────────
static void place_block(int i, int off_x = 0, int off_y = 0) {
    const SfBlock &k = _cur.blocks[i];
    lv_obj_set_pos(_block_objs[i], k.c * _cell + BLOCK_INSET + off_x, k.r * _cell + BLOCK_INSET + off_y);
}

static void refresh_score() {
    char buf[32];
    snprintf(buf, sizeof(buf), "Solved: %d", _solved);
    lv_label_set_text(_solved_label, buf);
    int best = max(_best_before, _solved);
    snprintf(buf, sizeof(buf), "Best: %d", best);
    lv_label_set_text(_best_label, buf);
}

// Lays out the frame, exit gap, walls and blocks for _cur.
static void draw_board() {
    int n = _cur.n;
    int max_w = SCREEN_W - BOARD_X - 2 * FRAME_W - EXIT_STICKOUT - 22;  // room for a right-side arrow
    int max_h = BOARD_AREA_H - 22 - 2 * FRAME_W - EXIT_STICKOUT - 22;
    _cell = min(max_w, max_h) / n;
    int px = _cell * n;

    lv_obj_set_pos(_frame, BOARD_X, BOARD_Y);
    lv_obj_set_size(_frame, px + 2 * FRAME_W, px + 2 * FRAME_W);
    lv_obj_set_pos(_field, FRAME_W, FRAME_W);
    lv_obj_set_size(_field, px, px);

    // Exit: a red notch in the frame (poking out past it) (frame coordinates), plus an arrow outside it
    // (panel coordinates)
    const SfBlock &red = _cur.blocks[0];
    int gx, gy, gw, gh, ax, ay;
    const char *sym;
    switch (_cur.exit_side) {
        case 'R':
            gx = FRAME_W + px; gy = FRAME_W + _cur.exit_pos * _cell;
            gw = FRAME_W + EXIT_STICKOUT; gh = red.h * _cell;
            ax = BOARD_X + gx + gw + 2; ay = BOARD_Y + gy + gh / 2 - 10;
            sym = LV_SYMBOL_RIGHT; break;
        case 'L':
            gx = -EXIT_STICKOUT; gy = FRAME_W + _cur.exit_pos * _cell;
            gw = FRAME_W + EXIT_STICKOUT; gh = red.h * _cell;
            ax = 0; ay = 0; sym = LV_SYMBOL_LEFT; break;   // arrow hidden, see below
        case 'U':
            gx = FRAME_W + _cur.exit_pos * _cell; gy = -EXIT_STICKOUT;
            gw = red.w * _cell; gh = FRAME_W + EXIT_STICKOUT;
            ax = BOARD_X + gx + gw / 2 - 8; ay = BOARD_Y - EXIT_STICKOUT - 20;
            sym = LV_SYMBOL_UP; break;
        default:  // 'D'
            gx = FRAME_W + _cur.exit_pos * _cell; gy = FRAME_W + px;
            gw = red.w * _cell; gh = FRAME_W + EXIT_STICKOUT;
            ax = BOARD_X + gx + gw / 2 - 8; ay = BOARD_Y + gy + gh + 1;
            sym = LV_SYMBOL_DOWN; break;
    }
    lv_obj_set_pos(_gap, gx, gy);
    lv_obj_set_size(_gap, gw, gh);
    lv_label_set_text(_exit_arrow, sym);
    lv_obj_set_pos(_exit_arrow, ax, ay);
    // A left exit has no room for an arrow (the board is left-aligned) -- the notch alone shows it
    if (_cur.exit_side == 'L') lv_obj_add_flag(_exit_arrow, LV_OBJ_FLAG_HIDDEN);
    else                       lv_obj_clear_flag(_exit_arrow, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_WALLS; i++) {
        if (i < _cur.wall_count) {
            lv_obj_set_size(_wall_objs[i], _cell, _cell);
            lv_obj_set_pos(_wall_objs[i], _cur.walls[i][1] * _cell, _cur.walls[i][0] * _cell);
            lv_obj_clear_flag(_wall_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_wall_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (i < _cur.block_count) {
            const SfBlock &k = _cur.blocks[i];
            lv_obj_set_size(_block_objs[i], k.w * _cell - 2 * BLOCK_INSET, k.h * _cell - 2 * BLOCK_INSET);
            uint32_t color = i == 0 ? C_RED : BLOCK_COLORS[(i - 1) % BLOCK_COLOR_COUNT];
            lv_obj_set_style_bg_color(_block_objs[i], lv_color_hex(color), LV_PART_MAIN);
            place_block(i);
            lv_obj_clear_flag(_block_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_block_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ── play ─────────────────────────────────────────────────────────────────────
static void stop_timer() {
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _slide_dir = _pending_dir = 0;
    _substep = 0;
    _won_pending = false;
}

static void show_quit_confirm(bool on) {
    _confirm_quit = on;
    if (on) { lv_obj_add_flag(_legend_play, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(_legend_quit, LV_OBJ_FLAG_HIDDEN); }
    else    { lv_obj_add_flag(_legend_quit, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(_legend_play, LV_OBJ_FLAG_HIDDEN); }
}

// Appends this board's result to the play log. Best-effort: a failed write just skips.
static void log_board(bool solved) {
    File f = SD.open(SF_LOG, FILE_APPEND);
    if (!f) return;
    f.printf("%d,%d,%d,%.1f,%d,%s\n", _cur_tier + 1, _presses, _resets,
             (millis() - _board_ms) / 1000.0f, solved ? 1 : 0, _cur_line);
    f.close();
}

static void on_solved() {
    stop_timer();
    log_board(true);
    _solved++;
    int uid = screen_extras_current_user_id();
    bool new_best = game_scores_submit(uid, GAME_KEY_SLIDE_FREE, _solved) && _solved > _best_before;
    refresh_score();
    char buf[48];
    snprintf(buf, sizeof(buf), new_best ? "SOLVED!\nNew best: %d" : "SOLVED!\nBoards: %d", _solved);
    lv_label_set_text(_banner, buf);
    enter_state(ST_SOLVED);
}

// One animation frame. Substep 0 runs the logic (one cell of movement, via slide_pass);
// the model is already at the new cell, so each frame draws the moved blocks trailing
// behind it by the part of the cell not yet covered.
static void slide_tick(lv_timer_t *) {
    if (_substep == 0) {
        _moved_mask = slide_pass(_slide_dir, _won_pending);
        if (_moved_mask && !_press_counted) { _presses++; _press_counted = true; }
        if (!_moved_mask) {
            // Slide finished -- run a queued press, if any
            char next = _pending_dir;
            _pending_dir = 0;
            if (next) { _slide_dir = next; _press_counted = false; return; }
            if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
            _slide_dir = 0;
            return;
        }
    }

    _substep++;
    int dr, dc;
    dir_delta(_slide_dir, dr, dc);
    int behind = (SLIDE_SUBSTEPS - _substep) * _cell / SLIDE_SUBSTEPS;  // px still to go
    for (int i = 0; i < _cur.block_count; i++)
        if (_moved_mask & (1u << i)) place_block(i, -dc * behind, -dr * behind);

    if (_substep >= SLIDE_SUBSTEPS) {
        _substep = 0;
        if (_won_pending) { on_solved(); return; }
    }
}

static void tilt(char d) {
    if (_confirm_quit) { show_quit_confirm(false); return; }
    if (_slide_dir) { _pending_dir = d; return; }
    _slide_dir = d;
    _press_counted = false;
    slide_tick(nullptr);                          // first frame right away, not a frame late
    if (_slide_dir && !_timer) _timer = lv_timer_create(slide_tick, SLIDE_FRAME_MS, nullptr);
}

static void cb_up()    { tilt('U'); }
static void cb_down()  { tilt('D'); }
static void cb_left()  { tilt('L'); }
static void cb_right() { tilt('R'); }

static void cb_reset() {
    if (_confirm_quit) { show_quit_confirm(false); return; }
    stop_timer();
    _resets++;
    _cur = _start;
    rebuild_occ();
    for (int i = 0; i < _cur.block_count; i++) place_block(i);
}

static void cb_exit() {
    stop_timer();
    if (_state == ST_PLAY && _presses > 0) log_board(false);  // gave up on this board
    screen_extras_return_to_list();
}

// Back mid-run: leaving loses the run's progress (the best is already saved), so after
// at least one solve it takes a second Back -- the in-place footer swap, see
// feedback-lightweight-confirm-pattern. Any other button cancels.
static void cb_back_play() {
    if (_solved == 0 || _confirm_quit) { cb_exit(); return; }
    show_quit_confirm(true);
}

static bool next_board() {
    if (!pick_board(_start)) return false;
    _presses = _resets = 0;
    _board_ms = millis();
    _cur = _start;
    rebuild_occ();
    draw_board();
    return true;
}

static void start_run() {
    _solved = 0;
    _used_count = 0;
    _best_before = game_scores_get_best(screen_extras_current_user_id(), GAME_KEY_SLIDE_FREE);
    refresh_score();
    if (!next_board()) { enter_state(ST_NO_BOARDS); return; }
    enter_state(ST_PLAY);
}

static void cb_start() { start_run(); }

static void cb_next() {
    if (millis() - _solved_ms < SOLVED_LOCKOUT_MS) return;
    if (!next_board()) { enter_state(ST_NO_BOARDS); return; }
    enter_state(ST_PLAY);
}

// "Your best / Top" lines for the splash screen.
static void format_scores(char *out, size_t len) {
    int best = game_scores_get_best(screen_extras_current_user_id(), GAME_KEY_SLIDE_FREE);
    int top;
    char name[40];
    if (game_scores_get_top(GAME_KEY_SLIDE_FREE, &top, name, sizeof(name)))
        snprintf(out, len, "Your best: %d\nTop: %s  %d", best, name, top);
    else
        snprintf(out, len, "Your best: %d\nNo top score yet!", best);
}

static void enter_state(SfState s) {
    _state = s;
    lv_obj_add_flag(_splash_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_play_panel,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_error_panel,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_banner,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_legend_solved, LV_OBJ_FLAG_HIDDEN);

    ButtonHandlers h;
    switch (s) {
        case ST_SPLASH: {
            stop_timer();
            char buf[80];
            format_scores(buf, sizeof(buf));
            lv_label_set_text(_splash_scores, buf);
            lv_obj_clear_flag(_splash_panel, LV_OBJ_FLAG_HIDDEN);
            h.up = h.down = h.left = h.right = h.enter = cb_start;
            h.back = cb_exit;
            break;
        }
        case ST_PLAY:
            lv_obj_clear_flag(_play_panel, LV_OBJ_FLAG_HIDDEN);
            show_quit_confirm(false);
            h.up = cb_up; h.down = cb_down; h.left = cb_left; h.right = cb_right;
            h.enter = cb_reset;
            h.back  = cb_back_play;
            h.fastTaps = true;  // quick taps during a slide animation must not get lost
            break;
        case ST_SOLVED:
            _solved_ms = millis();
            lv_obj_clear_flag(_play_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_banner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_legend_play, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_legend_quit, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_legend_solved, LV_OBJ_FLAG_HIDDEN);
            h.up = h.down = h.left = h.right = h.enter = cb_next;
            h.back = cb_exit;
            break;
        case ST_NO_BOARDS:
            stop_timer();
            lv_obj_clear_flag(_error_panel, LV_OBJ_FLAG_HIDDEN);
            h.back = cb_exit;
            break;
    }
    buttons_set_handlers(h);
}

// ── build ────────────────────────────────────────────────────────────────────
static lv_obj_t *make_panel() {
    lv_obj_t *p = lv_obj_create(_scr);
    lv_obj_set_size(p, SCREEN_W, PANEL_H);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *make_text(lv_obj_t *parent, uint32_t color, const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 260);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return lbl;
}

static lv_obj_t *make_legend(lv_obj_t *panel) {
    lv_obj_t *legend = ui_legend(panel);
    lv_obj_set_width(legend, SCREEN_W - 24);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    return legend;
}

static lv_obj_t *make_rect(lv_obj_t *parent, uint32_t color, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_splash_panel() {
    _splash_panel = make_panel();
    lv_obj_t *lbl = make_text(_splash_panel, C_CYAN, &lv_font_montserrat_20);
    lv_label_set_text(lbl,
        "SLIDE FREE\n\n"
        "Arrows tilt the box --\n"
        "every block slides.\n\n"
        "Get the red block\n"
        "out the exit.\n\n"
        "Stuck? Enter resets.");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -70);

    _splash_scores = make_text(_splash_panel, C_YELLOW, &lv_font_montserrat_20);
    lv_obj_align(_splash_scores, LV_ALIGN_CENTER, 0, 95);

    lv_obj_t *legend = make_legend(_splash_panel);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Any Button = Start", lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

static void build_play_panel() {
    _play_panel = make_panel();

    _solved_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_solved_label, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_solved_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_solved_label, LV_ALIGN_TOP_LEFT, 4, 4);

    _best_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_best_label, lv_color_hex(C_YELLOW), LV_PART_MAIN);
    lv_obj_set_style_text_font(_best_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_best_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    // Frame: a plain C_DIM rect with the playfield (_field) inset FRAME_W inside it.
    // Walls/blocks are children of the field, so their positions are plain cell
    // coordinates. The exit notch is a frame child created BEFORE the field, so red
    // draws on top of it while sliding out. Neither clips, so red stays visible
    // leaving the box.
    _frame = make_rect(_play_panel, C_DIM, 0);
    lv_obj_add_flag(_frame, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    _gap = make_rect(_frame, C_RED, 0);
    _field = make_rect(_frame, C_SURFACE, 0);
    lv_obj_add_flag(_field, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    for (int i = 0; i < MAX_WALLS; i++) {
        _wall_objs[i] = make_rect(_field, C_DIM, 0);
        lv_obj_add_flag(_wall_objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < MAX_BLOCKS; i++) {
        _block_objs[i] = make_rect(_field, BLOCK_COLORS[0], 6);
        lv_obj_add_flag(_block_objs[i], LV_OBJ_FLAG_HIDDEN);
    }

    _exit_arrow = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_exit_arrow, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_exit_arrow, &lv_font_montserrat_20, LV_PART_MAIN);

    _banner = make_text(_play_panel, C_YELLOW, &lv_font_montserrat_28);
    lv_obj_set_style_bg_color(_banner, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_banner, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_banner, 10, LV_PART_MAIN);
    lv_obj_align(_banner, LV_ALIGN_TOP_LEFT, 20, BOARD_Y + 90);

    char arrows[24];
    snprintf(arrows, sizeof(arrows), "%s%s%s%s Tilt", LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    _legend_play = make_legend(_play_panel);
    ui_legend_row(_legend_play, "", lv_color_hex(C_TEXT), arrows, lv_color_hex(C_YELLOW));
    ui_legend_row(_legend_play, "", lv_color_hex(C_TEXT), "Reset board", lv_color_hex(C_GREEN));
    ui_legend_row(_legend_play, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));

    _legend_quit = make_legend(_play_panel);
    ui_legend_row(_legend_quit, "", lv_color_hex(C_TEXT), "End this run?", lv_color_hex(C_TEXT));
    ui_legend_row(_legend_quit, "", lv_color_hex(C_TEXT), "Yes, quit", lv_color_hex(C_RED));
    ui_legend_row(_legend_quit, "", lv_color_hex(C_TEXT), "Any other = keep playing", lv_color_hex(C_YELLOW));

    _legend_solved = make_legend(_play_panel);
    ui_legend_row(_legend_solved, "", lv_color_hex(C_TEXT), "Any Button = Next board", lv_color_hex(C_YELLOW));
    ui_legend_row(_legend_solved, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

static void build_error_panel() {
    _error_panel = make_panel();
    lv_obj_t *lbl = make_text(_error_panel, C_ORANGE, &lv_font_montserrat_20);
    lv_label_set_text(lbl,
        "No Slide Free boards\nfound on the SD card.\n\n"
        "Copy the slidefree folder\nfrom tools/ to the card.");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t *legend = make_legend(_error_panel);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

// Loads Slide Free at its splash screen. Re-scans the tier files every entry, so a
// card with new boards is picked up without a reboot.
void screen_slide_free_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);
        build_splash_panel();
        build_play_panel();
        build_error_panel();
    }

    stop_timer();
    header_set_visible(true);
    header_set_title("SLIDE FREE");
    scan_tiers();
    enter_state(_tier_count ? ST_SPLASH : ST_NO_BOARDS);
    lv_scr_load(_scr);
}
