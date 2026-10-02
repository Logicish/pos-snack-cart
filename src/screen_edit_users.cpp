#include "screen_edit_users.h"
#include "screen_user_menu.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "users.h"
#include "checkouts.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>
#include <ctype.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Edit Users screen declared in screen_edit_users.h --
            three states: ST_LIST (alphabetical browse), ST_DETAIL (per-user rows),
            ST_NAME_EDIT (name wheel).
*/

// Deliberately a touch smaller than the visible row count so successive presses always
// overlap by a row and never skip one — same tuning rationale as ATTACH_PAGE in
// screen_add_item.cpp.
#define LIST_PAGE 7

// Name-edit wheel: same single-row-per-field widget as screen_enroll.cpp (11 chars per
// field, both first/last visible at once, inactive one dimmed) — reused as-is rather than
// the newer two-row-wrap style (screen_add_item.cpp/screen_payment_edit.cpp), since
// that's what these exact fields were originally typed with.
static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ -";
#define CHARSET_LEN 28
#define NAME_LEN    11
#define IDX_DASH    27

typedef enum { ST_LIST, ST_DETAIL, ST_NAME_EDIT } EditUsersState;

static lv_obj_t       *_scr;
static lv_obj_t       *_content;
static EditUsersState  _state;

// -- ST_LIST --
static lv_obj_t *_list_rows[MAX_USERS_LISTED];
static User       _list_users[MAX_USERS_LISTED];
static int         _list_count;
static int         _list_cursor;
static int         _list_prev_cursor = -1;

// -- ST_DETAIL --
#define DETAIL_ROW_COUNT 3
static int        _detail_user_id;
static lv_obj_t  *_detail_rows[DETAIL_ROW_COUNT];
static lv_obj_t  *_detail_row_lbls[DETAIL_ROW_COUNT];
static lv_obj_t  *_detail_balance_lbl;
static int         _detail_cursor;
static int         _detail_prev_cursor = -1;

// -- ST_NAME_EDIT --
static int8_t     _name_idx[2][NAME_LEN];  // [0]=first, [1]=last
static uint8_t     _name_cursor;
static uint8_t     _name_active_row;
static lv_obj_t   *_name_cell[2][NAME_LEN];
static lv_obj_t   *_name_cell_lbl[2][NAME_LEN];

static void build_list_ui();
static void build_detail_ui();
static void build_name_ui();

// Clears the content area between states.
static void clear_content() {
    lv_obj_clean(_content);
    memset(_list_rows,     0, sizeof(_list_rows));
    memset(_detail_rows,    0, sizeof(_detail_rows));
    memset(_detail_row_lbls,0, sizeof(_detail_row_lbls));
    memset(_name_cell,      0, sizeof(_name_cell));
    memset(_name_cell_lbl,  0, sizeof(_name_cell_lbl));
    _list_prev_cursor   = -1;
    _detail_prev_cursor = -1;
}

// ============ ST_LIST ============

// Short flag letters instead of spelled-out words ("Admin, Locked" -> "A L") — requested
// 2026-09-14 so the list scales to real names without wrapping/overflowing. "A"/"L" each
// occupy a fixed slot (space where the flag isn't set) so the pair reads consistently
// whether one, both, or neither is present, rather than the tag's own width jumping around.
static void format_status_tag(char *out, size_t out_len, const User &u) {
    snprintf(out, out_len, "%c %c", u.admin ? 'A' : ' ', u.active ? ' ' : 'L');
}

