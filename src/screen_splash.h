#pragma once
void screen_splash_push();
// Same pattern as screen_screensaver_is_active() — used by main.cpp's loop() to ignore
// scanner UART traffic while splash is up, so the boot-time GM65 config writes' ACK
// replies don't get misrouted into on_scan() as a bogus scan.
bool screen_splash_is_active();
