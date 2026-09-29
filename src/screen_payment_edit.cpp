#include "screen_payment_edit.h"
#include "screen_payment_menu.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "db.h"
#include "ui.h"
#include "payment_link.h"
#include <sqlite3.h>
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>
#include <ctype.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the generic payment-method editor declared in
            screen_payment_edit.h -- ST_VIEW (already set up: Update/Delete), ST_SCAN (scan
            the owner's own app QR, the default way in), ST_ZTYPE (Zelle only: phone or
            email?), ST_HANDLE, ST_NAME, ST_CONFIRM.
  Notes---- 2026-09-29: one account per method. An existing one opens on ST_VIEW; a new
            one opens on ST_SCAN, with "Type it" falling back to the wheels. Wheel
            geometry and charset are per-field, so Zelle can take a 10-digit phone number
            or a 33-char email. The Zelle token (typed phone/email, or the whole scanned
            bank link) lives in the same payment_methods.handle column -- no schema change.
*/

// Per-field wheel geometry -- how many rows, and how many cells in each.
typedef struct { uint8_t rows; uint8_t len[3]; } WheelGeom;

// Same wrapped two-row layout as screen_add_item.cpp's item-name field (11 + 7 = 18 chars,
// cursor auto-crosses the row boundary) for handles and the owner name.
static const WheelGeom GEOM_TEXT  = { 2, { 11,  7,  0 } };
static const WheelGeom GEOM_PHONE = { 1, { 10,  0,  0 } };  // US 10-digit number
static const WheelGeom GEOM_EMAIL = { 3, { 11, 11, 11 } };  // 33 chars covers typical addresses

#define MAX_ROWS  3
#define FIELD_LEN 11   // widest row -- array width
#define MAX_CHARS 33   // most cells any geometry has

// Handle charset includes digits/underscore (real handles commonly have both, e.g.
// "jane-doe21") — unlike the letters-only wheels used for people/item names elsewhere.
// Matching is case-insensitive on every payment app this targets, so keeping this
// uppercase-only (like every other wheel in this codebase) doesn't lose anything real.
static const char HANDLE_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
#define HANDLE_CHARSET_LEN 38
#define HANDLE_BLANK_IDX   36  // '-'

static const char NAME_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ -";
#define NAME_CHARSET_LEN 28
#define NAME_BLANK_IDX   27  // '-'

// Zelle phone: digits only, '-' as the blank filler (trimmed off the end like every wheel).
static const char PHONE_CHARSET[] = "0123456789-";
#define PHONE_CHARSET_LEN 11
#define PHONE_BLANK_IDX   10

// Zelle email: lowercase since it's shown back to the owner as an address. A '-' inside
// an address still works; only trailing dashes get trimmed as blanks.
static const char EMAIL_CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789@._+-";
#define EMAIL_CHARSET_LEN 41
#define EMAIL_BLANK_IDX   40

typedef enum { ST_VIEW, ST_SCAN, ST_ZTYPE, ST_HANDLE, ST_NAME, ST_CONFIRM } PaymentEditState;

static lv_obj_t        *_scr;
static lv_obj_t        *_content;
static PaymentEditState _state;

// Which payment_methods row this instance of the screen is editing — set fresh each time
// screen_payment_edit_push() is called, since the same screen/widgets are reused for
// Venmo/Zelle/Cashapp rather than building three near-identical screens.
static char _method[16];
static char _label[16];
static bool _is_zelle;
static bool _zelle_email;      // ST_ZTYPE's choice -- false = phone
static int  _ztype_cursor;     // 0 = phone, 1 = email
static lv_obj_t *_ztype_rows[2];

// The saved handle as loaded from the DB -- kept so picking a Zelle type re-prefills from
// it when it's the same kind (phone vs email), instead of starting blank.
static char _db_handle[PAYMENT_HANDLE_MAX];
static char _db_owner[64];
static bool _exists;  // this method already has a saved row -- opens on ST_VIEW

