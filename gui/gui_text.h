/*
 * hwtest GUI - plain-language text for the checks and result categories.
 */
#ifndef HWTEST_GUI_TEXT_H
#define HWTEST_GUI_TEXT_H

#include "../hwtest.h"

typedef struct {
    const char *key;     /* dx_set key                                   */
    const char *name;    /* what was checked, in plain words             */
    const char *warn;    /* a WARN in plain words (NULL: never warns)    */
    const char *fail;    /* a FAIL in plain words (NULL: never fails)    */
    const char *soft;    /* a WARN whose reason starts with this is only */
                         /* "not checked", not something to worry about */
} gui_check_text_t;

const gui_check_text_t *gui_check_text(const char *key);

/* A reading shown on a category's screen, taken from the report line
 * "  <label> : <value>" of a captured page - inside the block that starts
 * with "[<section>" when a section is given. */
enum {
    FACT_VALUE,   /* the value as printed                     */
    FACT_PAREN,   /* only what the value has in parentheses   */
    FACT_CUT,     /* the value up to its first " ("           */
};

typedef struct {
    const char *name;      /* what the owner reads                  */
    const char *page;
    const char *section;
    const char *label;
    u8 how;
} gui_fact_t;

typedef struct {
    const char *title;
    const char *icon;
    u32 colour;            /* icon colour, 0xRRGGBB                 */
    const char *rows[2];   /* verdict rows (g_dx_macros) it covers  */
    const char *pages[3];  /* report pages behind its details       */
    const char *ok;        /* shown when every check passed         */
    const gui_fact_t *facts; /* its readings, ended by a NULL name  */
} gui_cat_t;

extern const gui_cat_t g_gui_cats[];
extern const u32 g_gui_cat_n;

#endif
