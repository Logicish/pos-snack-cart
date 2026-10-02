#include "screen_laggy_fish.h"
#include "screens.h"
#include "screen_extras.h"
#include "game_scores.h"
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
  Notes---- REBUILT 2026-09-30 as a Flappy Bird clone (was a horizontal dodge/catch game
            -- see project memory, project-laggy-fish-design-notes, for the original).
            Any button but Back flaps; the fish falls under gravity and has to swim
            through the gaps in pipes scrolling in from the right. 3 lives: a hit costs
            one, blinks the fish invulnerable for a moment, and play carries on. Score is
            pipes passed, saved as a per-user best via game_scores.h on game over.
            Timing (2026-09-30, after the first hardware play): physics steps on a fixed
            SIM_MS clock against real elapsed time, and the screen only redraws every
            FRAME_MS. First version stepped once per lv_timer tick, so game speed wobbled
            with however long each frame took to render (more pipe on screen = slower).
            Now the choppiness is a deliberate, steady frame cap -- the fake lag -- while
            the game itself runs at a constant speed underneath it.
*/

LV_IMG_DECLARE(fish_flap);    // 48x32 downscale, in-game
LV_IMG_DECLARE(fish_right);   // full-size, win screen only

// ── layout ───────────────────────────────────────────────────────────────────
#define PANEL_H      (SCREEN_H - HDR_H)
#define SCORE_H       30   // strip below the main header for the Lives/Score readout
#define FOOTER_H      52
#define FIELD_Y      SCORE_H
#define FIELD_H      (PANEL_H - SCORE_H - FOOTER_H)

#define FISH_W        48
#define FISH_H        32
#define FISH_X        40   // fixed column -- the pipes move, the fish doesn't
#define FISH_HIT_INSET 6   // shrink the hitbox a bit -- fins brushing a pipe shouldn't count

// Sparkle ring around the fish on the new-best screen -- plain "*" glyphs at fixed
// offsets from panel center rather than a new art asset.
#define WIN_SPARKLE_COUNT 6
static const lv_point_t WIN_SPARKLE_OFFSETS[WIN_SPARKLE_COUNT] = {
    { -80, -20 }, { 80, -20 },
    { -95,  20 }, { 95,  20 },
    { -70,  75 }, { 70,  75 },
};

// ── timing ───────────────────────────────────────────────────────────────────
#define SIM_MS        40   // one physics step -- every per-tick constant below is per step
#define FRAME_MS      80   // redraw cap (~12.5fps) -- the deliberate lag; 40 = as smooth as
                           // the sim gets, if the panel can keep up
#define MAX_CATCHUP    4   // steps per frame at most, so one long stall can't fast-forward

// Serial-log average/max frame interval every ~5s while playing, for tuning FRAME_MS.
#define LAGGY_FISH_FPS_LOG 0

// ── physics (fixed-point, 1/16 px, per step) ─────────────────────────────────
#define FP            16
#define GRAVITY       10   // 0.625 px/tick^2
#define FLAP_VY     (-128) // -8 px/tick -- about a 50px hop
#define MAX_FALL_VY   160  // 10 px/tick terminal velocity

// ── pipes ────────────────────────────────────────────────────────────────────
#define MAX_PIPES          3
#define PIPE_W            50
#define PIPE_SPACING     180   // left edge to left edge
#define PIPE_MARGIN       30   // gap never closer than this to the ceiling/floor
#define PIPE_MAX_SHIFT   120   // max gap-center move between neighbors, keeps it reachable

// Difficulty ramp, driven by pipes passed rather than time.
#define GAP_START        140
#define GAP_MIN          105
#define GAP_SHRINK         3   // px narrower per pipe passed, down to GAP_MIN
#define SPEED_START        3   // px/tick
#define SPEED_MAX          6
#define SPEED_EVERY        8   // +1 speed every this many pipes

#define LIVES_START        3
#define INVULN_TICKS      40   // ~1.6s of blinking after a hit

struct Pipe {
    lv_obj_t *top;
    lv_obj_t *bottom;
    int16_t   x;
    int16_t   gap_top;
    int16_t   gap_bottom;
    bool      passed;
};

enum GameState { ST_SPLASH, ST_PLAYING, ST_GAMEOVER, ST_WIN };

static lv_obj_t *_scr;

static lv_obj_t *_splash_panel;
static lv_obj_t *_splash_scores_label;

