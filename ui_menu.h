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

void ui_boot_gui(void);
void ui_launcher(ui_selection_t *selection);
int ui_chainload_ram(void);
void *payload_load(const char *const *paths, u32 n, const char **used, u32 *size,
                   int *err);
void *ram_payload_load(const char **used, u32 *size, int *err);
void ram_payload_run(void *buf, u32 size);

#endif
