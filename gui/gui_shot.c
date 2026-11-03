/*
 * hwtest GUI - a screenshot, taken as Nyx takes one.
 *
 * Two fingers on the panel save what is on the screen as a BMP under
 * hwtest/screenshots/ on the SD card (Nyx asks for a third finger and
 * writes under bootloader/screenshots/). The frame is read straight from
 * LVGL's buffer, the landscape one the VIC rotates for the panel, so the
 * file is 1280 x 720 as the screen reads. The backlight blinks for the
 * shutter, and a notice says where the file went.
 */
#include "gui.h"
#include <display/di.h>
#include <mem/heap.h>
#include <rtc/max77620-rtc.h>
#include <soc/bpmp.h>
#include <storage/sd.h>

#define SHOT_HDR      0x36
#define SHOT_SZ       (GUI_W * GUI_H * 4)
#define SHOT_GAP_MS   2000             /* between two                  */
#define SHOT_NOTE_MS  2500             /* the notice stays this long   */

typedef struct {
    u16 magic;
    u32 size, rsvd, data_off, hdr_size;
    u32 width, height;
    u16 planes, pxl_bits;
    u32 comp, img_size, res_h, res_v;
    u64 rsvd2;
} __attribute__((packed)) bmp_t;

static lv_obj_t *s_note;
static lv_task_t *s_note_task;

static void _note_gone(void *param)
{
    if (s_note)
        lv_obj_del(s_note);
    s_note = NULL;
    s_note_task = NULL;
}

/* A line at the top of the screen, over everything, for a moment. */
static void _note(const char *text)
{
    if (s_note)
        lv_obj_del(s_note);
    if (s_note_task)
        lv_task_del(s_note_task);
    s_note = gui_panel(lv_layer_top(), GUI_MARGIN, 12, 100, 52);
    lv_obj_t *l = gui_label(s_note, &gui_font_20, GUI_COL_TEXT, text);
    lv_obj_set_pos(l, 20, 12);
    lv_obj_set_width(s_note, lv_obj_get_width(l) + 40);
    s_note_task = lv_task_create(_note_gone, SHOT_NOTE_MS, LV_TASK_PRIO_LOW, NULL);
    lv_task_once(s_note_task);
}

void gui_screenshot(void)
{
    static u32 last;
    u32 now = get_tmr_ms();
    if (last && now - last < SHOT_GAP_MS)
        return;
    last = now;

    /* What the panel shows, rows bottom-up as a BMP wants them. */
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_INVALID_WAY, false);
    u8 *bmp = malloc(SHOT_HDR + SHOT_SZ);
    if (!bmp) {
        _note(ICON_WARN "  Screenshot not taken  -  out of memory");
        return;
    }
    const u32 *fb = (const u32 *)NYX_FB2_ADDRESS;
    for (u32 y = 0; y < GUI_H; y++)
        memcpy(bmp + SHOT_HDR + (GUI_H - 1 - y) * GUI_W * 4, fb + y * GUI_W, GUI_W * 4);
    bmp_t *h = (bmp_t *)bmp;
    memset(h, 0, sizeof(*h));
    h->magic    = 0x4D42;
    h->size     = SHOT_HDR + SHOT_SZ;
    h->data_off = SHOT_HDR;
    h->hdr_size = 40;
    h->width    = GUI_W;
    h->height   = GUI_H;
    h->planes   = 1;
    h->pxl_bits = 32;
    h->img_size = SHOT_SZ;
    h->res_h    = 2834;
    h->res_v    = 2834;

    /* The shutter. */
    display_backlight_brightness(255, 0);
    msleep(120);
    display_backlight_brightness(GUI_BACKLIGHT, 0);

    char path[80], text[160];
    bool ok = false;
    if (g_sd_ok && !sd_mount()) {
        rtc_time_t t;
        max77620_rtc_get_time_adjusted(&t);
        f_mkdir("hwtest");
        f_mkdir("hwtest/screenshots");
        s_printf(path, "hwtest/screenshots/hwtest_%04d%02d%02d_%02d%02d%02d.bmp",
                 t.year, t.month, t.day, t.hour, t.min, t.sec);
        ok = sd_save_to_file(bmp, SHOT_HDR + SHOT_SZ, path) == 0;
    }
    free(bmp);
    if (ok)
        s_printf(text, ICON_SAVE "  Screenshot saved  -  %s", path);
    else
        strcpy(text, ICON_WARN "  Screenshot not saved  -  no SD card");
    _note(text);
}
