#include "screen_add_user.h"
#include "screen_user_menu.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "users.h"
#include "ui.h"
#include <lvgl.h>

static lv_obj_t *_scr;
static lv_obj_t *_lbl;

static void cb_back() {
    screen_user_menu_push();  // 2026-08-28 reorg -- was screen_menu_push()
}

bool screen_add_user_on_scan(const char *badge_id) {
    if (!_scr || lv_scr_act() != _scr) return false;

    if (users_find_by_badge(badge_id)) {
        lv_label_set_text(_lbl,
            "That badge is already\nenrolled.\n\n"
            "Scan another badge.");
    } else {
        screen_enroll_push(badge_id);
    }
    return true;
}

void screen_add_user_push() {
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

    lv_label_set_text(_lbl, "Scan the new person's badge.");

    header_set_visible(true);
    header_set_title("ADD USER");

    ButtonHandlers h;
    h.back = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
