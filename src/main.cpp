#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "theme.h"
#include "header.h"
#include "screens.h"
#include "screen_enroll.h"
#include "screen_browse.h"
#include "screen_menu.h"
#include "screen_pos.h"
#include "screen_blocked.h"
#include "screen_screensaver.h"
#include "screen_splash.h"
#include "screen_item_edit.h"
#include "screen_restock.h"
#include "screen_add_item.h"
#include "screen_add_user.h"
#include "screen_gm65_test.h"
#include "screen_price_scan.h"
#include "screen_admin_login.h"
#include "idle_timer.h"
#include "rtc.h"
#include "ui.h"
#include "users.h"
#include "items.h"
#include "buttons.h"
#include "backlight.h"
#include "db.h"
#include "webserver.h"

// ── hardware ──────────────────────────────────────────────────────────────────
TFT_eSPI       tft;
HardwareSerial scanner(1);

// 2026-08-26 — a soft ESP32 reset (e.g. a firmware reflash) doesn't necessarily power-
// cycle the GM65 (confirmed ambiguous earlier — see project memory), so it can come back
// up still holding whatever live/RAM state it had *before* the reset — caught for real
// when a reflash landed while the screensaver had it in Manual/scan-off mode, and it
// stayed off after boot with nothing to notice or correct it. Force the known-good
// locked config on every boot rather than trusting whatever state carried over. RAM-only
// writes (no EEPROM save — this isn't re-provisioning, just re-asserting), matching
// screen_gm65_test.cpp's act_setup_defaults() values exactly (see project memory:
// project_gm65_settings_lockin) but duplicated here rather than shared, per this
// codebase's established per-file self-contained convention for GM65 command helpers.
static void gm65_write_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t data) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x08, 0x01, addr_hi, addr_lo, data, 0xAB, 0xCD};
    scanner.write(cmd, 9);
}

// ── LVGL buffers (PSRAM) ──────────────────────────────────────────────────────
static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *buf1;
static lv_color_t        *buf2;

// ── idle screen ───────────────────────────────────────────────────────────────
static lv_obj_t *idle_scr;

void screen_idle_load() {
    header_set_visible(true);
    header_set_current_user("");
    header_set_title("START");

    // Explicit control scheme, 2026-08-26 — matches the transaction screen's legend
    // convention (see build_idle_screen()'s footer). ENTER opens screen_admin_login_push()
    // — Admin Menu access is now a deliberate button+scan combo, not an automatic
    // consequence of a badge happening to be an admin's (see on_scan() below and
    // screen_admin_login.h for the reasoning). BACK calls screen_screensaver_push()
    // directly — the exact same function the real idle-timeout uses (idle_timer.cpp), so
    // this is a manual "kill the scanner light now" shortcut, not a separate code path —
    // whenever GM65 scan-mode logic gets wired into screen_screensaver_push()/cb_wake(),
    // it automatically covers both the timeout and this manual trigger at once.
    ButtonHandlers h;
    h.left  = screen_price_scan_push;   // scan a UPC directly for its price
    h.right = screen_browse_push;       // scroll the existing Price Check/Browse list
    h.enter = screen_admin_login_push;
    h.back  = screen_screensaver_push;
    buttons_set_handlers(h);
    idle_timer_arm();

    lv_scr_load(idle_scr);
}

// ── LVGL callbacks ────────────────────────────────────────────────────────────
static void lv_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)color_p, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

