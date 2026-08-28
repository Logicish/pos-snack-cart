#include "screen_browse.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "items.h"
#include "buttons.h"
#include "idle_timer.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

#define FOOTER_H 52

// Deliberately a touch smaller than the visible row count (~8.5 rows fit in the 392px list
// area at this row height) so successive presses always overlap by a row and never skip
// one — same tuning rationale as ATTACH_PAGE in screen_add_item.cpp.
#define BROWSE_PAGE 7

static lv_obj_t *_scr;
static lv_obj_t *_list;
static lv_obj_t *_rows[MAX_ITEMS];
static int        _cursor;  // scroll-into-view anchor only -- Browse is read-only, no highlight

// Only the first MAX_ITEMS rows are ever built (see screen_browse_push()) — cursor math
// must wrap against that same cap, not the raw DB count, or it indexes _rows[] out of
// bounds. Same crash-cause as screen_pos.cpp's Manual Entry list, fixed 2026-08-25.
static int visible_count() {
    int n = items_count();
    return n > MAX_ITEMS ? MAX_ITEMS : n;
}

// 2026-08-28: Browse has nothing to select (Enter does nothing here), so unlike every
// other list screen there's no highlight box to move -- just scroll position. Settled by
// explicit choice over the alternative (keep a cursor highlight): a plain paging viewport.
static void scroll_to_cursor() {
    int n = visible_count();
    if (n == 0) return;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

// UP/DOWN page the viewport a screenful at a time -- Browse has no selection, so unlike
// every other list screen Up/Down don't step a cursor, they ARE the page keys. Both wrap
// (Down past the last page lands back on the first, Up before the first lands on the
// last) -- same wrap convention as the Left-only accelerator on Inventory/Menu/Admin
// Tools, just available in both directions here since Up/Down have no other job.
static void cb_page_down() {
    int n = visible_count();
    if (n == 0) return;
    if (_cursor >= n - 1) {
        _cursor = 0;
    } else {
        _cursor += BROWSE_PAGE;
        if (_cursor >= n) _cursor = n - 1;
    }
    scroll_to_cursor();
}

static void cb_page_up() {
    int n = visible_count();
    if (n == 0) return;
    if (_cursor <= 0) {
        _cursor = n - 1;
    } else {
        _cursor -= BROWSE_PAGE;
        if (_cursor < 0) _cursor = 0;
    }
    scroll_to_cursor();
}

static void cb_back() {
    screen_idle_load();
}

void screen_browse_push() {
    _cursor = 0;

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

        int n = items_count();
        for (int i = 0; i < n && i < MAX_ITEMS; i++) {
            const Item *it = items_get(i);

            lv_obj_t *row = lv_obj_create(_list);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
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

            char price_buf[8];
            snprintf(price_buf, sizeof(price_buf), "$%d.%02d", it->price_cents / 100, it->price_cents % 100);
            lv_obj_t *price_lbl = lv_label_create(row);
            lv_label_set_text(price_lbl, price_buf);
            lv_obj_set_style_text_color(price_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
            lv_obj_set_style_text_font(price_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        }

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char page_up_lbl[24], page_down_lbl[24];
        snprintf(page_up_lbl,   sizeof(page_up_lbl),   "%s Page Up",   LV_SYMBOL_UP);
        snprintf(page_down_lbl, sizeof(page_down_lbl), "%s Page Down", LV_SYMBOL_DOWN);
        ui_legend_row(legend, page_up_lbl, lv_color_hex(C_YELLOW), page_down_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    scroll_to_cursor();
    header_set_title("Browse");

    ButtonHandlers h;
    h.up   = cb_page_up;
    h.down = cb_page_down;
    h.back = cb_back;
    buttons_set_handlers(h);
    idle_timer_arm();

    lv_scr_load(_scr);
}
