#include "hwtest.h"
#include "ui_menu.h"

#define RAM_STAGE_ADDR      0xC0000000u
#define RAM_ENTRY_ADDR      0x40010000u
#define CHAIN_RELOC_ADDR    0x4003FF00u
#define RAM_MAX_PAYLOAD_SIZE (CHAIN_RELOC_ADDR - RAM_ENTRY_ADDR)

extern const u8 chain_reloc_start[];
extern const u8 chain_reloc_end[];

typedef void (*chain_reloc_fn_t)(u32 src, u32 dst, u32 size, u32 entry);

/* ui_menu uses the same logical landscape canvas as gfx/gfx.c: 1280x720
 * rotated 90 degrees into the Switch's physical 720x1280 framebuffer.
 * Hekate's public gfx_line() is pitch-linear and therefore cannot be used
 * here directly. The launcher is intentionally physical-key-only. */
static void _ui_pixel(int x, int y, u32 color)
{
    if ((u32)x >= 1280u || (u32)y >= 720u)
        return;
    gfx_ctxt.fb[(u32)y + (1279u - (u32)x) * 720u] = color;
}

static void _fill_rect(int x, int y, int w, int h, u32 color)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            _ui_pixel(xx, yy, color);
}

static void _title(const char *caption)
{
    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_HEADER, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_HEADER_Y);
    gfx_printf("[hwtest]");
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_CAPTION_Y);
    gfx_printf("%s", caption);
}

static void _footer(const char *action)
{
    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_FOOTER_Y);
    gfx_printf("VOL-: Up   VOL+: Down   PWR: %s", action);
}

static void _menu_row(int x, int y, int w, int h, bool selected, const char *text)
{
    _fill_rect(x, y, w, h, selected ? COL_DEFAULT : COL_BG);
    gfx_con_setcol(selected ? COL_BG : COL_DEFAULT, 1,
                   selected ? COL_DEFAULT : COL_BG);
    gfx_con.fntsz = 16;
    gfx_con_setpos(x + 16, y + 8);
    gfx_printf("%s", text);
}

static void _draw_launcher(int selected)
{
    static const char *items[] = {
        "Hardware test",
        "RAM test",
        "Custom test",
        "Power off",
    };
    static const char *help[] = {
        "Run the complete hardware diagnostic sweep.",
        "Run memory training and the RAM-content test.",
        "Choose which diagnostic groups should run.",
        "Shut down the console cleanly.",
    };

    gfx_clear_grey(0x1B);
    _title("Switch RCM hardware diagnostics");

    for (int i = 0; i < 4; i++)
        _menu_row(UI_MAIN_ROW_X, UI_MAIN_ROW_Y + i * UI_MAIN_ROW_STEP,
                  UI_MAIN_ROW_W, UI_MAIN_ROW_H, i == selected, items[i]);

    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_INFO, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_MAIN_HELP_Y);
    gfx_printf("%s", help[selected]);
    _footer("Select option");
}

struct custom_item {
    const char *name;
    u32 bit;
};

static const struct custom_item _custom_items[] = {
    { "Identity / fuses",       PROBE_G_IDENTITY },
    { "Power / thermal",       PROBE_G_POWER },
    { "Memory / clocks",       PROBE_G_MEMORY },
    { "Storage / gamecard",    PROBE_G_STORAGE },
    { "Wireless",              PROBE_G_WIRELESS },
    { "Audio hardware",        PROBE_G_AUDIO },
    { "Sound output",          PROBE_G_SOUND },
    { "Display",               PROBE_G_DISPLAY },
    { "Touch / inputs",        PROBE_G_INPUTS },
    { "Raw state",             PROBE_G_RAW },
};
#define CUSTOM_N ((int)(sizeof(_custom_items) / sizeof(_custom_items[0])))

static void _draw_custom_row(int idx, int selected, u32 mask)
{
    char label[64];
    bool on = (mask & _custom_items[idx].bit) != 0;
    s_printf(label, "[%c] %s", on ? 'x' : ' ', _custom_items[idx].name);
    _menu_row(UI_CUSTOM_ROW_X, UI_CUSTOM_ROW_Y + idx * UI_CUSTOM_ROW_STEP,
              UI_CUSTOM_ROW_W, UI_CUSTOM_ROW_H, idx == selected, label);
}

