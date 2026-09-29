#include "screen_admin_login.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "users.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Admin Login scan prompt declared in screen_admin_login.h.
  Notes---- Same armed-screen-intercepts-the-next-scan pattern as Restock/Add
            User/Price Check.
*/

static lv_obj_t *_scr;
static lv_obj_t *_lbl;

// Back returns to IDLE, cancelling the login attempt.
static void cb_back() {
    screen_idle_load();
}

// Consumes a scan while this screen is active -- routes to the Admin Menu if it's a
// recognized, active admin badge, otherwise shows why not and stays put.
bool screen_admin_login_on_scan(const char *badge_id) {
    if (!_scr || lv_scr_act() != _scr) return false;

    const User *u = users_find_by_badge(badge_id);
    if (!u) {
        lv_label_set_text(_lbl, "Badge not recognized.\n\nScan another.");
    } else if (!u->active) {
        lv_label_set_text(_lbl, "This user is inactive.\n\nScan another.");
    } else if (!u->admin) {
        lv_label_set_text(_lbl, "That's not an admin badge.\n\nScan another.");
    } else {
        users_set_current_admin(u->id);
        header_set_current_user(u->first_name);
        screen_menu_push();
    }
    return true;
}

// Loads the Admin Login scan prompt.
void screen_admin_login_push() {
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

    lv_label_set_text(_lbl, "Scan your admin badge.");

    header_set_visible(true);
    header_set_title("ADMIN LOGIN");

    ButtonHandlers h;
    h.back = cb_back;
    h.wantsScanner = true;  // this whole screen is "waiting for an admin badge scan"
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