static lv_obj_t *_play_panel;
static lv_obj_t *_field;
static lv_obj_t *_fish_img;
static lv_obj_t *_ready_label;
static lv_obj_t *_lives_label;
static lv_obj_t *_score_label;
static Pipe       _pipes[MAX_PIPES];

static lv_obj_t *_gameover_panel;
static lv_obj_t *_gameover_label;

static lv_obj_t *_win_panel;
static lv_obj_t *_win_label;
static lv_obj_t *_win_score_label;

static lv_timer_t *_timer;
static GameState    _state;

static int32_t _fish_y_fp;   // fish top edge within the field, 1/16 px
static int32_t _vy_fp;
static bool    _ready;       // waiting for the first flap -- nothing moves yet
static int     _invuln;      // ticks of post-hit invulnerability left
static int     _score;
static int     _lives;
static int     _best_before;       // this user's best going into the run
static int     _top_before;        // cart-wide top going into the run, -1 if none
static uint32_t _last_ms;          // real time the sim was last advanced to
static uint32_t _acc_ms;           // real time not yet spent on sim steps

static void enter_state(GameState s);

// ── helpers ──────────────────────────────────────────────────────────────────
static int current_gap() {
    int g = GAP_START - _score * GAP_SHRINK;
    return g < GAP_MIN ? GAP_MIN : g;
}

static int current_speed() {
    int s = SPEED_START + _score / SPEED_EVERY;
    return s > SPEED_MAX ? SPEED_MAX : s;
}

static void refresh_score_labels() {
    char buf[24];
    snprintf(buf, sizeof(buf), "Lives: %d", _lives);
    lv_label_set_text(_lives_label, buf);
    snprintf(buf, sizeof(buf), "Score: %d", _score);
    lv_label_set_text(_score_label, buf);
}

// "Your best / Top" lines shared by the splash and game-over screens.
static void format_scores(char *out, size_t len) {
    int uid  = screen_extras_current_user_id();
    int best = game_scores_get_best(uid, GAME_KEY_LAGGY_FISH);
    int top;
    char name[40];
    if (game_scores_get_top(GAME_KEY_LAGGY_FISH, &top, name, sizeof(name)))
        snprintf(out, len, "Your best: %d\nTop: %s  %d", best, name, top);
    else
        snprintf(out, len, "Your best: %d\nNo top score yet!", best);
}

// ── fish ─────────────────────────────────────────────────────────────────────
static void redraw_fish() {
    lv_obj_set_pos(_fish_img, FISH_X, _fish_y_fp / FP);
}

static void cb_flap() {
    if (_ready) {
        _ready = false;
        lv_obj_add_flag(_ready_label, LV_OBJ_FLAG_HIDDEN);
        _last_ms = millis();  // the clock starts now, not when the screen opened
        _acc_ms  = 0;
    }
    _vy_fp = FLAP_VY;
}

// ── pipes ────────────────────────────────────────────────────────────────────
// Picks a new gap for pipe `p`, within PIPE_MAX_SHIFT of the previous pipe's gap center.
static void pipe_new_gap(Pipe &p, int prev_center) {
    int gap  = current_gap();
    int lo   = PIPE_MARGIN + gap / 2;
    int hi   = FIELD_H - PIPE_MARGIN - gap / 2;
    int from = max(lo, prev_center - PIPE_MAX_SHIFT);
    int to   = min(hi, prev_center + PIPE_MAX_SHIFT);
    int center = random(from, to + 1);
    p.gap_top    = center - gap / 2;
    p.gap_bottom = p.gap_top + gap;
    p.passed     = false;

    lv_obj_set_size(p.top, PIPE_W, p.gap_top);
    lv_obj_set_size(p.bottom, PIPE_W, FIELD_H - p.gap_bottom);
}

static void pipe_place(Pipe &p) {
    lv_obj_set_pos(p.top, p.x, 0);
    lv_obj_set_pos(p.bottom, p.x, p.gap_bottom);
}

static int pipe_center(const Pipe &p) { return (p.gap_top + p.gap_bottom) / 2; }

