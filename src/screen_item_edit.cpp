#include "screen_item_edit.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "items.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

// Real prices in the catalog are all nickel multiples ($0.05 steps), so that's the
// adjustment granularity here too.
#define PRICE_STEP_CENTS 5

enum Row { ROW_PRICE, ROW_QTY1, ROW_QTY10, ROW_COUNT };

static lv_obj_t *_scr;
static lv_obj_t *_name_lbl;
static lv_obj_t *_price_row, *_price_val_lbl;
static lv_obj_t *_qty1_row;
static lv_obj_t *_qty10_row;
static lv_obj_t *_upc_header_lbl;
static lv_obj_t *_upc_list;
static lv_obj_t *_msg_lbl;

static void (*_return_cb)();
static int  _item_id;
static int  _price_cents;
static int  _stocked;
static int  _row_cursor;

static void format_price(int cents, char *out, size_t out_len) {
    bool neg = cents < 0;
    int c = neg ? -cents : cents;
    snprintf(out, out_len, "%s$%d.%02d", neg ? "-" : "", c / 100, c % 100);
}

static void set_msg(const char *text) {
    lv_label_set_text(_msg_lbl, text ? text : "");
}

static void refresh_header() {
    const Item *it = items_get_by_id(_item_id);
    char buf[64];
    if (it) {
        snprintf(buf, sizeof(buf), "%s   x%d", it->name, _stocked);
    } else {
        snprintf(buf, sizeof(buf), "(item not found)");
    }
    lv_label_set_text(_name_lbl, buf);

    char price_buf[16];
    format_price(_price_cents, price_buf, sizeof(price_buf));
    lv_label_set_text(_price_val_lbl, price_buf);
}

