/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Scanner screen declared in screen_gm65_test.h --
            Disable QR Config Scan / Settings / Restore Defaults, plus the raw
            hex-dump capture path.
*/

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
// A "Reset -- warning" row (true factory reset, register 0x00D9=0x50) lived here from
// 2026-08-26 until 2026-09-14, when it was removed outright: redundant with Setup
// Defaults for anything this project has actually needed to fix, while carrying real
// destructive risk Setup Defaults doesn't (wiping every register, not just the 3 this
// project cares about, risked knocking the module out of the UART "Series Output" mode
// this whole project depends on — recovery meant physically rescanning the config QR
// sheet). Same reasoning that got Reset DB deleted outright rather than kept "just in
// case" — see project memory/snack_cart_pos.md.
//
// "Read Settings" and "Sound Mode" moved out to their own sub-screen, screen_gm65_settings.cpp
// ("Settings" row below), same day — that screen decodes the raw bytes into labeled fields
// instead of a bare hex dump, and adds a couple of settings (buzzer/LED on-off) this list
// never exposed at all.
#include "screen_gm65_test.h"
#include "screen_gm65_settings.h"
#include "screens.h"
#include "screen_admin_tools.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

extern HardwareSerial scanner;  // owned by main.cpp; this screen only ever writes to it

#define ROW_COUNT 3
#define FOOTER_H  52

static lv_obj_t *_scr;
static lv_obj_t *_rows[ROW_COUNT];
static lv_obj_t *_row_lbls[ROW_COUNT];  // row 0 changes text dynamically
static lv_obj_t *_log_lbl;
static int        _cursor;
static int        _prev_cursor = -1;

// Reordered 2026-09-14: Toggle Scan Mode first (the everyday action), Settings added
// (opens screen_gm65_settings.cpp), Restore Defaults moved to last and renamed from
// "Setup Defaults" -- reads better as the "put it back to known-good" action once it's
// not the first/only thing on the list.
//
// Row 0 changed the same day, later: was "Toggle Scan Mode" (Induction/Manual sensing,
// register 0x0000 bits 0-1) -- retired in favor of the config-QR-disable toggle below,
// since the screensaver already has its own independent, already-proven copy of that
// exact Induction/Manual toggle (see screen_screensaver.cpp's write_reg(0x54)/(0x57)) --
// this row was only ever a manual diagnostic convenience for it, not the real trigger.
static const char *ROW_LABELS[ROW_COUNT] = {
    "Disable QR Config Scan",  // overwritten immediately by update_qr_row_label()
    "Settings",
    "Restore Defaults",
};

// Sends a single-register write command to the GM65.
static void write_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t data) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x08, 0x01, addr_hi, addr_lo, data, 0xAB, 0xCD};
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

// fwd decls -- "list plumbing" section further down, needed by the QR-confirm screen
// below so its Yes/No handlers can restore the list's own button handlers on return.
static void refresh_cursor();
static void cb_up();
static void cb_down();
static void cb_enter();

// ── row actions ───────────────────────────────────────────────────────────────────
// On-screen label is "Restore Defaults" as of 2026-09-14 (moved to the last row too) --
// function name kept as-is, same convention as this screen's own "Scanner"/GM65 Test
// rename (see the file header comment): the label changed, not what it does.
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

// ── config-QR-disable toggle — added 2026-09-14, the project's own flagged pre-launch
// step (see project memory project_gm65_settings_lockin's "remaining backlog"). Register
// 0x0003 bit 0 is what the manual (cross-checked across the GM65/GM65-S/DFR0660 texts,
// same sources as everything else in this file) calls "Settlement Code" -- its own
// translated term for the special setup/config QR codes (the same kind on the physical
// config sheet used to originally provision this module into UART "Series Output" mode):
// 1 = Close (module stops treating a scanned QR as a config command, just reports its
// contents like any other barcode), 0 = Open (normal -- a config QR reconfigures the
// module). 0x03 (not 0x01) is the value already pinned for "disable" in project memory --
// this table's own OCR extraction is garbled on bit 1's label, so trusting the
// already-recorded value rather than re-deriving a guess from a known-bad table read.
//
// Gated behind a confirm screen (see build_qr_confirm_ui() below) since this one has a
// real, easy-to-forget consequence: while disabled, the physical config-QR sheet stops
// working entirely -- the only way back is this same toggle, not a rescan. Persisted via
// save_to_eeprom() like Restore Defaults, so it survives a real power cycle; deliberately
// NOT re-asserted at boot the way registers 0x0000/0x0007/0x000A are, since this is meant
// to be a deliberate, rarely-changed admin choice, not a locked invariant main.cpp should
// silently fight if someone re-enables it on purpose to fix something.
static bool _qr_scan_disabled = false;  // display-only tracking -- resets to "enabled" on
                                          // reboot regardless of the chip's real persisted
                                          // state, since nothing here reads it back

