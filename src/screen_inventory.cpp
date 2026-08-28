#include "screen_inventory.h"
#include "screen_item_edit.h"
#include "screen_inventory_menu.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "items.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

#define FOOTER_H 52

// Deliberately a touch smaller than the visible row count (~8.5 rows fit in the 392px list
// area at this row height) so successive presses always overlap by a row and never skip
// one — same tuning rationale as ATTACH_PAGE in screen_add_item.cpp.
#define INVENTORY_PAGE 7

static lv_obj_t *_scr;
static lv_obj_t *_list;
static lv_obj_t *_rows[MAX_ITEMS];
static lv_obj_t *_qty_lbls[MAX_ITEMS];
static int        _cursor;
static int        _prev_cursor = -1;

// Same MAX_ITEMS-vs-items_count() cap as screen_browse.cpp/screen_pos.cpp's list screens --
// see items.h's 2026-08-25 comment for why this matters (a real crash, not caution for its
// own sake).
static int visible_count() {
    int n = items_count();
    return n > MAX_ITEMS ? MAX_ITEMS : n;
}

static void refresh_cursor() {
    int n = visible_count();
    if (n == 0) return;

    if (_prev_cursor >= 0 && _prev_cursor < n && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;

    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

static void cb_up() {
    int n = visible_count();
    if (n == 0) return;
    _cursor = (_cursor - 1 + n) % n;
    refresh_cursor();
}

static void cb_down() {
    int n = visible_count();
    if (n == 0) return;
    _cursor = (_cursor + 1) % n;
    refresh_cursor();
}

// LEFT: jump ~a screenful down the list, same wrap-at-the-end behavior as
// screen_add_item.cpp's ATTACH_PAGE/cb_attach_pagedown.
static void cb_page_down() {
    int n = visible_count();
    if (n == 0) return;
    if (_cursor >= n - 1) {
        _cursor = 0;
    } else {
        _cursor += INVENTORY_PAGE;
        if (_cursor >= n) _cursor = n - 1;
    }
    refresh_cursor();
}

static void cb_back() {
    screen_inventory_menu_push();  // 2026-08-28 reorg -- was screen_menu_push()
}

static void cb_select() {
    const Item *it = items_get(_cursor);
    if (it) screen_item_edit_push(it->id, screen_inventory_push);
}

void screen_inventory_push() {
    bool first_build = (_scr == nullptr);

    if (first_build) {
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

        int n = items_count();
        for (int i = 0; i < n && i < MAX_ITEMS; i++) {
            const Item *it = items_get(i);

            lv_obj_t *row = lv_obj_create(_list);
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

            lv_obj_t *name_lbl = lv_label_create(row);
            lv_label_set_text(name_lbl, it->name);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

            lv_obj_t *qty_lbl = lv_label_create(row);
            lv_obj_set_style_text_color(qty_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
            lv_obj_set_style_text_font(qty_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
            _qty_lbls[i] = qty_lbl;
        }

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char left_arrow[24], move_lbl[24];
        snprintf(left_arrow, sizeof(left_arrow), "%s Next Screen", LV_SYMBOL_LEFT);
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, left_arrow, lv_color_hex(C_YELLOW), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }
    // Deliberately NOT resetting _cursor here (unlike screen_browse.cpp) -- walking the
    // whole catalog is the point of this screen, so backing out of one item to check the
    // next should resume where you were, not restart from the top every time.

    // Quantities DO need refreshing every visit, though -- editing an item via item-edit
    // and backing out here shouldn't leave this list showing a stale count.
    int n = items_count();
    for (int i = 0; i < n && i < MAX_ITEMS; i++) {
        const Item *it = items_get(i);
        char qty_buf[16];
        snprintf(qty_buf, sizeof(qty_buf), "x%d", it->stocked);
        lv_label_set_text(_qty_lbls[i], qty_buf);
    }

    header_set_visible(true);
    header_set_title("INVENTORY COUNT");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_down;
    h.enter = cb_select;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
