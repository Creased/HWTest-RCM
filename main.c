/*
 * hwtest - one-shot Switch hardware probe / repair report.
 *
 * Inits the BPMP-side peripherals via Hekate's BDK and walks every chip the
 * payload knows about. Output is mirrored three ways:
 *
 *   - LCD (paged, with health-coloured values)
 *   - UART_B (Joy-Con right port, 115200 8N1, 1.8 V, CRLF line endings)
 *   - SD card text file at backup/<emmc_serial>/hwtest.txt
 *
 * Use it on a console with a faulty screen by capturing the UART output, or
 * pull the SD card afterwards if no UART rig is at hand.
 *
 * Navigation:
 *   VOL+ / 'n'   next page
 *   VOL- / 'p'   previous page
 *   'r'          refresh current page (re-reads chips)
 *   's'          re-save SD report
 *   POWER / 'q'  power off the console
 */

#include "hwtest.h"



/* ------------------------------------------------------------------------ */
/* Boot-time progress indicator                                             */
/*                                                                          */
/* During the boot UART dump (_log_uart_only = true), nothing is being      */
/* drawn to the LCD - it sits on the static splash screen. To distinguish   */
/* "still working" from "crashed", redraw a single status line below the   */
/* splash with a spinner glyph + current probe name. The spinner ticks     */
/* every call so even a single long-running probe (I2C census, ~3 s, scans */
/* 0x08-0x77 on three buses) shows visible motion.                         */
/*                                                                          */
/* The status row lives at fixed pixel coordinates so it never collides    */
/* with the scrolling splash text. Once the dump phase ends and the pager  */
/* takes over, the screen gets cleared and the status row is gone.        */

#define STATUS_ROW_PX  (16 * 8)   /* row 8: under the splash + init steps */
#define STATUS_BLANK   "                                                                              "

static u32 g_status_spin_idx = 0;

/* Explicit gate for the on-LCD progress spinner. The boot dump phase
 * sets this true; everything else (save_report, host-driven 'G'
 * group-refresh, plain 'r' single-page refresh) leaves it false so
 * probes don't scribble the spinner over the pager output. We can't
 * just check `_log_uart_only` because the 'G' handler also flips that
 * to suppress LCD writes, we need a distinct concept. */
bool g_show_status = false;

void status_set(const char *msg)
{
    /* Runs once per probe during the boot sweep, which makes it the natural
     * place to honour a reboot request. Deliberately BEFORE the g_show_status
     * early-out: the host must be able to reclaim the console even when the
     * spinner isn't being drawn. */
    uart_poll_reboot();

    if (!g_show_status)
        return;

    static const char spin[] = "|/-\\";
    u32 saved_x = gfx_con.x;
    u32 saved_y = gfx_con.y;
    u32 saved_fg = gfx_con.fgcol;

    /* Wipe the line, then redraw. STATUS_BLANK is exactly the LCD width
     * so nothing left over from a previous (longer) message remains. */
    gfx_con_setpos(0, STATUS_ROW_PX);
    gfx_con.fgcol = COL_BG;
    gfx_puts(STATUS_BLANK);
    gfx_con_setpos(0, STATUS_ROW_PX);
    gfx_con.fgcol = COL_INFO;
    gfx_printf("  [%c] %s", spin[g_status_spin_idx & 3], msg);
    g_status_spin_idx++;

    /* Restore the cursor + colour so any subsequent _log_emit (which
     * shouldn't fire while _log_uart_only is true, but defensive) lands
     * back where it expected. */
    gfx_con_setpos(saved_x, saved_y);
    gfx_con.fgcol = saved_fg;
}

/* ------------------------------------------------------------------------ */
/* Pager + UART command loop                                                */

typedef void (*probe_fn_t)(void);

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
struct page_entry {
    probe_fn_t  fn;
    const char *name;
};

static const struct page_entry g_pages[] = {
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

    /* Low-level dumps. probe_uart_b is excluded in the JC_PROBE build
     * where the Joy-Con stack owns UART_B. */
    {probe_gpio_census,"Raw state"},
#ifndef JC_PROBE
    {probe_uart_b,     "Raw state"},
#endif
    {probe_reset,      "Raw state"},
    {probe_pmc_scratch,"Raw state"},
};
#define N_PAGES (sizeof(g_pages) / sizeof(g_pages[0]))

