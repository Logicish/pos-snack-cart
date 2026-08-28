#pragma once
#include <stddef.h>

// TEMPORARY diagnostic screen — see screen_gm65_test.cpp header comment.
void screen_gm65_test_push();

// Called from main.cpp's loop() for every raw line read off the scanner UART, BEFORE
// the normal badge/UPC on_scan() dispatch. Returns true (and shows a hex dump on screen)
// only when this screen is the one currently active — otherwise the caller should fall
// through to its usual handling. Exists so a GM65 command's own reply/ACK doesn't get
// misrouted into on_scan() and bounce the tester out to "Badge not recognized."
bool screen_gm65_test_capture(const char *data, size_t len);