// Two persistent field buffers (handle survives while editing name and vice versa, so
// Back-a-field doesn't lose what was typed) -- _idx points at whichever is active.
static int8_t      _idx_handle[MAX_ROWS][FIELD_LEN];
static int8_t      _idx_name[MAX_ROWS][FIELD_LEN];
static int8_t    (*_idx)[FIELD_LEN];
static const WheelGeom *_geom;
static uint8_t     _wcursor, _wrow;
static lv_obj_t   *_cell[MAX_ROWS][FIELD_LEN];
static lv_obj_t   *_cell_lbl[MAX_ROWS][FIELD_LEN];
static const char *_charset;
static int         _charset_len;
static lv_obj_t   *_status_lbl;  // ST_SCAN's / ST_HANDLE's error message
static bool        _scanned;     // handle came from ST_SCAN, not the wheel

// Computed (trimmed) field values, captured when leaving each field via cb_*_next() --
// read again at Save time rather than re-deriving from _idx, since by ST_CONFIRM _idx no
// longer points at either field specifically.
static char _handle_buf[PAYMENT_HANDLE_MAX];  // a scanned Zelle link is ~130 chars
static char _owner_buf[MAX_CHARS + 1];

static void build_view_ui();
static void build_scan_ui();
static void build_ztype_ui();
static void build_handle_ui();
static void build_name_ui();
static void build_confirm_ui();

// Clears the content area between states.
static void clear_content() {
    lv_obj_clean(_content);
    memset(_cell,       0, sizeof(_cell));
    memset(_cell_lbl,   0, sizeof(_cell_lbl));
    memset(_ztype_rows, 0, sizeof(_ztype_rows));
    _status_lbl = nullptr;
}

// Loads a string into a field's wheel state, one char per cell in row order, matched
// case-insensitively against the charset; unmatched/absent characters become the blank.
static void load_field(int8_t dest[MAX_ROWS][FIELD_LEN], const char *str, const WheelGeom &g,
                       const char *charset, int charset_len, int8_t blank_idx) {
    int len = strlen(str);
    int flat = 0;
    for (int row = 0; row < MAX_ROWS; row++) {
        for (int col = 0; col < FIELD_LEN; col++) {
            int8_t val = blank_idx;
            if (row < g.rows && col < g.len[row]) {
                if (flat < len) {
                    char c = (char)tolower((unsigned char)str[flat]);
                    for (int i = 0; i < charset_len; i++) {
                        if (tolower((unsigned char)charset[i]) == c) { val = (int8_t)i; break; }
                    }
                }
                flat++;
            }
            dest[row][col] = val;
        }
    }
}

// Concatenates the active field's rows, then trims the trailing run of dashes/spaces --
// same trim philosophy as screen_add_item.cpp's get_name().
static void get_field(char *out, size_t out_len) {
    char buf[MAX_CHARS + 1];
    int pos = 0;
    for (int row = 0; row < _geom->rows; row++)
        for (int col = 0; col < _geom->len[row]; col++)
            buf[pos++] = _charset[(uint8_t)_idx[row][col]];
    buf[pos] = '\0';

    while (pos > 0 && (buf[pos - 1] == '-' || buf[pos - 1] == ' ')) buf[--pos] = '\0';

    strncpy(out, buf, out_len - 1);
    out[out_len - 1] = '\0';
}

// Points the wheel at the handle field, with the charset/geometry this method needs.
static void select_handle_field() {
    _idx = _idx_handle;
    if (_is_zelle && _zelle_email) {
        _geom = &GEOM_EMAIL; _charset = EMAIL_CHARSET; _charset_len = EMAIL_CHARSET_LEN;
    } else if (_is_zelle) {
        _geom = &GEOM_PHONE; _charset = PHONE_CHARSET; _charset_len = PHONE_CHARSET_LEN;
    } else {
        _geom = &GEOM_TEXT;  _charset = HANDLE_CHARSET; _charset_len = HANDLE_CHARSET_LEN;
    }
    _wrow = 0;
    _wcursor = 0;
}

