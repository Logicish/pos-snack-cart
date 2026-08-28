// TEMPORARY diagnostic screen, 2026-08-26 — GM65 software-side settings test + lock-in.
// Protocol originally reverse-derived from DFRobot's GM65_scanner_for_Arduino library
// (an early attempt to text-extract our own manual PDF failed — no pdftoppm available in
// this environment, and that PDF turned out to be image-based anyway). Later the same
// day, a real manual WAS obtained (from the official Hangzhou Grow Technology site) and
// cross-checked against DFRobot's own datasheet and the GM65-S manual — all three agree
// on everything below, so this is now verified documentation, not just reverse-engineered
// guesswork. Frame format: 0x7E 0x00 0x08 0x01 <addr_hi> <addr_lo> <data> 0xAB 0xCD
// (write); 0xAB 0xCD is an officially-documented "skip CRC" placeholder, not a real
// computed CRC (CRC_CCITT, poly 0x1021, if ever needed). Register 0x0000 packs working
// mode (bits 0-1), light (2-3), aim (4-5), buzzer-mute (6), and LED indicator (7) into
// one byte.
//
// Narrowed from an earlier 10-row per-field-toggle version down to 3, once real hands-on
// testing settled on a fixed target config: Induction working mode + Light Normal + Aim
// Normal + Buzzer on + LED indicator off = register 0x0000 = 0x57 (bit6=1 for buzzer ON —
// empirically the OPPOSITE polarity from the reference library's "silent_mode" naming,
// confirmed on real hardware; Aim flipped from an initial Off to Normal after real
// scanning without it proved "trial and error" on distance/placement), register 0x0007
// (sleep-on-idle) left at 0x00/off — sleep is superseded by a planned physical GND-side
// power switch (screensaver = GM65 unpowered) rather than a live UART sleep command, so
// there's no more reason to expose per-field toggles for values that are now fixed.
// Buzzer tone/volume isn't in this library at all and is explicitly back-burnered — see
// project memory (the beep defaulted to a noticeably quiet volume, a real pre-ship
// concern given the enclosure, but not blocking this round).
//
// screen_gm65_test_capture() is called directly from main.cpp's loop(), ahead of the
// normal on_scan() badge/UPC dispatch. The GM65 ACKs commands — confirmed by watching
// this screen boot out to "Badge not recognized" every time a setting changed, before
// this existed — and those ACK bytes need somewhere to go that isn't a badge lookup.
// Response frames are binary, not clean text, so this shows a hex dump rather than
// trying to render it as a string. One known limitation: the capture only sees whatever
// Stream::readStringUntil('\n') handed back, so a reply containing a literal 0x0A byte
// would still get split early — the same pre-existing limitation the rest of this
// project's scan reading has always had; not worth solving for a temporary tool.
//
// "Read Settings" doubles as the settings-persist-across-power-loss test: send it, power
// the module fully off and back on with nothing else sent, send it again, compare. A
// first real attempt at this FAILED (reverted to factory default 0xD6) — turned out a
// plain register write only changes the live/RAM value; a separate "Save Zone Bit to
// Internal Flash" command (0x0009 type, real CRC, found in the real manual) is required
// to persist it, and Setup Defaults below now sends that too.
//
// Factory Reset here sends 0x50 (verified real "reset to factory defaults" per the
// manual) — NOT the DFRobot library's 0x55, which turned out to be a different command
// entirely ("restore user-defined factory settings," a separate saved-baseline slot this
// project never populates). Deliberately not using that save-a-custom-baseline mechanism
// at all: it still requires writing the real values first, so it's pure overhead on top
// of Setup Defaults for no real benefit, and it's untested machinery on a module with a
// track record of not doing what its docs promise.
#include "screen_gm65_test.h"
#include "screens.h"
#include "screen_admin_tools.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

extern HardwareSerial scanner;  // owned by main.cpp; this screen only ever writes to it

#define ROW_COUNT 5
#define FOOTER_H  52

static lv_obj_t *_scr;
static lv_obj_t *_rows[ROW_COUNT];
static lv_obj_t *_row_lbls[ROW_COUNT];  // rows 3 and 4 change text dynamically
static lv_obj_t *_log_lbl;
static int        _cursor;
static int        _prev_cursor = -1;

static const char *ROW_LABELS[ROW_COUNT] = {
    "Setup Defaults",
    "Read Settings",
    "Reset -- warning",
    "Toggle Scan Mode: On",
    "Sound Mode: - Hz",  // placeholder — updated once the row is actually pressed
};

