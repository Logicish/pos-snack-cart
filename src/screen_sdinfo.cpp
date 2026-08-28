#include "screen_sdinfo.h"
#include "screens.h"
#include "screen_admin_tools.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "db.h"
#include "items.h"
#include "users.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <SD.h>
#include <stdarg.h>

#define FOOTER_H 52

static lv_obj_t *_scr;
static lv_obj_t *_content;  // scrollable -- a long file listing can run past one screen
static lv_obj_t *_lbl;

// 2026-08-28: was screen_menu_push() (kicked all the way out to the main Admin Menu
// instead of back one level to Advanced Tools).
static void cb_back() {
    screen_admin_tools_push();
}

static void append(String &out, const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out += buf;
}

static void refresh_report() {
    String out;

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        out += "SD: NOT DETECTED\n\n";
    } else {
        const char *typeName = (cardType == CARD_MMC)  ? "MMC"  :
                                (cardType == CARD_SD)   ? "SDSC" :
                                (cardType == CARD_SDHC) ? "SDHC" : "UNKNOWN";
        append(out, "SD: %s, %llu MB\n\n", typeName,
               (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
    }

    if (db_handle()) {
        append(out, "DB: OPEN (/sd/pos.db)\nItems: %d   Users: %d\n\n", items_count(), users_count());
    } else {
        out += "DB: NOT OPEN\n\n";
    }

    out += "Files on SD:\n";
    File root = SD.open("/");
    if (!root) {
        out += "(couldn't open root dir)\n";
    } else {
        File f = root.openNextFile();
        int n = 0;
        while (f) {
            append(out, "%s (%lu B)\n", f.name(), (unsigned long)f.size());
            f.close();
            f = root.openNextFile();
            if (++n >= 20) { out += "...\n"; break; }  // guard against a huge listing
        }
        if (n == 0) out += "(empty)\n";
        root.close();
    }

    lv_label_set_text(_lbl, out.c_str());
    lv_obj_scroll_to_y(_content, 0, LV_ANIM_OFF);  // back to the top on every refresh
}

static void cb_retry() {
    if (!db_handle()) {
        db_init();
        items_init();
        users_import_from_sd();
    }
    refresh_report();
}

// UP/DOWN page the report directly -- nothing to select here, just a report to read, same
// idiom as screen_browse.cpp/screen_db_view.cpp. Guards against the file listing (up to
// 20 entries) running off the bottom of the screen, which the old fixed top-left layout
// had no answer for.
static void cb_page_up()   { lv_obj_scroll_by(_content, 0,  300, LV_ANIM_OFF); }
static void cb_page_down() { lv_obj_scroll_by(_content, 0, -300, LV_ANIM_OFF); }

void screen_sdinfo_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _content = lv_obj_create(_scr);
        lv_obj_set_size(_content, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(_content, LV_ALIGN_TOP_MID, 0, HDR_H);
        lv_obj_set_style_bg_opa(_content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_content, 16, LV_PART_MAIN);

        _lbl = lv_label_create(_content);
        lv_label_set_long_mode(_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_lbl, SCREEN_W - 32);
        lv_obj_set_style_text_color(_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char page_up_lbl[24], page_down_lbl[24];
        snprintf(page_up_lbl,   sizeof(page_up_lbl),   "%s Page Up",   LV_SYMBOL_UP);
        snprintf(page_down_lbl, sizeof(page_down_lbl), "%s Page Down", LV_SYMBOL_DOWN);
        ui_legend_row(legend, page_up_lbl, lv_color_hex(C_YELLOW), page_down_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Retry", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("SD INFO");
    refresh_report();

    ButtonHandlers h;
    h.up    = cb_page_up;
    h.down  = cb_page_down;
    h.enter = cb_retry;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