static void _draw_custom(int selected, u32 mask)
{
    gfx_clear_grey(0x1B);
    _title("Custom hardware test");

    for (int i = 0; i < CUSTOM_N; i++)
        _draw_custom_row(i, selected, mask);

    _menu_row(UI_CUSTOM_ROW_X, UI_CUSTOM_ROW_Y + CUSTOM_N * UI_CUSTOM_ROW_STEP + 12,
              UI_CUSTOM_ROW_W, UI_CUSTOM_ROW_H, selected == CUSTOM_N,
              "Run selected tests");
    _menu_row(UI_CUSTOM_ROW_X, UI_CUSTOM_ROW_Y + (CUSTOM_N + 1) * UI_CUSTOM_ROW_STEP + 12,
              UI_CUSTOM_ROW_W, UI_CUSTOM_ROW_H, selected == CUSTOM_N + 1, "Back");

    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_INFO, 1, COL_BG);
    gfx_con_setpos(UI_CUSTOM_SIDE_X, 128);
    gfx_printf("POWER toggles");
    gfx_con_setpos(UI_CUSTOM_SIDE_X, 152);
    gfx_printf("selected group");
    _footer("Toggle / select");
}

static int _custom_menu(u32 *mask)
{
    int selected = 0;
    _draw_custom(selected, *mask);

    for (;;) {
        u8 btn = btn_read();
        int activated = -1;

        /* VOL+ moves forward (down the list), as it turns the page in the
         * pager; VOL- goes back. */
        if (btn & BTN_VOL_DOWN) {
            selected = (selected + CUSTOM_N + 1) % (CUSTOM_N + 2);
        } else if (btn & BTN_VOL_UP) {
            selected = (selected + 1) % (CUSTOM_N + 2);
        } else if (btn & BTN_POWER) {
            activated = selected;
        }

        if (activated >= 0) {
            if (activated < CUSTOM_N) {
                u32 bit = _custom_items[activated].bit;
                *mask ^= bit;
                if (bit == PROBE_G_AUDIO && !(*mask & PROBE_G_AUDIO))
                    *mask &= ~PROBE_G_SOUND;
                if (bit == PROBE_G_SOUND && (*mask & PROBE_G_SOUND))
                    *mask |= PROBE_G_AUDIO;
            } else if (activated == CUSTOM_N) {
                if (*mask)
                    return 1;
            } else {
                return 0;
            }
        }

        if (btn || activated >= 0) {
            _draw_custom(selected, *mask);
            if (btn)
                while (btn_read() == btn)
                    msleep(15);
        } else {
            msleep(15);
        }
    }
}

void ui_launcher(ui_selection_t *selection)
{
    int selected = 0;
    u32 custom_mask = PROBE_G_ALL;

    for (;;) {
        _draw_launcher(selected);

        for (;;) {
            u8 btn = btn_read();
            int activated = -1;

            if (btn & BTN_VOL_DOWN) {           /* VOL+ forward, as above */
                selected = (selected + 3) % 4;
            } else if (btn & BTN_VOL_UP) {
                selected = (selected + 1) % 4;
            } else if (btn & BTN_POWER) {
                activated = selected;
            }

            if (activated == 0) {
                selection->action = UI_ACTION_HARDWARE;
                selection->probe_mask = PROBE_G_ALL;
                return;
            }
            if (activated == 1) {
                selection->action = UI_ACTION_RAM;
                selection->probe_mask = 0;
                return;
            }
            if (activated == 2) {
                if (_custom_menu(&custom_mask)) {
                    selection->action = UI_ACTION_CUSTOM;
                    selection->probe_mask = custom_mask;
                    return;
                }
                _draw_launcher(selected);
            }
            if (activated == 3) {
                while (btn_read() & BTN_POWER)
                    msleep(20);
                gfx_clear_grey(0x1B);
                _title("Power off");
                gfx_con_setcol(COL_DEFAULT, 0, COL_BG);
                gfx_con_setpos(70, 180);
                gfx_printf("Powering off...");
                i2c_send_byte(I2C_5, MAX77620_I2C_ADDR,
                              MAX77620_REG_ONOFFCNFG1, MAX77620_ONOFFCNFG1_PWR_OFF);
                while (1)
                    ;
            }

            if (btn) {
                _draw_launcher(selected);
                if (btn)
                    while (btn_read() == btn)
                        msleep(15);
            } else {
                msleep(15);
            }
        }
    }
}

/* Hand over to the GUI stage when the SD card carries one.
 *
 * The GUI (gui/, built by `make gui`) is far too large for an RCM payload,
 * so it lives on the SD card and runs from DRAM, the way hekate hands over
 * to Nyx. Its header (gui/start.S) carries a magic, its size and its load
 * address; anything that does not match is ignored and the text launcher
 * runs as before, as it does with no SD card at all. Holding VOL- while
 * the payload starts skips the GUI on purpose. Does not return when the
 * GUI starts. */
#define GUI_MAGIC 0x55475748u   /* "HWGU" */

