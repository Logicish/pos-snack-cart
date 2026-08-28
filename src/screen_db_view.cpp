#include "screen_db_view.h"
#include "screen_db_menu.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "db.h"
#include "ui.h"
#include <sqlite3.h>
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>

#define FOOTER_H 52

// UI display cap for this screen's row pool, not a DB storage limit — same convention as
// items.h's MAX_ITEMS.
#define MAX_DB_ROWS 200

// Deliberately a touch smaller than a screenful so successive presses always overlap and
// never skip a row -- rough tuning here since card height varies per table (column
// count differs), unlike the fixed-height rows elsewhere this convention is copied from
// (see ATTACH_PAGE in screen_add_item.cpp).
#define VIEW_PAGE 4

struct TableInfo { const char *title; const char *sql; };

// Order matches screen_db_view.h's DbTable enum / screen_db_menu.cpp's menu order.
// SELECT * -- column names/values are rendered generically (see build_row_text()) rather
// than hand-written per table, so adding a table here is the only step needed.
static const TableInfo TABLES[DB_TABLE_COUNT] = {
    { "DB: USERS",           "SELECT * FROM users ORDER BY id;" },
    { "DB: ITEMS",           "SELECT * FROM items ORDER BY id;" },
    { "DB: CHECKOUTS",       "SELECT * FROM checkouts ORDER BY id DESC;" },  // newest first
    { "DB: PAYMENT METHODS", "SELECT * FROM payment_methods ORDER BY method;" },
    { "DB: CONFIG",          "SELECT * FROM config ORDER BY key;" },
};

static lv_obj_t *_scr;
static lv_obj_t *_list;
static lv_obj_t *_rows[MAX_DB_ROWS];  // scroll-into-view anchors only -- read-only, no highlight
static int        _row_count;
static int        _cursor;
static DbTable     _table;

// One row's columns rendered generically as "name: value" lines via SQLite's own column
// metadata (sqlite3_column_name/text) -- works for any table without per-table display
// code. Diagnostic only, "for me mainly": raw values (e.g. users.password_hash) show
// as-is, no special-casing.
static void build_row_text(sqlite3_stmt *stmt, char *out, size_t out_len) {
    int n = sqlite3_column_count(stmt);
    int pos = 0;
    for (int i = 0; i < n && pos < (int)out_len - 1; i++) {
        const char *name = sqlite3_column_name(stmt, i);
        const unsigned char *val = sqlite3_column_text(stmt, i);
        int written = snprintf(out + pos, out_len - pos, "%s%s: %s",
                                i == 0 ? "" : "\n", name, val ? (const char *)val : "NULL");
        if (written < 0) break;
        pos += written;
    }
    out[out_len - 1] = '\0';
}

static void rebuild_rows() {
    lv_obj_clean(_list);
    memset(_rows, 0, sizeof(_rows));
    _row_count = 0;
    _cursor = 0;

    sqlite3 *db = db_handle();
    if (!db) {
        lv_obj_t *lbl = lv_label_create(_list);
        lv_label_set_text(lbl, "DB not available.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        return;
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, TABLES[_table].sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (_row_count < MAX_DB_ROWS && sqlite3_step(stmt) == SQLITE_ROW) {
            char buf[256];
            build_row_text(stmt, buf, sizeof(buf));

            lv_obj_t *card = lv_obj_create(_list);
            lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
            lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
            lv_obj_set_style_bg_color(card, lv_color_hex(C_SURFACE), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(card);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl, LV_PCT(100));
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);

            _rows[_row_count++] = card;
        }
        sqlite3_finalize(stmt);
    }

    if (_row_count == 0) {
        lv_obj_t *lbl = lv_label_create(_list);
        lv_label_set_text(lbl, "No rows.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    } else if (_row_count == MAX_DB_ROWS) {
        lv_obj_t *lbl = lv_label_create(_list);
        char buf[32];
        snprintf(buf, sizeof(buf), "... showing first %d", MAX_DB_ROWS);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    }
}

// UP/DOWN page the viewport directly -- same idiom as screen_browse.cpp: nothing to
// select here, just data to read, so no highlight cursor. Both wrap.
static void cb_page_up() {
    if (_row_count == 0) return;
    if (_cursor <= 0) {
        _cursor = _row_count - 1;
    } else {
        _cursor -= VIEW_PAGE;
        if (_cursor < 0) _cursor = 0;
    }
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

static void cb_page_down() {
    if (_row_count == 0) return;
    if (_cursor >= _row_count - 1) {
        _cursor = 0;
    } else {
        _cursor += VIEW_PAGE;
        if (_cursor >= _row_count) _cursor = _row_count - 1;
    }
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

static void cb_back() {
    screen_db_menu_push();
}

void screen_db_view_push(DbTable table) {
    _table = table;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _list = lv_obj_create(_scr);
        lv_obj_set_size(_list, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(_list, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H);
        lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_list, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_list, 6, LV_PART_MAIN);
        lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char page_up_lbl[24], page_down_lbl[24];
        snprintf(page_up_lbl,   sizeof(page_up_lbl),   "%s Page Up",   LV_SYMBOL_UP);
        snprintf(page_down_lbl, sizeof(page_down_lbl), "%s Page Down", LV_SYMBOL_DOWN);
        ui_legend_row(legend, page_up_lbl, lv_color_hex(C_YELLOW), page_down_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title(TABLES[table].title);
    rebuild_rows();

    ButtonHandlers h;
    h.up   = cb_page_up;
    h.down = cb_page_down;
    h.back = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