// ── init helpers ──────────────────────────────────────────────────────────────
static void lvgl_init() {
    lv_init();
    buf1 = (lv_color_t *)heap_caps_malloc(SCREEN_W * 80 * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf2 = (lv_color_t *)heap_caps_malloc(SCREEN_W * 80 * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, SCREEN_W * 80);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_W;
    disp_drv.ver_res  = SCREEN_H;
    disp_drv.flush_cb = lv_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
}

static void build_idle_screen() {
    idle_scr = lv_scr_act();
    lv_obj_set_style_bg_color(idle_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(idle_scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(idle_scr);
    lv_label_set_text(label, "Scan Badge\nTo Start");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 280);
    lv_obj_set_style_text_color(label, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, HDR_H / 2);

    // Explicit two-line legend, same convention as the transaction screen — every active
    // button spelled out rather than left to spatial/color intuition (see
    // feedback-ux-explicit-legends in project memory).
    lv_obj_t *legend = ui_legend(idle_scr);
    lv_obj_set_width(legend, SCREEN_W - 40);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -16);

    char left_arrow[24], right_arrow[24];
    snprintf(left_arrow, sizeof(left_arrow), "%s Price Check", LV_SYMBOL_LEFT);
    snprintf(right_arrow, sizeof(right_arrow), "Browse Items %s", LV_SYMBOL_RIGHT);
    ui_legend_row(legend, left_arrow, lv_color_hex(C_YELLOW), right_arrow, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Admin Login", lv_color_hex(C_GREEN), "Screen Saver", lv_color_hex(C_RED));
}

// ── badge scan routing ────────────────────────────────────────────────────────
static void on_scan(const char *badge_id) {
    if (screen_enroll_on_scan(badge_id)) return;
    if (screen_pos_on_scan(badge_id)) return;  // TRANSACTION screen active — treat as an item UPC, not a badge
    if (screen_restock_on_scan(badge_id)) return;
    if (screen_add_item_on_scan(badge_id)) return;
    if (screen_item_edit_on_scan(badge_id)) return;  // viewing an item — link another UPC to it
    if (screen_add_user_on_scan(badge_id)) return;   // admin's "Add User" mode armed — this scan is the new person's badge, not a login attempt
    if (screen_price_scan_on_scan(badge_id)) return; // Start screen's scan-first Price Check armed
    if (screen_admin_login_on_scan(badge_id)) return; // Start screen's Admin Login armed

    const User *u = users_find_by_badge(badge_id);
    if (u) {
        if (!u->active) {
            Serial.printf("[SCAN] Inactive user %d: %s %s — turned away\n", u->id, u->first_name, u->last_name);
            char msg[80];
            snprintf(msg, sizeof(msg), "Sorry %s,\nplease see the cart\nowner to continue.", u->first_name);
            screen_blocked_push(msg);
            return;
        }
        // 2026-08-26 — a plain scan no longer routes admins to the Admin Menu. That's now
        // a deliberate two-step action (screen_admin_login_push(), Start screen's Green
        // button, above) — a real fix, not just a preference: before this, an admin could
        // NEVER reach their own transaction via a plain badge scan at all, since this
        // branch always intercepted first. Now every known active user, admin or not,
        // just starts their own transaction on a plain scan.
        Serial.printf("[SCAN] Known user %d: %s %s\n", u->id, u->first_name, u->last_name);
        // TODO: screen_confirm_user_push(u->id, u->first_name) — for now go straight to POS
        header_set_current_user(u->first_name);
        screen_pos_push(u->id);
    } else {
        // 2026-08-25: used to self-enroll here (screen_enroll_push(badge_id)) — removed per
        // the owner's explicit "no guest checkout, all users manually enrolled by an admin"
        // call. See Add User (Admin Menu) for the replacement admin-gated enrollment path.
        Serial.printf("[SCAN] Unknown badge: %s — not enrolled\n", badge_id);
        screen_blocked_push("Badge not recognized.\n\nPlease see an admin to\nget enrolled.");
    }
}

// ── Arduino ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);
    backlight_init();

    if (!db_init()) {
        // Non-fatal on purpose: still boot into the menu so the DB Check screen is
        // reachable to diagnose what went wrong, rather than hard-halting blind.
        Serial.println("[FATAL] db_init() failed — DB features unavailable this boot");
    }
    items_init();
    users_import_from_sd();
    users_backfill_admin_passwords();  // bootstraps a password for any admin that predates this schema
    idle_timer_load_from_config();     // picks up a saved screensaver timeout, defaults to 3 min otherwise
    rtc_init();  // DS3231 -- safe no-op if not wired yet, see rtc.h
    webserver_init();  // AP mode; admin actions now gated behind a real login, see webserver.cpp

    lvgl_init();
    header_init();
    buttons_init();
    build_idle_screen();
    screen_splash_push();  // real boot flow: splash → (timeout) → IDLE

    scanner.begin(9600, SERIAL_8N1, 17, 16);  // ESP RX=17 (← GM65 TX), ESP TX=16 (→ GM65 RX)

    // Force the known-good config every boot (see comment at gm65_write_reg() above).
    // Free real estate time-wise — the splash screen is already up for SPLASH_DURATION_MS
    // regardless, so this rides along with a wait the user experiences anyway. Small
    // delays between writes, not fired back-to-back — matches the pattern already proven
    // safe in screen_gm65_test.cpp's act_setup_defaults(). The ACK replies themselves
    // aren't drained here at all anymore: screen_splash_is_active() (checked in loop(),
    // same pattern as the screensaver) ignores any scanner traffic for as long as splash
    // is actually on screen, so there's no need to guess a "long enough" blocking delay —
    // it's covered for the splash's whole real duration, however long that ends up being.
    gm65_write_reg(0x00, 0x00, 0x57);  // Induction + Light Normal + Aim Normal + Buzzer on + LED off
    delay(30);
    gm65_write_reg(0x00, 0x07, 0x00);  // sleep-on-idle off
    delay(30);
    gm65_write_reg(0x00, 0x0A, 0x64);  // buzzer passive mode @ 2000Hz — 0x00 (Active) produces
                                        // just a click on this unit's actual buzzer hardware,
                                        // confirmed via a real register readback, not a guess
    delay(30);
}

void loop() {
    buttons_poll();  // sample input first — don't let a slow render delay picking up a press
    lv_timer_handler();
    idle_timer_check();

    if (scanner.available()) {
        String code = scanner.readStringUntil('\n');

        if (screen_gm65_test_capture(code.c_str(), code.length())) {
            // GM65 Test screen is active and this looked like a command reply/ACK, not a
            // real badge/UPC scan — consumed above (as a hex dump), don't fall through.
        } else {
            code.trim();
            if (code.length() > 0) {
                // Screensaver deliberately doesn't wake on a scan (only button presses) —
                // see screen_screensaver_is_active(). Splash gets the same treatment so the
                // boot-time GM65 config writes' ACK replies (setup(), above) don't get
                // misrouted here as a bogus scan. Still drain the UART either way so bytes
                // don't pile up.
                if (screen_screensaver_is_active() || screen_splash_is_active()) {
                    Serial.printf("SCAN: [%s] — ignored, splash/screensaver active\n", code.c_str());
                } else {
                    Serial.printf("SCAN: [%s]\n", code.c_str());
                    on_scan(code.c_str());
                }
            }
        }
    }

    delay(1);
}
