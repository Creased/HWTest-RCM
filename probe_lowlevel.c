/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: lowlevel probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* PMC reset reason + MAX77620 fault latches + RTC                          */
/*                                                                          */
/* Boot forensics. Three sources, all surface why the previous power cycle  */
/* ended:                                                                   */
/*   1. PMC RST_STATUS (reg 0x1B4, low 3 bits) - Tegra-side reason: power-  */
/*      on, watchdog, thermal sensor, software, LP0 wake, AOTAG.            */
/*   2. MAX77620 NVERC (reg 0x0C, latched in non-volatile bits) - PMIC-     */
/*      side reason: forced shutdown, watchdog, hard reset, thermal         */
/*      overload, low-battery shutdown, MBO/MBU power-supply faults.        */
/*   3. MAX77620 INTLBT / IRQSD - thermal-alarm and per-rail power-fault    */
/*      latches captured since the last clear.                              */
/* On a normally-shut-down console all four read 0x00. Any non-zero value   */
/* is a real triage signal - the box rebooted because of *something*. We    */
/* also dump the current RTC time so a tech can correlate "console says     */
/* boot was at 14:23 on 2024-08-12" against their workshop log.             */
/* PMC scratch register decode. The Power Management Controller has
 * 256 32-bit "scratch" registers that survive across SoC resets and
 * deep-sleep cycles, they're how Hekate, HOS package1, fuses ipatch,
 * and Atmosphere stash boot-handover state. Most slots are opaque
 * (used by HOS internals and not externally documented), but a few
 * have well-known meanings that are useful for repair-tech diagnosis:
 *
 *   SCRATCH0  : reboot-mode flags written by Hekate / TZ to choose the
 *               next-boot path. Bits we know:
 *                 BIT(0)  WARMBOOT       (resume from sleep)
 *                 BIT(1)  RCM            (next reboot enters BootROM RCM)
 *                 BIT(29) PAYLOAD        (chainload a payload after pkg1)
 *                 BIT(30) BOOTLOADER     (chainload a CFW bootloader)
 *                 BIT(31) RECOVERY       (force fusee/recovery path)
 *
 *   SCRATCH20 : ODM customer config, bit 18 (0x40000) = "debug console"
 *               flag that Hekate explicitly clears in hw_init.
 *
 *   SCRATCH200: temporarily holds reset cause across the BootROM->bldr
 *               handover; Hekate captures it then zeroes it.
 *
 * Read-only here. Just dump the values + decode the documented bits. */
void probe_pmc_scratch(void)
{
    HEADER("[PMC scratch]");

    u32 s0   = PMC(APBDEV_PMC_SCRATCH0);
    u32 s20  = PMC(APBDEV_PMC_SCRATCH20);
    u32 s200 = PMC(APBDEV_PMC_SCRATCH200);

    log_color(COL_DEFAULT, "  SCRATCH0     : 0x%08X\n", s0);
    if (s0 & PMC_SCRATCH0_MODE_WARMBOOT)
        LOG("                 - WARMBOOT bit (resume-from-sleep flag set)\n");
    if (s0 & PMC_SCRATCH0_MODE_RCM)
        LOG("                 - RCM bit (next reboot would re-enter BootROM RCM)\n");
    if (s0 & PMC_SCRATCH0_MODE_PAYLOAD)
        LOG("                 - PAYLOAD bit (CFW payload chainload requested)\n");
    if (s0 & PMC_SCRATCH0_MODE_BOOTLOADER)
        LOG("                 - BOOTLOADER bit (CFW bootloader chainload requested)\n");
    if (s0 & PMC_SCRATCH0_MODE_RECOVERY)
        LOG("                 - RECOVERY bit (force fusee/recovery path)\n");

    log_color(COL_DEFAULT, "  SCRATCH20    : 0x%08X\n", s20);
    if (s20 & 0x00040000)
        log_color(COL_INFO,
            "                 - bit 18 set: ODM debug-console flag (HOS would enable kgdb)\n");

    log_color(COL_DEFAULT, "  SCRATCH200   : 0x%08X\n", s200);

    /* Hekate "next-boot intent". Despite living in the MAX77620's RTC
     * scratch slots (2 slots, 6 bits each), this has nothing to do with
     * time; it's just the only battery-backed scratch storage on the
     * board, so Hekate piggybacks on it to communicate with itself
     * across SoC resets ("next reboot, force the menu" / "next reboot,
     * do UMS"). The reason field decodes to:
     *   0 NOP  , normal boot, no special handling
     *   1 SELF , autoboot_idx into autoboot_list
     *   2 MENU , force Hekate menu
     *   3 UMS  , force USB Mass Storage of partition ums_idx
     *   4 REC  , set SCRATCH0 RECOVERY and reboot to self
     *   5 PANIC, bootloader-side panic (T210B01 only)
     *
     * If the unit unexpectedly enters Hekate menu instead of HOS at
     * every boot, REASON=2 stuck here would explain it. We surface it
     * as "Hekate boot intent" so the line doesn't read like a sibling
     * of the RTC time entries above. */
    rtc_reboot_reason_t rr = {0};
    bool valid = max77620_rtc_get_reboot_reason(&rr);
    static const char *kReasons[6] = {
        "NOP (normal boot)", "SELF (autoboot)", "MENU (force Hekate menu)",
        "UMS (force USB mass storage)", "REC (recovery)", "PANIC"};
    if (!valid) {
        log_color(COL_OK,
            "  Hekate intent: none (normal boot, no override pending)\n");
    } else {
        u32 reason = rr.dec.reason & 0xF;
        log_color(reason == 0 ? COL_OK : COL_INFO,
            "  Hekate intent: %d (%s)\n",
            reason, reason < 6 ? kReasons[reason] : "?");
        if (reason == 1)
            LOG("                 autoboot_idx=%d list=%d\n",
                rr.dec.autoboot_idx, rr.dec.autoboot_list);
        if (reason == 3)
            LOG("                 ums_idx=%d\n", rr.dec.ums_idx);
    }
}