static void write_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t data) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x08, 0x01, addr_hi, addr_lo, data, 0xAB, 0xCD};
    scanner.write(cmd, 9);
}

// `count` is a real field in the protocol (manual: "Datas: Numbers of zone bit for
// Sequential read"), not just a fixed marker — the reply comes back with that many data
// bytes in one frame, registers starting at addr_hi:addr_lo read sequentially.
static void read_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t count) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x07, 0x01, addr_hi, addr_lo, count, 0xAB, 0xCD};
    scanner.write(cmd, 9);
}

// "Save Zone Bit to Internal Flash" — a real, official command (verified 2026-08-26 via
// the actual GM65 manual, cross-checked against DFRobot's own datasheet and the GM65-S
// manual, all identical). A plain register write only changes the live/RAM value; this
// separate command is required to make it survive a real power cycle. Fixed, invariant
// bytes straight from the manual (real CRC, not our usual 0xAB 0xCD placeholder) — saves
// the *entire* zone bit list at once, not just the registers this screen touches.
static void save_to_eeprom() {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0xDE, 0xC8};
    scanner.write(cmd, 9);
}

// 2026-08-28: was screen_menu_push() (kicked all the way out to the main Admin Menu
// instead of back one level to Advanced Tools -- same bug screen_sdinfo.cpp had).
static void cb_back() { screen_admin_tools_push(); }

// ── row actions ───────────────────────────────────────────────────────────────────
static void act_setup_defaults() {
    // Bit 6 empirically confirmed BACKWARDS from the reference library's naming/comment
    // ("silent_mode, 1=on/0=off") on this real hardware, 2026-08-26 — 0x07 (bit6=0) was
    // silent, 0xD6's bit6=1 (factory default) was audible. Trusting the bench test over
    // the translated comment: bit6=1 is what actually produces sound.
    // Aim flipped back to Normal (bits4-5=01) same day, after Off made real scanning a
    // "bit of trial and error" — the aim dot turned out to help judge distance/placement,
    // not just be cosmetic laser clutter as first assumed.
    write_reg(0x00, 0x00, 0x57);  // Induction + Light Normal + Aim Normal + Buzzer ON (bit6=1) + LED off
    write_reg(0x00, 0x07, 0x00);  // sleep-on-idle off
    // CORRECTED 2026-08-26 — was 0x00 (Active buzzer mode). A multi-register Read Settings
    // (0x0000-0x000D in one frame) caught a real, reproducible difference at THIS address
    // specifically between a "click" state and a working "BEEP" state: 0x00 vs 0x64. Active
    // mode doesn't just glitch when switched away from and back — it's simply the wrong
    // buzzer mode for this unit's actual hardware, full stop. 0x64 = passive buzzer mode
    // driven at 100*20=2000Hz, confirmed by ear to produce a clean beep. (The earlier "Active
    // sounds best" verdict from the original 4-way sound test was based on a UI bug: the
    // sound-mode row increments *before* writing, so the first press never actually sent
    // 0x00 — the true 0x00 was only reached on wraparound, which is exactly the press that
    // was reported as "glitching.")
    write_reg(0x00, 0x0A, 0x64);
    delay(50);
    save_to_eeprom();  // makes the above survive a real power cycle, not just a soft reset
    lv_label_set_text(_log_lbl, "Sent: reg0000=0x57, reg0007=0x00,\nreg000A=0x64, saved to flash\n(watch for an ACK below)");
}

static void act_read_settings() {
    // 14 bytes covers registers 0x0000-0x000D in one frame: mode/light/aim/buzzer-mute/
    // LED (0x0000), decode-prompt (0x0002), settlement-code (0x0003), image-stabilization/
    // read-interval/single-read-time (0x0004-06), sleep+free-time (0x0007-08), buzzer
    // frequency mode (0x000A) + duration (0x000B), misc piezo bits (0x000C), serial-
    // output/encoding (0x000D) — real values instead of guessing from 0x0000 alone.
    read_reg(0x00, 0x00, 0x0E);
    lv_label_set_text(_log_lbl, "Sent read for reg0000-000D...\n(waiting for reply below)");
}

// ── factory reset — confirm-gated, real risk of reverting the module out of the UART
// "Series Output" mode this whole project depends on. A recovery command fires right
// after, but if that doesn't stick the physical config QR sheet is the fallback.
static lv_obj_t *_confirm_scr;
static lv_obj_t *_confirm_lbl;

