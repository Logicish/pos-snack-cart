#include "screen_admin_password.h"
#include "screen_security_menu.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "webserver.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>
#include <ctype.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the admin password editor declared in screen_admin_password.h.
*/

// Same wrapped-two-row wheel geometry as screen_wifi_password.cpp (11 + 7 = 18 chars,
// cursor auto-crosses the row boundary), single field instead of a pair.
#define ROW0_LEN 11
#define ROW1_LEN  7
#define FIELD_LEN 11  // widest row -- array width
#define TOTAL_LEN (ROW0_LEN + ROW1_LEN)

// Letters/digits/dash/underscore -- same charset as the WiFi password editor, plenty for
// a password that's only ever typed on this same wheel (no real strength requirement).
static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
#define CHARSET_LEN 38
#define BLANK_IDX   36  // '-'

#define MIN_LEN 4  // matches the old web Change Password page's "too short" threshold

typedef enum { ST_PASSWORD, ST_CONFIRM } AdminPwState;

static lv_obj_t    *_scr;
static lv_obj_t    *_content;
static AdminPwState _state;

static int8_t     _idx[2][FIELD_LEN];
static uint8_t     _wcursor, _wrow;
static lv_obj_t   *_cell[2][FIELD_LEN];
static lv_obj_t   *_cell_lbl[2][FIELD_LEN];
static lv_obj_t   *_error_lbl;  // ST_PASSWORD only -- "at least N characters" on a failed Next

static char _password_buf[TOTAL_LEN + 1];

static void build_password_ui();
static void build_confirm_ui();

// Returns how many cells the given wheel row has.
static inline uint8_t row_len(uint8_t row) { return row == 0 ? ROW0_LEN : ROW1_LEN; }

static void clear_content() {
    lv_obj_clean(_content);
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));
    _error_lbl = nullptr;
}

// Loads the current password into the wheel state, one char per cell -- prefills with
// whatever's currently saved so editing doesn't start from a blank field.
static void load_field(const char *str) {
    int len = strlen(str);
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < row_len(row); col++) {
            int flat = (row == 0) ? col : ROW0_LEN + col;
            int8_t val = BLANK_IDX;
            if (flat < len) {
                char c = (char)toupper((unsigned char)str[flat]);
                for (int i = 0; i < CHARSET_LEN; i++) {
                    if (CHARSET[i] == c) { val = (int8_t)i; break; }
                }
            }
            _idx[row][col] = val;
        }
    }
}

// Concatenates row 0 + row 1, then trims the trailing run of dashes -- same trim
// philosophy as screen_wifi_password.cpp's get_field().
static void get_field(char *out, size_t out_len) {
    char buf[TOTAL_LEN + 1];
    int pos = 0;
    for (int i = 0; i < ROW0_LEN; i++) buf[pos++] = CHARSET[(uint8_t)_idx[0][i]];
    for (int i = 0; i < ROW1_LEN; i++) buf[pos++] = CHARSET[(uint8_t)_idx[1][i]];
    buf[pos] = '\0';

    while (pos > 0 && buf[pos - 1] == '-') buf[--pos] = '\0';

    strncpy(out, buf, out_len - 1);
    out[out_len - 1] = '\0';
}

// Redraws every wheel cell, highlighting the cursor cell.
static void refresh_wheel_cells() {
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < row_len(row); col++) {
            if (!_cell_lbl[row][col]) continue;

            char buf[2] = { CHARSET[(uint8_t)_idx[row][col]], '\0' };
            lv_label_set_text(_cell_lbl[row][col], buf);

            bool is_cursor = ((int)_wrow == row && (int)_wcursor == col);
            lv_color_t color = is_cursor ? lv_color_hex(C_CYAN) : lv_color_hex(C_TEXT);
            lv_obj_set_style_text_color(_cell_lbl[row][col], color, LV_PART_MAIN);

            lv_border_side_t side = is_cursor ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_NONE;
            lv_obj_set_style_border_side(_cell[row][col], side, LV_PART_MAIN);
            lv_obj_set_style_border_width(_cell[row][col], is_cursor ? 2 : 0, LV_PART_MAIN);
            lv_obj_set_style_border_color(_cell[row][col], lv_color_hex(C_CYAN), LV_PART_MAIN);
        }
    }
}

