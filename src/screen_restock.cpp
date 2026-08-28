#include "screen_restock.h"
#include "screen_item_edit.h"
#include "screen_inventory_menu.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "items.h"
#include "ui.h"
#include <lvgl.h>

static lv_obj_t *_scr;
static lv_obj_t *_lbl;

static void cb_back() {
    screen_inventory_menu_push();  // 2026-08-28 reorg -- was screen_menu_push()
}

bool screen_restock_on_scan(const char *upc) {
    if (!_scr || lv_scr_act() != _scr) return false;

    const Item *it = items_find_by_upc(upc);
    if (it) {
        screen_item_edit_push(it->id, screen_restock_push);
    } else {
        lv_label_set_text(_lbl,
            "Not recognized.\n\n"
            "Use \"Add/Attach Item\" from\n"
            "the Admin Menu to link it,\n"
            "then come back here.\n\n"
            "Scan another item.");
    }
    return true;
}

void screen_restock_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _lbl = lv_label_create(_scr);
        lv_label_set_long_mode(_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_lbl, 280);
        lv_obj_set_style_text_color(_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_align(_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(_lbl, LV_ALIGN_CENTER, 0, 0);

        ui_footer_cancel(_scr);
    }

    lv_label_set_text(_lbl, "Scan an item to restock it.");

    header_set_visible(true);
    header_set_title("RESTOCK");

    ButtonHandlers h;
    h.back = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
