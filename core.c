/*
 * hwtest - one-shot Switch hardware probe / repair report.
 *
 * What both front ends share: the page table and the sweep that walks it,
 * the per-step breadcrumb, and the UART reboot hook every long wait polls.
 * main.c is the text launcher and pager of the RCM payload; gui/ is the
 * LVGL front end loaded from the SD card. Each supplies its own
 * status_set(), which is where the two draw their progress.
 */
#include "hwtest.h"

/* Explicit gate for the on-LCD progress spinner. The boot dump phase
 * sets this true; everything else (save_report, host-driven 'G'
 * group-refresh, plain 'r' single-page refresh) leaves it false so
 * probes don't scribble the spinner over the pager output. We can't
 * just check `_log_uart_only` because the 'G' handler also flips that
 * to suppress LCD writes, we need a distinct concept. */
bool g_show_status = false;
u32 g_run_mask = PROBE_G_ALL;

/* A step INSIDE a probe, for when the console dies straight after it.
 * status_set() only names the page, and pages share names - BT, Wi-Fi bring-up
 * and the Wi-Fi link all spin as "Wireless" - so "it freezes in Wireless"
 * could be any of them. This names the step on both sinks a tester might have:
 *
 *  - UART, synchronously: uart_send_crlf() waits for TX idle, so the line has
 *    left the wire before the next, possibly fatal, access is made.
 *  - the LCD status row. The framebuffer is in DRAM and the BPMP maps all of
 *    DRAM write-back cached (bdk soc/bpmp.c mmu_entries), so a line drawn
 *    just before a bus hang can sit in the cache and never be scanned out.
 *    Clean it, or the screen shows the PREVIOUS step and blames the wrong one.
 *
 * Keep messages short: the line buffer is fixed. */
void status_step(const char *msg)
{
    char line[96];
    s_printf(line, "[step] %s\n", msg);
    uart_send_crlf(UART_B, (u8 *)line, strlen(line));

    status_set(msg);
    if (g_show_status)
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLEAN_WAY, false);
}

/* Each entry is ONE LCD page (one probe) with a display name. Multiple
 * consecutive entries can share a display name; from the LCD pager's
 * point of view they're separate pages (so each fits on screen), but
 * over UART they emit the same `--- name ---` boundary. The host
 * viewer's parser treats same-named boundaries as the same logical
 * page and accumulates [Section] headers into it, which is what we
 * want, since each probe emits its own [Section] header for its data.
 *
 * One probe per entry caps each LCD page at that single probe's worth
 * of text while keeping the host viewer's grouped presentation: an
 * entry driving several probes overflows the screen once a domain
 * holds more than ~45 rows of output. */
const struct page_entry g_pages[] = {
    /* Index 0: top-level verdict. Depends on globals populated by every
     * other probe, so run_all_probes runs it LAST. The pager shows it
     * as the first navigable page. */
    {probe_verdict,    "Diagnostics"},

    /* Identity. */
    {probe_soc,        "SoC"},
    {probe_fuses,      "Fuses"},
    {probe_kfuse,      "Fuses"},

    /* Power & charging. Each probe gets its own LCD page; they all
     * share the "Power & charging" name so the host viewer groups them. */
    {probe_pmic,       "Power & charging"},
    {probe_max77812,   "Power & charging"},
    {probe_pmic_gpios, "Power & charging"},
    {probe_regulators, "Power & charging"},
    {probe_5v,         "Power & charging"},
    {probe_battery,    "Power & charging"},
    {probe_charger,    "Power & charging"},
    {probe_usbpd,      "Power & charging"},
    {probe_thermal,    "Power & charging"},
    {probe_fan,        "Power & charging"},

    /* Memory & clocks. */
    {probe_dram,       "Memory & clocks"},
    {probe_clocks,     "Memory & clocks"},

    /* All storage probes share one display name, so partitions, GPT and
     * health land in a single "Storage" pane on the host viewer. */
    {probe_storage,    "Storage"},
    {probe_serial,     "Storage"},
    {probe_partitions, "Storage"},
    {probe_emmc_health,"Storage"},
    {probe_gpt,        "Storage"},
    {probe_boot0_pkg1, "Storage"},
    {probe_autorcm,    "Storage"},
    {probe_sd_content, "Storage"},
    /* Last in the group: the counters must account for every read the
     * probes above performed. */
    {probe_storage_errors, "Storage"},
    {probe_gamecard,   "Storage"},

    /* Wireless: the two halves of the CYW4356. BT first - it is the
     * cheaper measurement and its result frames the PCIe one. */
    {probe_bt_radio,   "Wireless"},
    {probe_wifi_pcie,  "Wireless"},
    {probe_wifi_link,  "Wireless"},

    /* Audio codec. */
    {probe_audio,        "Audio"},
    {probe_audio_clocks, "Audio"},
    {probe_audio_beep,   "Audio"},

    /* Display + backlight. */
    {probe_display,    "Display"},
    {probe_backlight,  "Display"},

    /* Touch + Joy-Con + buttons + ambient light. */
    {probe_touch,      "Inputs"},
    {probe_als,        "Inputs"},
    {probe_joycon,     "Inputs"},
    {probe_inputs,     "Inputs"},

    /* Low-level dumps. UART_B is always the debug/report console. */
    {probe_gpio_census,"Raw state"},
    {probe_uart_b,     "Raw state"},
    {probe_reset,      "Raw state"},
    {probe_pmc_scratch,"Raw state"},
};