void run_all_probes(void)
{
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
        status_set(g_pages[i].name);
        log_color(COL_HEADER, "\n--- %s ---\n", g_pages[i].name);
        g_pages[i].fn();
    }
    status_set(g_pages[0].name);
    log_color(COL_HEADER, "\n--- %s ---\n", g_pages[0].name);
    g_pages[0].fn();
    status_set("done - rendering pager");
}

/* Compute the position of LCD page idx within its display group, e.g.
 * for a Storage entry where idx = 17 returns (group_idx=4, group_total=7)
 * meaning "this is the 4th of 7 pages with name 'Storage'". The LCD
 * header surfaces this so the user knows where they are inside a
 * multi-page domain. */
static void _page_group_pos(int idx, int *out_group_idx, int *out_group_total)
{
    const char *name = g_pages[idx].name;
    int total = 0, pos = 0;
    for (u32 i = 0; i < N_PAGES; i++) {
        if (strcmp(g_pages[i].name, name) == 0) {
            total++;
            if ((int)i <= idx) pos = total;
        }
    }
    *out_group_idx = pos;
    *out_group_total = total;
}

static void render_page(int idx)
{
    gfx_clear_grey(0x1B);
    gfx_con_setpos(0, 0);
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);

    /* The page-number header + help banner + separator are LCD chrome;
     * the host viewer doesn't need them and showing them just confuses
     * its parser (they don't match any of the three line shapes it
     * recognises). Suppress them from UART, then re-enable so the
     * actual probe body still ships over the wire. */
    int gidx, gtot;
    _page_group_pos(idx, &gidx, &gtot);
    bool prev = _log_no_uart;
    _log_no_uart = true;
    if (gtot > 1) {
        log_color(COL_HEADER, "hwtest - %s [%d/%d]  -  page %d/%d\n",
                  g_pages[idx].name, gidx, gtot, idx + 1, (int)N_PAGES);
    } else {
        log_color(COL_HEADER, "hwtest - %s  -  page %d/%d\n",
                  g_pages[idx].name, idx + 1, (int)N_PAGES);
    }
    LOG("n next | p prev | r refresh | a all | w wifi | s save | R reboot | q off\n");
    LOG("=============================================================\n\n");
    _log_no_uart = prev;

    /* Page body goes to BOTH sinks. We emit the same `--- Name ---`
     * boundary the boot dump uses so the host viewer parses each
     * pager-driven re-render as a fresh page (or refresh of an
     * existing one). The host viewer treats consecutive identical
     * boundary names as the same logical page (Storage is 7 LCD
     * pages but one host pane). */
    log_color(COL_HEADER, "\n--- %s ---\n", g_pages[idx].name);
    g_pages[idx].fn();
}

/* One byte of push-back, so uart_poll_reboot() can look at an incoming
 * character without stealing it from the pager. */
static int g_uart_pushback = -1;

