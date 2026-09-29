#include "screen_enroll.h"
#include "screens.h"
#include "screen_add_user.h"
#include "header.h"
#include "theme.h"
#include "users.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the enrollment widget declared in screen_enroll.h -- four
            states: ST_FIRST/ST_LAST (name wheel), ST_CONFIRM (name summary),
            ST_SCAN (re-scan to confirm the badge before saving).
*/

// ── character set ─────────────────────────────────────────────────────────────
// Indices: 0-25 = A-Z, 26 = space, 27 = dash
static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ -";
#define CHARSET_LEN 28
#define NAME_LEN    11
#define IDX_DASH    27
#define IDX_A       0

// ── state ─────────────────────────────────────────────────────────────────────
typedef enum { ST_FIRST, ST_LAST, ST_CONFIRM, ST_SCAN } EnrollState;

static char        _badge_id[48];
static int8_t      _idx[2][NAME_LEN];
static uint8_t     _cursor;
static uint8_t     _active_row;
static EnrollState _state;

// ── LVGL handles ──────────────────────────────────────────────────────────────
static lv_obj_t *_scr;
static lv_obj_t *_content;
static lv_obj_t *_cell[2][NAME_LEN];
static lv_obj_t *_cell_lbl[2][NAME_LEN];
static lv_obj_t *_scan_status_lbl;

// ── helpers ───────────────────────────────────────────────────────────────────

// Reads one name-wheel row into a trimmed string.
static void get_name(uint8_t row, char *out) {
    int last_real = -1;
    for (int i = 0; i < NAME_LEN; i++) {
        char c = CHARSET[(uint8_t)_idx[row][i]];
        if (c != '-' && c != ' ') last_real = i;
    }
    for (int i = 0; i <= last_real; i++) out[i] = CHARSET[(uint8_t)_idx[row][i]];
    out[last_real + 1] = '\0';
}

// True if the given name-wheel row has at least one real character typed.
static bool name_valid(uint8_t row) {
    char buf[NAME_LEN + 1];
    get_name(row, buf);
    return buf[0] != '\0';
}

// 2026-08-26 — split out of what used to be an all-22-cells refresh_cells() called on
// every single Up/Down/Left/Right press. That meant ~5 LVGL calls (text + 4 style props)
// per cell x 22 cells = 100+ calls per keypress, most of them rewriting a value that
// didn't change, firing every REPEAT_INTERVAL_MS while held — real, measurable overhead,
// not just a feeling. Up/Down only ever change the one cell under the cursor; Left/Right
// only move which cell is highlighted (old + new). Both now touch just what changed.
static void refresh_cell(uint8_t row, uint8_t col) {
    if (!_cell_lbl[row][col]) return;

    char buf[2] = { CHARSET[(uint8_t)_idx[row][col]], '\0' };
    lv_label_set_text(_cell_lbl[row][col], buf);

    bool is_cursor = ((int)_active_row == row && (int)_cursor == col);
    bool is_active = ((int)_active_row == row);

    lv_color_t color = is_cursor ? lv_color_hex(C_CYAN)
                     : is_active  ? lv_color_hex(C_TEXT)
                                  : lv_color_hex(C_DIM);
    lv_obj_set_style_text_color(_cell_lbl[row][col], color, LV_PART_MAIN);

    lv_border_side_t side = is_cursor ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_NONE;
    lv_obj_set_style_border_side(_cell[row][col], side, LV_PART_MAIN);
    lv_obj_set_style_border_width(_cell[row][col], is_cursor ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(_cell[row][col], lv_color_hex(C_CYAN), LV_PART_MAIN);
}

// Full refresh — only actually needed when EVERY cell's active/dim state changes at once
// (switching between first-name and last-name rows), not on a per-keypress hot path.
static void refresh_cells() {
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < NAME_LEN; col++)
            refresh_cell(row, col);
}

// ── forward declarations ──────────────────────────────────────────────────────
static void build_input_ui();
static void build_confirm_ui();
static void build_scan_ui();

// ── button callbacks ──────────────────────────────────────────────────────────

// Left: moves the cursor left within the active row.
static void cb_left() {
    if (_cursor > 0) {
        uint8_t old = _cursor;
        _cursor--;
        refresh_cell(_active_row, old);      // loses the cursor highlight
        refresh_cell(_active_row, _cursor);  // gains it
    }
}

