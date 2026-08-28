#define USER_SETUP_LOADED

#define ILI9488_DRIVER

// --- SPI pins (HSPI / SPI2, shared with SD) ---
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_MISO 13  // SD only — ILI9488 SDO left unconnected

// --- Display control pins ---
#define TFT_CS    8
#define TFT_DC   10
#define TFT_RST   9
#define TFT_BL   18
#define TFT_BACKLIGHT_ON HIGH

// --- Fonts ---
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// --- SPI speeds ---
// Bumped to 40MHz 2026-08-17 to test on breadboard jumper wires (was 27MHz, conservative
// default). Revert to 27MHz if draws get glitchy/flaky before this is on a PCB.
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
