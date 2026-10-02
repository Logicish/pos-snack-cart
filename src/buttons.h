#pragma once

#include <stddef.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- 6-button physical input driver: Up/Down/Left/Right/Enter/Back. Direct
            dispatch, not LVGL lv_group/keypad focus -- each screen registers plain
            callbacks for whatever the 6 buttons mean in its current state.
*/

typedef void (*ButtonCb)();

struct ButtonHandlers {
    ButtonCb up    = nullptr;
    ButtonCb down  = nullptr;
    ButtonCb left  = nullptr;
    ButtonCb right = nullptr;
    ButtonCb enter = nullptr;
    ButtonCb back  = nullptr;
    // Whether THIS screen wants the GM65 actively sensing (light on, watching for a
    // scan) -- defaults false. buttons_set_handlers() only sends the actual GM65
    // mode-switch command when this differs from whatever the previous screen wanted,
    // so hopping between two screens that both want it off (or both want it on) costs
    // nothing extra. See buttons.cpp for the toggle + ACK-drain mechanics.
    bool wantsScanner = false;
    // 2026-09-30 -- games only (Laggy Fish). A pin interrupt latches every press, so a
    // quick tap that starts and ends between two buttons_poll() calls (easy while a frame
    // is rendering) still dispatches. Skips the 20ms debounce for that press; off by
    // default so every other screen keeps the proven input path unchanged.
    bool fastTaps = false;
};

void buttons_init();  // call once in setup() -- sets up the 6 GPIO pins
void buttons_poll();  // call every loop() iteration -- debounces + dispatches presses

// Boot stuck-button check, 2026-10-02. Call once at the end of setup(). Any button
// reading as held down is shorted or jammed -- left alone it would auto-repeat forever
// -- so it is ignored until it reads released (it then works normally again). Fills
// `names` with the stuck ones, e.g. "Up+Enter" (or "" if none); returns how many.
int buttons_check_stuck(char *names, size_t names_len);

void buttons_set_handlers(const ButtonHandlers &h);  // call from every screen's build/rebuild

// Call once from setup(), right after scanner.begin() and the boot-time forced GM65
// config write -- reconciles the scanner's real hardware state with whatever screen is
// currently active (buttons_set_handlers() may already have run once or more before
// this, while the scanner UART wasn't even initialized yet, e.g. for the splash screen).
void buttons_scanner_ready();