// Highlights the currently-selected list row.
static void refresh_list_cursor() {
    if (_list_count == 0) return;
    if (_list_prev_cursor >= 0 && _list_prev_cursor < _list_count && _list_prev_cursor != _list_cursor) {
        lv_obj_set_style_bg_opa(_list_rows[_list_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_list_rows[_list_cursor], LV_OPA_30, LV_PART_MAIN);
    _list_prev_cursor = _list_cursor;
    lv_obj_scroll_to_view(_list_rows[_list_cursor], LV_ANIM_OFF);
}

// Up: moves the selection up one row, wrapping.
static void cb_list_up() {
    if (_list_count == 0) return;
    _list_cursor = (_list_cursor - 1 + _list_count) % _list_count;
    refresh_list_cursor();
}

// Down: moves the selection down one row, wrapping.
static void cb_list_down() {
    if (_list_count == 0) return;
    _list_cursor = (_list_cursor + 1) % _list_count;
    refresh_list_cursor();
}

// LEFT/RIGHT jump ~a screenful at a time, both directions wrapping -- remapped
// 2026-09-15 from a Left-only forward accelerator to match Browse/Price, Item Lookup,
// Balances, Inventory, and the Admin Menu's paging convention.
static void cb_list_page_left() {
    if (_list_count == 0) return;
    if (_list_cursor <= 0) {
        _list_cursor = _list_count - 1;
    } else {
        _list_cursor -= LIST_PAGE;
        if (_list_cursor < 0) _list_cursor = 0;
    }
    refresh_list_cursor();
}

static void cb_list_page_right() {
    if (_list_count == 0) return;
    if (_list_cursor >= _list_count - 1) {
        _list_cursor = 0;
    } else {
        _list_cursor += LIST_PAGE;
        if (_list_cursor >= _list_count) _list_cursor = _list_count - 1;
    }
    refresh_list_cursor();
}

// Back returns to the Users submenu.
static void cb_list_back() {
    screen_user_menu_push();
}

// Enter opens the detail view for the highlighted user.
static void cb_list_select() {
    if (_list_count == 0) return;
    _detail_user_id = _list_users[_list_cursor].id;
    _detail_cursor  = 0;
    _state = ST_DETAIL;
    clear_content();
    build_detail_ui();
}

// Builds the ST_LIST UI, re-querying the current user list.
static void build_list_ui() {
    lv_obj_t *list = lv_obj_create(_content);
    lv_obj_set_size(list, LV_PCT(100), 1);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(list, 0, LV_PART_MAIN);  // 12px side inset, same as every screen
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    _list_count = users_get_all(_list_users, MAX_USERS_LISTED);
    if (_list_cursor >= _list_count) _list_cursor = _list_count > 0 ? _list_count - 1 : 0;

    if (_list_count == 0) {
        lv_obj_t *lbl = lv_label_create(list);
        lv_label_set_text(lbl, "No users enrolled.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    } else {
        for (int i = 0; i < _list_count; i++) {
            const User &u = _list_users[i];

            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_layout(row, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            _list_rows[i] = row;

            // Name takes whatever space is left after the flag slot's fixed natural width
            // (below) and ellipsizes instead of overflowing/wrapping — guarantees the row
            // always fits regardless of how long an imported name turns out to be, rather
            // than assuming a max length up front.
            char name_buf[NAME_FIELD_LEN * 2 + 2];
            snprintf(name_buf, sizeof(name_buf), "%s %s", u.first_name, u.last_name);
            lv_obj_t *name_lbl = lv_label_create(row);
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(name_lbl, 1);
            lv_label_set_text(name_lbl, name_buf);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(u.active ? C_TEXT : C_DIM), LV_PART_MAIN);
            lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

            char tag_buf[8];
            format_status_tag(tag_buf, sizeof(tag_buf), u);
            lv_obj_t *tag_lbl = lv_label_create(row);
            lv_label_set_text(tag_lbl, tag_buf);
            lv_obj_set_style_text_color(tag_lbl, lv_color_hex(u.active ? C_ORANGE : C_RED), LV_PART_MAIN);
            lv_obj_set_style_text_font(tag_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        }
    }

    lv_obj_t *legend = ui_legend(_content);
    char move_lbl[24], page_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "Move %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    snprintf(page_lbl, sizeof(page_lbl), "Page %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    ui_legend_row(legend, move_lbl, lv_color_hex(C_YELLOW), page_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    refresh_list_cursor();

    ButtonHandlers h;
    h.up    = cb_list_up;
    h.down  = cb_list_down;
    h.left  = cb_list_page_left;
    h.right = cb_list_page_right;
    h.enter = cb_list_select;
    h.back  = cb_list_back;
    buttons_set_handlers(h);
}

// ============ ST_DETAIL ============

// Redraws the three detail row labels from the current user record.
static void refresh_detail_row_labels() {
    const User *u = users_get_by_id(_detail_user_id);
    if (!u) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "Name: %s %s", u->first_name, u->last_name);
    lv_label_set_text(_detail_row_lbls[0], buf);

    snprintf(buf, sizeof(buf), "Admin: %s", u->admin ? "Yes" : "No");
    lv_label_set_text(_detail_row_lbls[1], buf);

    snprintf(buf, sizeof(buf), "Locked: %s", u->active ? "No" : "Yes");
    lv_label_set_text(_detail_row_lbls[2], buf);
}

// Highlights the currently-selected detail row.
static void refresh_detail_cursor() {
    if (_detail_prev_cursor >= 0 && _detail_prev_cursor != _detail_cursor) {
        lv_obj_set_style_bg_opa(_detail_rows[_detail_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_detail_rows[_detail_cursor], LV_OPA_30, LV_PART_MAIN);
    _detail_prev_cursor = _detail_cursor;
}

// Up: moves the selection up one row, wrapping.
static void cb_detail_up() {
    _detail_cursor = (_detail_cursor - 1 + DETAIL_ROW_COUNT) % DETAIL_ROW_COUNT;
    refresh_detail_cursor();
}

// Down: moves the selection down one row, wrapping.
static void cb_detail_down() {
    _detail_cursor = (_detail_cursor + 1) % DETAIL_ROW_COUNT;
    refresh_detail_cursor();
}

// Back returns to the user list.
static void cb_detail_back() {
    _state = ST_LIST;
    clear_content();
    build_list_ui();
}

// True if the detail view is showing the admin who is logged in right now.
static bool viewing_self() {
    return _detail_user_id == users_get_current_admin();
}

// Enter acts on the selected row (opens the name wheel, or toggles Admin/Locked in place).
// 2026-09-29 (owner's rule): the logged-in admin can't change their own account. They
// passed Admin Login, so they're an active admin, and since nobody else is editing while
// they're logged in, the cart can never be left without an active admin to log in with.
static void cb_detail_enter() {
    if (viewing_self()) return;
    const User *u = users_get_by_id(_detail_user_id);
    if (!u) return;

    switch (_detail_cursor) {
        case 0: {  // Name -- open the wheel, prefilled with the current names
            for (int col = 0; col < NAME_LEN; col++) {
                char c = (col < (int)strlen(u->first_name)) ? toupper((unsigned char)u->first_name[col]) : 0;
                const char *p = c ? strchr(CHARSET, c) : nullptr;
                _name_idx[0][col] = p ? (int8_t)(p - CHARSET) : IDX_DASH;

                c = (col < (int)strlen(u->last_name)) ? toupper((unsigned char)u->last_name[col]) : 0;
                p = c ? strchr(CHARSET, c) : nullptr;
                _name_idx[1][col] = p ? (int8_t)(p - CHARSET) : IDX_DASH;
            }
            _name_active_row = 0;
            _name_cursor = 0;
            _state = ST_NAME_EDIT;
            clear_content();
            build_name_ui();
            break;
        }
        case 1:  // Admin toggle
            users_set_admin(_detail_user_id, !u->admin);
            refresh_detail_row_labels();
            break;
        case 2:  // Locked toggle -- users.active is the underlying (non-inverted) column
            users_set_active(_detail_user_id, !u->active);
            refresh_detail_row_labels();
            break;
    }
}

// Builds the ST_DETAIL UI for the currently-selected user.
static void build_detail_ui() {
    const User *u = users_get_by_id(_detail_user_id);
    header_set_title(u ? (String(u->first_name) + " " + String(u->last_name)).c_str() : "EDIT USER");

    _detail_balance_lbl = lv_label_create(_content);
    lv_obj_set_width(_detail_balance_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_detail_balance_lbl, lv_color_hex(C_ORANGE), LV_PART_MAIN);
    lv_obj_set_style_text_font(_detail_balance_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    char balance_buf[32];
    int cents = checkouts_get_balance_cents(_detail_user_id);
    snprintf(balance_buf, sizeof(balance_buf), "Balance: $%d.%02d", cents / 100, cents % 100);
    lv_label_set_text(_detail_balance_lbl, balance_buf);

    bool self = viewing_self();
    if (self) {
        lv_obj_t *note = lv_label_create(_content);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_label_set_text(note, "This is you. Your own account can't be changed here.");
        lv_obj_set_width(note, LV_PCT(100));
        lv_obj_set_style_text_color(note, lv_color_hex(C_ORANGE), LV_PART_MAIN);
        lv_obj_set_style_text_font(note, &lv_font_montserrat_16, LV_PART_MAIN);
    }

    lv_obj_t *list = lv_obj_create(_content);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < DETAIL_ROW_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        _detail_rows[i] = row;

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(self ? C_DIM : C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
        _detail_row_lbls[i] = lbl;
    }
    refresh_detail_row_labels();

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(_content);
    char move_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    refresh_detail_cursor();

    ButtonHandlers h;
    h.up    = cb_detail_up;
    h.down  = cb_detail_down;
    h.enter = cb_detail_enter;
    h.back  = cb_detail_back;
    buttons_set_handlers(h);
}

// ============ ST_NAME_EDIT ============
// Same widget as screen_enroll.cpp's build_input_ui() -- both rows always visible, only
// the active one bright, Left/Right move within a row, Up/Down cycle the letter, Enter
// advances (first -> last -> save), Back steps back a field / cancels without saving.

// Reads one name-wheel row into a trimmed string.
static void get_name(uint8_t row, char *out) {
    int last_real = -1;
    for (int i = 0; i < NAME_LEN; i++) {
        char c = CHARSET[(uint8_t)_name_idx[row][i]];
        if (c != '-' && c != ' ') last_real = i;
    }
    for (int i = 0; i <= last_real; i++) out[i] = CHARSET[(uint8_t)_name_idx[row][i]];
    out[last_real + 1] = '\0';
}

// True if the given name-wheel row has at least one real character typed.
static bool name_row_valid(uint8_t row) {
    char buf[NAME_LEN + 1];
    get_name(row, buf);
    return buf[0] != '\0';
}

// Redraws one name-wheel cell -- bright + bordered if it's the cursor, dim if it's the
// inactive row, normal otherwise.
static void refresh_name_cell(uint8_t row, uint8_t col) {
    if (!_name_cell_lbl[row][col]) return;

    char buf[2] = { CHARSET[(uint8_t)_name_idx[row][col]], '\0' };
    lv_label_set_text(_name_cell_lbl[row][col], buf);

    bool is_cursor = ((int)_name_active_row == row && (int)_name_cursor == col);
    bool is_active  = ((int)_name_active_row == row);

    lv_color_t color = is_cursor ? lv_color_hex(C_CYAN)
                     : is_active  ? lv_color_hex(C_TEXT)
                                  : lv_color_hex(C_DIM);
    lv_obj_set_style_text_color(_name_cell_lbl[row][col], color, LV_PART_MAIN);

    lv_border_side_t side = is_cursor ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_NONE;
    lv_obj_set_style_border_side(_name_cell[row][col], side, LV_PART_MAIN);
    lv_obj_set_style_border_width(_name_cell[row][col], is_cursor ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(_name_cell[row][col], lv_color_hex(C_CYAN), LV_PART_MAIN);
}

// Redraws every name-wheel cell.
static void refresh_name_cells() {
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < NAME_LEN; col++)
            refresh_name_cell(row, col);
}

// Left: moves the cursor left within the active row.
static void cb_name_left() {
    if (_name_cursor > 0) {
        uint8_t old = _name_cursor;
        _name_cursor--;
        refresh_name_cell(_name_active_row, old);
        refresh_name_cell(_name_active_row, _name_cursor);
    }
}

// Right: moves the cursor right within the active row.
static void cb_name_right() {
    if (_name_cursor < NAME_LEN - 1) {
        uint8_t old = _name_cursor;
        _name_cursor++;
        refresh_name_cell(_name_active_row, old);
        refresh_name_cell(_name_active_row, _name_cursor);
    }
}

// Up: cycles the letter under the cursor forward.
static void cb_name_up() {
    _name_idx[_name_active_row][_name_cursor] = (_name_idx[_name_active_row][_name_cursor] + 1) % CHARSET_LEN;
    refresh_name_cell(_name_active_row, _name_cursor);
}

// Down: cycles the letter under the cursor backward.
static void cb_name_down() {
    _name_idx[_name_active_row][_name_cursor] = (_name_idx[_name_active_row][_name_cursor] + CHARSET_LEN - 1) % CHARSET_LEN;
    refresh_name_cell(_name_active_row, _name_cursor);
}

// Enter: advances first name -> last name -> save.
static void cb_name_next() {
    if (_name_active_row == 0) {
        if (!name_row_valid(0)) return;
        _name_active_row = 1;
        _name_cursor = 0;
        refresh_name_cells();
    } else {
        if (!name_row_valid(1)) return;
        char first[NAME_LEN + 1], last[NAME_LEN + 1];
        get_name(0, first);
        get_name(1, last);
        users_set_name(_detail_user_id, first, last);
        _state = ST_DETAIL;
        clear_content();
        build_detail_ui();
    }
}

// Back steps to the previous field, or -- from the first field -- cancels without saving
// and returns to the detail view unchanged.
static void cb_name_back() {
    if (_name_active_row == 1) {
        _name_active_row = 0;
        _name_cursor = 0;
        refresh_name_cells();
    } else {
        _state = ST_DETAIL;
        clear_content();
        build_detail_ui();
    }
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
        _name_cell[row][col] = cell;

        lv_obj_t *lbl = lv_label_create(cell);
        char buf[2] = { CHARSET[(uint8_t)_name_idx[row][col]], '\0' };
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_center(lbl);
        _name_cell_lbl[row][col] = lbl;
    }
}

// Builds the ST_NAME_EDIT UI.
static void build_name_ui() {
    lv_obj_t *lbl1 = lv_label_create(_content);
    lv_label_set_text(lbl1, "First Name:");
    lv_obj_set_style_text_color(lbl1, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl1, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(lbl1, LV_PCT(100));
    build_char_row(_content, 0);

    lv_obj_t *spacer = lv_obj_create(_content);
    lv_obj_set_size(spacer, LV_PCT(100), 12);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(spacer, 0, LV_PART_MAIN);

    lv_obj_t *lbl2 = lv_label_create(_content);
    lv_label_set_text(lbl2, "Last Name:");
    lv_obj_set_style_text_color(lbl2, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(lbl2, LV_PCT(100));
    build_char_row(_content, 1);

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

    refresh_name_cells();

    ButtonHandlers h;
    h.up    = cb_name_up;
    h.down  = cb_name_down;
    h.left  = cb_name_left;
    h.right = cb_name_right;
    h.enter = cb_name_next;
    h.back  = cb_name_back;
    buttons_set_handlers(h);
}

// ============ public ============

// Loads the Edit Users screen, always starting at ST_LIST.
void screen_edit_users_push() {
    _state = ST_LIST;
    _list_cursor = 0;

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

    header_set_visible(true);
    header_set_title("EDIT USERS");
    build_list_ui();

    lv_scr_load(_scr);
}
