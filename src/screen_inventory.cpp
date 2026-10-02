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

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Inventory screen declared in screen_inventory.h.
*/

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

// Highlights the currently-selected row.
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

// Up: moves the selection up one row, wrapping.
static void cb_up() {
    int n = visible_count();
    if (n == 0) return;
    _cursor = (_cursor - 1 + n) % n;
    refresh_cursor();
}

// Down: moves the selection down one row, wrapping.
static void cb_down() {
    int n = visible_count();
    if (n == 0) return;
    _cursor = (_cursor + 1) % n;
    refresh_cursor();
}

// LEFT/RIGHT jump ~a screenful at a time, both directions wrapping -- remapped
// 2026-09-15 from a Left-only forward accelerator to match Browse/Price, Item Lookup,
// and Balances' paging convention.
static void cb_page_left() {
    int n = visible_count();
    if (n == 0) return;
    if (_cursor <= 0) {
        _cursor = n - 1;
    } else {
        _cursor -= INVENTORY_PAGE;
        if (_cursor < 0) _cursor = 0;
    }
    refresh_cursor();
}

static void cb_page_right() {
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

// Back returns to the Inventory submenu.
static void cb_back() {
    screen_inventory_menu_push();  // 2026-08-28 reorg -- was screen_menu_push()
}

// Enter opens the highlighted item in the shared item-edit screen.
static void cb_select() {
    const Item *it = items_get(_cursor);
    if (it) screen_item_edit_push(it->id, screen_inventory_push);
}

// Loads the Inventory screen, rebuilding the row list from the current catalog.
void screen_inventory_push() {
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
        lv_obj_set_style_pad_hor(_list, 12, LV_PART_MAIN);  // 12px side inset, same as every screen
        lv_obj_set_style_pad_row(_list, 6, LV_PART_MAIN);
        lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char move_lbl[24], page_lbl[24];
        snprintf(move_lbl, sizeof(move_lbl), "Move %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        snprintf(page_lbl, sizeof(page_lbl), "Page %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
        ui_legend_row(legend, move_lbl, lv_color_hex(C_YELLOW), page_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    // Rows are rebuilt every visit, not just the first -- items_count() can grow between
    // visits (Add/Attach Item happens on a different screen entirely), and the old
    // build-once/refresh-labels-only approach left a newly added item with no row or
    // label at all, so the quantity refresh below wrote through a null _qty_lbls[]
    // pointer for it (a real crash, caught in review before it ever hit hardware).
    // _cursor is deliberately NOT reset here (unlike screen_browse.cpp) -- walking the
    // whole catalog is the point of this screen, so backing out of one item to check the
    // next should resume where you were, not restart from the top every time. Just
    // clamped in case the catalog shrank (an item got deleted) since the last visit.
    lv_obj_clean(_list);
    int n = items_count();
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    for (int i = 0; i < n; i++) {
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

        // it can be null here if the underlying query failed (e.g. a real DB error) --
        // items_count() and items_get() aren't atomic against each other, and a real crash
        // was traced to this exact gap (see db.cpp's items.hidden verification log for one
        // way this can happen). Show an obviously-broken placeholder row instead of
        // dereferencing a null pointer.
        lv_obj_t *name_lbl = lv_label_create(row);
        if (!it) {
            lv_label_set_text(name_lbl, "(error loading item)");
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_RED), LV_PART_MAIN);
        } else if (it->hidden) {
            // Hidden items stay in this list on purpose (see items.h) -- this is the "pull
            // it back up to unhide/restock/edit it" path -- but need to visually stand out
            // from the catalog's normal, sellable items so scrolling past one doesn't read
            // as a still-active item.
            char name_buf[ITEM_NAME_LEN + 12];
            snprintf(name_buf, sizeof(name_buf), "%s [HIDDEN]", it->name);
            lv_label_set_text(name_lbl, name_buf);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        } else {
            lv_label_set_text(name_lbl, it->name);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        }
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        char qty_buf[16];
        if (it) snprintf(qty_buf, sizeof(qty_buf), "x%d", it->stocked);
        else    snprintf(qty_buf, sizeof(qty_buf), "--");
        lv_obj_t *qty_lbl = lv_label_create(row);
        lv_label_set_text(qty_lbl, qty_buf);
        lv_obj_set_style_text_color(qty_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_text_font(qty_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        _qty_lbls[i] = qty_lbl;
    }
    if (_cursor >= n) _cursor = n > 0 ? n - 1 : 0;
    _prev_cursor = -1;  // rows are new lv_obj_t*s every rebuild -- force a fresh highlight

    header_set_visible(true);
    header_set_title("INVENTORY");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_left;
    h.right = cb_page_right;
    h.enter = cb_select;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
