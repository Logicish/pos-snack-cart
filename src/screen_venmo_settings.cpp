#include "screen_venmo_settings.h"
#include "screen_settings.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "db.h"
#include "ui.h"
#include <sqlite3.h>
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>
#include <ctype.h>

// Same wrapped-two-row wheel geometry as screen_add_item.cpp's item-name field (11 + 7 =
// 18 chars, cursor auto-crosses the row boundary) — proven layout, reused for two fields
// in sequence here (handle, then owner display name) instead of one.
#define ROW0_LEN 11
#define ROW1_LEN  7
#define FIELD_LEN 11  // widest row -- array width
#define TOTAL_LEN (ROW0_LEN + ROW1_LEN)

// Handle charset includes digits/underscore (real Venmo handles commonly have both, e.g.
// "jane-doe21") — unlike the letters-only wheels used for people/item names elsewhere.
// Venmo's own matching is case-insensitive, so keeping this uppercase-only (like every
// other wheel in this codebase) doesn't lose anything real.
static const char HANDLE_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
#define HANDLE_CHARSET_LEN 38
#define HANDLE_BLANK_IDX   36  // '-'

static const char NAME_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ -";
#define NAME_CHARSET_LEN 28
#define NAME_BLANK_IDX   27  // '-'

typedef enum { ST_HANDLE, ST_NAME, ST_CONFIRM } VenmoState;

static lv_obj_t  *_scr;
static lv_obj_t  *_content;
static VenmoState _state;

// Two persistent field buffers (handle survives while editing name and vice versa, so
// Back-a-field doesn't lose what was typed) -- _idx points at whichever is active.
static int8_t     _idx_handle[2][FIELD_LEN];
static int8_t     _idx_name[2][FIELD_LEN];
static int8_t    (*_idx)[FIELD_LEN];
static uint8_t     _wcursor, _wrow;
static lv_obj_t   *_cell[2][FIELD_LEN];
static lv_obj_t   *_cell_lbl[2][FIELD_LEN];
static const char *_charset;
static int         _charset_len;

// Computed (trimmed) field values, captured when leaving each field via cb_*_next() --
// read again at Save time rather than re-deriving from _idx, since by ST_CONFIRM _idx no
// longer points at either field specifically.
static char _handle_buf[TOTAL_LEN + 1];
static char _owner_buf[TOTAL_LEN + 1];

static void build_handle_ui();
static void build_name_ui();
static void build_confirm_ui();

static inline uint8_t row_len(uint8_t row) { return row == 0 ? ROW0_LEN : ROW1_LEN; }

static void clear_content() {
    lv_obj_clean(_content);
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));
}

// Loads an existing DB string into a field's wheel state, uppercased, one char per cell,
// unmatched/absent characters fall back to the blank filler. Row 0 first then row 1, same
// concatenation order get_field() reads back.
static void load_field(int8_t dest[2][FIELD_LEN], const char *str,
                        const char *charset, int charset_len, int8_t blank_idx) {
    int len = strlen(str);
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < row_len(row); col++) {
            int flat = (row == 0) ? col : ROW0_LEN + col;
            int8_t val = blank_idx;
            if (flat < len) {
                char c = (char)toupper((unsigned char)str[flat]);
                for (int i = 0; i < charset_len; i++) {
                    if (charset[i] == c) { val = (int8_t)i; break; }
                }
            }
            dest[row][col] = val;
        }
    }
}

// Concatenates row 0 + row 1 from the currently-active field/charset, then trims the
// trailing run of dashes/spaces -- same trim philosophy as screen_add_item.cpp's
// get_name(), reused here for both fields (the handle charset has no space at all, so
// that half of the check is simply a no-op there).
static void get_field(char *out, size_t out_len) {
    char buf[TOTAL_LEN + 1];
    int pos = 0;
    for (int i = 0; i < ROW0_LEN; i++) buf[pos++] = _charset[(uint8_t)_idx[0][i]];
    for (int i = 0; i < ROW1_LEN; i++) buf[pos++] = _charset[(uint8_t)_idx[1][i]];
    buf[pos] = '\0';

    while (pos > 0 && (buf[pos - 1] == '-' || buf[pos - 1] == ' ')) buf[--pos] = '\0';

    strncpy(out, buf, out_len - 1);
    out[out_len - 1] = '\0';
}

static void refresh_wheel_cells() {
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < row_len(row); col++) {
            if (!_cell_lbl[row][col]) continue;

            char buf[2] = { _charset[(uint8_t)_idx[row][col]], '\0' };
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

static void cb_wheel_up() {
    _idx[_wrow][_wcursor] = (int8_t)((_idx[_wrow][_wcursor] + 1) % _charset_len);
    refresh_wheel_cells();
}

static void cb_wheel_down() {
    _idx[_wrow][_wcursor] = (int8_t)((_idx[_wrow][_wcursor] + _charset_len - 1) % _charset_len);
    refresh_wheel_cells();
}

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
        char buf[2] = { _charset[(uint8_t)_idx[row][col]], '\0' };
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_center(lbl);
        _cell_lbl[row][col] = lbl;
    }
}

// ============ ST_HANDLE ============