static lv_obj_t *_confirm_scr;
static lv_obj_t *_confirm_prompt_lbl;
static void build_qr_confirm_ui();

// Redraws row 0's label to describe the action Enter will take next.
static void update_qr_row_label() {
    lv_label_set_text(_row_lbls[0], _qr_scan_disabled ? "Enable QR Config Scan" : "Disable QR Config Scan");
}

// Enter on the confirm screen: writes register 0x0003, persists it, flips the tracked
// state, and returns to the Scanner list.
static void cb_qr_confirm_yes() {
    bool disabling = !_qr_scan_disabled;
    write_reg(0x00, 0x03, disabling ? 0x03 : 0x00);
    delay(50);
    save_to_eeprom();  // makes this survive a real power cycle, not just a soft reset
    _qr_scan_disabled = disabling;
    update_qr_row_label();

    lv_label_set_text(_log_lbl, disabling
        ? "Sent reg0003=0x03 (config QR\nscanning disabled), saved to\nflash. (watch for an ACK below)"
        : "Sent reg0003=0x00 (config QR\nscanning re-enabled), saved to\nflash. (watch for an ACK below)");
    header_set_title("SCANNER");
    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);
    refresh_cursor();
    lv_scr_load(_scr);
}

// Back on the confirm screen: cancels, returns to the Scanner list unchanged.
static void cb_qr_confirm_no() {
    header_set_title("SCANNER");
    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);
    refresh_cursor();
    lv_scr_load(_scr);
}

// Enter on row 0: opens the confirm screen instead of toggling directly.
static void act_toggle_qr_scan() {
    if (!_confirm_scr) build_qr_confirm_ui();

    char buf[160];
    if (!_qr_scan_disabled) {
        snprintf(buf, sizeof(buf),
            "Disable QR config scanning?\n\n"
            "The module will stop reacting to\n"
            "setup/config QR codes. The\n"
            "physical config sheet won't work\n"
            "again until this is turned back\n"
            "on from this same screen.");
    } else {
        snprintf(buf, sizeof(buf),
            "Re-enable QR config scanning?\n\n"
            "The module will start reacting\n"
            "to setup/config QR codes again.");
    }
    lv_label_set_text(_confirm_prompt_lbl, buf);

    header_set_title("CONFIRM");

    ButtonHandlers h;
    h.enter = cb_qr_confirm_yes;
    h.back  = cb_qr_confirm_no;
    buttons_set_handlers(h);

    lv_scr_load(_confirm_scr);
}

// Builds the confirm screen's content, once.
static void build_qr_confirm_ui() {
    _confirm_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_confirm_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_confirm_scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *content = lv_obj_create(_confirm_scr);
    lv_obj_set_size(content, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(content, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(content, 16, LV_PART_MAIN);
    // The footer legend is this flex column's last child, so pad_ver's bottom inset was
    // also its distance from the true screen edge -- overridden separately to match the
    // ~6px margin every explicitly-aligned legend elsewhere uses (see
    // screen_item_edit.cpp's identical fix). This screen's own main list uses
    // ui_legend(_scr) instead, which was never affected.
    lv_obj_set_style_pad_bottom(content, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    _confirm_prompt_lbl = lv_label_create(content);
    lv_label_set_long_mode(_confirm_prompt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_confirm_prompt_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_confirm_prompt_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_confirm_prompt_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(content);
    ui_legend_row(legend, "Confirm", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));
}

typedef void (*RowAction)();
static RowAction ROW_ACTIONS[ROW_COUNT] = {
    act_toggle_qr_scan, screen_gm65_settings_push, act_setup_defaults,
};

// ── list plumbing ─────────────────────────────────────────────────────────────────
// Highlights the currently-selected row.
static void refresh_cursor() {
    if (_prev_cursor >= 0 && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

static void cb_up()    { _cursor = (_cursor - 1 + ROW_COUNT) % ROW_COUNT; refresh_cursor(); }  // Up: previous row, wrapping
static void cb_down()  { _cursor = (_cursor + 1) % ROW_COUNT; refresh_cursor(); }               // Down: next row, wrapping
static void cb_enter() { ROW_ACTIONS[_cursor](); }                                              // Enter: runs the selected row's action

// Consumes one raw line off the scanner UART while this screen is active, showing it as
// a hex dump. See screen_gm65_test.h for why this exists.
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

// Loads the Scanner screen.
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
    update_qr_row_label();  // reflects the tracked state, not just the built-in initial label
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