void probe_reset(void)
{
    HEADER("[Reset + PMIC latches + RTC + Hekate intent]");

    /* 1. Tegra PMC reset status */
    u32 rst = PMC(APBDEV_PMC_RST_STATUS) & PMC_RST_STATUS_MASK;
    static const char *kRstNames[] = {
        "POR (cold boot - power-on reset)",
        "WATCHDOG (BPMP timeout - hung firmware?)",
        "SENSOR (thermal trip - SoC overheat)",
        "SW_MAIN (software reboot)",
        "LP0 (wake from deep sleep)",
        "AOTAG (always-on tag-based reset)",
        "(reserved 6)",
        "(reserved 7)",
    };
    /* POR is the only "expected" reason on a cold boot. Anything else
     * means the previous session ended unexpectedly. */
    u32 col = (rst == PMC_RST_STATUS_POR) ? COL_OK : COL_WARN;
    if (rst == 1 || rst == 2) col = COL_ERR;
    log_color(col, "  PMC RST_STATUS: %d (%s)\n", rst, kRstNames[rst & 7]);

    /* 2. MAX77620 NVERC (non-volatile reset cause) */
    u8 nverc = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_NVERC);
    /* Severe latches = real fault evidence: the PMIC watchdog fired, a
     * thermal overload tripped, or the battery rail over-loaded. Battery-
     * depletion events (MBLSD low-battery shutdown, MBU under-voltage)
     * happen on every deep discharge and on a battery pull while powered -
     * power-history noise on an otherwise healthy PMIC, so they warn.
     * User-triggered resets (SHDN/HDRST/RSTIN) warn too. */
    u8 nverc_severe = nverc & (MAX77620_NVERC_WTCHDG | MAX77620_NVERC_TOVLD |
                               MAX77620_NVERC_MBO);
    dx_set("pmic_nverc",
        nverc_severe ? DX_FAIL : nverc ? DX_WARN : DX_PASS,
        nverc_severe ? "severe latch 0x%02X" :
        nverc        ? "soft latch 0x%02X" : "", nverc);
    log_color(nverc ? COL_WARN : COL_OK,
        "  PMIC NVERC   : 0x%02X%s\n", nverc, nverc ? "" : " (clean)");
    if (nverc & MAX77620_NVERC_SHDN)    log_color(COL_WARN, "                 - SHDN  : forced shutdown via EN0\n");
    if (nverc & MAX77620_NVERC_WTCHDG)  log_color(COL_ERR,  "                 - WTCHDG: PMIC watchdog tripped\n");
    if (nverc & MAX77620_NVERC_HDRST)   log_color(COL_WARN, "                 - HDRST : hard reset (long power-button)\n");
    if (nverc & MAX77620_NVERC_TOVLD)   log_color(COL_ERR,  "                 - TOVLD : thermal overload trip\n");
    if (nverc & MAX77620_NVERC_MBLSD)   log_color(COL_WARN, "                 - MBLSD : main-batt low-voltage shutdown\n");
    if (nverc & MAX77620_NVERC_MBO)     log_color(COL_ERR,  "                 - MBO   : main-batt overload\n");
    if (nverc & MAX77620_NVERC_MBU)     log_color(COL_WARN, "                 - MBU   : main-batt under-voltage\n");
    if (nverc & MAX77620_NVERC_RSTIN)   log_color(COL_WARN, "                 - RSTIN : RST input asserted\n");

    /* 3. INTLBT and IRQSD: latched IRQ events */
    u8 intlbt = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_INTLBT);
    u8 irqsd  = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_IRQSD);
    dx_set("pmic_intlbt", intlbt ? DX_WARN : DX_PASS,
        intlbt ? "thermal/low-batt latch 0x%02X" : "", intlbt);
    /* IRQSD bits latch on every SD-rail buck transition, including the
     * normal cold-boot ramp from off to in-regulation: the per-bit
     * decode below is informational. Only escalate the verdict when
     * NVERC already shows a severe latch (i.e. the box rebooted because
     * of something), in which case IRQSD pinpoints which rail tripped.
     * Otherwise: PASS, with the bits visible on the detail page for a
     * tech who wants to look. */
    dx_set("pmic_irqsd",
        (irqsd && nverc_severe) ? DX_FAIL : DX_PASS,
        (irqsd && nverc_severe)
            ? "rail-fault latch 0x%02X (NVERC severe)" : "",
        irqsd);
    log_color(intlbt ? COL_WARN : COL_OK,
        "  PMIC INTLBT  : 0x%02X%s\n", intlbt, intlbt ? "" : " (no thermal/low-batt events)");
    if (intlbt & MAX77620_IRQ_TJALRM2_MASK) log_color(COL_ERR,  "                 - TJALRM2: junction-temp alarm 2\n");
    if (intlbt & MAX77620_IRQ_TJALRM1_MASK) log_color(COL_WARN, "                 - TJALRM1: junction-temp alarm 1\n");
    if (intlbt & MAX77620_IRQ_LBM_MASK)     log_color(COL_WARN, "                 - LBM    : low-battery monitor\n");
    /* IRQSD note: the four PFI_SDx bits latch power-fault edges, they
     * fire whenever a SD-rail buck regulator transitions through the
     * fault threshold. The PMIC sets them during:
     *   - chip POR / HDRST as each buck ramps from off to in-regulation
     *   - a controlled shutdown via ONOFFCNFG1.PWR_OFF (the 'q' key on
     *     this payload uses that path), where rails ramp DOWN through
     *     the threshold one at a time, you typically see SD0/SD1/SD3
     *     set, SD2 (PLL) often stays clear because PLL was the first
     *     rail brought down
     *   - real in-band power faults during operation
     *
     * We can't tell the three apart from IRQSD alone, the bit pattern
     * is the same. The NVERC line above carries the actual reset cause
     * (clean = nothing the PMIC noticed; SHDN/HDRST/etc = a recorded
     * event). So we surface IRQSD as info-coloured per-bit lines and
     * let the tech cross-reference NVERC + RST_STATUS instead of
     * raising a yellow flag on every partial pattern. */
    if (irqsd == 0) {
        log_color(COL_OK, "  PMIC IRQSD   : 0x%02X (no SD-rail power faults)\n", irqsd);
    } else {
        log_color(COL_DEFAULT,
            "  PMIC IRQSD   : 0x%02X (SD-rail thresholds latched - cross-check NVERC)\n",
            irqsd);
    }
    if (irqsd & MAX77620_IRQSD_PFI_SD0) log_color(COL_DEFAULT, "                 - PFI_SD0 : CPU rail threshold latched\n");
    if (irqsd & MAX77620_IRQSD_PFI_SD1) log_color(COL_DEFAULT, "                 - PFI_SD1 : DDR rail threshold latched\n");
    if (irqsd & MAX77620_IRQSD_PFI_SD2) log_color(COL_DEFAULT, "                 - PFI_SD2 : PLL rail threshold latched\n");
    if (irqsd & MAX77620_IRQSD_PFI_SD3) log_color(COL_DEFAULT, "                 - PFI_SD3 : 1V8 rail threshold latched\n");

    /* 4. RTC current time. Two things to know about Switch RTC:
     *
     *    (a) Hekate's BDK already 1-indexes time->month internally
     *        (1=January, 12=December), so do NOT add 1 again on display
     *        - a second increment renders January as 02.
     *
     *    (b) The MAX77620 calendar registers are not the same time the
     *        user sees in HOS. HOS keeps its own epoch offset (in system
     *        save data and PMC scratch) and adds it to whatever the
     *        MAX77620 calendar reads. After a coin-cell flat-discharge
     *        or a board reflow that briefly drops VBAT, the calendar
     *        resets to 2000-01-01 00:00:00 while HOS quietly keeps the
     *        offset and still shows the right wall time. So a year
     *        reading at the cold-boot default is informational, not a
     *        fault; we flag it as such instead of pretending the read
     *        is broken.
     *
     *    Hekate's own GUI uses an epoch offset stored in
     *    /bootloader/nyx.ini ([config] timeoffset=<hex>, optionally
     *    timedst=0|1) so its time display matches HOS. We parse the
     *    same fields to surface the same wall time in the dump when the
     *    user has set up the offset in Hekate. The MAX77620 raw remains
     *    on the line below for techs who want to see the underlying
     *    register state.
     */
    rtc_time_t raw = {0};
    max77620_rtc_get_time(&raw);
    bool unset = (raw.year <= 2010);
    log_color(unset ? COL_DEFAULT : COL_OK,
        "  RTC raw      : %04d-%02d-%02d %02d:%02d:%02d%s\n",
        raw.year, raw.month, raw.day, raw.hour, raw.min, raw.sec,
        unset ? "  (calendar unset, HOS keeps its own offset)" : "");

    int  nyx_offset    = 0;
    bool nyx_offset_ok = false;
    bool nyx_dst       = false;
    {
        FIL fp;
        FRESULT fr = f_open(&fp, "0:/bootloader/nyx.ini", FA_READ);
        if (fr == FR_OK) {
            /* nyx.ini is small (under 1 KiB in practice). Read into a
             * stack buffer and scan line-by-line for the two keys we
             * care about. We don't need a full INI parser, only
             * find `timeoffset=` and `timedst=` outside any other
             * section, accepting the same hex/decimal encodings
             * Hekate writes. */
            static char ini[2048];
            UINT br = 0;
            f_read(&fp, ini, sizeof(ini) - 1, &br);
            f_close(&fp);
            ini[br] = 0;

            char *p = ini;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                char *eol = p;
                while (*eol && *eol != '\n' && *eol != '\r') eol++;
                /* timeoffset=<hex>, possibly negative ("-e10"). */
                if (!nyx_offset_ok && (size_t)(eol - p) > 11 &&
                    memcmp(p, "timeoffset=", 11) == 0) {
                    char *v = p + 11;
                    int sign = 1;
                    if (*v == '-') { sign = -1; v++; }
                    int n = 0;
                    bool any = false;
                    while (v < eol) {
                        int hi = (*v >= '0' && *v <= '9') ? *v - '0'
                              : (*v >= 'a' && *v <= 'f') ? *v - 'a' + 10
                              : (*v >= 'A' && *v <= 'F') ? *v - 'A' + 10
                              : -1;
                        if (hi < 0) break;
                        n = (n << 4) | hi;
                        any = true;
                        v++;
                    }
                    if (any) {
                        nyx_offset    = sign * n;
                        nyx_offset_ok = true;
                    }
                } else if ((size_t)(eol - p) > 8 &&
                           memcmp(p, "timedst=", 8) == 0) {
                    nyx_dst = (p[8] == '1');
                }
                p = (*eol) ? eol + 1 : eol;
            }
        }
    }

    /* Hekate writes timeoffset=1 as a sentinel meaning "user opted out
     * of the offset": treat it as absent. Anything else is honoured. */
    if (nyx_offset_ok && nyx_offset != 0 && nyx_offset != 1) {
        max77620_rtc_set_epoch_offset(nyx_offset);
        max77620_rtc_set_auto_dst(nyx_dst);
        rtc_time_t adj = {0};
        max77620_rtc_get_time_adjusted(&adj);
        /* bdk's s_printf doesn't grok the %+d sign-forcing flag, so
         * print the offset's sign byte by hand. */
        log_color(COL_OK,
            "  RTC adjusted : %04d-%02d-%02d %02d:%02d:%02d"
            "  (nyx.ini offset %s%d s%s)\n",
            adj.year, adj.month, adj.day, adj.hour, adj.min, adj.sec,
            nyx_offset >= 0 ? "+" : "", nyx_offset,
            nyx_dst ? ", auto-DST" : "");
    } else if (unset) {
        LOG("  RTC adjusted : not available"
            " (no /bootloader/nyx.ini timeoffset)\n");
    }
}


