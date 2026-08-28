#include "screen_blocked.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include <lvgl.h>
#include <Arduino.h>

static lv_obj_t *_scr;
static lv_obj_t *_lbl;

static void cb_back() {
    screen_idle_load();
}

void screen_blocked_push(const char *message) {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _lbl = lv_label_create(_scr);
        lv_label_set_long_mode(_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_lbl, 280);
        lv_obj_set_style_text_color(_lbl, lv_color_hex(C_ORANGE), LV_PART_MAIN);
        lv_obj_set_style_text_font(_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_align(_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(_lbl, LV_ALIGN_CENTER, 0, 0);
    }

    header_set_visible(true);
    header_set_current_user("");
    lv_label_set_text(_lbl, message);

    ButtonHandlers h;
    h.back = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
