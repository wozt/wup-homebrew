/* LVGL's configuration for the GamePad (854x480 RGB565, ARM926 without FPU, 4 MB of RAM): what the menu and apps
 * get from <lvgl.h>. tools/lvgl.py builds the library from this. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
/* memory and text from the C library (picolibc): one heap for everything */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130                               /* 6.2 inches, 854 pixels wide: 158; a little denser reads better */
#define LV_USE_OS LV_OS_NONE
#define LV_DRAW_BUF_ALIGN 4

/* software drawing straight into the screen's framebuffer */
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565   1
#define LV_DRAW_SW_SUPPORT_RGB565A8 1
#define LV_DRAW_SW_SUPPORT_RGB888   0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 1                /* icons and PNGs with transparency */
#define LV_DRAW_SW_SUPPORT_L8   0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_A8   1
#define LV_DRAW_SW_SUPPORT_I1   0
#define LV_DRAW_SW_COMPLEX 1                         /* rounded corners, shadows, gradients */
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
#define LV_GRADIENT_MAX_STOPS 2
#define LV_CACHE_DEF_SIZE (64 * 1024)                /* decoded images (app icons) */

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL   0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE  0

/* fonts: Inter with Font Awesome symbols (fonts, tools/mkfont_lv.py) */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(inter_16) LV_FONT_DECLARE(inter_20) LV_FONT_DECLARE(inter_24) LV_FONT_DECLARE(inter_32) LV_FONT_DECLARE(inter_hud)
#define LV_FONT_DEFAULT &inter_20
#define LV_USE_FONT_PLACEHOLDER 0

/* widgets */
#define LV_WIDGETS_HAS_DEFAULT_VALUE 1
#define LV_USE_ANIMIMG     0
#define LV_USE_ARC         1
#define LV_USE_BAR         1
#define LV_USE_BUTTON      1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR    0
#define LV_USE_CANVAS      0
#define LV_USE_CHART       0
#define LV_USE_CHECKBOX    1
#define LV_USE_DROPDOWN    1
#define LV_USE_IMAGE       1
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD    1
#define LV_USE_LABEL       1
#define LV_USE_LED         0
#define LV_USE_LINE        1
#define LV_USE_LIST        1
#define LV_USE_MENU        0
#define LV_USE_MSGBOX      1
#define LV_USE_ROLLER      1
#define LV_USE_SCALE       0
#define LV_USE_SLIDER      1
#define LV_USE_SPAN        0
#define LV_USE_SPINBOX     0
#define LV_USE_SPINNER     1
#define LV_USE_SWITCH      1
#define LV_USE_TEXTAREA    1
#define LV_USE_TABLE       0
#define LV_USE_TABVIEW     0
#define LV_USE_TILEVIEW    0
#define LV_USE_WIN         0

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 0
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO   0
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#define LV_USE_LODEPNG 0                             /* no PNG images: icons are .wup bitmaps, everything else is LV_SYMBOL_* */
#define LV_USE_FS_POSIX 1                            /* "A:/apps/x/icon.png": files through open/read (sys.c, littlefs) */
#define LV_FS_POSIX_LETTER 'A'
#define LV_FS_POSIX_PATH ""
#define LV_FS_POSIX_CACHE_SIZE 0
#define LV_USE_SYSMON   0
#define LV_USE_OBSERVER 1
#define LV_BUILD_EXAMPLES 0
#endif