static void cb_confirm_no() { screen_gm65_test_push(); }
static void cb_confirm_yes() {
    // 0x50 = true reset-to-factory-defaults, per the real manual (verified 2026-08-26).
    // The DFRobot library's 0x55 is actually a DIFFERENT command — "restore user-defined
    // factory settings" (a separate saved-baseline slot, set via a distinct 0x56 command
    // this project never uses) — not a real factory reset at all. Corrected before this
    // button was ever tested on real hardware.
    write_reg(0x00, 0xD9, 0x50);
    delay(200);
    write_reg(0x00, 0x0D, 0x00);  // best-effort recovery: force back to Series Output mode

    lv_label_set_text(_confirm_lbl,
        "Factory reset sent, then a\n"
        "Series Output re-apply was\n"
        "sent right behind it.\n\n"
        "Go back and run Setup\n"
        "Defaults, then Read Settings\n"
        "to confirm.\n\n"
        "If nothing responds, re-scan\n"
        "the config QR sheet (baud\n"
        "9600, Series Output) to\n"
        "recover.\n\n"
        "BACK = return");

    ButtonHandlers h;
    h.back = cb_confirm_no;
    buttons_set_handlers(h);
}

static void act_factory_reset() {
    if (!_confirm_scr) {
        _confirm_scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_confirm_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_confirm_scr, LV_OPA_COVER, LV_PART_MAIN);

        _confirm_lbl = lv_label_create(_confirm_scr);
        lv_label_set_long_mode(_confirm_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_confirm_lbl, 260);
        lv_obj_set_style_text_color(_confirm_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_confirm_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_align(_confirm_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(_confirm_lbl, LV_ALIGN_CENTER, 0, 0);
    }

    header_set_visible(true);
    header_set_title("FACTORY RESET?");
    lv_label_set_text(_confirm_lbl,
        "This wipes ALL GM65 settings,\n"
        "possibly including the UART\n"
        "\"Series Output\" mode this\n"
        "whole project depends on.\n\n"
        "A recovery command fires\n"
        "right after, but if that\n"
        "doesn't stick you may need\n"
        "the physical config QR sheet\n"
        "to get scanning back.\n\n"
        "ENTER = do it anyway\n"
        "BACK = cancel");

    ButtonHandlers h;
    h.enter = cb_confirm_yes;
    h.back  = cb_confirm_no;
    buttons_set_handlers(h);

    lv_scr_load(_confirm_scr);
}

// ── scan-mode toggle — REPLACED the 0x00D9=0xA5 deep-sleep experiment, 2026-08-26, after
// real testing (holding a hand in front of the module while sending 0xA5) proved that
// command is just the module's normal "nothing detected" idle state, not a real power-
// down — it still lights up and reacts to anything presented, which doesn't solve "no
// random light during screensaver" at all. This instead clears/restores the working-mode
// bits (0-1) of register 0x0000 — a plain register write, the same proven mechanism as
// Setup Defaults, not a special/uncertain command. Manual mode (00) requires an explicit
// trigger this project never sends, so there's no autonomous sensing loop running at all
// to react to anything nearby; switching back to 0x57 restores full Induction mode
// (light/aim/buzzer/LED bits are untouched either way, only bits 0-1 change).
static bool _gm65_scan_off = false;

static void act_toggle_scan_mode() {
    if (!_gm65_scan_off) {
        write_reg(0x00, 0x00, 0x54);  // Manual mode, no auto-sensing; light/aim/buzzer/LED unchanged
        _gm65_scan_off = true;
        lv_label_set_text(_row_lbls[3], "Toggle Scan Mode: Off");
        lv_label_set_text(_log_lbl, "Sent reg0000=0x54 (Manual\nmode -- no auto-sensing).\n(watch for an ACK below)");
    } else {
        write_reg(0x00, 0x00, 0x57);  // back to the full locked config (Induction + rest)
        _gm65_scan_off = false;
        lv_label_set_text(_row_lbls[3], "Toggle Scan Mode: On");
        lv_label_set_text(_log_lbl, "Sent reg0000=0x57 (back to\nInduction).\n(watch for an ACK below)");
    }
}

// ── sound mode — zone bit 0x000A ("Frequency for successfully read sound," verified
// 2026-08-26 via the real manual). 0x00 = active buzzer mode; 0x01-0xFF = passive buzzer
// mode, driven at Value*20 Hz. CORRECTED same day: 0x00 ("Active") is NOT a safe default
// on this unit — a multi-register Read Settings caught it producing an audible "click"
// instead of a beep, confirmed reproducible, not a one-off. 0x64 (2000Hz) is the real
// confirmed-good value and is now what act_setup_defaults() actually locks in — this row
// still includes 0x00 for reference/comparison, not because it's recommended. RAM-only
// write, same as the scan-mode toggle — not saved to flash on its own.
struct SoundMode { uint8_t value; const char *label; };
static const SoundMode SOUND_MODES[] = {
    {0x64, "Passive 2000Hz (locked default)"},
    {0x32, "Passive 1000Hz"},
    {0x7D, "Passive 2500Hz"},
    {0xC8, "Passive 4000Hz"},
    {0x00, "Active (do not use)"},
};
#define SOUND_MODE_COUNT (sizeof(SOUND_MODES) / sizeof(SOUND_MODES[0]))
static size_t _sound_mode_idx = 0;

static void act_toggle_sound_mode() {
    _sound_mode_idx = (_sound_mode_idx + 1) % SOUND_MODE_COUNT;
    const SoundMode &m = SOUND_MODES[_sound_mode_idx];
    write_reg(0x00, 0x0A, m.value);

    char row[40];
    snprintf(row, sizeof(row), "Sound Mode: %s", m.label);
    lv_label_set_text(_row_lbls[4], row);

    char log[64];
    snprintf(log, sizeof(log), "Sent reg000A=0x%02X (%s)\nScan something to hear it.", m.value, m.label);
    lv_label_set_text(_log_lbl, log);
}

typedef void (*RowAction)();
static RowAction ROW_ACTIONS[ROW_COUNT] = {
    act_setup_defaults, act_read_settings, act_factory_reset, act_toggle_scan_mode, act_toggle_sound_mode,
};

// ── list plumbing ─────────────────────────────────────────────────────────────────
static void refresh_cursor() {
    if (_prev_cursor >= 0 && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

static void cb_up()    { _cursor = (_cursor - 1 + ROW_COUNT) % ROW_COUNT; refresh_cursor(); }
static void cb_down()  { _cursor = (_cursor + 1) % ROW_COUNT; refresh_cursor(); }
static void cb_enter() { ROW_ACTIONS[_cursor](); }

bool screen_gm65_test_capture(const char *data, size_t len) {
    if (!_scr || lv_scr_act() != _scr) return false;

    char hex[128] = "";
    // 2026-08-26: bumped from 20/64 — the widened Read Settings (0x0000-0x000D, 14 data
    // bytes) replies with a 20-byte frame; this leaves real headroom above that, not a
    // tight fit.
    size_t shown = len < 32 ? len : 32;
    for (size_t i = 0; i < shown; i++) {
        char b[4];
        snprintf(b, sizeof(b), "%02X ", (uint8_t)data[i]);
        strncat(hex, b, sizeof(hex) - strlen(hex) - 1);
    }

    char out[192];
    if (len == 0) {
        snprintf(out, sizeof(out), "Received: (0 bytes)");
    } else {
        snprintf(out, sizeof(out), "Received (%u bytes):\n%s", (unsigned)len, hex);
    }
    lv_label_set_text(_log_lbl, out);
    return true;
}

void screen_gm65_test_push() {
    _cursor = 0;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        // Fixed-height + non-scrollable clipped row 4 at this screen's real font/padding
        // sizes — same class of bug the Admin Menu hit (see snack_cart_pos.md), fixed the
        // same way: make it scrollable and let refresh_cursor()'s scroll-to-view handle
        // any row count rather than hand-tuning a height that breaks the next time a row
        // gets added.
        lv_obj_t *list = lv_obj_create(_scr);
        lv_obj_set_size(list, SCREEN_W, 280);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, HDR_H);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
        lv_obj_set_layout(list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

        for (int i = 0; i < ROW_COUNT; i++) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 14, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            _rows[i] = row;

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, ROW_LABELS[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
            _row_lbls[i] = lbl;
        }

        _log_lbl = lv_label_create(_scr);
        lv_label_set_long_mode(_log_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_log_lbl, 280);
        lv_obj_set_style_text_color(_log_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(_log_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        // Bottom offset leaves room for the legend below it (added 2026-08-28 -- this
        // screen had no on-screen button guidance at all before).
        lv_obj_align(_log_lbl, LV_ALIGN_BOTTOM_MID, 0, -(FOOTER_H + 10));

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 28);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char move_lbl[24];
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Select", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("SCANNER");
    lv_label_set_text(_log_lbl, "No data received yet.");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
