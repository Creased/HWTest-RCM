/*
 * hwtest - one-shot Switch hardware probe / repair report.
 *
 * Shared declarations: includes, logging + finding APIs, and the prototype
 * of every probe. Each probe domain lives in its own .c file; main.c keeps
 * the boot flow, the page table, the pager and the UART command loop.
 */
#ifndef HWTEST_H
#define HWTEST_H

#include <bdk.h>
#include <gfx_utils.h>
#include <libs/fatfs/ff.h>
#ifdef JC_PROBE
#include <input/joycon.h>
#endif
#include <stdarg.h>
#include <string.h>

#include "cpu_mbox.h"

extern void pivot_stack(u32 stack);
extern char *emmcsn_path_impl(char *path, char *sub_dir, char *filename,
                              sdmmc_storage_t *storage);

/* The WLAN/BT combo is the same silicon whether the package is laser-marked
 * BCM4356XKUBG (Broadcom) or CYW4356XUBG (Cypress, post-acquisition). There
 * is no software-visible difference, so the report never claims one marking
 * over the other. */
#define WLAN_CHIP_NAME "BCM/CYW4356"

/* Radio control pins shared by the Bluetooth and Wi-Fi probes. PH0 = wifi_en,
 * PH1 = WL_REG_ON, PH4 = BT_REG_ON. PINMUX_AUX offsets; BDK names the first
 * two pads. */
#define PMX_PH0_WIFI_EN    PINMUX_AUX_WIFI_EN   /* 0x1B4 */
#define PMX_PH1_WL_REG_ON  PINMUX_AUX_WIFI_RST  /* 0x1B8 */
#define PMX_PH4_BT_REG_ON  0x1C4

/* GPIO controller offsets (bdk/soc/gpio.c:21-38). Port H = 7 -> bank 1
 * slot 3 -> 0x10C; port I = 8 -> 0x200; port K = 10 -> 0x208. */
#define GPH_CNF  0x10C
#define GPH_OE   0x11C
#define GPH_OUT  0x12C
#define GPH_IN   0x13C
#define GPH_MCNF 0x18C
#define GPH_MOE  0x19C
#define GPH_MOUT 0x1AC
#define GPI_CNF  0x200
#define GPK_MCNF 0x288
#define GPK_MOE  0x298

/* Masked GPIO write: one 32-bit store of (pins << 8) | value, so only the
 * named pins move. BDK's gpio_config/gpio_write/gpio_output_enable are
 * read-modify-writes over the whole port register. */
#define GP_MWR(off, pins, hi) \
    (GPIO(off) = ((u32)(pins) << 8) | ((hi) ? (u32)(pins) : 0u))

/* ---- colours ---- */
#define COL_HEADER  0xFFFF8000   /* orange */
#define COL_DEFAULT 0xFFCCCCCC   /* light grey */
#define COL_OK      0xFF96FF00   /* green */
#define COL_WARN    0xFFFFDD00   /* yellow */
#define COL_ERR     0xFFFF5050   /* red */
#define COL_INFO    0xFF00DDFF   /* cyan, for section subtitles */
#define COL_BG      0xFF1B1B1B

/* LCD geometry: 1280x720 landscape, 16 px per char = 80 columns. */
#define LCD_COLS 80

/* ---- three-sink logging (LCD + UART_B + report capture buffer) ---- */

extern char _log_buf[512];
extern bool _log_uart_only;
extern bool _log_no_uart;
extern char *_log_capture;
extern u32  _log_capture_pos;
extern u32  _log_capture_cap;

#ifdef JC_PROBE
static inline void uart_send_crlf(u32 idx, const u8 *buf, u32 n)
{
    (void)idx; (void)buf; (void)n;
}
#else
void uart_send_crlf(u32 idx, const u8 *buf, u32 n);
#endif

void _log_emit(u32 color);
void _log_color_impl(u32 color, const char *fmt, ...);
void log_mute(bool on);

/* The assert catches a format-string LITERAL that is already too wide for
 * the LCD; _log_emit folds longer RENDERED lines onto a continuation. */
#define LOG(fmt, ...) do {                                \
    _Static_assert(sizeof(fmt) <= LCD_COLS + 2,           \
        "LOG format string wider than 80-col LCD");       \
    s_printf(_log_buf, fmt, ##__VA_ARGS__);               \
    _log_emit(COL_DEFAULT);                               \
} while (0)

#define log_color(color, fmt, ...) do {                   \
    _Static_assert(sizeof(fmt) <= LCD_COLS + 2,           \
        "log_color format string wider than 80-col LCD"); \
    _log_color_impl(color, fmt, ##__VA_ARGS__);           \
} while (0)

#define HEADER(s)  log_color(COL_INFO, "%s\n", s)

/* ---- diagnostic-finding registry ---- */

typedef enum { DX_PASS, DX_WARN, DX_FAIL } dx_sev_t;

typedef struct {
    const char *key;        /* string literal, immortal */
    dx_sev_t   sev;
    char       detail[48];  /* short reason, "" on a clean pass */
} dx_finding_t;

void dx_reset(void);
void dx_set(const char *key, dx_sev_t sev, const char *fmt, ...);
const dx_finding_t *dx_get(const char *key);

#define DX_CAP 48

/* ---- shared helpers ---- */

#define HEALTH_NONE 0x7FFFFFFE

u32 health_color(int v, int warn_lo, int warn_hi, int err_lo, int err_hi);
const char *panel_model_name(u16 dec);
const char *_pmx_pull_str(u32 pmx);
void status_set(const char *msg);
void uart_poll_reboot(void);
void msleep_poll(u32 ms);
int save_report(void);

/* ---- shared state ---- */

extern bool g_show_status;
extern bool g_wifi_armed;
extern bool g_wifi_brought_up;
extern bool g_sd_ok;
extern bool g_emmc_ok;
extern char *g_report_body;

/* ---- probes ---- */

void probe_soc(void);
void probe_fuses(void);
void probe_kfuse(void);
void probe_pmic(void);
void probe_max77812(void);
void probe_pmic_gpios(void);
void probe_regulators(void);
void probe_5v(void);
void probe_battery(void);
void probe_charger(void);
void probe_usbpd(void);
void probe_thermal(void);
void probe_fan(void);
void probe_dram(void);
void probe_clocks(void);
void probe_storage(void);
void probe_serial(void);
void probe_partitions(void);
void probe_emmc_health(void);
void probe_gpt(void);
void probe_boot0_pkg1(void);
void probe_autorcm(void);
void probe_sd_content(void);
void probe_storage_errors(void);
void probe_bt_radio(void);
void probe_audio(void);
void probe_audio_clocks(void);
void probe_audio_beep(void);
void probe_wifi_pcie(void);
void probe_wifi_link(void);
void probe_display(void);
void probe_backlight(void);
void probe_touch(void);
void probe_als(void);
void probe_joycon(void);
void probe_inputs(void);
void probe_gpio_census(void);
void probe_uart_b(void);
void probe_reset(void);
void probe_pmc_scratch(void);
void probe_verdict(void);

void run_all_probes(void);

#endif /* HWTEST_H */