// True if the fish's (inset) hitbox overlaps pipe `p`'s solid parts.
static bool pipe_hits_fish(const Pipe &p, int fish_y) {
    int fl = FISH_X + FISH_HIT_INSET, fr = FISH_X + FISH_W - FISH_HIT_INSET;
    int ft = fish_y + FISH_HIT_INSET, fb = fish_y + FISH_H - FISH_HIT_INSET;
    if (fr < p.x || fl > p.x + PIPE_W) return false;
    return ft < p.gap_top || fb > p.gap_bottom;
}

// ── game tick ────────────────────────────────────────────────────────────────
// Costs a life and starts the invulnerability blink, or ends the run on the last one.
// Returns true if the run ended (caller must stop touching this tick's state).
static bool take_hit() {
    _lives--;
    refresh_score_labels();
    if (_lives <= 0) {
        enter_state(ST_GAMEOVER);
        return true;
    }
    _invuln = INVULN_TICKS;
    _vy_fp  = FLAP_VY;  // a little hop so a floor hit doesn't immediately re-hit
    return false;
}

// One fixed physics step: moves everything, scores, and checks hits. Doesn't touch any
// LVGL positions -- render_frame() does that once per frame. Returns true if the run
// ended (caller must stop stepping).
static bool sim_step() {
    // Fish
    _vy_fp += GRAVITY;
    if (_vy_fp > MAX_FALL_VY) _vy_fp = MAX_FALL_VY;
    _fish_y_fp += _vy_fp;
    if (_fish_y_fp < 0) { _fish_y_fp = 0; _vy_fp = 0; }  // ceiling just stops you
    int fish_y = _fish_y_fp / FP;

    bool floor_hit = fish_y + FISH_H >= FIELD_H;
    if (floor_hit) {
        _fish_y_fp = (FIELD_H - FISH_H) * FP;
        fish_y = FIELD_H - FISH_H;
    }

    // Pipes
    int speed = current_speed();
    bool pipe_hit = false;
    int rightmost = 0;
    for (int i = 0; i < MAX_PIPES; i++)
        if (_pipes[i].x > _pipes[rightmost].x) rightmost = i;

    for (int i = 0; i < MAX_PIPES; i++) {
        Pipe &p = _pipes[i];
        p.x -= speed;

        if (p.x + PIPE_W < 0) {
            // Recycle to the back of the line
            p.x = _pipes[rightmost].x + PIPE_SPACING;
            pipe_new_gap(p, pipe_center(_pipes[rightmost]));
            rightmost = i;
        }

        if (!p.passed && p.x + PIPE_W < FISH_X) {
            p.passed = true;
            _score++;
            refresh_score_labels();
        }

        if (_invuln == 0 && pipe_hits_fish(p, fish_y)) {
            pipe_hit = true;
            p.passed = true;  // no point for a pipe you crashed into
        }
    }

    if (_invuln > 0) {
        _invuln--;
    } else if (floor_hit || pipe_hit) {
        if (take_hit()) return true;
    }
    return false;
}