// Right: moves the cursor right within the active row.
static void cb_right() {
    if (_cursor < NAME_LEN - 1) {
        uint8_t old = _cursor;
        _cursor++;
        refresh_cell(_active_row, old);
        refresh_cell(_active_row, _cursor);
    }
}

// Up: cycles the letter under the cursor forward.
static void cb_up() {
    _idx[_active_row][_cursor] = (_idx[_active_row][_cursor] + 1) % CHARSET_LEN;
    refresh_cell(_active_row, _cursor);
}

// Down: cycles the letter under the cursor backward.
static void cb_down() {
    _idx[_active_row][_cursor] = (_idx[_active_row][_cursor] + CHARSET_LEN - 1) % CHARSET_LEN;
    refresh_cell(_active_row, _cursor);
}

// Enter: advances first name -> last name -> confirm.
static void cb_next() {
    if (_active_row == 0) {
        if (!name_valid(0)) return;
        _active_row = 1;
        _cursor = 0;
        refresh_cells();
    } else {
        if (!name_valid(1)) return;
        _state = ST_CONFIRM;
        lv_obj_clean(_content);
        memset(_cell,     0, sizeof(_cell));
        memset(_cell_lbl, 0, sizeof(_cell_lbl));
        build_confirm_ui();
    }
}

// Back: steps back a field, or cancels to IDLE from the first field.
static void cb_back() {
    if (_active_row == 1) {
        _active_row = 0;
        _cursor = 0;
        refresh_cells();
    } else {
        screen_idle_load();
    }
}

// Enter: proceeds to the re-scan confirm step.
static void cb_yes() {
    _state = ST_SCAN;
    lv_obj_clean(_content);
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));
    build_scan_ui();
}

// Back: returns to the confirm step.
static void cb_back_from_scan() {
    _state = ST_CONFIRM;
    lv_obj_clean(_content);
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));
    build_confirm_ui();
}

// Back: returns to editing the last name.
static void cb_no() {
    _state = ST_LAST;
    _active_row = 1;
    _cursor = 0;
    lv_obj_clean(_content);
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));
    build_input_ui();
}

// ── UI builders ───────────────────────────────────────────────────────────────

// Builds one "Enter First/Last Name:" hint label.
static lv_obj_t *make_section_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(lbl, LV_PCT(100));
    return lbl;
}

