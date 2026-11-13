/*
 * hwtest GUI - Lockpick_RCM, from Home.
 *
 * A shortcut to Lockpick_RCM, which saves the console's keys to the SD card
 * as switch/prod.keys, the file the health check takes the eMMC's BIS key
 * from (probe_storage.c). Its tile is greyed out when it is not on the card,
 * looked for where hekate keeps payloads, then at the card's root, under the
 * names the packs give it: Lockpick_RCM.bin (deejay87's Universal Pack, the
 * releases) and Lockpick_RCM_Pro.bin (HATS); FAT matches them in any case.
 * It starts as the RAM test does: read off the card, the debug USB
 * stopped, the clocks back at boot speed, then staged and run at 0x40010000
 * as RCM runs a payload (ram_payload_run). Coming back to hwtest takes a
 * restart.
 */
#include "gui.h"
#include "../ui_menu.h"

static const char *const s_lp_paths[] = {
    "bootloader/payloads/Lockpick_RCM.bin",
    "bootloader/payloads/Lockpick_RCM_Pro.bin",
    "Lockpick_RCM.bin",
    "Lockpick_RCM_Pro.bin",
};

bool gui_lockpick_found(void)
{
    if (!g_sd_ok || sd_mount())
        return false;
    FILINFO fi;
    for (u32 i = 0; i < ARRAY_SIZE(s_lp_paths); i++)
        if (f_stat(s_lp_paths[i], &fi) == FR_OK && !(fi.fattrib & AM_DIR))
            return true;
    return false;
}

static lv_res_t _lp_close(lv_obj_t *btns, const char *txt)
{
    gui_mbox_close(btns);
    return LV_RES_INV;
}

static lv_res_t _lp_action(lv_obj_t *btns, const char *txt)
{
    if (!strstr(txt, "Start"))
        return _lp_close(btns, txt);

    const char *used = NULL;
    u32 size = 0;
    int err;
    void *buf = payload_load(s_lp_paths, ARRAY_SIZE(s_lp_paths), &used, &size, &err);
    if (buf) {
        char line[96];
        s_printf(line, "gui: starting %s, %d bytes\n", used, size);
        gui_uart_puts(line);
        gui_dbg_end();                /* no USB DMA into what it loads over */
        gui_clock_fast(false);        /* it starts from the boot clock */
        ram_payload_run(buf, size);   /* does not return */
    }
    /* The loader powers the card down when it fails; bring it back. */
    g_sd_ok = sd_initialize(false) == 0;
    gui_mbox_close(btns);
    static const char *ok[] = { "\221OK", "" };
    gui_mbox(err == 1 ? "#FF3C28 The SD card could not be read.#" :
             err == 2 ? "#FF3C28 Lockpick_RCM is not on the SD card any more.#" :
                        "#FF3C28 Lockpick_RCM on the SD card is too large or too "
                        "small to run.#",
             ok, _lp_close);
    return LV_RES_INV;
}

void gui_lockpick(void)
{
    static const char *map[] = { "\221Start", "\221Cancel", "" };
    gui_mbox("#FF8000 Start Lockpick_RCM?#\n\n"
             "hwtest closes and Lockpick_RCM takes over. It saves the "
             "console's keys to switch/prod.keys on the SD card, where the "
             "health check reads them. Restart the console to come back.",
             map, _lp_action);
}
