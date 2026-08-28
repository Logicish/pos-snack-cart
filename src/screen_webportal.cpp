#include "screen_webportal.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "webserver.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

static lv_obj_t *_scr;
static lv_obj_t *_ssid_lbl;
static lv_obj_t *_password_lbl;
static lv_obj_t *_qr;
static lv_obj_t *_url_lbl;

static void cb_back() {
    webserver_stop_ap();  // radio only runs while this screen is open
    screen_menu_push();
}

void screen_webportal_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = lv_obj_create(_scr);
        lv_obj_set_size(content, SCREEN_W, SCREEN_H - HDR_H);
        lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(content, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_row(content, 6, LV_PART_MAIN);
        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *connect_hint = lv_label_create(content);
        lv_label_set_text(connect_hint, "Connect your device's WiFi to:");
        lv_obj_set_style_text_color(connect_hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(connect_hint, &lv_font_montserrat_16, LV_PART_MAIN);

        _ssid_lbl = lv_label_create(content);
        lv_obj_set_style_text_color(_ssid_lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_text_font(_ssid_lbl, &lv_font_montserrat_28, LV_PART_MAIN);

        _password_lbl = lv_label_create(content);
        lv_obj_set_style_text_color(_password_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_password_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        lv_obj_t *spacer = lv_obj_create(content);
        lv_obj_set_size(spacer, 1, 8);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(spacer, 0, LV_PART_MAIN);

        lv_obj_t *scan_hint = lv_label_create(content);
        lv_label_set_text(scan_hint, "Then scan for the page:");
        lv_obj_set_style_text_color(scan_hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(scan_hint, &lv_font_montserrat_16, LV_PART_MAIN);

        // Same lv_qrcode widget screen_pos.cpp's Payment screen already uses for the
        // Venmo QR -- this one only ever needs a plain URL (no scanner-app quirks like
        // Venmo's own in-app scanner has), so no extra hint about which camera to use.
        _qr = lv_qrcode_create(content, 180, lv_color_hex(C_BG), lv_color_hex(C_TEXT));

        _url_lbl = lv_label_create(content);
        lv_obj_set_style_text_color(_url_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(_url_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

        ui_footer_cancel(_scr);
    }

    webserver_start_ap();  // radio comes up fresh every time this screen is entered

    lv_label_set_text(_ssid_lbl, webserver_ap_ssid());

    char password_buf[48];
    snprintf(password_buf, sizeof(password_buf), "Password: %s", webserver_ap_password());
    lv_label_set_text(_password_lbl, password_buf);

    char url[40];
    snprintf(url, sizeof(url), "http://%s/", WiFi.softAPIP().toString().c_str());
    lv_qrcode_update(_qr, url, strlen(url));
    lv_label_set_text(_url_lbl, url);

    header_set_visible(true);
    header_set_title("WEB PORTAL");

    ButtonHandlers h;
    h.back = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