/* Execution group for each page. Index 0 is the aggregate Diagnostics page
 * and is always emitted after the selected data probes. Keep this parallel
 * to g_pages so the existing page table remains simple and parser tooling can
 * continue to inventory probe functions directly. */
const u32 g_page_groups[] = {
    0,
    PROBE_G_IDENTITY, PROBE_G_IDENTITY, PROBE_G_IDENTITY,
    PROBE_G_POWER, PROBE_G_POWER, PROBE_G_POWER, PROBE_G_POWER, PROBE_G_POWER,
    PROBE_G_POWER, PROBE_G_POWER, PROBE_G_POWER, PROBE_G_POWER, PROBE_G_POWER,
    PROBE_G_MEMORY, PROBE_G_MEMORY,
    PROBE_G_STORAGE, PROBE_G_STORAGE, PROBE_G_STORAGE, PROBE_G_STORAGE,
    PROBE_G_STORAGE, PROBE_G_STORAGE, PROBE_G_STORAGE, PROBE_G_STORAGE,
    PROBE_G_STORAGE, PROBE_G_STORAGE,
    PROBE_G_WIRELESS, PROBE_G_WIRELESS, PROBE_G_WIRELESS,
    PROBE_G_AUDIO, PROBE_G_AUDIO, PROBE_G_SOUND,
    PROBE_G_DISPLAY, PROBE_G_DISPLAY,
    PROBE_G_INPUTS, PROBE_G_INPUTS, PROBE_G_INPUTS, PROBE_G_INPUTS,
    PROBE_G_RAW, PROBE_G_RAW, PROBE_G_RAW, PROBE_G_RAW,
};
_Static_assert(sizeof(g_pages) / sizeof(g_pages[0]) == N_PAGES,
               "N_PAGES in hwtest.h must match g_pages");
_Static_assert(sizeof(g_page_groups) / sizeof(g_page_groups[0]) == N_PAGES,
               "g_page_groups must match g_pages");

bool page_enabled(int idx)
{
    return idx == 0 || (g_page_groups[idx] & g_run_mask) != 0;
}

void run_probe_mask(u32 mask)
{
    g_run_mask = mask & PROBE_G_ALL;
    /* Data probes populate the dx registry; the verdict aggregates it.
     * Reset between sweeps so a refresh-all (`a`) doesn't see stale
     * findings from the previous run. The verdict runs LAST because it
     * reads what the data probes deposited; the pager still shows it
     * first via index 0. */
    dx_reset();

#if WIFI_CFG_EARLY_REGON
    /* Power the WLAN section at the very START of the sweep, so by the time
     * the Wi-Fi probe enumerates - many probes and several seconds later -
     * the CYW4356 has been powered continuously. That mirrors HOS, where the
     * RE shows WL_REG_ON is a boot-on/always-on regulator held high from
     * early boot (the pcie sysmodule never touches the WifiReset pad), NOT
     * cycled ~150 ms before enumeration as this probe does. Pair with
     * WIFI_CFG_COLD_CYCLE=0 so the Wi-Fi probe leaves it high. PH1 is driven
     * as a plain GPIO output; the BT probe restores it high too. */
    PINMUX_AUX(PMX_PH1_WL_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_1, 1);
    GP_MWR(GPH_MCNF, GPIO_PIN_1, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_1, 1);
    (void)GPIO(GPH_OUT);
#endif

    for (u32 i = 1; i < N_PAGES; i++) {
        if (!page_enabled((int)i))
            continue;
        status_set(g_pages[i].name);
        log_color(COL_HEADER, "\n--- %s ---\n", g_pages[i].name);
        g_pages[i].fn();
    }
    status_set(g_pages[0].name);
    log_color(COL_HEADER, "\n--- %s ---\n", g_pages[0].name);
    g_pages[0].fn();
    status_set("done - rendering pager");
}

