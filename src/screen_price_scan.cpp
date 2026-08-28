// Scan-first price check, 2026-08-26 — Start screen's Left arrow. Complements the
// existing scroll-first "Browse Items" (screen_browse.cpp, Right arrow): scan a UPC, see
// its price immediately, no badge and no scrolling needed. Same on_scan()-consumption
// pattern as Restock/Add-Item/etc. — only active while this screen is actually on top.
#include "screen_price_scan.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "items.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <stdio.h>

static lv_obj_t *_scr;
static lv_obj_t *_lbl;

static void cb_back() {
    screen_idle_load();
}

bool screen_price_scan_on_scan(const char *upc) {
    if (!_scr || lv_scr_act() != _scr) return false;

    const Item *it = items_find_by_upc(upc);
    if (it) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n$%d.%02d\n\nScan another to check it.",
                 it->name, it->price_cents / 100, it->price_cents % 100);
        lv_label_set_text(_lbl, buf);
    } else {
        lv_label_set_text(_lbl, "Not recognized.\n\nScan another.");
    }
    return true;
}

void screen_price_scan_push() {
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

    lv_label_set_text(_lbl, "Scan an item to check\nits price.");

    header_set_visible(true);
    header_set_title("PRICE CHECK");

    ButtonHandlers h;
    h.back = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