void probe_gpio_census(void)
{
    HEADER("[GPIO pin census]");

    /* {label, pinmux_offset (0 = none), port, pin, role}
     * Roles must fit in <=18 chars so the rendered row stays under
     * LCD_COLS. The verbose form is in source comments above
     * each block. */
    struct pin_row {
        const char *label;     /* 4 chars padded ("PA5 " or "PCC3") */
        u32         pmx_off;
        u32         port;
        u32         pin;
        const char *role;      /* <= 18 chars; extra detail goes in comments */
    };
    static const struct pin_row rows[] = {
        /* 5V regulator master enable */
        { "PA5 ", PINMUX_AUX_SATA_LED_ACTIVE, GPIO_PORT_A, GPIO_PIN_5,
          "5V regulator EN"          /* FAN5333 enable */ },

        /* LCD pipeline */
        { "PI0 ", 0,                          GPIO_PORT_I, GPIO_PIN_0,
          "LCD AVDD CH2 EN"          /* +5.4 V */ },
        { "PI1 ", 0,                          GPIO_PORT_I, GPIO_PIN_1,
          "LCD AVDD CH1 EN"          /* -5.4 V */ },
        { "PV0 ", PINMUX_AUX_LCD_BL_PWM,      GPIO_PORT_V, GPIO_PIN_0,
          "LCD BL PWM"               /* PWM0 channel */ },
        { "PV1 ", PINMUX_AUX_LCD_BL_EN,       GPIO_PORT_V, GPIO_PIN_1,
          "LCD BL EN" },
        { "PV2 ", PINMUX_AUX_LCD_RST,         GPIO_PORT_V, GPIO_PIN_2,
          "LCD reset (act-low)" },

        /* Joy-Con R rail (UART_B path, our debug port) */
        { "PK3 ", PINMUX_AUX_GPIO_PK3,        GPIO_PORT_K, GPIO_PIN_3,
          "JC-R charge EN"           /* gates Tegra<->rail buffer */ },
        { "PG0 ", PINMUX_AUX_UART2_TX,        GPIO_PORT_G, GPIO_PIN_0,
          "UART2_TX (JC-R)" },
        { "PG1 ", PINMUX_AUX_UARTX_RX(UART_B),GPIO_PORT_G, GPIO_PIN_1,
          "UART2_RX (JC-R)" },
        { "PH6 ", PINMUX_AUX_GPIO_PH6,        GPIO_PORT_H, GPIO_PIN_6,
          "JC-R attach detect" },

        /* Joy-Con L rail (UART_C path) */
        { "PCC3", 0,                          GPIO_PORT_CC, GPIO_PIN_3,
          "JC-L charge EN" },
        { "PD1 ", PINMUX_AUX_UART3_TX,        GPIO_PORT_D, GPIO_PIN_1,
          "UART3_TX (JC-L)" },
        { "PD0 ", PINMUX_AUX_UARTX_RX(UART_C),GPIO_PORT_D, GPIO_PIN_0,
          "UART3_RX (JC-L)" },
        { "PE6 ", PINMUX_AUX_GPIO_PE6,        GPIO_PORT_E, GPIO_PIN_6,
          "JC-L attach detect" },

        /* Front-panel buttons */
        { "PX6 ", 0,                          GPIO_PORT_X, GPIO_PIN_6,
          "VOL+ btn (act-low)" },
        { "PX7 ", 0,                          GPIO_PORT_X, GPIO_PIN_7,
          "VOL- btn (act-low)" },
        { "PY1 ", 0,                          GPIO_PORT_Y, GPIO_PIN_1,
          "HOME btn (Lite SKU)" },
    };

    /* Compile-time check: a worst-case rendered row must fit in 80
     * chars. The "WIDEST_GPIO_ROW" template below mirrors the format
     * string with each %-substitution replaced by the widest value it
     * can produce. sizeof()-1 strips the trailing NUL. If a future
     * format change blows past LCD_COLS, the build fails here
     * with a clear message instead of silently clipping pixels. */
#define WIDEST_GPIO_ROW \
    "  PCC3 : fn=X pull-up tri ie=X SPIO OE=X OUT=X IN=X  (XXXXXXXXXXXXXXXXXX)"
    _Static_assert(sizeof(WIDEST_GPIO_ROW) - 1 <= LCD_COLS,
                   "GPIO census row format overflows the LCD width");
#undef WIDEST_GPIO_ROW

    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        const struct pin_row *r = &rows[i];
        u32 base = ((r->port >> 2) << 8) + ((r->port & 3) << 2);
        u8  cnf = GPIO(base + 0x00);
        u8  oe  = GPIO(base + 0x10);
        u8  out = GPIO(base + 0x20);
        u8  in  = GPIO(base + 0x30);
        bool gpio_mode = (cnf & r->pin) != 0;

        if (r->pmx_off) {
            u32 pmx  = PINMUX_AUX(r->pmx_off);
            u32 fn   = pmx & PINMUX_FUNC_MASK;
            bool tri = pmx & PINMUX_TRISTATE;
            bool ie  = pmx & PINMUX_INPUT_ENABLE;
            /* Pre-pad pull string to 7 chars; bdk's s_printf doesn't
             * support the %-Ns left-justified width flag. */
            char pull_pad[8];
            const char *p = _pmx_pull_str(pmx);
            u32 plen = strlen(p);
            if (plen > 7) plen = 7;
            memcpy(pull_pad, p, plen);
            for (u32 k = plen; k < 7; k++) pull_pad[k] = ' ';
            pull_pad[7] = 0;
            /* Format kept compact to fit LCD_COLS. The pmx hex
             * value is not printed - all four decoded fields
             * (fn / pull / tri / ie) carry the same info more
             * readably. The SD-card-saved report has full detail.
             *
             * Pin name is the key, everything else is the value, with
             * a colon between, this makes the host viewer render the
             * page as a proper table (key column = pin label, value
             * column = decoded state) instead of free-form notes. */
            LOG("  %s : fn=%d %s %s ie=%d %s OE=%d OUT=%d IN=%d  (%s)\n",
                r->label, fn, pull_pad,
                tri ? "tri" : "drv", !!ie,
                gpio_mode ? "GPIO" : "SPIO",
                !!(oe & r->pin), !!(out & r->pin), !!(in & r->pin),
                r->role);
        } else {
            /* No dedicated PINMUX entry for this pin (front-panel
             * buttons, charge-EN gating that uses GPIO directly). */
            LOG("  %s : pmx=(n/a)              %s OE=%d OUT=%d IN=%d  (%s)\n",
                r->label,
                gpio_mode ? "GPIO" : "SPIO",
                !!(oe & r->pin), !!(out & r->pin), !!(in & r->pin),
                r->role);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* UART debug port (UART_B / Joy-Con R rail)                                */
/*                                                                          */
/* The Joy-Con-R chassis-side debug header carries UART_B (115200 8N1, 1.8  */
/* V logic). hwtest writes log lines out of it; this probe reports the      */
/* full RX-side configuration so a chassis-adapter wiring issue can be      */
/* localised without scope or multimeter:                                   */
/*                                                                          */
/*   host USB-UART -> chassis cable -> chassis-side level shifter           */
/*                  (powered from the JC-R 5V rail enabled via PA5)         */
/*       -> chassis connector -> Tegra side buffer (gated by PK3 high)      */
/*       -> Tegra pad PG1 (UART2_RX) -> UART2 RX FIFO -> UART_LSR.RDR=1     */
/*                                                                          */
/* If ANY of those links breaks, RX stops working but TX still does because */
/* the return path is asymmetric. We dump pinmux, the relevant GPIO         */
/* enables driving the level-shifter rail, the UART controller registers,   */
/* and a 10 ms sample of the line via GPIO so:                              */
/*                                                                          */
/*   line stuck LOW   = level shifter unpowered or shorted to GND           */
/*   line idle HIGH    = shifter alive, host quiet                          */
/*   line transitions  = host adapter is sending; if no bytes land in FIFO  */
/*                       check baud / framing                               */
/*                                                                          */
/* Purely passive and read-only. After sampling PG1 we restore SPIO so the  */
/* UART regains control of the pin for the rest of the session. */
#ifndef JC_PROBE
void probe_uart_b(void)
{
    HEADER("[UART_B debug port]");

    /* 1. Pinmux for PG1 (UART2_RX). Must be function 0 (UART) with input
     *    enable on and SOMETHING anchoring the idle state HIGH - either
     *    an internal pull-up (switch-coreboot's choice, idles HIGH on
     *    its own) or pure tristate (Hekate's default, relies on the
     *    external driver to hold the line HIGH between bytes - works
     *    for Joy-Cons but not for passive USB-UART chassis adapters).
     *    PINMUX_AUX_UARTX_RX(idx) for UART_B resolves to offset 0xF8;
     *    the BDK only exposes the indexed helper, no UART2-specific
     *    macro. */
    u32 pmx = PINMUX_AUX(PINMUX_AUX_UARTX_RX(UART_B));
    u32 fn  = pmx & PINMUX_FUNC_MASK;
    bool tri = pmx & PINMUX_TRISTATE;
    bool ie  = pmx & PINMUX_INPUT_ENABLE;
    u32 pull = pmx & PINMUX_PULL_MASK;
    const char *pull_name = pull == PINMUX_PULL_UP   ? "pull-up"
                          : pull == PINMUX_PULL_DOWN ? "pull-down"
                          : pull == PINMUX_PULL_NONE ? "no-pull"
                          : "?";
    LOG("  UART2_RX pmx : 0x%08X\n", pmx);
    log_color((fn == 0 && ie) ? COL_OK : COL_ERR,
        "  pad config   : fn=%d %s, %s, %s, %s\n", fn,
        fn == 0 ? "(UART)" : "(WRONG)",
        tri ? "tristate" : "drive-allowed",
        pull_name,
        ie ? "input-enable" : "INPUT-DISABLED");
    if (pull == PINMUX_PULL_UP)
        log_color(COL_OK,
            "  idle anchor  : internal PULL_UP (line idles HIGH on its own)\n");
    else if (tri && pull == PINMUX_PULL_NONE) {
        log_color(COL_WARN,
            "  idle anchor  : tristate only (line floats unless driven)\n");
        log_color(COL_WARN,
            "                 OK with Joy-Cons, fails on passive USB-UART\n");
    }
    else if (pull == PINMUX_PULL_DOWN)
        log_color(COL_ERR,
            "  idle anchor  : PULL_DOWN (line idles LOW - UART will see permanent BREAK)\n");

    /* 2. PA5 = master 5V regulator EN, and 3. PK3 = Joy-Con-R buffer /
     *    charge enable. Both are driven as GPIO outputs by Hekate but
     *    the corresponding PINMUX_AUX entries leave INPUT_ENABLE off
     *    (PINMUX_AUX_SATA_LED_ACTIVE = 1, PINMUX_AUX_GPIO_PK3 = drive
     *    + pull-down + fn2). With the input buffer disabled, gpio_read()
     *   , which samples the IN register, always returns 0 even when
     *    we're actively driving HIGH. So we read OUT (what we wrote)
     *    plus CNF (GPIO vs SPIO mode) and OE (output enabled) directly,
     *    bypassing the BDK helper. Tegra X1 GPIO bank layout:
     *      bank base    = (port >> 2) << 8
     *      port slot    = (port & 3) << 2
     *      CNF=+0x00, OE=+0x10, OUT=+0x20, IN=+0x30 */
    {
        u32 base_a = ((GPIO_PORT_A >> 2) << 8) + ((GPIO_PORT_A & 3) << 2);
        u8  cnf_a = GPIO(base_a + 0x00);
        u8  oe_a  = GPIO(base_a + 0x10);
        u8  out_a = GPIO(base_a + 0x20);
        u8  in_a  = GPIO(base_a + 0x30);
        bool driven_high_a = (cnf_a & GPIO_PIN_5) && (oe_a & GPIO_PIN_5)
                             && (out_a & GPIO_PIN_5);
        log_color(driven_high_a ? COL_OK : COL_ERR,
            "  PA5 (5V_EN)  : %s   (CNF=%d OE=%d OUT=%d IN=%d)\n",
            driven_high_a ? "HIGH (driven)" : "NOT driven HIGH",
            !!(cnf_a & GPIO_PIN_5), !!(oe_a & GPIO_PIN_5),
            !!(out_a & GPIO_PIN_5), !!(in_a & GPIO_PIN_5));
    }
    {
        u32 base_k = ((GPIO_PORT_K >> 2) << 8) + ((GPIO_PORT_K & 3) << 2);
        u8  cnf_k = GPIO(base_k + 0x00);
        u8  oe_k  = GPIO(base_k + 0x10);
        u8  out_k = GPIO(base_k + 0x20);
        u8  in_k  = GPIO(base_k + 0x30);
        bool driven_high_k = (cnf_k & GPIO_PIN_3) && (oe_k & GPIO_PIN_3)
                             && (out_k & GPIO_PIN_3);
        log_color(driven_high_k ? COL_OK : COL_ERR,
            "  PK3 (JC-R EN): %s   (CNF=%d OE=%d OUT=%d IN=%d)\n",
            driven_high_k ? "HIGH (driven)" : "NOT driven HIGH",
            !!(cnf_k & GPIO_PIN_3), !!(oe_k & GPIO_PIN_3),
            !!(out_k & GPIO_PIN_3), !!(in_k & GPIO_PIN_3));
    }
    /* Also report the software-side regulator state (what reg_5v_dev
     * tracks). On Mariko PA5 is gated by both this and the FAN5333 IC's
     * own enable detection; the [5V regulator] page already shows
     * regulator_5v_get_dev_enabled() per device. */

    /* 4. UART_B controller registers. UART_B base = 0x70006040. */
    volatile u32 *uart = (volatile u32 *)0x70006040;
    u8  lcr  = uart[3] & 0xFF;   /* offset 0x0C */
    u8  mcr  = uart[4] & 0xFF;   /* offset 0x10 */
    u8  lsr  = uart[5] & 0xFF;   /* offset 0x14 */
    u32 vstat = uart[11];        /* offset 0x2C VENDOR_STATUS */
    u32 rx_count = (vstat >> 8) & 0x3F;
    LOG("  UART_LCR     : 0x%02X (DLAB=%d, word=%d)\n",
        lcr, (lcr >> 7) & 1, (lcr & 3) + 5);
    LOG("  UART_MCR     : 0x%02X\n", mcr);

    /* Baud-rate divisor (DLL / DLM) — shadowed at offsets 0x00 / 0x04
     * when LCR.DLAB=1. We flip DLAB, snapshot the divisor latches,
     * then restore the original LCR. The whole sequence completes in
     * microseconds — no in-flight TX bytes are lost (THR continues to
     * empty since the shift register is independent of the latch
     * mux). A divisor of 0 means the UART was never initialised.
     *
     * The UART input clock is *not* OSC. Tegra X1 routes it through
     * CLK_SOURCE_UARTB (CLOCK 0x17C):
     *   bits 31:30 = source: 00=PLLP_OUT0 (408 MHz fixed),
     *                01=PLLC2_OUT0, 10=PLLC_OUT0, 11=CLK_M (= OSC).
     *   bit   24   = UART_SRC_CLK_DIV_EN — when 0 the source-side
     *                divisor is BYPASSED (clock passes straight
     *                through); when 1 it's applied. Hekate only sets
     *                bit 24 for the 1 M / 3 M baud paths; for 115 200
     *                the divisor field still reads 2 but the divider
     *                is bypassed, so UART_B input = 408 MHz directly.
     *   bits  7:0  = N (encodes 1+N/2 fractional division when bit 24
     *                is set; ignored when not).
     * Canonical 115 200 baud: src=0, bit 24=0, divisor latch=221 →
     * effective baud = 408_000_000 / (16 * 221) = 115_345 (0.13 %
     * fast — well within UART tolerance). */
    uart[3] = (lcr | 0x80);          /* set DLAB */
    u8 dll = uart[0] & 0xFF;
    u8 dlm = uart[1] & 0xFF;
    uart[3] = lcr;                   /* restore */
    u32 divisor = ((u32)dlm << 8) | dll;

    u32 clk_src_reg = CLOCK(0x17C);
    u32 clk_src     = (clk_src_reg >> 30) & 0x3;
    bool src_div_en = (clk_src_reg & (1u << 24)) != 0;
    u32 clk_div_n   = clk_src_reg & 0xFF;
    u32 src_khz;
    const char *src_name;
    switch (clk_src) {
    case 0: src_khz = 408000;                src_name = "PLLP_OUT0"; break;
    case 3: src_khz = clock_get_osc_freq();  src_name = "CLK_M";     break;
    /* PLLC / PLLC2 paths exist but hekate doesn't use them for UART. */
    default: src_khz = 0;                    src_name = "PLLC?";     break;
    }
    /* When bit 24 is clear the source-side divider is bypassed - input
     * == source. When set, input = src * 2 / (N + 2). */
    u32 uart_in_khz = !src_khz ? 0
                      : src_div_en ? (src_khz * 2) / (clk_div_n + 2)
                                   : src_khz;
    u32 baud = (divisor && uart_in_khz)
               ? (uart_in_khz * 1000) / (16 * divisor) : 0;
    LOG("  CLK_SOURCE   : 0x%08X (src=%s, src-div=%s, %d kHz in)\n",
        clk_src_reg, src_name,
        src_div_en ? "ON" : "bypassed", uart_in_khz);
    log_color((divisor && baud >= DEBUG_UART_BAUDRATE * 95 / 100
                       && baud <= DEBUG_UART_BAUDRATE * 105 / 100)
              ? COL_OK : COL_WARN,
        "  Baud divisor : %d (DLL=0x%02X DLM=0x%02X) -> %d baud (target %d)\n",
        divisor, dll, dlm, baud, DEBUG_UART_BAUDRATE);
    LOG("  UART_LSR     : 0x%02X (%s%s%s%s%s%s%s%s)\n", lsr,
        (lsr & 0x80) ? "FIFOE "  : "",
        (lsr & 0x40) ? "TMTY "   : "",
        (lsr & 0x20) ? "THRE "   : "",
        (lsr & 0x10) ? "BRK "    : "",
        (lsr & 0x08) ? "FERR "   : "",
        (lsr & 0x04) ? "PERR "   : "",
        (lsr & 0x02) ? "OVRF "   : "",
        (lsr & 0x01) ? "RDR"     : "");
    /* FERR / PERR / BRK suggest baud-rate mismatch or wrong framing. */
    if (lsr & 0x1C)
        log_color(COL_ERR,
            "                 ^- framing/parity/break errors latched - check baud\n");
    LOG("  VENDOR_STAT  : 0x%08X (RX_FIFO=%d byte(s))\n", vstat, rx_count);

    /* 5. Sample PG1 as a raw GPIO for ~10 ms. UART idle state is HIGH
     *    (mark = 1). If the line is dead, we'll see it stuck LOW or
     *    floating. If the host adapter is currently transmitting, we
     *    should see edges. We restore SPIO before returning. */
    /* Section header so the line sample shows up as its own
     * collapsible table in the host viewer instead of a stray note. */
    log_color(COL_INFO, "[raw PG1 sample (10 ms via GPIO)]\n");
    gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_GPIO);
    int high = 0, low = 0, edges = 0, last = -1;
    u32 end = get_tmr_us() + 10000;
    while ((s32)(end - get_tmr_us()) > 0) {
        int v = gpio_read(GPIO_PORT_G, GPIO_PIN_1);
        if (v) high++; else low++;
        if (last != -1 && v != last) edges++;
        last = v;
    }
    gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_SPIO);  /* hand back to UART */
    LOG("    samples      : %d high, %d low, %d edges\n", high, low, edges);
    if (edges > 0) {
        log_color(COL_OK,
            "    Verdict      : line ACTIVE (%d transitions) - host IS sending\n", edges);
    } else if (high > 0 && low == 0) {
        log_color(COL_OK,
            "    Verdict      : line idle HIGH - shifter alive, host quiet\n");
    } else if (low > 0 && high == 0) {
        log_color(COL_ERR,
            "    Verdict      : line stuck LOW - shifter unpowered or shorted to GND\n");
    } else {
        log_color(COL_WARN,
            "    Verdict      : mixed (%d high / %d low / no edges) - line floating\n",
            high, low);
    }

}
#endif  /* !JC_PROBE */

