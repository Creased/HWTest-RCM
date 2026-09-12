#ifndef HWTEST_UI_THEME_H
#define HWTEST_UI_THEME_H

/* Shared LCD menu theme for the normal launcher and the RAM service payload.
 * Coordinates are in the 1280x720 logical landscape canvas used by gfx.c. */
#define COL_HEADER  0xFFFF8000u  /* orange */
#define COL_DEFAULT 0xFFCCCCCCu  /* light grey */
#define COL_OK      0xFF96FF00u  /* green */
#define COL_WARN    0xFFFFDD00u  /* yellow */
#define COL_ERR     0xFFFF5050u  /* red */
#define COL_INFO    0xFF00DDFFu  /* cyan */
#define COL_BG      0xFF1B1B1Bu

#define UI_TITLE_X          64
#define UI_HEADER_Y         48
#define UI_CAPTION_Y        80
#define UI_FOOTER_Y         656

#define UI_MAIN_ROW_X       64
#define UI_MAIN_ROW_Y       144
#define UI_MAIN_ROW_W       768
#define UI_MAIN_ROW_H       32
#define UI_MAIN_ROW_STEP    48
#define UI_MAIN_HELP_Y      336

#define UI_CUSTOM_ROW_X     64
#define UI_CUSTOM_ROW_Y     120
#define UI_CUSTOM_ROW_W     896
#define UI_CUSTOM_ROW_H     32
#define UI_CUSTOM_ROW_STEP  36
#define UI_CUSTOM_SIDE_X    1000

#endif