// Points the wheel at the owner-name field.
static void select_name_field() {
    _idx = _idx_name;
    _geom = &GEOM_TEXT;
    _charset = NAME_CHARSET;
    _charset_len = NAME_CHARSET_LEN;
    _wrow = 0;
    _wcursor = 0;
}

// Redraws every wheel cell, highlighting the cursor cell.
static void refresh_wheel_cells() {
    for (int row = 0; row < _geom->rows; row++) {
        for (int col = 0; col < _geom->len[row]; col++) {
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

// Left: moves the cursor left, crossing onto the previous row at the boundary.
static void cb_wheel_left() {
    if (_wcursor > 0) {
        _wcursor--;
    } else if (_wrow > 0) {
        _wrow--;
        _wcursor = _geom->len[_wrow] - 1;
    } else {
        return;
    }
    refresh_wheel_cells();
}

// Right: moves the cursor right, crossing onto the next row at the boundary.
static void cb_wheel_right() {
    if (_wcursor < _geom->len[_wrow] - 1) {
        _wcursor++;
    } else if (_wrow + 1 < _geom->rows) {
        _wrow++;
        _wcursor = 0;
    } else {
        return;
    }
    refresh_wheel_cells();
}

// Up: cycles the character under the cursor forward.
static void cb_wheel_up() {
    _idx[_wrow][_wcursor] = (int8_t)((_idx[_wrow][_wcursor] + 1) % _charset_len);
    refresh_wheel_cells();
}

// Down: cycles the character under the cursor backward.
static void cb_wheel_down() {
    _idx[_wrow][_wcursor] = (int8_t)((_idx[_wrow][_wcursor] + _charset_len - 1) % _charset_len);
    refresh_wheel_cells();
}

// Builds one row of character cells for whichever field is active.
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

    for (int col = 0; col < _geom->len[row]; col++) {
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

// Adds the flex spacer that pushes the footer legend to the bottom.
static void add_grow(lv_obj_t *parent) {
    lv_obj_t *grow = lv_obj_create(parent);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);
}

// Adds the standard four-way wheel legend with the given Enter/Back labels.
static void add_wheel_legend(const char *enter_lbl, const char *back_lbl) {
    lv_obj_t *legend = ui_legend(_content);
    char cursor_lbl[28], letter_lbl[28];
    snprintf(cursor_lbl, sizeof(cursor_lbl), "%s%s Cursor", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    snprintf(letter_lbl, sizeof(letter_lbl), "Letter %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, cursor_lbl, lv_color_hex(C_YELLOW), letter_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, enter_lbl, lv_color_hex(C_GREEN), back_lbl, lv_color_hex(C_RED));
}

// ============ ST_SCAN (scan the owner's own "my code") ============

// Right: skip scanning and type the handle on the wheel instead.
static void cb_scan_type() {
    _scanned = false;
    clear_content();
    if (_is_zelle) {
        _state = ST_ZTYPE;
        build_ztype_ui();
    } else {
        _state = ST_HANDLE;
        select_handle_field();
        build_handle_ui();
    }
}

// Back: to the view screen if this method is already set up, else out to the menu.
static void cb_scan_back() {
    if (_exists) {
        _state = ST_VIEW;
        clear_content();
        build_view_ui();
    } else {
        screen_payment_menu_push();
    }
}

// Builds the ST_SCAN UI -- the default first step for every method.
static void build_scan_ui() {
    lv_obj_t *prompt = lv_label_create(_content);
    char prompt_buf[48];
    snprintf(prompt_buf, sizeof(prompt_buf), "Scan your %s QR code", payment_method_label(_method));
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_label_set_text(prompt, prompt_buf);
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Where each app keeps the owner's own code. Turning the phone's brightness up helps
    // the scanner read a screen.
    const char *where =
        _is_zelle                          ? "Bank app > Zelle > your QR code" :
        strcmp(_method, "cashapp") == 0    ? "Cash App > profile > QR code" :
                                             "Venmo > Me > scan icon > My code";
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, where);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    _status_lbl = lv_label_create(_content);
    lv_label_set_long_mode(_status_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_status_lbl, "");
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_status_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(_status_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(_status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    add_grow(_content);

    lv_obj_t *legend = ui_legend(_content);
    char type_lbl[24];
    snprintf(type_lbl, sizeof(type_lbl), "Type it %s", LV_SYMBOL_RIGHT);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), type_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Cancel", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.right = cb_scan_type;
    h.back  = cb_scan_back;
    h.wantsScanner = true;  // waiting for the owner's own payment QR
    buttons_set_handlers(h);
}

// ============ ST_ZTYPE (Zelle only: phone or email) ============

// Highlights the selected Zelle-type row.
static void refresh_ztype_cursor() {
    for (int i = 0; i < 2; i++) {
        if (_ztype_rows[i]) {
            lv_obj_set_style_bg_opa(_ztype_rows[i], i == _ztype_cursor ? LV_OPA_30 : LV_OPA_TRANSP,
                                    LV_PART_MAIN);
        }
    }
}

// Up/Down: toggles between the two rows.
static void cb_ztype_move() {
    _ztype_cursor = 1 - _ztype_cursor;
    refresh_ztype_cursor();
}

// Enter: locks in phone vs email, prefills from the saved token if it's the same kind.
static void cb_ztype_select() {
    _zelle_email = (_ztype_cursor == 1);
    // A scanned bank link isn't a typed phone/email, so it never prefills the wheel.
    bool saved_is_link  = strncmp(_db_handle, "https://", 8) == 0;
    bool saved_is_email = strchr(_db_handle, '@') != nullptr;
    const char *prefill = (_db_handle[0] && !saved_is_link && saved_is_email == _zelle_email)
                          ? _db_handle : "";
    if (_zelle_email) {
        load_field(_idx_handle, prefill, GEOM_EMAIL, EMAIL_CHARSET, EMAIL_CHARSET_LEN, EMAIL_BLANK_IDX);
    } else {
        load_field(_idx_handle, prefill, GEOM_PHONE, PHONE_CHARSET, PHONE_CHARSET_LEN, PHONE_BLANK_IDX);
    }
    _state = ST_HANDLE;
    select_handle_field();
    clear_content();
    build_handle_ui();
}

static void cb_ztype_back() {
    _state = ST_SCAN;
    clear_content();
    build_scan_ui();
}

// Builds the ST_ZTYPE picker UI.
static void build_ztype_ui() {
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Your Zelle is registered to your:");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    static const char *LABELS[2] = { "Phone number", "Email" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_create(_content);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        _ztype_rows[i] = row;

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, LABELS[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    }

    add_grow(_content);

    lv_obj_t *legend = ui_legend(_content);
    char move_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Select", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));

    refresh_ztype_cursor();

    ButtonHandlers h;
    h.up    = cb_ztype_move;
    h.down  = cb_ztype_move;
    h.enter = cb_ztype_select;
    h.back  = cb_ztype_back;
    buttons_set_handlers(h);
}

// ============ ST_HANDLE ============

// Returns an error message if the Zelle token isn't usable yet, or nullptr if it's fine.
// Venmo/Cash App handles have no fixed shape to check.
static const char *validate_handle(const char *h) {
    if (!_is_zelle) return nullptr;
    if (!_zelle_email) {
        if (strlen(h) != 10) return "Enter all 10 digits";
        for (const char *p = h; *p; p++) if (!isdigit((unsigned char)*p)) return "Digits only, no gaps";
        return nullptr;
    }
    const char *at = strchr(h, '@');
    if (!at || at == h)                      return "Email needs name@domain.com";
    const char *dot = strrchr(at, '.');
    if (!dot || dot == at + 1 || !dot[1])    return "Email needs name@domain.com";
    if (strchr(at + 1, '@'))                 return "Only one @ allowed";
    return nullptr;
}

// Enter: validates, saves the handle field and proceeds to the owner-name field.
static void cb_handle_next() {
    char buf[MAX_CHARS + 1];
    get_field(buf, sizeof(buf));

    // Removing a method is the view screen's Delete now, so a blank handle is just an error.
    const char *err = buf[0] ? validate_handle(buf) : "Enter something first";
    if (err) {
        if (_status_lbl) lv_label_set_text(_status_lbl, err);
        return;
    }
    strncpy(_handle_buf, buf, sizeof(_handle_buf) - 1);
    _handle_buf[sizeof(_handle_buf) - 1] = '\0';

    _state = ST_NAME;
    select_name_field();
    clear_content();
    build_name_ui();
}

// Back: Zelle returns to the phone/email picker; the others go back to the scan step.
static void cb_handle_back() {
    clear_content();
    if (_is_zelle) {
        _state = ST_ZTYPE;
        build_ztype_ui();
    } else {
        _state = ST_SCAN;
        build_scan_ui();
    }
}

// Builds the ST_HANDLE entry UI.
static void build_handle_ui() {
    lv_obj_t *hint = lv_label_create(_content);
    char hint_buf[40];
    if (_is_zelle)                           snprintf(hint_buf, sizeof(hint_buf), _zelle_email ? "Zelle email:" : "Zelle phone number:");
    else if (strcmp(_method, "cashapp") == 0) snprintf(hint_buf, sizeof(hint_buf), "Cash App $cashtag (no $):");
    else                                      snprintf(hint_buf, sizeof(hint_buf), "%s handle (no @):", _label);
    lv_label_set_text(hint, hint_buf);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    for (int row = 0; row < _geom->rows; row++) build_wheel_row(_content, row);

    _status_lbl = lv_label_create(_content);
    lv_label_set_text(_status_lbl, "");
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_status_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(_status_lbl, LV_PCT(100));

    add_grow(_content);
    add_wheel_legend("Next", "Back");

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

// Enter: saves the owner-name field and proceeds to confirm.
static void cb_name_next() {
    get_field(_owner_buf, sizeof(_owner_buf));
    _state = ST_CONFIRM;
    clear_content();
    build_confirm_ui();
}

// Back returns to wherever the handle came from: the scan step, or the handle field with
// whatever was already typed there (_idx_handle is a separate persistent array, untouched
// by editing the name field).
static void cb_name_back() {
    clear_content();
    if (_scanned) {
        _state = ST_SCAN;
        build_scan_ui();
        return;
    }
    _state = ST_HANDLE;
    select_handle_field();
    build_handle_ui();
}

// Builds the ST_NAME (owner display name) UI.
static void build_name_ui() {
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Owner name (shown as \"Payment to: ...\"):");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    for (int row = 0; row < _geom->rows; row++) build_wheel_row(_content, row);

    add_grow(_content);
    add_wheel_legend("Next", "Back");

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
// this SQLite build (see project memory: feedback-no-upsert-syntax). `_method` picks which
// payment_methods row this writes to -- previously always 'venmo', parameterized 2026-09-14
// so the same function serves Venmo/Zelle/Cashapp (and any future method) alike.
static void save_payment_info(const char *handle, const char *owner) {
    sqlite3 *db = db_handle();
    if (!db) return;

    sqlite3_stmt *stmt;
    bool exists = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM payment_methods WHERE method=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, _method, -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }

    if (exists) {
        if (sqlite3_prepare_v2(db, "UPDATE payment_methods SET display_name=?, handle=?, enabled=1 WHERE method=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, handle, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, _method, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else if (sqlite3_prepare_v2(db, "INSERT INTO payment_methods (method, display_name, handle, enabled) VALUES (?, ?, ?, 1);", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, _method, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, owner, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, handle, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Deletes this method's row outright -- it drops out of Payment's "Other Payment" cycle,
// and no stale handle/name is left behind in the DB.
static void remove_payment_info() {
    sqlite3 *db = db_handle();
    if (!db) return;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "DELETE FROM payment_methods WHERE method=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, _method, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Enter: writes the handle+owner name and returns to the Payment Info submenu, which
// shows the new account under the method's name.
static void cb_confirm_save() {
    save_payment_info(_handle_buf, _owner_buf);
    screen_payment_menu_push();
}

// Back: returns to editing the owner name.
static void cb_confirm_back() {
    _state = ST_NAME;
    select_name_field();
    clear_content();
    build_name_ui();
}

// Adds the "Account: ... / Owner: ..." summary shared by ST_CONFIRM and ST_VIEW. A
// scanned Zelle link is shown as the phone/email inside it, formatted exactly as the
// bank did -- also the easiest way to spot a wrong code.
static void add_account_summary(const char *handle, const char *owner) {
    char account[80];
    payment_display_handle(_method, handle, account, sizeof(account));
    char summary[160];
    snprintf(summary, sizeof(summary), "%s\nOwner: %s", account, owner[0] ? owner : "(none)");
    lv_obj_t *lbl = lv_label_create(_content);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, summary);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

// Builds the ST_CONFIRM UI.
static void build_confirm_ui() {
    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_label_set_text(prompt, "Save this payment info?");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    add_account_summary(_handle_buf, _owner_buf);

    add_grow(_content);

    lv_obj_t *legend = ui_legend(_content);
    ui_legend_row(legend, "Save", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.enter = cb_confirm_save;
    h.back  = cb_confirm_back;
    buttons_set_handlers(h);
}

// ============ ST_VIEW (method already set up) ============

// Enter: replace the saved account -- starts over at the scan step.
static void cb_view_update() {
    _scanned = false;
    _state = ST_SCAN;
    clear_content();
    build_scan_ui();
}

static void cb_view_back() {
    screen_payment_menu_push();
}

// Delete confirm, Enter: removes the method and returns to the menu (now "Not set").
static void cb_view_delete_yes() {
    remove_payment_info();
    screen_payment_menu_push();
}

// Delete confirm, Back: cancels -- redraws the normal view.
static void cb_view_delete_no() {
    clear_content();
    build_view_ui();
}

static void build_view(bool confirm_delete);

// Left: swaps in the delete confirm.
static void cb_view_delete_ask() {
    clear_content();
    build_view(true);
}

// Builds the view (or, with confirm_delete, the in-place delete confirm -- same screen,
// footer swapped, per the lightweight-confirm pattern; a deleted method is one scan away
// from being back, so a separate confirm screen isn't warranted).
static void build_view(bool confirm_delete) {
    char status[48];
    snprintf(status, sizeof(status), confirm_delete ? "Delete %s?" : "%s is set up:",
             payment_method_label(_method));
    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_label_set_text(prompt, status);
    lv_obj_set_style_text_color(prompt, lv_color_hex(confirm_delete ? C_RED : C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    add_account_summary(_db_handle, _db_owner);

    add_grow(_content);

    lv_obj_t *legend = ui_legend(_content);
    ButtonHandlers h;
    if (confirm_delete) {
        ui_legend_row(legend, "Yes, delete", lv_color_hex(C_GREEN), "No", lv_color_hex(C_RED));
        h.enter = cb_view_delete_yes;
        h.back  = cb_view_delete_no;
    } else {
        char delete_lbl[24];
        snprintf(delete_lbl, sizeof(delete_lbl), "%s Delete", LV_SYMBOL_LEFT);
        ui_legend_row(legend, delete_lbl, lv_color_hex(C_YELLOW), "", lv_color_hex(C_TEXT));
        ui_legend_row(legend, "Update", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
        h.left  = cb_view_delete_ask;
        h.enter = cb_view_update;
        h.back  = cb_view_back;
    }
    buttons_set_handlers(h);
}

static void build_view_ui() {
    build_view(false);
}

// ============ public ============

// Loads the editor for the given payment method, prefilled from its DB row.
void screen_payment_edit_push(const char *method, const char *display_label) {
    strncpy(_method, method, sizeof(_method) - 1);
    _method[sizeof(_method) - 1] = '\0';
    strncpy(_label, display_label, sizeof(_label) - 1);
    _label[sizeof(_label) - 1] = '\0';
    _is_zelle = strcmp(_method, "zelle") == 0;

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

    // Prefill both fields from whatever's already in payment_methods (blank if the DB
    // isn't available yet, or this method has no row yet -- the wheels just start empty).
    _db_owner[0]  = '\0';
    _db_handle[0] = '\0';
    if (db_handle()) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_handle(), "SELECT display_name, handle FROM payment_methods WHERE method=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, _method, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *dn = sqlite3_column_text(stmt, 0);
                const unsigned char *hd = sqlite3_column_text(stmt, 1);
                if (dn) strncpy(_db_owner,  (const char *)dn, sizeof(_db_owner) - 1);
                if (hd) strncpy(_db_handle, (const char *)hd, sizeof(_db_handle) - 1);
            }
            sqlite3_finalize(stmt);
        }
    }
    _exists = _db_handle[0] != '\0';
    load_field(_idx_name, _db_owner, GEOM_TEXT, NAME_CHARSET, NAME_CHARSET_LEN, NAME_BLANK_IDX);
    if (!_is_zelle) {
        load_field(_idx_handle, _db_handle, GEOM_TEXT, HANDLE_CHARSET, HANDLE_CHARSET_LEN, HANDLE_BLANK_IDX);
    }
    _handle_buf[0] = '\0';
    _owner_buf[0]  = '\0';

    // Just the method name ("VENMO"), not "VENMO PAYMENT INFO" -- per explicit direction
    // 2026-09-14, matches how the Payment Info submenu rows are already named.
    char title[16];
    snprintf(title, sizeof(title), "%s", _label);
    for (char *p = title; *p; p++) *p = (char)toupper((unsigned char)*p);
    header_set_visible(true);
    header_set_title(title);

    if (_is_zelle) {
        // If they type it instead, start on whichever kind is already saved.
        _ztype_cursor = strchr(_db_handle, '@') ? 1 : 0;
    }
    _scanned = false;
    // One account per method: an existing one opens on its view (Update/Delete), a new
    // one goes straight to scanning the owner's own code.
    if (_exists) {
        _state = ST_VIEW;
        build_view_ui();
    } else {
        _state = ST_SCAN;
        build_scan_ui();
    }

    lv_scr_load(_scr);
}

// Consumes a scan on ST_SCAN -- parses the owner's own payment QR into the handle and
// moves on to the owner-name step. An unrecognized code shows what was actually read.
bool screen_payment_edit_on_scan(const char *code) {
    if (!_scr || lv_scr_act() != _scr || _state != ST_SCAN) return false;

    char handle[PAYMENT_HANDLE_MAX], name[48];
    if (!payment_parse_scan(_method, code, handle, sizeof(handle), name, sizeof(name))) {
        // Printable ASCII only -- LVGL's fonts can't draw anything else.
        char shown[41];
        size_t n = 0;
        for (const char *p = code; *p && n + 1 < sizeof(shown); p++) {
            if (*p >= 0x20 && *p < 0x7F) shown[n++] = *p;
        }
        shown[n] = '\0';
        char msg[96];
        snprintf(msg, sizeof(msg), "Not a %s code. Read:\n%s", payment_method_label(_method), shown);
        if (_status_lbl) lv_label_set_text(_status_lbl, msg);
        Serial.printf("[PAYMENT] unrecognized %s scan: %s\n", _method, code);
        return true;
    }

    strncpy(_handle_buf, handle, sizeof(_handle_buf) - 1);
    _handle_buf[sizeof(_handle_buf) - 1] = '\0';
    _scanned = true;
    // Zelle codes carry the recipient name -- prefill the owner-name wheel with it.
    if (name[0]) load_field(_idx_name, name, GEOM_TEXT, NAME_CHARSET, NAME_CHARSET_LEN, NAME_BLANK_IDX);

    _state = ST_NAME;
    select_name_field();
    clear_content();
    build_name_ui();
    return true;
}