// Pushes the current sim state to the screen -- the only place positions change.
static void render_frame() {
    for (int i = 0; i < MAX_PIPES; i++) pipe_place(_pipes[i]);
    // Blink every 4 steps while invulnerable
    if (_invuln > 0 && (_invuln / 4) % 2) lv_obj_add_flag(_fish_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(_fish_img, LV_OBJ_FLAG_HIDDEN);
    redraw_fish();
}

// Fires every FRAME_MS: runs however many SIM_MS steps real time says are due (a late
// frame catches up instead of slowing the game), then draws once.
static void tick_cb(lv_timer_t *) {
    if (_ready) return;

    uint32_t now = millis();
    _acc_ms += now - _last_ms;
    _last_ms = now;
    if (_acc_ms > MAX_CATCHUP * SIM_MS) _acc_ms = MAX_CATCHUP * SIM_MS;

    while (_acc_ms >= SIM_MS) {
        _acc_ms -= SIM_MS;
        if (sim_step()) return;  // game over -- panel is already hidden
    }
    render_frame();

#if LAGGY_FISH_FPS_LOG
    static uint32_t prev, sum, worst, n;
    uint32_t dt = now - prev;
    prev = now;
    if (dt < 1000) { sum += dt; if (dt > worst) worst = dt; n++; }
    if (n >= 60) {
        Serial.printf("[FISH] frame avg %lums, max %lums\n", (unsigned long)(sum / n), (unsigned long)worst);
        sum = worst = n = 0;
    }
#endif
}

// ── state transitions ────────────────────────────────────────────────────────
static void stop_timer() {
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
}

static void start_game() {
    _score  = 0;
    _lives  = LIVES_START;
    _invuln = 0;
    _vy_fp  = 0;
    _ready  = true;
    _fish_y_fp = ((FIELD_H - FISH_H) / 2) * FP;
    lv_obj_clear_flag(_fish_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_ready_label, LV_OBJ_FLAG_HIDDEN);
    redraw_fish();
    refresh_score_labels();

    int prev_center = FIELD_H / 2;
    for (int i = 0; i < MAX_PIPES; i++) {
        _pipes[i].x = SCREEN_W + 60 + i * PIPE_SPACING;  // first pipe arrives after a beat
        pipe_new_gap(_pipes[i], prev_center);
        pipe_place(_pipes[i]);
        prev_center = pipe_center(_pipes[i]);
    }

    int uid = screen_extras_current_user_id();
    _best_before = game_scores_get_best(uid, GAME_KEY_LAGGY_FISH);
    char name[40];
    if (!game_scores_get_top(GAME_KEY_LAGGY_FISH, &_top_before, name, sizeof(name)))
        _top_before = -1;

    enter_state(ST_PLAYING);

    stop_timer();
    _timer = lv_timer_create(tick_cb, FRAME_MS, nullptr);
}

// Back from any state: leaves Laggy Fish, back to the already-identified Extras menu.
static void cb_exit() {
    stop_timer();
    screen_extras_return_to_list();
}

// Any non-Back button on the splash or end screens (re)starts the game. On the end
// screens, presses in the first END_LOCKOUT_MS are ignored -- otherwise a flap mashed
// just as the last life goes restarts instantly and skips past the score.
#define END_LOCKOUT_MS 800
static uint32_t _ended_ms;

static void cb_start_or_retry() {
    if ((_state == ST_GAMEOVER || _state == ST_WIN) && millis() - _ended_ms < END_LOCKOUT_MS) return;
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
        case ST_SPLASH: {
            stop_timer();
            char buf[80];
            format_scores(buf, sizeof(buf));
            lv_label_set_text(_splash_scores_label, buf);
            lv_obj_clear_flag(_splash_panel, LV_OBJ_FLAG_HIDDEN);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_retry;
            h.back = cb_exit;
            break;
        }

        case ST_PLAYING:
            lv_obj_clear_flag(_play_panel, LV_OBJ_FLAG_HIDDEN);
            h.up = h.down = h.left = h.right = h.enter = cb_flap;
            h.back = cb_exit;
            h.fastTaps = true;  // quick flap taps mid-render must not get lost
            break;

        case ST_GAMEOVER: {
            stop_timer();
            _ended_ms = millis();
            // Save first -- a new personal best gets the sparkle screen instead.
            int uid = screen_extras_current_user_id();
            if (game_scores_submit(uid, GAME_KEY_LAGGY_FISH, _score)) {
                enter_state(ST_WIN);
                return;
            }
            lv_obj_clear_flag(_gameover_panel, LV_OBJ_FLAG_HIDDEN);
            char scores[80];
            format_scores(scores, sizeof(scores));
            char buf[140];
            snprintf(buf, sizeof(buf), "GAME OVER\n\nScore: %d\n\n%s", _score, scores);
            lv_label_set_text(_gameover_label, buf);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_retry;
            h.back = cb_exit;
            break;
        }

        case ST_WIN: {
            // Reached only from ST_GAMEOVER, after a new personal best saved.
            lv_obj_clear_flag(_win_panel, LV_OBJ_FLAG_HIDDEN);
            bool record = _score > _top_before;
            lv_label_set_text(_win_label, record ? "NEW CART RECORD!" : "NEW PERSONAL BEST!");
            char buf[48];
            snprintf(buf, sizeof(buf), "Score: %d\nOld best: %d", _score, _best_before);
            lv_label_set_text(_win_score_label, buf);
            h.up = h.down = h.left = h.right = h.enter = cb_start_or_retry;
            h.back = cb_exit;
            break;
        }
    }
    buttons_set_handlers(h);
}

// ── build ────────────────────────────────────────────────────────────────────
// Full-height transparent panel under the header -- each game state gets one.
static lv_obj_t *make_panel() {
    lv_obj_t *p = lv_obj_create(_scr);
    lv_obj_set_size(p, SCREEN_W, PANEL_H);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

// Centered wrapped label, the text style every panel here uses.
static lv_obj_t *make_text(lv_obj_t *parent, uint32_t color, const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 260);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return lbl;
}