// Left: moves the cursor left, crossing onto the previous row at the boundary.
static void cb_wheel_left() {
    if (_wcursor > 0) {
        _wcursor--;
    } else if (_wrow == 1) {
        _wrow = 0;
        _wcursor = ROW0_LEN - 1;
    } else {
        return;
    }
    refresh_wheel_cells();
}

// Right: moves the cursor right, crossing onto the next row at the boundary.
static void cb_wheel_right() {
    if (_wcursor < row_len(_wrow) - 1) {
        _wcursor++;
    } else if (_wrow == 0) {
        _wrow = 1;
        _wcursor = 0;
    } else {
        return;
    }
    refresh_wheel_cells();
}

// Up: cycles the letter under the cursor forward.
static void cb_wheel_up() {
    _idx[_wrow][_wcursor] = (int8_t)((_idx[_wrow][_wcursor] + 1) % CHARSET_LEN);
    refresh_wheel_cells();
}

// Down: cycles the letter under the cursor backward.
static void cb_wheel_down() {
    _idx[_wrow][_wcursor] = (int8_t)((_idx[_wrow][_wcursor] + CHARSET_LEN - 1) % CHARSET_LEN);
    refresh_wheel_cells();
}

// Builds one row of character cells for the password wheel.
static void build_wheel_row(lv_obj_t *parent, uint8_t row) {
    lv_obj_t *row_cont = lv_obj_create(parent);
    lv_obj_set_size(row_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row_cont, 7, LV_PART_MAIN);
    lv_obj_set_layout(row_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int col = 0; col < row_len(row); col++) {
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

// ============ ST_PASSWORD ============

static void cb_password_next() {
    get_field(_password_buf, sizeof(_password_buf));
    if (strlen(_password_buf) < MIN_LEN) {
        if (_error_lbl) {
            lv_label_set_text(_error_lbl, "Needs to be at least 4 characters.");
        }
        return;
    }
    _state = ST_CONFIRM;
    clear_content();
    build_confirm_ui();
}

static void cb_password_back() {
    screen_security_menu_push();  // Back cancels the whole editor
}

// Builds the ST_PASSWORD entry UI.
static void build_password_ui() {
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_text(hint, "Admin password (min 4 characters):");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    build_wheel_row(_content, 0);
    build_wheel_row(_content, 1);

    _error_lbl = lv_label_create(_content);
    lv_label_set_long_mode(_error_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_error_lbl, "");
    lv_obj_set_style_text_color(_error_lbl, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_error_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(_error_lbl, LV_PCT(100));

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(_content);
    char cursor_lbl[28], letter_lbl[28];
    snprintf(cursor_lbl, sizeof(cursor_lbl), "%s%s Cursor", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    snprintf(letter_lbl, sizeof(letter_lbl), "Letter %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, cursor_lbl, lv_color_hex(C_YELLOW), letter_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Next", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));

    refresh_wheel_cells();

    ButtonHandlers h;
    h.up    = cb_wheel_up;
    h.down  = cb_wheel_down;
    h.left  = cb_wheel_left;
    h.right = cb_wheel_right;
    h.enter = cb_password_next;
    h.back  = cb_password_back;
    buttons_set_handlers(h);
}

// ============ ST_CONFIRM ============

static void cb_confirm_save() {
    webserver_set_admin_password(_password_buf);  // already length-checked before reaching here
    screen_security_menu_push();
}

// Back: returns to editing the password.
static void cb_confirm_back() {
    _state = ST_PASSWORD;
    _wrow = 0;
    _wcursor = 0;
    clear_content();
    build_password_ui();
}

// Builds the ST_CONFIRM UI.
static void build_confirm_ui() {
    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_text(prompt, "Save this admin password?");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(_content);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, _password_buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *note = lv_label_create(_content);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, "Shared by every admin logging into the Web Portal -- takes effect on the next login attempt.");
    lv_obj_set_style_text_color(note, lv_color_hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(_content);
    ui_legend_row(legend, "Save", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.enter = cb_confirm_save;
    h.back  = cb_confirm_back;
    buttons_set_handlers(h);
}

// ============ public ============

// Loads the admin password editor, prefilled with the current value.
void screen_admin_password_push() {
    _state = ST_PASSWORD;

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
        lv_obj_set_flex_align(_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
    } else {
        clear_content();
    }

    load_field(webserver_admin_password());
    _password_buf[0] = '\0';

    _wrow = 0;
    _wcursor = 0;

    header_set_visible(true);
    header_set_title("ADMIN PASSWORD");
    build_password_ui();

    lv_scr_load(_scr);
}
