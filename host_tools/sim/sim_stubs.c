/*
 * The console, as far as the screens can tell: everything below LVGL that
 * the GUI stage links against, answered here so the screen code itself can
 * run on a PC.
 *
 * Nothing pretends to be hardware. The buttons are never down, the panel
 * never powers on, the Joy-Con is absent; what the harness wants to press
 * it injects through the same path the USB console uses (gui_inject_tap(),
 * gui_inject_key()), which is the path a finger or a button travels. The
 * clock is the harness's own, so a run draws the same frames every time.
 */
#include "../../gui/gui.h"
#include <string.h>

/* The Wi-Fi mailbox, as memory rather than as a block of DRAM: 64 KiB is
 * what the console reserves for it. */
unsigned char sim_mbox_base[0x10000] __attribute__((aligned(8)));

/* ---- time ----
 *
 * Counted in microseconds and moved on by the harness, never by the host's
 * clock: a frame then lands in the same place on every run, and a
 * screenshot can be compared with the last one. */
u32 sim_us;

u32  get_tmr_us(void) { return sim_us; }
u32  get_tmr_ms(void) { return sim_us / 1000; }
u32  get_tmr_s(void)  { return sim_us / 1000000; }
void usleep(u32 us)   { sim_us += us; }
void msleep(u32 ms)   { sim_us += ms * 1000; }

/* ---- input: nothing is touched or pressed ---- */

u8   btn_read(void)     { return 0; }
u8   btn_read_vol(void) { return 0; }
int  touch_power_on_safe(void) { return 1; }    /* no panel: s_touch_ok off */
int  touch_poll(touch_event_t *e) { memset(e, 0, sizeof(*e)); return 0; }
void jc_init_hw(void) { }
jc_gamepad_rpt_t *joycon_poll(void) { return NULL; }
bool gui_jc_colour_read(int side, u8 *rgb) { (void)side; (void)rgb; return false; }
void gui_jc_attach_show(int side, const u8 *body) { (void)side; (void)body; }

/* ---- the SD card ----
 *
 * One file, hwtest/hwtest.ini, and the harness says what is in it: the
 * Wi-Fi screen reads saved passphrases out of it (dbgcfg.c), and that is
 * the only read this side makes. */
const char *sim_ini;

void *sd_file_read(const char *path, u32 *fsize)
{
    (void)path;
    if (!sim_ini)
        return NULL;
    u32 n = (u32)strlen(sim_ini);
    char *buf = malloc(n + 1);
    memcpy(buf, sim_ini, n + 1);
    *fsize = n;
    return buf;
}

/* ---- the SoC and the board ---- */

u32  fuse_read_hw_type(void) { return 0; }      /* an Erista, not a Lite */
void panel_id_set(u32 raw) { (void)raw; }
void panel_id_lost(void) { }
void bpmp_mmu_enable(void) { }
void bpmp_mmu_maintenance(u32 op, bool unused) { (void)op; (void)unused; }
void bpmp_clk_rate_set(bpmp_freq_t fid) { (void)fid; }
void heap_init(void *base) { (void)base; }      /* malloc is the host's */
void probe_reset_snapshot(void) { }
void gui_hw_prepare(void) { }
void gui_clock_init(void) { }
void gui_clock_fast(bool fast) { (void)fast; }
void wifi_cpu0_stop(void) { }

/* How many times the WLAN section has been put back into reset. There are
 * no pins here to look at, so the count is what the screens are held to. */
u32 sim_chip_stops;
void wifi_chip_stop(void) { sim_chip_stops++; }
serial_res_t console_serial(char out[25]) { strcpy(out, "SIM0000000000"); return 0; }
int  bq24193_get_property(enum BQ24193_reg_prop prop, int *value)
{
    (void)prop;
    *value = 0;
    return 0;
}
int  max17050_get_property(enum MAX17050_reg reg, int *value)
{
    (void)reg;
    *value = 0;
    return 0;
}

/* dbg_uart_up()'s world. The console's settings are read here; moving the
 * console to another UART is not something a picture can show. */
void pinmux_config_uart(u32 idx) { (void)idx; }
void clock_enable_uart(u32 idx) { (void)idx; }
void regulator_5v_enable(u8 dev) { (void)dev; }
void gpio_direction_output(u32 port, u32 pins, int high)
{ (void)port; (void)pins; (void)high; }
void gpio_config(u32 port, u32 pins, int mode)
{ (void)port; (void)pins; (void)mode; }
void uart_init(u32 idx, u32 baud, u32 mode) { (void)idx; (void)baud; (void)mode; }
void uart_invert(u32 idx, bool enable, u32 mask)
{ (void)idx; (void)enable; (void)mask; }
u32  uart_recv(u32 idx, u8 *buf, u32 len)
{ (void)idx; (void)buf; (void)len; return 0; }

/* ---- the display path ----
 *
 * The console draws into a buffer and has the VIC rotate it onto the panel.
 * Here LVGL's buffer is the picture, and sim.c takes it straight to a PNG,
 * so none of this is reached.
 */
u32 *display_init_window_a_pitch_vic(void) { return NULL; }
int  vic_init(void) { return 0; }
void vic_set_surface(const vic_surface_t *sfc) { (void)sfc; }
int  vic_compose(void) { return 0; }
int  vic_wait_idle(void) { return 0; }
bool dpout_video_on(void) { return false; }

/* ---- the debug console ---- */

bool _log_uart_only;
void dbg_send(const u8 *buf, u32 n)
{
    /* What the console would print over UART goes to stdout, a line at a
     * time: the harness's own output is already there. */
    char line[256];
    u32 i = 0;
    while (i < n) {
        u32 k = 0;
        while (i < n && k < sizeof(line) - 1)
            line[k++] = (char)buf[i++];
        line[k] = 0;
        sim_say("%s", line);
    }
}
void gui_dbg_usb_start(void) { }
void gui_dbg_init(bool lite) { (void)lite; }
void gui_dbg_poll(void) { }
void gui_dbg_rails(void) { }
void gui_cmd_poll(void) { }
void gui_screenshot(void) { }

/* ---- the screens the harness does not build ---- */

void gui_home(void) { }
