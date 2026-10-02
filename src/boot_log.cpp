#include "boot_log.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_system.h>
#include <stdarg.h>
#include <time.h>

/*
  Author--- LogicishDesigns
  Date----- October 2026
  Function- Implements the boot log declared in boot_log.h.
*/

#define BOOT_LOG_OLD_PATH  "/boot_log_old.csv"
#define BOOT_LOG_MAX_BYTES (512UL * 1024UL)  // ~300 bytes a boot -- about 1700 boots per file

static char _details[768];
static bool _written;

// Adds one key=value pair to this boot's details, separated by ';'.
void boot_log_note(const char *key, const char *fmt, ...) {
    if (_written) return;
    size_t len = strlen(_details);
    if (len + 2 >= sizeof(_details)) return;

    int n = snprintf(_details + len, sizeof(_details) - len, "%s%s=", len ? ";" : "", key);
    if (n < 0) return;
    len = strlen(_details);

    va_list args;
    va_start(args, fmt);
    vsnprintf(_details + len, sizeof(_details) - len, fmt, args);
    va_end(args);
}

// Maps esp_reset_reason() to a short, spreadsheet-friendly name.
const char *boot_log_reset_reason() {
    switch (esp_reset_reason()) {
        case ESP_RST_UNKNOWN:  return "UNKNOWN";    // what a USB reflash reports on this board
        case ESP_RST_POWERON:  return "POWER_ON";
        case ESP_RST_EXT:      return "RESET_PIN";
        case ESP_RST_SW:       return "SOFTWARE";   // esp_restart(), and most reflashes
        case ESP_RST_PANIC:    return "CRASH";
        case ESP_RST_INT_WDT:  return "WATCHDOG_INT";
        case ESP_RST_TASK_WDT: return "WATCHDOG_TASK";
        case ESP_RST_WDT:      return "WATCHDOG";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_DEEPSLEEP:return "DEEP_SLEEP";
        default: {
            // Anything newer than this list (USB/JTAG resets on the S3 -- what a reflash
            // over the USB cable reports) -- keep the raw code so it can be looked up.
            static char other[16];
            snprintf(other, sizeof(other), "OTHER_%d", (int)esp_reset_reason());
            return other;
        }
    }
}

// Brownout, crash, or any watchdog -- the restarts nobody asked for.
bool boot_log_unexpected_restart() {
    switch (esp_reset_reason()) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

// Appends this boot's line, starting a fresh file (with header) when needed.
void boot_log_write() {
    if (_written) return;
    _written = true;
    if (SD.cardType() == CARD_NONE) return;

    // Rotate a full log -- keep exactly one older file.
    if (SD.exists(BOOT_LOG_PATH)) {
        File f = SD.open(BOOT_LOG_PATH, FILE_READ);
        size_t size = f ? f.size() : 0;
        if (f) f.close();
        if (size > BOOT_LOG_MAX_BYTES) {
            SD.remove(BOOT_LOG_OLD_PATH);
            SD.rename(BOOT_LOG_PATH, BOOT_LOG_OLD_PATH);
        }
    }

    bool fresh = !SD.exists(BOOT_LOG_PATH);
    File f = SD.open(BOOT_LOG_PATH, FILE_APPEND);
    if (!f) {
        Serial.println("[BOOTLOG] couldn't open " BOOT_LOG_PATH);
        return;
    }
    if (fresh) f.print("time,restart_reason,details\n");

    // Local wall-clock time stored as UTC -- same convention as every other timestamp here.
    time_t now = time(nullptr);
    struct tm tmval;
    gmtime_r(&now, &tmval);
    char when[20];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmval);

    // Details may contain commas from a note's value -- quote the field so the CSV stays
    // three columns regardless.
    f.printf("%s,%s,\"%s\"\n", when, boot_log_reset_reason(), _details);
    f.close();
    Serial.printf("[BOOTLOG] %s,%s,%s\n", when, boot_log_reset_reason(), _details);
}