// Builds one row of character cells for the name wheel.
static void build_char_row(lv_obj_t *parent, uint8_t row) {
    lv_obj_t *row_cont = lv_obj_create(parent);
    lv_obj_set_size(row_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_layout(row_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int col = 0; col < NAME_LEN; col++) {
        lv_obj_t *cell = lv_obj_create(row_cont);
        lv_obj_set_size(cell, 20, 28);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        _cell[row][col] = cell;

        lv_obj_t *lbl = lv_label_create(cell);
        char buf[2] = { CHARSET[(uint8_t)_idx[row][col]], '\0' };
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_center(lbl);
        _cell_lbl[row][col] = lbl;
    }
}

// Builds the ST_FIRST/ST_LAST (name wheel) UI.
static void build_input_ui() {
    make_section_label(_content, "Enter First Name:");
    build_char_row(_content, 0);

    lv_obj_t *spacer = lv_obj_create(_content);
    lv_obj_set_size(spacer, LV_PCT(100), 16);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(spacer, 0, LV_PART_MAIN);

    make_section_label(_content, "Enter Last Name:");
    build_char_row(_content, 1);

    // grow spacer to push footer to bottom
    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    // Same wrapped-name char wheel as screen_add_item.cpp -- matching legend: Left/Right
    // walk the cursor, Up/Down change the letter, Green advances (first -> last -> confirm),
    // Red steps back a field / exits.
    lv_obj_t *legend = ui_legend(_content);
    char cursor_lbl[28], letter_lbl[28];
    snprintf(cursor_lbl, sizeof(cursor_lbl), "%s%s Cursor", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    snprintf(letter_lbl, sizeof(letter_lbl), "Letter %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, cursor_lbl, lv_color_hex(C_YELLOW), letter_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Next", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    refresh_cells();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_left;
    h.right = cb_right;
    h.enter = cb_next;
    h.back  = cb_back;
    buttons_set_handlers(h);
}

// Builds the ST_CONFIRM UI.
static void build_confirm_ui() {
    char first[NAME_LEN + 1], last[NAME_LEN + 1];
    get_name(0, first);
    get_name(1, last);

    char full[NAME_LEN * 2 + 4];
    snprintf(full, sizeof(full), "%s %s", first, last);

    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_text(prompt, "Add this person?");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *name_lbl = lv_label_create(_content);
    lv_label_set_text(name_lbl, full);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_width(name_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(_content);
    ui_legend_row(legend, "Yes", lv_color_hex(C_GREEN), "No", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.enter = cb_yes;
    h.back  = cb_no;
    buttons_set_handlers(h);
}

// Builds the ST_SCAN (re-scan-confirm) UI.
static void build_scan_ui() {
    _scan_status_lbl = nullptr;

    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_text(prompt, "Scan your badge to confirm");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    _scan_status_lbl = lv_label_create(_content);
    lv_label_set_text(_scan_status_lbl, "");
    lv_obj_set_style_text_color(_scan_status_lbl, lv_color_hex(C_ORANGE), LV_PART_MAIN);
    lv_obj_set_style_text_font(_scan_status_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(_scan_status_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(_scan_status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *legend = ui_legend(_content);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Cancel", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.back = cb_back_from_scan;
    h.wantsScanner = true;  // waiting for the same badge again, to confirm it
    buttons_set_handlers(h);
}

// ── public ────────────────────────────────────────────────────────────────────

// Consumes the ST_SCAN re-scan -- creates the user on a matching badge, else shows a mismatch.
bool screen_enroll_on_scan(const char *badge_id) {
    if (_state != ST_SCAN || !_scr || lv_scr_act() != _scr) return false;

    if (strcmp(badge_id, _badge_id) == 0) {
        char first[NAME_LEN + 1], last[NAME_LEN + 1];
        get_name(0, first);
        get_name(1, last);
        int user_id = users_create(_badge_id, first, last);
        if (user_id < 0) {
            Serial.println("[ENROLL] user store full");
            if (_scan_status_lbl) lv_label_set_text(_scan_status_lbl, "Save failed - try again");
            return true;
        }
        Serial.printf("[ENROLL] Created user %d: %s %s\n", user_id, first, last);
        // 2026-08-25: used to drop straight into screen_pos_push(user_id) here, which made
        // sense for self-service enrollment (a nurse enrolling herself naturally continues
        // into her own transaction). Self-enrollment is gone now (owner's explicit "no guest
        // checkout, admin-only enrollment" call) -- the only caller of this flow is an admin
        // enrolling someone else via Add User, so this should return the admin to that scan
        // prompt (ready for the next new person), not leave them sitting in a stranger's cart.
        screen_add_user_push();
    } else {
        Serial.printf("[ENROLL] Badge mismatch during confirm scan\n");
        if (_scan_status_lbl) lv_label_set_text(_scan_status_lbl, "Wrong badge - try again");
    }
    return true;
}

// Loads the enrollment widget for the given (already-confirmed-unenrolled) badge.
void screen_enroll_push(const char *badge_id) {
    strncpy(_badge_id, badge_id, sizeof(_badge_id) - 1);
    _badge_id[sizeof(_badge_id) - 1] = '\0';

    _state      = ST_FIRST;
    _active_row = 0;
    _cursor     = 0;

    for (int row = 0; row < 2; row++) {
        _idx[row][0] = IDX_A;
        for (int col = 1; col < NAME_LEN; col++) _idx[row][col] = IDX_DASH;
    }
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);
    }

    if (!_content) {
        _content = lv_obj_create(_scr);
        lv_obj_set_size(_content, SCREEN_W, SCREEN_H - HDR_H);
        lv_obj_align(_content, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(_content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(_content, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(_content, 12, LV_PART_MAIN);
        // The footer legend is this flex column's last child, so pad_ver's bottom inset
        // was also its distance from the true screen edge -- overridden separately to
        // match the ~6px margin every explicitly-aligned legend elsewhere uses (see
        // screen_item_edit.cpp's identical fix).
        lv_obj_set_style_pad_bottom(_content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_content, 8, LV_PART_MAIN);
        lv_obj_set_layout(_content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(_content, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(_content, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clean(_content);
    }

    header_set_current_user("");
    build_input_ui();
    lv_scr_load(_scr);
}