static void refresh_upcs() {
    lv_obj_clean(_upc_list);

    char upcs[MAX_UPCS_PER_ITEM][UPC_LEN];
    int n = items_get_upcs(_item_id, upcs, MAX_UPCS_PER_ITEM);

    if (n == 0) {
        lv_obj_t *lbl = lv_label_create(_upc_list);
        lv_label_set_text(lbl, "(none yet -- scan to link one)");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *lbl = lv_label_create(_upc_list);
        lv_label_set_text(lbl, upcs[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    }
}

static void refresh_row_highlight() {
    lv_obj_t *rows[ROW_COUNT] = { _price_row, _qty1_row, _qty10_row };
    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_set_style_bg_opa(rows[i], i == _row_cursor ? LV_OPA_30 : LV_OPA_TRANSP, LV_PART_MAIN);
    }
}

static void save_and_refresh() {
    items_update(_item_id, _price_cents, _stocked);
    refresh_header();
}

// Up/Down move the row cursor, Left/Right adjust the selected row's value -- swapped
// 2026-08-25 from an initial Up/Down-adjusts/Left-Right-navigates layout, to match this
// screen's own navigation convention with the rest of the codebase (Up/Down = move,
// Left/Right = act). Note Left/Right don't auto-repeat on hold the way Up/Down do (see
// buttons.cpp) -- cranking a value through many steps now takes individual presses.
static void cb_row_prev() {
    _row_cursor = (_row_cursor - 1 + ROW_COUNT) % ROW_COUNT;
    refresh_row_highlight();
}

static void cb_row_next() {
    _row_cursor = (_row_cursor + 1) % ROW_COUNT;
    refresh_row_highlight();
}

static void cb_adjust_down() {
    set_msg(nullptr);
    switch (_row_cursor) {
        // Price shouldn't go negative -- a real dollar amount below zero makes no sense.
        // Stock deliberately CAN go negative (see items.h) -- that's a different, intentional
        // signal, not a bug, so it's not clamped here.
        case ROW_PRICE: if (_price_cents >= PRICE_STEP_CENTS) _price_cents -= PRICE_STEP_CENTS; break;
        case ROW_QTY1:  _stocked -= 1;  break;
        case ROW_QTY10: _stocked -= 10; break;
    }
    save_and_refresh();
}

static void cb_adjust_up() {
    set_msg(nullptr);
    switch (_row_cursor) {
        case ROW_PRICE: _price_cents += PRICE_STEP_CENTS; break;
        case ROW_QTY1:  _stocked += 1;  break;
        case ROW_QTY10: _stocked += 10; break;
    }
    save_and_refresh();
}

static void cb_back() {
    set_msg(nullptr);
    if (_return_cb) _return_cb();
}

// value_lbl_out is optional (nullptr for the two quantity rows, which don't need their own
// value display since the live count is already shown in the header) -- when given, the
// value label is created between the row label and the hint, matching the mockup's layout.
static lv_obj_t *make_row(lv_obj_t *parent, const char *label, const char *hint, lv_obj_t **value_lbl_out) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    if (value_lbl_out) {
        lv_obj_t *val_lbl = lv_label_create(row);
        lv_obj_set_style_text_color(val_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        *value_lbl_out = val_lbl;
    }

    lv_obj_t *hint_lbl = lv_label_create(row);
    lv_label_set_text(hint_lbl, hint);
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

    return row;
}

bool screen_item_edit_on_scan(const char *upc) {
    if (!_scr || lv_scr_act() != _scr) return false;

    ItemUpcLinkResult r = item_upcs_link(_item_id, upc);
    switch (r) {
        case ITEM_UPC_LINKED:
            set_msg("Linked.");
            refresh_upcs();
            break;
        case ITEM_UPC_ALREADY_HERE:
            set_msg("Already linked to this item.");
            break;
        case ITEM_UPC_COLLISION: {
            const Item *other = items_find_by_upc(upc);
            char buf[64];
            snprintf(buf, sizeof(buf), "Already linked to: %s", other ? other->name : "another item");
            set_msg(buf);
            break;
        }
        case ITEM_UPC_ERROR:
            set_msg("Scan failed -- try again.");
            break;
    }
    return true;
}

void screen_item_edit_push(int item_id, void (*on_back)()) {
    _item_id    = item_id;
    _return_cb  = on_back;
    _row_cursor = ROW_PRICE;

    const Item *it = items_get_by_id(item_id);
    _price_cents = it ? it->price_cents : 0;
    _stocked     = it ? it->stocked     : 0;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = lv_obj_create(_scr);
        lv_obj_set_size(content, SCREEN_W, SCREEN_H - HDR_H);
        lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(content, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

        _name_lbl = lv_label_create(content);
        lv_label_set_long_mode(_name_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_name_lbl, LV_PCT(100));
        lv_obj_set_style_text_color(_name_lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_text_font(_name_lbl, &lv_font_montserrat_20, LV_PART_MAIN);

        _price_row = make_row(content, "Price", "+- $0.05", &_price_val_lbl);
        _qty1_row  = make_row(content, "Quantity", "+- 1", nullptr);
        _qty10_row = make_row(content, "Quantity", "+- 10", nullptr);

        _upc_header_lbl = lv_label_create(content);
        lv_label_set_text(_upc_header_lbl, "UPC codes   (scan to link)");
        lv_obj_set_style_text_color(_upc_header_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_upc_header_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        _upc_list = lv_obj_create(content);
        lv_obj_set_size(_upc_list, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(_upc_list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_upc_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_upc_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_upc_list, 2, LV_PART_MAIN);
        lv_obj_set_layout(_upc_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_upc_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(_upc_list, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *grow = lv_obj_create(content);
        lv_obj_set_size(grow, LV_PCT(100), 1);
        lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
        lv_obj_set_flex_grow(grow, 1);

        _msg_lbl = lv_label_create(content);
        lv_label_set_long_mode(_msg_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_msg_lbl, LV_PCT(100));
        lv_obj_set_style_text_color(_msg_lbl, lv_color_hex(C_ORANGE), LV_PART_MAIN);
        lv_obj_set_style_text_font(_msg_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

        lv_obj_t *legend = ui_legend(content);
        char row_lbl[24], adj_lbl[24];
        snprintf(row_lbl, sizeof(row_lbl), "%s%s Row", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        snprintf(adj_lbl, sizeof(adj_lbl), "Adjust %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
        ui_legend_row(legend, row_lbl, lv_color_hex(C_YELLOW), adj_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    set_msg(nullptr);
    refresh_header();
    refresh_upcs();
    refresh_row_highlight();

    header_set_visible(true);
    header_set_title("ITEM");

    ButtonHandlers h;
    h.up    = cb_row_prev;
    h.down  = cb_row_next;
    h.left  = cb_adjust_down;
    h.right = cb_adjust_up;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
