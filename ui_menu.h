#ifndef HWTEST_UI_MENU_H
#define HWTEST_UI_MENU_H

#include <bdk.h>

typedef enum {
    UI_ACTION_HARDWARE = 1,
    UI_ACTION_RAM      = 2,
    UI_ACTION_CUSTOM   = 3,
} ui_action_t;

typedef struct {
    ui_action_t action;
    u32 probe_mask;
} ui_selection_t;

void ui_launcher(ui_selection_t *selection);
int ui_chainload_ram(void);

#endif
