#pragma once
#include <stdint.h>

void backlight_init();              // call once in setup(), after tft.init()
void backlight_set(uint8_t percent); // 0-100