static void add_footer(lv_obj_t *panel, const char *action) {
    lv_obj_t *legend = ui_legend(panel);
    lv_obj_set_width(legend, SCREEN_W - 24);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), action, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Exit", lv_color_hex(C_RED));
}

static void build_splash_panel() {
    _splash_panel = make_panel();

    lv_obj_t *lbl = make_text(_splash_panel, C_CYAN, &lv_font_montserrat_20);
    lv_label_set_text(lbl,
        "LAGGY FISH\n\n"
        "Any button = flap\n"
        "Swim through the gaps!\n\n"
        "3 lives");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -70);

    _splash_scores_label = make_text(_splash_panel, C_YELLOW, &lv_font_montserrat_20);
    lv_obj_align(_splash_scores_label, LV_ALIGN_CENTER, 0, 70);

    add_footer(_splash_panel, "Any Button = Start");
}

static void build_play_panel() {
    _play_panel = make_panel();

    _lives_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_lives_label, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_lives_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_lives_label, LV_ALIGN_TOP_LEFT, 4, 4);

    _score_label = lv_label_create(_play_panel);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_score_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    // The field clips its children, so pipes slide in/out of the edges cleanly. (Negative
    // object x here is fine -- LVGL clips before flush; it was only a negative *flush*
    // offset that crashed the ILI9488, see main.cpp's lv_flush() history.)
    _field = lv_obj_create(_play_panel);
    lv_obj_set_size(_field, SCREEN_W, FIELD_H);
    lv_obj_set_pos(_field, 0, FIELD_Y);
    lv_obj_set_style_bg_color(_field, lv_color_hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_field, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(_field, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(_field, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_field, 0, LV_PART_MAIN);
    lv_obj_clear_flag(_field, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < MAX_PIPES; i++) {
        lv_obj_t **parts[2] = { &_pipes[i].top, &_pipes[i].bottom };
        for (int j = 0; j < 2; j++) {
            lv_obj_t *o = lv_obj_create(_field);
            lv_obj_set_style_bg_color(o, lv_color_hex(C_GREEN), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
            lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            *parts[j] = o;
        }
    }

    // Fish created after the pipes so it draws on top of them while blinking through one.
    _fish_img = lv_img_create(_field);
    lv_img_set_src(_fish_img, &fish_flap);

    _ready_label = make_text(_field, C_YELLOW, &lv_font_montserrat_20);
    lv_label_set_text(_ready_label, "Press any button\nto flap!");
    lv_obj_align(_ready_label, LV_ALIGN_CENTER, 30, 70);

    add_footer(_play_panel, "Any Button = Flap");
}

static void build_gameover_panel() {
    _gameover_panel = make_panel();
    _gameover_label = make_text(_gameover_panel, C_CYAN, &lv_font_montserrat_20);
    lv_obj_align(_gameover_label, LV_ALIGN_CENTER, 0, -10);
    add_footer(_gameover_panel, "Any Button = Retry");
}

static void build_win_panel() {
    _win_panel = make_panel();

    _win_label = make_text(_win_panel, C_YELLOW, &lv_font_montserrat_24);
    lv_obj_align(_win_label, LV_ALIGN_CENTER, 0, -130);

    // Sparkle ring first, fish image on top -- draw order matters so the fish isn't
    // hidden behind a sparkle that happens to overlap its bounding box.
    for (int i = 0; i < WIN_SPARKLE_COUNT; i++) {
        lv_obj_t *lbl = lv_label_create(_win_panel);
        lv_label_set_text(lbl, "*");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_YELLOW), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_CENTER, WIN_SPARKLE_OFFSETS[i].x, WIN_SPARKLE_OFFSETS[i].y - 20);
    }

    lv_obj_t *fish = lv_img_create(_win_panel);
    lv_img_set_src(fish, &fish_right);
    lv_obj_align(fish, LV_ALIGN_CENTER, 0, -20);

    _win_score_label = make_text(_win_panel, C_TEXT, &lv_font_montserrat_20);
    lv_obj_align(_win_score_label, LV_ALIGN_CENTER, 0, 100);

    add_footer(_win_panel, "Any Button = Retry");
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
