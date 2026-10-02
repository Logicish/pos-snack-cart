#include "screen_splash.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "session_timer.h"
#include <lvgl.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the boot splash screen declared in screen_splash.h -- shows
            the wordmark for SPLASH_DURATION_MS, then falls through to IDLE.
*/

#define SPLASH_DURATION_MS 2000  // how long the splash shows before falling through to IDLE

LV_IMG_DECLARE(fish_right);

static lv_obj_t   *_scr;
static lv_timer_t *_timer;

// Fires once SPLASH_DURATION_MS has elapsed, moving on to IDLE.
static void cb_timeout(lv_timer_t *t) {
    lv_timer_del(t);
    _timer = nullptr;
    screen_idle_load();
}

// 2026-08-26 — used to jump straight to screen_menu_push() (Admin Menu), a leftover dev
// shortcut from before real badge-gating existed. Admin Menu access is a real badge scan
// only now (see main.cpp's on_scan()/screen_admin_login.h) — Back here just skips ahead
// to IDLE instead, same as letting the splash timer run out.
static void cb_back() {
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    screen_idle_load();
}

// True if this screen is the one currently on screen.
bool screen_splash_is_active() {
    return _scr && lv_scr_act() == _scr;
}

// Loads the splash screen and starts its auto-advance timer.
void screen_splash_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *img = lv_img_create(_scr);
        lv_img_set_src(img, &fish_right);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t *wordmark = lv_label_create(_scr);
        lv_label_set_text(wordmark, "LogicishDesigns.com");
        lv_obj_set_style_text_color(wordmark, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(wordmark, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align_to(wordmark, img, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
    }

    header_set_visible(false);

    ButtonHandlers h;
    h.back = cb_back;
    buttons_set_handlers(h);
    session_timer_disarm();  // nothing logged in yet at boot

    lv_scr_load(_scr);

    screen_splash_release(SPLASH_DURATION_MS);
}

// Boot's version, 2026-10-02: same screen, but no timer -- it goes up before the DB is even
// mounted and stays through all of boot, until setup() calls screen_splash_release().
void screen_splash_hold() {
    screen_splash_push();
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
}

// Starts (or restarts) the countdown to IDLE, hold_ms from now.
void screen_splash_release(uint32_t hold_ms) {
    if (_timer) lv_timer_del(_timer);
    _timer = lv_timer_create(cb_timeout, hold_ms, nullptr);
}
