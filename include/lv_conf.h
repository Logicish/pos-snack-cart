#if 1  /* required by LVGL — do not remove */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR
 *====================*/
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP    0  /* TFT_eSPI pushColors(swap=true) handles byte order */

/*====================
   MEMORY — PSRAM
 *====================*/
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE            <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size)        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE(ptr)          heap_caps_free(ptr)
#define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/*====================
   TICK — Arduino millis()
 *====================*/
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE      "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/*====================
   HAL
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD      20   /* ms — ~50 fps target */
#define LV_INDEV_DEF_READ_PERIOD     10   /* ms */
#define LV_INDEV_DEF_LONG_PRESS_TIME 400  /* ms before auto-repeat starts */
#define LV_INDEV_DEF_LONG_PRESS_REP_TIME 80 /* ms between repeat events */

/*====================
   FEATURES
 *====================*/
#define LV_USE_LOG          0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_OBJ           0
#define LV_USE_ASSERT_MEM_INTEGRITY 0

/* Widgets */
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1  /* required by LV_USE_QRCODE below (PAYMENT screen) */
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_SPAN       0
#define LV_USE_MSGBOX     1
#define LV_USE_OBJMASK    0
#define LV_USE_METER      0
#define LV_USE_ANALOGCLOCK 0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     1
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_MENU       1
#define LV_USE_PAGINATOR  0

/* 3rd party libs */
#define LV_USE_QRCODE     1  /* PAYMENT screen — Venmo deep link */

/* Layouts */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* Themes */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1        /* start dark */
#define LV_THEME_DEFAULT_GROW 1
#define LV_USE_THEME_BASIC   0
#define LV_USE_THEME_MONO    0

/*====================
   FONTS — Montserrat
 *====================*/
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_FONT_UNSCII_8   0
#define LV_FONT_UNSCII_16  0

#define LV_USE_FONT_SUBPX       0
#define LV_USE_FONT_COMPRESSED  0

/*====================
   TEXT
 *====================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8

/*====================
   MISC
 *====================*/
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DITHER_GRADIENT 0
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0
#define LV_USE_LARGE_COORD 0

#define LV_SPRINTF_CUSTOM 0
#define LV_USE_BUILTIN_SNPRINTF 1

#define LV_USE_USER_DATA 1

#define LV_USE_ANIMIMG 1

#endif /* LV_CONF_H */
#endif /* "Content enable" */