static int uart_getc(void)
{
#ifdef JC_PROBE
    /* No DEBUG_UART_PORT in this build -> UART_B isn't initialised, so
     * uart_recv would block on UART_LSR.RDR forever. The pager loop
     * still works, just without keyboard shortcuts over serial. */
    return -1;
#else
    if (g_uart_pushback >= 0) {
        int c = g_uart_pushback;
        g_uart_pushback = -1;
        return c;
    }
    u8 c;
    if (uart_recv(UART_B, &c, 1) != 1)
        return -1;
    return c;
#endif
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
static void do_reboot(void)
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
 * Anything that isn't 'R' is pushed back so the pager still sees it. */
void uart_poll_reboot(void)
{
    int c = uart_getc();
    if (c < 0)
        return;
    if (c == 'R')
        do_reboot();
    else if (g_uart_pushback < 0)
        g_uart_pushback = c;
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

void ipl_main(void)
{
    /* Bring up clocks, regulators, I2C, etc. */
    hw_init();

    /* Move the stack into IPL load region and wire up a heap. */
    pivot_stack(IPL_LOAD_ADDR);
    heap_init((void *)IPL_HEAP_START);

    /* Bring up everything UART_B RX needs on the right Joy-Con rail.
     * hw_init() only configures TX (PG0) for the debug log; RX requires
     * four additional steps that are otherwise only done from
     * bdk/input/joycon.c::_jc_power_supply when a real Joy-Con is being
     * polled:
     *
     *   1. Route PG1 (UART2_RX) to SPIO mode so the Tegra UART2
     *      controller actually owns the pin instead of the GPIO block.
     *   2. Override hw_init's PINMUX_AUX_UART2_RX (= INPUT_ENABLE | TRISTATE)
     *      with INPUT_ENABLE | PULL_UP. The Tegra UART idles at logic HIGH
     *      and Hekate's TRISTATE setup leaves the pin high-impedance with
     *      no internal pull. Joy-Cons happen to drive the line continuously
     *      so they get away with it, but a passive USB-UART chassis adapter
     *      that only drives during transmission lets the line float between
     *      bytes and the receiver sees noise / framing errors.
     *      switch-coreboot's _config_uart_b enables PULL_UP for exactly
     *      this reason
     *      (https://github.com/lulle2007200/switch-coreboot/blob/593ce32688de8600c6e61e7ccdd0e785f0e3267c/src/soc/nvidia/tegra210/lp0/sc7_exit/tegra_sc7_exit.c#L72).
     *   3. Enable the 5 V supply to the Joy-Con-R rail. The RX line on
     *      the chassis goes through a level-shifter that's powered from
     *      this rail; without it the line stays floating regardless of
     *      what the host adapter sends.
     *   4. Drive PK3 high (Joy-Con-R charge-enable). This gates the
     *      MOSFET that bridges the Tegra-side signals to the rail-side
     *      pads.
     *
     * TX appears to "just work" without (2)/(3)/(4) because the Tegra side
     * drives the line directly, but the return path is asymmetric. */
#ifndef JC_PROBE
    gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_SPIO);
    PINMUX_AUX(PINMUX_AUX_UARTX_RX(UART_B)) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP;
    regulator_5v_enable(REGULATOR_5V_JC_R);
    PINMUX_AUX(PINMUX_AUX_GPIO_PK3) = PINMUX_DRIVE_4X | PINMUX_PULL_DOWN | 2;
    gpio_direction_output(GPIO_PORT_K, GPIO_PIN_3, GPIO_HIGH);

    /* Drain any phantom byte the receiver caught while the line was
     * transitioning from "stuck LOW" (no pull) to "idle HIGH" (PULL_UP
     * applied above). The LOW->HIGH edge briefly looks like a START bit
     * + 8 zero data bits + STOP bit, decoding to a spurious 0x00 in the
     * RX FIFO. uart_init() already cleared the FIFO during hw_init(),
     * but our PINMUX override happens *after* uart_init, so the
     * transition byte arrives later. Settle the line first then flush. */
    usleep(2000);
    {
        volatile u32 *uart_b_lsr = (volatile u32 *)(0x70006040 + 0x14);
        volatile u32 *uart_b_rbr = (volatile u32 *)(0x70006040 + 0x00);
        while (*uart_b_lsr & 0x01)        /* LSR.RDR */
            (void)*uart_b_rbr;            /* read and discard */
    }
#endif  /* !JC_PROBE , JC_PROBE-specific init runs after display setup so we can animate it */

    static const char banner[] =
        "\n=== hwtest === read-only,"
        " single write: backup/<emmc_serial>/hwtest.txt\n";
    uart_send_crlf(UART_B, (u8 *)banner, strlen(banner));

    /* Bring up the LCD + console. The LCD is physically 720 x 1280
     * portrait, but our gfx_putc renders in landscape: cursor coords go
     * 0..1279 along the long axis and 0..719 along the short axis. The
     * pixel-write helper inside gfx.c rotates each pixel 90 deg CW.
     * Hold the console with the right Joy-Con rail (UART side) at the
     * top to read the screen naturally. */
    display_init();
    u32 *fb = display_init_window_a_pitch();
    gfx_init_ctxt(fb, 1280, 720, 720);
    gfx_con_init();

    /* Render the "initialising" splash IMMEDIATELY (before backlight
     * ramp + before any other init) so when the panel becomes visible
     * the user sees content rather than a few seconds of dark screen.
     * The pager loop's first render_page() will overwrite this. */
    gfx_clear_grey(0x1B);
    gfx_con_setpos(0, 0);
    gfx_con_setcol(COL_HEADER, 1, COL_BG);
    gfx_printf("hwtest - initialising\n\n");
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);

    /* Backlight on (1 s ramp). Drawing happens in parallel with the
     * ramp - by the time the panel is bright enough to read, the
     * splash text below will already be on screen. */
    display_backlight_pwm_init();
    display_backlight_brightness(100, 1000);

    /* SD card + eMMC init done unconditionally before the boot dump.
     *
     * This brings up LDO2 (SDMMC1 rail), enables SDMMC1/SDMMC4 clocks,
     * and writes the SD/eMMC pinmux. The lazy entry point is
     * probe_storage, which runs AFTER probe_regulators and probe_clocks
     * in page order: leaving the init to it makes the boot dump capture
     * those regulators / clocks in their *pre-init* state while a
     * subsequent 'a' refresh-all sees them post-init. Doing the init up
     * front makes both captures byte-identical for the storage-derived
     * fields.
     *
     * Failure handling: sd_initialize and emmc_initialize each return
     * non-zero when the card / chip can't be brought up. Both have
     * bounded timeouts (~1-2 s) inside Hekate, so an empty SD slot or
     * a faulty SD reader produces a fail-then-continue, NOT a hang.
     * probe_storage reads sd_storage.initialized + emmc_storage.initialized
     * to detect this state and prints "sd_initialize FAILED" cleanly
     * instead of crashing. save_report sees g_sd_ok=false and logs
     * "[save] SD not ready" without retrying. This means the payload
     * boots and runs end-to-end whether the user RCM-injected via SD,
     * via TegraRcmGUI, or via a unit with a dead SD reader, there's
     * no failure mode that strands them at a black screen.
     *
     * UHS-I timing note: on the JC_PROBE build, sd_initialize must run
     * BEFORE jc_init_hw, the long Joy-Con poll between rail-up and
     * SD init would otherwise cause the CMD11 1.8 V switch to silently
     * fall back to HS25 at 3.3 V. */
    gfx_printf("  SD card / eMMC init     ...");
    sd_initialize(false);                /* return value intentionally
                                            ignored; probe_storage will
                                            re-check sd_storage.initialized
                                            and skip / report cleanly */
    emmc_initialize(false);              /* same, eMMC is soldered, so
                                            failure here is genuinely a
                                            board-fault diagnostic, not
                                            a missing-card situation */
    gfx_printf(" done\n");

#ifdef JC_PROBE
    /* Joy-Con stack init: powers both rails, runs Hekate's PH6/PE6
     * unlatch dance, then poll-spins the controllers for 5 s so the
     * MCU has time to wake up + answer the Discover request. */
    gfx_printf("  Joy-Con stack init      ...");
    jc_init_hw();
    gfx_printf(" done\n");

    /* 250 polls × 20 ms = 5 s. Print one dot every ~250 ms (every 12
     * iterations) so the operator can see polling is making progress. */
    gfx_printf("  Polling Joy-Cons (~5 s) ...");
    for (u32 i = 0; i < 250; i++) {
        joycon_poll();
        if (i && (i % 12 == 0))
            gfx_printf(".");
        msleep(20);
    }
    gfx_printf(" done\n");
#endif

    /* Common second phase: thermal sensor + clock boost, then probes.
     * These steps are quick (<100 ms) so they get a single combined
     * line rather than per-step animation. */
    gfx_printf("  Hardware setup          ...");
    /* TMP451 needs an explicit conversion-rate write to leave its
     * power-on 0.0625 Hz mode. */
    tmp451_init();
    /* Match Hekate: boost BPMP/SCLK before SDMMC init. */
    bpmp_clk_rate_set(BPMP_CLK_NORMAL);
    gfx_printf(" done\n\n");
    gfx_printf("Running probes, building report...\n");

    /* Allocate the SD-report buffer BEFORE the boot dump and arm the
     * capture sink. Every line emitted by the dump pass below also
     * goes into this buffer via _log_capture_append, so save_report
     * can later just dump the buffer to SD without re-running every
     * probe, which would cost an extra 5-10 s. */
    const u32 report_cap = 32 * 1024;
    g_report_body = (char *)malloc(report_cap);
    if (g_report_body) {
        g_report_body[0] = 0;
        _log_capture     = g_report_body;
        _log_capture_pos = 0;
        _log_capture_cap = report_cap;
    }

    /* Dump every probe to UART once at boot - the serial log is the
     * primary diagnostic capture. The status spinner is only enabled
     * here, subsequent on-demand probe runs (host 'G' refresh, plain
     * 'r' refresh, save_report) leave g_show_status false so probes
     * don't draw over the pager. */
    g_show_status  = true;
    _log_uart_only = true;
    log_color(COL_HEADER, "\n=== hwtest full UART dump ===\n");
    run_all_probes();
    log_color(COL_HEADER, "\n=== end of dump - pager active on LCD ===\n\n");
    _log_uart_only = false;
    g_show_status  = false;

    /* Detach the capture sink. The buffer remains for save_report. */
    _log_capture = NULL;

    /* Auto-save the report to SD if we managed to reach the eMMC for
     * the directory name. Re-saving via the 's' UART key just writes
     * the same buffer again, no probes re-run. */
    if (g_sd_ok && g_emmc_ok)
        save_report();

    int page = 0;
    render_page(page);

    for (;;) {
        int c   = uart_getc();
        u8  btn = btn_read();

        bool advance = (c == 'n' || c == ' ') || (btn & BTN_VOL_UP);
        bool back    = (c == 'p' || c == 'b') || (btn & BTN_VOL_DOWN);
        bool refresh = (c == 'r');
        bool refresh_all = (c == 'a' || c == 'A');
        bool save    = (c == 's' || c == 'S');
        /* 'R' (capital only - lowercase 'r' is refresh, 'b' is prev-page). */
        bool reboot  = (c == 'R');
        bool quit    = (c == 'q' || c == 'Q') || (btn & BTN_POWER);

        if (advance || back) {
            /* Batch consecutive nav chars from the UART RX FIFO before
             * rendering. Without this, sending "nnnnn" from the host
             * would re-run probes 5 times and dump 5 page bodies over
             * UART, slow and wasteful when the user only cares about
             * the final destination. We accumulate a delta then render
             * once for the destination page. Anything other than n/p/
             * space/b breaks the batch. */
            int delta = advance ? 1 : -1;
            for (;;) {
                int next = uart_getc();
                if (next == 'n' || next == ' ') delta++;
                else if (next == 'p' || next == 'b') delta--;
                else if (next < 0) break;       /* FIFO empty, render now */
                else {
                    /* Different command queued. uart_recv has already
                     * consumed the byte and it can't be pushed back to
                     * the FIFO, so it is dropped here; following a nav
                     * burst with another command in one write is rare
                     * in practice. */
                    break;
                }
            }
            int new_page = ((int)page + delta) % (int)N_PAGES;
            if (new_page < 0) new_page += (int)N_PAGES;
            page = new_page;
            render_page(page);
        } else if (refresh) {
            render_page(page);
        } else if (c == 'W' || c == 'w') {
            /* Re-run the PCIe Wi-Fi probe on demand. It runs in the boot
             * sweep as well (WIFI_CFG_AUTORUN), so this is for repeating it
             * without a reboot - after moving the console, or to watch a
             * marginal link train twice. The bring-up itself is the risky
             * part: a mis-sequenced one stalls CPU0 on the bus, which the
             * BPMP supervises and recovers from by powergating the
             * cluster. */
            bool prev = _log_uart_only;
            _log_uart_only = true;
            log_color(COL_HEADER, "\n--- Wireless ---\n");
            g_wifi_armed = true;
            probe_wifi_pcie();
            _log_uart_only = prev;
        } else if (c == 'G') {
            /* Group refresh: 'G' followed by an ASCII group name and a
             * '\n'. Re-runs every probe whose g_pages[].name matches the
             * supplied string and re-emits each over UART, WITHOUT
             * touching the LCD. The host viewer uses this to refresh a
             * single pane (e.g. "Storage") without dragging the on-
             * console display away from whatever the operator is
             * looking at.
             *
             * 200 ms timeout on the name read; any byte other than \n
             * / \r terminates the name normally. Empty name = no-op. */
            char gname[64];
            u32 glen = 0;
            u32 deadline = get_tmr_us() + 200000;
            while (glen < sizeof(gname) - 1) {
                int b = uart_getc();
                if (b < 0) {
                    if ((s32)(get_tmr_us() - deadline) > 0) break;
                    msleep(2);
                    continue;
                }
                if (b == '\n' || b == '\r') break;
                gname[glen++] = (char)b;
            }
            gname[glen] = 0;
            if (glen > 0) {
                bool prev = _log_uart_only;
                _log_uart_only = true;   /* suppress LCD writes */
                for (u32 i = 0; i < N_PAGES; i++) {
                    if (strcmp(g_pages[i].name, gname) == 0) {
                        log_color(COL_HEADER, "\n--- %s ---\n",
                                  g_pages[i].name);
                        g_pages[i].fn();
                    }
                }
                _log_uart_only = prev;
            }
        } else if (refresh_all) {
            /* 'a' = re-run every probe and re-emit the full UART dump,
             * just like the boot-time sweep. The host viewer uses this to
             * pull a fresh snapshot of every domain in one shot.
             * The pager's currently-shown LCD page is then re-rendered
             * so the on-console display stays in sync. */
            _log_uart_only = true;
            log_color(COL_HEADER, "\n=== hwtest full UART dump ===\n");
            run_all_probes();
            log_color(COL_HEADER, "\n=== end of dump - pager active on LCD ===\n\n");
            _log_uart_only = false;
            render_page(page);
        } else if (save) {
            save_report();
        } else if (reboot) {
            /* 'R' = reboot, via do_reboot's MAX77620 SFT_RST power cycle.
             *
             * This is the return path for an iterative test loop: on a
             * console whose boot payload is a UART sideloader (modchip or
             * RCM jig re-injects it every boot), rebooting hands control
             * straight back to that loader, so the host can push the next
             * build without anyone touching the hardware.
             *
             * That loop needs a real power cycle, not a warm PMC MAIN_RST:
             * a warm reset lands in RCM without re-triggering the modchip,
             * which only glitches at power-on. See do_reboot for the full
             * sequence and for why power_set_state is not used. */
            do_reboot();
        } else if (quit) {
            while (btn_read() & BTN_POWER)
                ;
            /* Power the console off cleanly. Asserting MAX77620_REG_ONOFFCNFG1
             * bit PWR_OFF tells the main PMIC to drop every regulator rail in
             * the proper sequence, the SoC powers down for real.
             *
             * Why not reboot to RCM? `power_set_state(REBOOT_RCM)` runs
             * hw_deinit which hangs partway through (display_end works,
             * but the BPMP/MMU teardown stalls on this payload). The bare
             * watchdog PMC reset DOES work but only re-enters RCM, it
             * doesn't reload Hekate, so a picofly-less console ends up
             * in RCM with no payload waiting. A clean power-off lets the
             * user press POWER themselves to wake the modchip / RCM jig
             * for the next payload. */
            log_color(COL_HEADER, "Powering off...\n");
            i2c_send_byte(I2C_5, MAX77620_I2C_ADDR,
                          MAX77620_REG_ONOFFCNFG1, MAX77620_ONOFFCNFG1_PWR_OFF);
            while (1)
                ;
        } else {
            msleep(20);
        }

        /* Debounce: if a button is still held, wait for release before
         * accepting the next press. UART chars don't need debouncing. */
        if (btn) {
            while (btn_read() == btn)
                msleep(20);
        }
    }
}
