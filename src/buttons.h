#pragma once

// 6-button physical input: Up/Down/Left/Right/Enter/Back.
// Direct dispatch, not LVGL lv_group/keypad focus — each screen registers
// plain callbacks for whatever the 6 buttons mean in its current state.

typedef void (*ButtonCb)();

struct ButtonHandlers {
    ButtonCb up    = nullptr;
    ButtonCb down  = nullptr;
    ButtonCb left  = nullptr;
    ButtonCb right = nullptr;
    ButtonCb enter = nullptr;
    ButtonCb back  = nullptr;
};

void buttons_init();
void buttons_poll();  // call every loop() iteration
void buttons_set_handlers(const ButtonHandlers &h);