static void cb_handle_next() {
    get_field(_handle_buf, sizeof(_handle_buf));
    _state = ST_NAME;
    _idx = _idx_name;
    _charset = NAME_CHARSET;
    _charset_len = NAME_CHARSET_LEN;
    _wrow = 0;
    _wcursor = 0;
    clear_content();
    build_name_ui();
}

static void cb_handle_back() {
    screen_settings_push();  // first field -- Back cancels the whole editor
}

static void build_handle_ui() {
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_text(hint, "Venmo handle:");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    build_wheel_row(_content, 0);
    build_wheel_row(_content, 1);

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
    h.enter = cb_handle_next;
    h.back  = cb_handle_back;
    buttons_set_handlers(h);
}

// ============ ST_NAME (owner display name) ============

static void cb_name_next() {
    get_field(_owner_buf, sizeof(_owner_buf));
    _state = ST_CONFIRM;
    clear_content();
    build_confirm_ui();
}

// Back returns to the handle field, keeping whatever was already typed there
// (_idx_handle is a separate persistent array, untouched by editing the name field).
static void cb_name_back() {
    _state = ST_HANDLE;
    _idx = _idx_handle;
    _charset = HANDLE_CHARSET;
    _charset_len = HANDLE_CHARSET_LEN;
    _wrow = 0;
    _wcursor = 0;
    clear_content();
    build_handle_ui();
}

static void build_name_ui() {
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Owner name (shown as \"Payment to: ...\"):");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    build_wheel_row(_content, 0);
    build_wheel_row(_content, 1);

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
    ui_legend_row(legend, "Next", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    refresh_wheel_cells();

    ButtonHandlers h;
    h.up    = cb_wheel_up;
    h.down  = cb_wheel_down;
    h.left  = cb_wheel_left;
    h.right = cb_wheel_right;
    h.enter = cb_name_next;
    h.back  = cb_name_back;
    buttons_set_handlers(h);
}

// ============ ST_CONFIRM ============

// Same check-then-branch UPSERT workaround as screen_admin_tools.cpp's upsert_venmo() and
// webserver.cpp's handle_admin_settings() -- `ON CONFLICT...DO UPDATE` silently no-ops on
// this SQLite build (see project memory: feedback-no-upsert-syntax).
static void save_venmo(const char *handle, const char *owner) {
    sqlite3 *db = db_handle();
    if (!db) return;

    sqlite3_stmt *stmt;
    bool exists = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM payment_methods WHERE method='venmo';", -1, &stmt, nullptr) == SQLITE_OK) {
        exists = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }

    if (exists) {
        if (sqlite3_prepare_v2(db, "UPDATE payment_methods SET display_name=?, handle=?, enabled=1 WHERE method='venmo';", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, handle, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else if (sqlite3_prepare_v2(db, "INSERT INTO payment_methods (method, display_name, handle, enabled) VALUES ('venmo', ?, ?, 1);", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, handle, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

static void cb_confirm_save() {
    save_venmo(_handle_buf, _owner_buf);
    screen_settings_push();
}

static void cb_confirm_back() {
    _state = ST_NAME;
    _idx = _idx_name;
    _charset = NAME_CHARSET;
    _charset_len = NAME_CHARSET_LEN;
    _wrow = 0;
    _wcursor = 0;
    clear_content();
    build_name_ui();
}

static void build_confirm_ui() {
    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_text(prompt, "Save this payment info?");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    char summary[80];
    snprintf(summary, sizeof(summary), "Handle: %s\nOwner: %s",
             _handle_buf[0] ? _handle_buf : "(none)",
             _owner_buf[0]  ? _owner_buf  : "(none)");
    lv_obj_t *lbl = lv_label_create(_content);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, summary);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

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

void screen_venmo_settings_push() {
    _state = ST_HANDLE;

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
        lv_obj_set_style_pad_row(_content, 8, LV_PART_MAIN);
        lv_obj_set_layout(_content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
    } else {
        clear_content();
    }

    // Prefill both fields from whatever's already in payment_methods (blank if the DB
    // isn't available yet or nothing's been set -- the wheels just start empty).
    char handle[64] = "", owner[64] = "";
    if (db_handle()) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_handle(), "SELECT display_name, handle FROM payment_methods WHERE method='venmo';", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *dn = sqlite3_column_text(stmt, 0);
                const unsigned char *hd = sqlite3_column_text(stmt, 1);
                if (dn) strncpy(owner,  (const char *)dn, sizeof(owner) - 1);
                if (hd) strncpy(handle, (const char *)hd, sizeof(handle) - 1);
            }
            sqlite3_finalize(stmt);
        }
    }
    load_field(_idx_handle, handle, HANDLE_CHARSET, HANDLE_CHARSET_LEN, HANDLE_BLANK_IDX);
    load_field(_idx_name,   owner,  NAME_CHARSET,   NAME_CHARSET_LEN,   NAME_BLANK_IDX);
    _handle_buf[0] = '\0';
    _owner_buf[0]  = '\0';

    _idx = _idx_handle;
    _charset = HANDLE_CHARSET;
    _charset_len = HANDLE_CHARSET_LEN;
    _wrow = 0;
    _wcursor = 0;

    header_set_visible(true);
    header_set_title("VENMO PAYMENT INFO");
    build_handle_ui();

    lv_scr_load(_scr);
}
