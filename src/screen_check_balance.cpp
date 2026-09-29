#include "screen_check_balance.h"
#include "screens.h"
#include "screen_extras.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include "checkouts.h"
#include "db.h"
#include "payment_link.h"
#include <lvgl.h>
#include <Arduino.h>
#include <sqlite3.h>
#include <time.h>
#include <ctype.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Check Balance screen declared in screen_check_balance.h --
            two states, ST_LIST (the current Extras user's own outstanding checkouts)
            and ST_QR (re-viewing one transaction's payment QR). Read-only throughout --
            no Clear here, that stays admin-only (see screen_balances.cpp).
*/

#define FOOTER_H     52
#define BALANCE_PAGE  7

typedef enum { ST_LIST, ST_QR } BalanceState;
static BalanceState _state;

static lv_obj_t *_scr;
static lv_obj_t *_content;
static lv_obj_t *_rows[MAX_BALANCES];
static OutstandingCheckout _data[MAX_BALANCES];
static int        _count;
static int        _cursor;
static int        _prev_cursor;

// ST_QR -- which enabled payment_methods row is currently displayed. Reset to 0
// (-> venmo-first) each time ST_QR is freshly entered from the list; Right cycles it
// without resetting, wrapping via the row count -- same convention as screen_pos.cpp's
// Payment screen's _payment_method_index.
static int        _qr_method_index;

static void build_list_ui();
static void build_qr_ui();

// Returns how many payment_methods rows are enabled -- same as screen_pos.cpp's
// count_enabled_payment_methods(), duplicated per this codebase's established per-file
// self-contained convention rather than exposing screen_pos.cpp's internals.
static int count_enabled_payment_methods() {
    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM payment_methods WHERE enabled = 1;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

// Reads the index'th enabled payment method (venmo always sorted first) -- same query
// and priority order as screen_pos.cpp's get_payment_method_at(). display_name is needed
// since 2026-09-29: the Zelle QR payload includes the owner's name.
static bool get_payment_method_at(int index, char *method, size_t method_len,
                                   char *owner, size_t owner_len,
                                   char *handle, size_t handle_len) {
    method[0] = '\0';
    owner[0]  = '\0';
    handle[0] = '\0';

    sqlite3_stmt *stmt;
    bool have_handle = false;
    const char *sql =
        "SELECT method, handle, display_name FROM payment_methods WHERE enabled = 1 "
        "ORDER BY CASE WHEN method = 'venmo' THEN 0 ELSE 1 END, method LIMIT 1 OFFSET ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, index);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *mt = sqlite3_column_text(stmt, 0);
            const unsigned char *hd = sqlite3_column_text(stmt, 1);
            const unsigned char *dn = sqlite3_column_text(stmt, 2);
            if (mt) { strncpy(method, (const char *)mt, method_len - 1); method[method_len - 1] = '\0'; }
            if (dn) { strncpy(owner, (const char *)dn, owner_len - 1); owner[owner_len - 1] = '\0'; }
            if (hd && hd[0] != '\0') {
                strncpy(handle, (const char *)hd, handle_len - 1);
                handle[handle_len - 1] = '\0';
                have_handle = true;
            }
        }
        sqlite3_finalize(stmt);
    }
    return have_handle;
}

// Highlights the currently-selected row.
static void refresh_cursor() {
    if (_count == 0) return;
    if (_prev_cursor >= 0 && _prev_cursor < _count && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

// Up/Down step one row, Left/Right jump ~a screenful -- same paging convention as
// Browse/Price, Item Lookup, Balances, Inventory, Admin Menu, Advanced Tools, and Edit
// Users (see snack_cart_pos.md's 2026-09-15 paging-consistency sweep).
static void cb_up() {
    if (_count == 0) return;
    _cursor = (_cursor - 1 + _count) % _count;
    refresh_cursor();
}

static void cb_down() {
    if (_count == 0) return;
    _cursor = (_cursor + 1) % _count;
    refresh_cursor();
}

static void cb_page_left() {
    if (_count == 0) return;
    if (_cursor <= 0) {
        _cursor = _count - 1;
    } else {
        _cursor -= BALANCE_PAGE;
        if (_cursor < 0) _cursor = 0;
    }
    refresh_cursor();
}

static void cb_page_right() {
    if (_count == 0) return;
    if (_cursor >= _count - 1) {
        _cursor = 0;
    } else {
        _cursor += BALANCE_PAGE;
        if (_cursor >= _count) _cursor = _count - 1;
    }
    refresh_cursor();
}

// Enter: views the highlighted transaction's QR.
static void cb_view() {
    if (_count == 0) return;
    _state = ST_QR;
    _qr_method_index = 0;
    lv_obj_clean(_content);
    build_qr_ui();
}

// Right, while viewing a QR: cycles to the next enabled payment method.
static void cb_qr_other() {
    _qr_method_index++;  // build_qr_ui() wraps this against the real row count
    lv_obj_clean(_content);
    build_qr_ui();
}

// Back from ST_LIST: returns to the Extras menu (not IDLE -- Check Balance is reached
// from there, not directly from the Start screen).
static void cb_list_back() {
    screen_extras_return_to_list();
}

// Back from ST_QR: returns to the balance list.
static void cb_qr_back() {
    _state = ST_LIST;
    lv_obj_clean(_content);
    build_list_ui();
}

// Builds the ST_LIST UI, re-querying the current Extras user's outstanding checkouts.
static void build_list_ui() {
    header_set_title("CHECK BALANCE");  // build_qr_ui() overwrites this; restore it when
                                        // coming back from there, not just on first entry

    lv_obj_t *note = lv_label_create(_content);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, "Admin hasn't verified these yet.\nDoesn't mean they weren't paid.");
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_color(note, lv_color_hex(C_ORANGE), LV_PART_MAIN);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *list = lv_obj_create(_content);
    lv_obj_set_size(list, LV_PCT(100), 1);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    _count = checkouts_get_outstanding_for_user(screen_extras_current_user_id(), _data, MAX_BALANCES);
    _prev_cursor = -1;

    if (_count == 0) {
        lv_obj_t *hint = lv_label_create(list);
        lv_label_set_text(hint, "No outstanding balance.");
        lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    } else {
        for (int i = 0; i < _count; i++) {
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
            _rows[i] = row;

            char date_buf[8] = "--";
            if (_data[i].created_at > 0) {
                time_t t = (time_t)_data[i].created_at;
                struct tm tmval;
                gmtime_r(&t, &tmval);
                strftime(date_buf, sizeof(date_buf), "%m/%d", &tmval);
            }
            char left_buf[24];
            snprintf(left_buf, sizeof(left_buf), "#%d   %s", _data[i].id, date_buf);
            lv_obj_t *left_lbl = lv_label_create(row);
            lv_label_set_text(left_lbl, left_buf);
            lv_obj_set_style_text_color(left_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(left_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

            char price_buf[12];
            snprintf(price_buf, sizeof(price_buf), "$%d.%02d",
                      _data[i].total_price_cents / 100, _data[i].total_price_cents % 100);
            lv_obj_t *price_lbl = lv_label_create(row);
            lv_label_set_text(price_lbl, price_buf);
            lv_obj_set_style_text_color(price_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
            lv_obj_set_style_text_font(price_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        }
        if (_cursor >= _count) _cursor = _count - 1;
        refresh_cursor();
    }

    lv_obj_t *legend = ui_legend(_content);
    char move_lbl[24], page_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "Move %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    snprintf(page_lbl, sizeof(page_lbl), "Page %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    ui_legend_row(legend, move_lbl, lv_color_hex(C_YELLOW), page_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "View", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_left;
    h.right = cb_page_right;
    h.enter = cb_view;
    h.back  = cb_list_back;
    buttons_set_handlers(h);
}

// Builds the ST_QR UI for _data[_cursor] -- same payment_link.cpp QR the Payment screen
// shows, just built from a real stored checkout id instead of a predicted one.
static void build_qr_ui() {
    const OutstandingCheckout &c = _data[_cursor];

    char title[24];
    snprintf(title, sizeof(title), "BALANCE #%d", c.id);
    header_set_title(title);

    int method_count = count_enabled_payment_methods();
    if (method_count > 0 && _qr_method_index >= method_count) _qr_method_index = 0;

    char method[16] = "";
    char owner[32]  = "";
    char handle[PAYMENT_HANDLE_MAX] = "";
    bool have_method = method_count > 0 &&
        get_payment_method_at(_qr_method_index, method, sizeof(method), owner, sizeof(owner),
                              handle, sizeof(handle));

    // Same tabs + "Payment to" as the Payment screen -- Right cycles methods here too.
    if (have_method) {
        payment_add_tabs(_content, _qr_method_index);

        char to_buf[64];
        snprintf(to_buf, sizeof(to_buf), "Payment to: %s", owner[0] ? owner : "Owner");
        lv_obj_t *to_lbl = lv_label_create(_content);
        lv_label_set_text(to_lbl, to_buf);
        lv_obj_set_style_text_color(to_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(to_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_width(to_lbl, LV_PCT(100));
        lv_obj_set_style_text_align(to_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    if (!have_method) {
        lv_obj_t *warn = lv_label_create(_content);
        lv_label_set_text(warn, "Payment info not set yet\n(Admin > Settings > Payment Info)");
        lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(warn, lv_color_hex(C_ORANGE), LV_PART_MAIN);
        lv_obj_set_style_text_font(warn, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_width(warn, LV_PCT(100));
        lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else if (!payment_add_qr(_content, method, handle, owner, c.total_price_cents, c.id)) {
        // A method with no QR format (or a payload too long to fit) -- show it as text.
        char fallback_buf[80];
        snprintf(fallback_buf, sizeof(fallback_buf), "%s: %s", payment_method_label(method), handle);
        lv_obj_t *fallback = lv_label_create(_content);
        lv_label_set_text(fallback, fallback_buf);
        lv_label_set_long_mode(fallback, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(fallback, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(fallback, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_width(fallback, LV_PCT(100));
        lv_obj_set_style_text_align(fallback, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    char total_buf[24];
    snprintf(total_buf, sizeof(total_buf), "Total:  $%d.%02d", c.total_price_cents / 100, c.total_price_cents % 100);
    lv_obj_t *total_lbl = lv_label_create(_content);
    lv_label_set_text(total_lbl, total_buf);
    lv_obj_set_style_text_color(total_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
    lv_obj_set_style_text_font(total_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(total_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(total_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    // ui_footer_cancel() doesn't work inside _content -- it aligns itself absolutely,
    // but _content is a flex column, so the layout engine overrides that and the label
    // just falls wherever flex flow puts it (landed bottom-LEFT in practice, not
    // bottom-right like every other Back/Cancel footer in this codebase). Use a real
    // ui_legend() row instead, same as every other _content-based screen's footer.
    lv_obj_t *legend = ui_legend(_content);
    char right_arrow[24];
    snprintf(right_arrow, sizeof(right_arrow), "Other Payment %s", LV_SYMBOL_RIGHT);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), right_arrow, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Back", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.right = cb_qr_other;
    h.back  = cb_qr_back;
    buttons_set_handlers(h);
}

// Loads the Check Balance screen for whichever user Extras' badge gate identified.
void screen_check_balance_push() {
    _cursor = 0;
    _state  = ST_LIST;

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
        lv_obj_set_style_pad_bottom(_content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_content, 8, LV_PART_MAIN);
        lv_obj_set_layout(_content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(_content, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clean(_content);
    }

    header_set_visible(true);
    build_list_ui();  // sets the header title itself

    lv_scr_load(_scr);
}
