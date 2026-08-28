#include "backlight.h"
#include <Arduino.h>

#define BL_PIN      18
#define BL_CHANNEL  0
#define BL_FREQ_HZ  5000
#define BL_RES_BITS 8

void backlight_init() {
    ledcSetup(BL_CHANNEL, BL_FREQ_HZ, BL_RES_BITS);
    ledcAttachPin(BL_PIN, BL_CHANNEL);
    backlight_set(100);
}

void backlight_set(uint8_t percent) {
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledcWrite(BL_CHANNEL, duty);
}