void ui_boot_gui(void)
{
    u32 size = 0;
    if (btn_read() & BTN_VOL_DOWN) {
        /* Let go first, or the launcher takes the held key as a step up
         * and opens on "Power off". */
        while (btn_read() & BTN_VOL_DOWN)
            msleep(20);
        return;
    }
    /* Card detect first, set up as bdk's SDMMC1 bring-up does: with no card,
     * sd_mount() would print its errors over the splash before the launcher
     * replaces it. */
    PINMUX_AUX(PINMUX_AUX_GPIO_PZ1) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP | 2;
    gpio_direction_input(GPIO_PORT_Z, GPIO_PIN_1);
    usleep(100);
    if (!sdmmc_get_sd_inserted() || sd_mount())
        return;
    u32 *img = sd_file_read("hwtest/gui.bin", &size);
    sd_end();
    if (!img)
        return;
    u32 dst = img[3];
    if (size < 16 || img[1] != GUI_MAGIC || dst != NYX_LOAD_ADDR ||
        img[2] - dst > size) {
        free(img);
        return;
    }
    memcpy((void *)dst, img, size);
    bpmp_mmu_disable();
    ((void (*)(void))dst)();
}

/* The RAM test's payload, read off the SD card. NULL when it cannot be, with
 * *err saying why: 1 no SD card, 2 no file, 3 a file of the wrong size. The
 * SD card is unmounted again on every failure. */
void *ram_payload_load(const char **used, u32 *size, int *err)
{
    static const char *paths[] = {
        "bootloader/payloads/hwtest_ram.bin",
        "hwtest_ram.bin",
        "/hwtest_ram.bin",
    };
    void *buf = NULL;

    *err = 1;
    if (sd_mount())
        return NULL;

    for (u32 i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        buf = sd_file_read(paths[i], size);
        if (buf) {
            *used = paths[i];
            break;
        }
    }
    *err = 2;
    if (buf && (*size < 0x100u || ALIGN(*size, 0x10) > RAM_MAX_PAYLOAD_SIZE)) {
        free(buf);
        buf = NULL;
        *err = 3;
    }
    if (!buf)
        sd_end();
    return buf;
}

/* Run a payload ram_payload_load() returned. Does not return. */
void ram_payload_run(void *buf, u32 size)
{
    /* Match the safe ordering used by Hekate payload launching: shut down SD
     * before touching the fixed DRAM staging area. Keep the file buffer alive
     * until after staging; this path never returns on success. */
    sd_end();

    u32 staged_size = ALIGN(size, 0x10);
    memset((void *)RAM_STAGE_ADDR, 0, staged_size);
    memcpy((void *)RAM_STAGE_ADDR, buf, size);

    /* Execute the handoff from upper IRAM, not from DRAM. The tiny relocator
     * copies the staged RAM payload to the same 0x40010000 address used by RCM
     * injection, then jumps there. The RAM payload's own loader relocates to
     * 0x40007000 and decompresses the diagnostic as usual. */
    u32 reloc_size = (u32)(chain_reloc_end - chain_reloc_start);
    memcpy((void *)CHAIN_RELOC_ADDR, chain_reloc_start, reloc_size);

    bpmp_mmu_disable();
    ((chain_reloc_fn_t)CHAIN_RELOC_ADDR)(RAM_STAGE_ADDR, RAM_ENTRY_ADDR,
                                         staged_size, RAM_ENTRY_ADDR);
    while (1)
        ;
}

int ui_chainload_ram(void)
{
    u32 size = 0;
    const char *used = NULL;
    int err;

    gfx_clear_grey(0x1B);
    _title("RAM test launcher");
    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_DEFAULT, 0, COL_BG);
    gfx_con_setpos(70, 180);
    gfx_printf("Loading dedicated RAM diagnostic from SD...");

    void *buf = ram_payload_load(&used, &size, &err);
    if (buf) {
        gfx_con_setcol(COL_OK, 0, COL_BG);
        gfx_con_setpos(70, 230);
        gfx_printf("Loaded %s (%d bytes)", used, size);
        gfx_con_setcol(COL_INFO, 1, COL_BG);
        gfx_con_setpos(70, 275);
        gfx_printf("Starting RAM test...");
        msleep(350);
        ram_payload_run(buf, size);
    }

    gfx_con_setcol(COL_ERR, 0, COL_BG);
    gfx_con_setpos(70, 240);
    if (err == 1) {
        gfx_printf("SD card could not be mounted.");
    } else if (err == 2) {
        gfx_printf("RAM payload not found.");
        gfx_con_setcol(COL_DEFAULT, 0, COL_BG);
        gfx_con_setpos(70, 280);
        gfx_printf("Copy hwtest_ram.bin to SD root or bootloader/payloads/.");
    } else {
        gfx_printf("RAM payload has invalid size: %d bytes", size);
    }
    gfx_con_setcol(COL_DEFAULT, 0, COL_BG);
    gfx_con_setpos(70, 360);
    gfx_printf("Press POWER to return to the hwtest menu.");
    while (!(btn_read() & BTN_POWER))
        msleep(20);
    while (btn_read() & BTN_POWER)
        msleep(20);
    return 1;
}