void run_all_probes(void)
{
    run_probe_mask(PROBE_G_ALL);
}

/* Push-back FIFO, so uart_poll_reboot() can drain the UART looking for 'R'
 * without stealing anything from the pager. A single slot used to do this,
 * and one stray key queued ahead of the 'R' parked in it for good: every
 * poll re-read that same byte and never reached the 'R' behind it. */
#define UART_PUSHBACK_N 16
static u8  g_uart_pb[UART_PUSHBACK_N];
static u32 g_uart_pb_head, g_uart_pb_tail;

int uart_getc(void)
{
    if (g_uart_pb_tail != g_uart_pb_head)
        return g_uart_pb[g_uart_pb_tail++ % UART_PUSHBACK_N];
    u8 c;
    if (!UART_DEBUG_ON || uart_recv(UART_B, &c, 1) != 1)
        return -1;
    return c;
}

/* Hand a byte back to uart_getc(); dropped if the FIFO is full. */
void uart_ungetc(u8 c)
{
    if (g_uart_pb_head - g_uart_pb_tail < UART_PUSHBACK_N)
        g_uart_pb[g_uart_pb_head++ % UART_PUSHBACK_N] = c;
}

/* Reboot the console. Split out of the pager so it can be reached from
 * anywhere, including mid-probe.
 *
 * This does a full PMIC power cycle (MAX77620 SFT_RST with the soft-reset
 * wake event armed), not a bare PMC MAIN_RST. The difference matters on a
 * modchipped console: MAIN_RST is only a warm reset into RCM, and a Mariko
 * just sits there - its RCM is patched, and the modchip glitches at power-on
 * so it never re-triggers, leaving the console silent until a manual power
 * cycle. SFT_RST drops every rail and comes back through POR, which the
 * modchip does catch, so the boot payload (the sideloader) is re-injected
 * and the host keeps control.
 *
 * Same sequence as BDK's power_set_state(POWER_OFF_REBOOT), minus the
 * hw_deinit() it starts with - that stalls partway through on this payload. */
void do_reboot(void)
{
    log_color(COL_HEADER, "Rebooting (PMIC power cycle)...\n");
    msleep(50);                     /* let the UART line drain first */

    u8 reg = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG2);
    reg |= MAX77620_ONOFFCNFG2_SFT_RST_WK;   /* wake back up after the cycle */
    i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG2, reg);
    i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG1,
                  MAX77620_ONOFFCNFG1_SFT_RST);
    while (1)
        ;
}

/* Emergency escape hatch: check whether the host asked us to reboot, and do
 * it immediately if so.
 *
 * The pager only reads UART once the whole probe sweep has finished, which
 * leaves a long window (and any hung probe) with no way back. A host driving
 * this over a sideloader needs to be able to reclaim the console at ANY
 * point to push a new build, so this is called from the per-probe status
 * tick and from every long wait.
 *
 * Everything waiting is drained; anything that isn't 'R' is pushed back so
 * the pager still sees it (beyond the FIFO's depth it is dropped). */
void uart_poll_reboot(void)
{
    u8 c;
    while (UART_DEBUG_ON && uart_recv(UART_B, &c, 1) == 1) {
        if (c == 'R')
            do_reboot();
        uart_ungetc(c);
    }
}

/* msleep() that stays responsive to a reboot request. Use instead of a bare
 * msleep() for anything the operator would have to sit through. */
void msleep_poll(u32 ms)
{
    while (ms) {
        u32 slice = ms > 20 ? 20 : ms;
        msleep(slice);
        ms -= slice;
        uart_poll_reboot();
    }
}
