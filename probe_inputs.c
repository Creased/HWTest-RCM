/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: inputs probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* Physical input census - buttons + Joy-Con rail attach + AC adapter       */
/*                                                                          */
/* Surface every human-input pin we can read in one place so a repair tech  */
/* can verify each switch / contact in a single page. All checks are pure   */
/* GPIO / I2C reads, no debounce / no wait.                                 */
/*                                                                          */
/*   POWER       : MAX77620 ONOFFSTAT.EN0 over I2C5 (active high when       */
/*                 button is pressed). Erista routes the same line to a     */
/*                 GPIO too but the resistor is missing on retail boards.   */
/*   VOL+        : PX6 active-low (pressed = 0)                             */
/*   VOL-        : PX7 active-low                                           */
/*   HOME (Sio)  : PY1 active-low; only HOAG (Switch Lite) has it physical  */
/*   JC-R rail   : PH6 active-low (Joy-Con or chassis adapter pulls down)   */
/*   JC-L rail   : PE6 active-low                                           */
/*   AC adapter  : MAX77620 ONOFFSTAT.ACOK = bit 1                          */
/*                                                                          */
/* On a healthy console with no buttons pressed and no Joy-Cons attached    */
/* every line reads "released / empty". With a chassis adapter on JC-R the  */
/* corresponding line reads "ATTACHED" - useful confirmation that the rail  */
/* connector spring contacts are seating properly. */
void probe_inputs(void)
{
    HEADER("[Input census]");

    /* Power & AC-OK from MAX77620 ONOFFSTAT (reg 0x15). */
    u8 onoffstat = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFSTAT);
    bool power_pressed = (onoffstat & MAX77620_ONOFFSTAT_EN0) != 0;
    bool ac_ok         = (onoffstat & (1u << 1)) != 0;
    log_color(power_pressed ? COL_WARN : COL_OK,
        "  POWER btn  : %s   (ONOFFSTAT.EN0=%d)\n",
        power_pressed ? "PRESSED" : "released", power_pressed);
    log_color(ac_ok ? COL_OK : COL_DEFAULT,
        "  AC adapter : %s    (ONOFFSTAT.ACOK=%d)\n",
        ac_ok ? "present" : "absent ", ac_ok);
    LOG("  ONOFFSTAT  : 0x%02X\n", onoffstat);

    /* VOL +/- = PX6 / PX7, active low. We can't use btn_read() because it
     * also returns POWER and we want each pin individually. */
    int vol_up   = gpio_read(GPIO_PORT_X, GPIO_PIN_6);
    int vol_down = gpio_read(GPIO_PORT_X, GPIO_PIN_7);
    log_color(vol_up   ? COL_OK : COL_WARN, "  VOL+ btn   : %s   (PX6=%d)\n",
        vol_up   ? "released" : "PRESSED ", vol_up);
    log_color(vol_down ? COL_OK : COL_WARN, "  VOL- btn   : %s   (PX7=%d)\n",
        vol_down ? "released" : "PRESSED ", vol_down);

    /* HOME = PY1, active low. Only HOAG (Switch Lite) physically routes
     * this; on Icosa/Iowa/AULA the trace exists but no switch is fitted,
     * so it idles HIGH (or floating, depending on internal pull). */
    bool is_hoag = (fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG);
    int home = gpio_read(GPIO_PORT_Y, GPIO_PIN_1);
    if (is_hoag) {
        log_color(home ? COL_OK : COL_WARN,
            "  HOME btn   : %s   (PY1=%d, Lite-only)\n",
            home ? "released" : "PRESSED ", home);
    } else {
        log_color(COL_DEFAULT,
            "  HOME btn   : N/A (no physical switch on this SKU)   (PY1=%d)\n",
            home);
    }

    /* Joy-Con rail attach: PH6 (R) and PE6 (L), active low. Hekate's
     * _jc_rail_detect briefly enables UART2_TX/UART3_TX as inputs and
     * unlatches via a dummy gpio_read first; for a passive census we
     * skip the unlatch dance and just sample the GPIOs directly.
     * Active-low because a connected Joy-Con (or chassis adapter that
     * shorts the detect line) pulls PH6/PE6 to GND. */
    int jc_r = gpio_read(GPIO_PORT_H, GPIO_PIN_6);
    int jc_l = gpio_read(GPIO_PORT_E, GPIO_PIN_6);
    log_color(!jc_r ? COL_OK : COL_DEFAULT,
        "  JC-R rail  : %s    (PH6=%d, IsAttached active-low)\n",
        !jc_r ? "ATTACHED" : "empty   ", jc_r);
    log_color(!jc_l ? COL_OK : COL_DEFAULT,
        "  JC-L rail  : %s    (PE6=%d, IsAttached active-low)\n",
        !jc_l ? "ATTACHED" : "empty   ", jc_l);

    /* MAX77620 power-button + wake-source configuration. Two registers:
     *   ONOFFCNFG1 (0x41):
     *     bit 7    SFT_RST   - 1=SFT shutdown on long-press / 0=hard reset
     *     bits 5:3 MRT       - manual reset hold time (encoded 2..16 s)
     *     bit 2    SLPEN     - SLEEP enable
     *     bit 1    PWR_OFF   - power-off command latch
     *   ONOFFCNFG2 (0x42):
     *     bit 7 SFT_RST_WK   - wake from sleep on soft-reset event
     *     bit 6 WD_RST_WK    - wake on watchdog reset
     *     bit 4 WK_ACOK      - wake on AC-adapter insert
     *     bit 3 WK_MBATT     - wake on main-battery event
     *     bit 2 WK_ALARM1    - wake on RTC alarm 1
     *     bit 1 WK_ALARM2    - wake on RTC alarm 2
     *     bit 0 WK_EN0       - wake on POWER button press
     * For repair: a tech seeing "POWER btn does nothing" can verify
     * WK_EN0 isn't masked here. A console that wakes spontaneously
     * usually has an unintended wake source set (WK_MBATT on a
     * deteriorating battery is the classic). */
    u8 cnfg1 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG1);
    u8 cnfg2 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG2);
    u8 mrt   = (cnfg1 & MAX77620_ONOFFCNFG1_MRT_MASK) >> MAX77620_ONOFFCNFG1_MRT_SHIFT;
    /* MRT encoding: each tick = 2 s, range 2..16 s. Switch HOS programs
     * 8 s (mrt code 011) — a held POWER triggers a hard reset after 8 s,
     * matching the documented "press and hold POWER for 8 seconds to
     * force-reset" behavior. */
    LOG("  ONOFFCNFG1 : 0x%02X (MRT=%d -> %d s, %s)\n",
        cnfg1, mrt, (mrt + 1) * 2,
        (cnfg1 & MAX77620_ONOFFCNFG1_SFT_RST) ? "long-press = SFT shutdown"
                                              : "long-press = hard reset");
    LOG("  ONOFFCNFG2 : 0x%02X (wake:%s%s%s%s%s%s%s)\n", cnfg2,
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_EN0)    ? " POWER"   : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_ACOK)   ? " ACOK"    : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_MBATT)  ? " MBATT"   : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_ALARM1) ? " ALARM1"  : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_ALARM2) ? " ALARM2"  : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WD_RST_WK) ? " WDOG"    : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_SFT_RST_WK)? " SFT-RST" : "");
}

/* ------------------------------------------------------------------------ */
/* Joy-Con rail attachment                                                  */
/*                                                                          */
/* UART_B remains dedicated to the 115200 diagnostic console, so hwtest does
 * not take ownership of the right Joy-Con serial protocol. The two physical
 * IsAttached inputs are still useful repair evidence and are safe to sample. */

void probe_joycon(void)
{
    HEADER("[Joy-Con rail attachment]");

    int jc_r_attached = !gpio_read(GPIO_PORT_H, GPIO_PIN_6);
    int jc_l_attached = !gpio_read(GPIO_PORT_E, GPIO_PIN_6);
    log_color(jc_r_attached ? COL_OK : COL_DEFAULT,
        "  JC-R rail     : %s (PH6 IsAttached)\n",
        jc_r_attached ? "ATTACHED" : "empty   ");
    log_color(jc_l_attached ? COL_OK : COL_DEFAULT,
        "  JC-L rail     : %s (PE6 IsAttached)\n",
        jc_l_attached ? "ATTACHED" : "empty   ");
    LOG("  serial poll   : not used; UART_B stays on hwtest debug\n");
}


void probe_touch(void)
{
    HEADER("[Touch panel (FTS4, I2C3 @ 0x49)]");

    /* Do not call BDK touch_power_on() here. That helper contains Hekate's
     * maintenance shortcut: if VOL+ and VOL- are both held it runs the ITO
     * production test and, when that passes, executes autotune followed by
     * FTS4_CMD_SAVE_CX_TUNING. The save command writes compensation data to
     * the touch controller's EEPROM, which is not an acceptable implicit
     * side effect for a repair sweep.
     *
     * Reproduce only the public BDK power/reset/sense sequence and omit the
     * button-triggered calibration branch. touch_sys_reset() is global in
     * BDK touch.c but not part of the public header, so declare it locally. */
    extern int touch_sys_reset(void);
    PINMUX_AUX(PINMUX_AUX_DAP4_SCLK) = PINMUX_PULL_DOWN | 1;
    gpio_direction_output(GPIO_PORT_J, GPIO_PIN_7, GPIO_LOW);
    usleep(20);
    max7762x_regulator_set_voltage(REGULATOR_LDO6, 2900000);
    max7762x_regulator_enable(REGULATOR_LDO6, true);
    pinmux_config_i2c(I2C_3);
    clock_enable_i2c(I2C_3);
    i2c_init(I2C_3);
    usleep(1000);
    gpio_write(GPIO_PORT_J, GPIO_PIN_7, GPIO_HIGH);
    msleep(10);

    int power_rc = touch_sys_reset();
    if (!power_rc)
        power_rc = touch_sense_enable();
    log_color(power_rc ? COL_ERR : COL_OK,
        "  power-on     : %s\n", power_rc ? "FAILED (panel cable / IC fault?)" : "OK");
    if (power_rc) {
        dx_set("touch_id", DX_FAIL, "safe touch init failed");
        dx_set("touch_ito", DX_WARN, "not run because touch init failed");
        touch_power_off();
        return;
    }

    touch_info_t *info = touch_get_chip_info();
    log_color(info->chip_id == 0x3670 ? COL_OK : COL_ERR,
        "  chip_id      : 0x%04X\n", info->chip_id);
    dx_set("touch_id", info->chip_id == 0x3670 ? DX_PASS : DX_FAIL,
        info->chip_id == 0x3670 ? "" : "chip_id 0x%04X != FTS4", info->chip_id);
    LOG("  fw_ver       : 0x%04X\n", info->fw_ver);
    LOG("  config_id    : 0x%02X\n", info->config_id);
    LOG("  config_ver   : 0x%02X\n", info->config_ver);
    log_color(info->clone ? COL_WARN : COL_OK,
        "  clone        : %s\n", info->clone ? "yes" : "no");

    touch_panel_info_t *panel = touch_get_panel_vendor();
    int panel_idx = panel ? (s8)panel->idx : -2;
    if (panel)
        LOG("  vendor       : %s (idx %d)\n", panel->vendor, panel_idx);
    else
        log_color(COL_ERR, "  vendor       : <error>\n");

    touch_fw_info_t fw = {0};
    if (touch_get_fw_info(&fw) == 0) {
        /* Pairing table mirrors Hekate gui_info.c:1264. */
        struct { u32 fw_id; int idx; } pairs[] = {
            {0x00100100, -1}, {0x00100200, 0}, {0x00120100, 0},
            {0x32000001, 0},  {0x001A0300, 1}, {0x32000102, 1},
            {0x00290100, 2},  {0x32000302, 2}, {0x31051820, 3},
            {0x32000402, 3},  {0x32000501, 4}, {0x33000502, 4},
            {0x33000503, 4},  {0x33000510, 4}, {0xFFFFFFFF, -1},
        };
        int expected = -3;
        for (u32 i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
            if (pairs[i].fw_id == fw.fw_id) { expected = pairs[i].idx; break; }
        bool paired = (expected != -3 && expected == panel_idx);

        LOG("  fw_id        : %02X.%02X.%02X.%02X\n",
            (fw.fw_id >> 24) & 0xFF, (fw.fw_id >> 16) & 0xFF,
            (fw.fw_id >>  8) & 0xFF,  fw.fw_id        & 0xFF);
        LOG("  ftb_ver      : 0x%04X\n", fw.ftb_ver);
        LOG("  fw_rev       : 0x%04X (display %02X%02X)\n",
            fw.fw_rev, fw.fw_rev & 0xFF, (fw.fw_rev >> 8) & 0xFF);
        log_color(paired ? COL_OK : COL_ERR,
            "  pairing      : %s\n", paired ? "OK" : "MISMATCH");
    } else {
        log_color(COL_ERR, "  fw read FAILED\n");
    }

    /* FTS4's ITO production command is exactly the electrical test a repair
     * tool wants: it checks force/sense opens and shorts and returns an error
     * class plus the affected line. It resets the controller before/after the
     * test but does NOT save compensation data. Keep this separate from the
     * EEPROM-writing autotune path above. */
    if (info->chip_id != FTS4_I2C_CHIP_ID || info->clone) {
        log_color(COL_WARN,
            "  ITO test     : skipped (unsupported/clone controller)\n");
        dx_set("touch_ito", DX_WARN, "unsupported or clone controller");
    } else {
        static const char *ito_err[] = {
            "no error",
            "force open", "sense open",
            "force-GND short", "sense-GND short",
            "force-VCM short", "sense-VCM short",
            "force-force short", "sense-sense short",
            "force-sense short", "FPC force open", "FPC sense open",
            "key force open", "key sense open",
            "reserved", "reserved", "reserved"
        };
        u8 err[2] = {0};
        int ito_rc = touch_panel_ito_test(err);
        if (ito_rc) {
            log_color(COL_WARN, "  ITO test     : no result within timeout\n");
            dx_set("touch_ito", DX_WARN, "ITO command timed out");
        } else if (!err[0] && !err[1]) {
            log_color(COL_OK, "  ITO test     : PASS, no opens/shorts\n");
            dx_set("touch_ito", DX_PASS, "");
        } else {
            const char *why = err[0] < ARRAY_SIZE(ito_err) ? ito_err[err[0]] :
                              err[0] == ITO_MAX_ERR_REACHED ? "max errors" : "unknown";
            log_color(COL_ERR, "  ITO test     : FAIL err %02X pin %d (%s)\n",
                err[0], err[1], why);
            dx_set("touch_ito", DX_FAIL, "err %02X pin %d %s", err[0], err[1], why);
        }
    }

    /* Leave the controller reset and the shared touch/ALS rail off. The ALS
     * probe follows this page and powers LDO6 back up for itself. */
    touch_power_off();
}

/* Rohm BH1730 ambient-light sensor on I2C2 @ 0x29. The ALS sits on the
 * front bezel near the speaker grille and drives auto-brightness on
 * stock HOS. A failing ALS makes the screen stuck at min or max
 * brightness regardless of light, which a tech easily mistakes for a
 * backlight or panel fault. Probe surfaces:
 *   - chip ID byte (BH1730 returns 0x71: part 0x7, rev 0x1)
 *   - configured gain / cycle (driver default = 64x, 38)
 *   - raw visible / IR ADC counts after one integration window
 *   - decoded lux (over-limit flagged when any channel saturates).
 * als_power_on() enables LDO6 (the touch probe deliberately powers that
 * shared rail back down first), pinmuxes I2C2, and triggers a continuous-conversion
 * mode. Default integration time is ~103 ms (cycle * 2.7 ms). */
void probe_als(void)
{
    HEADER("[BH1730 ambient light sensor, I2C2 @ 0x29]");
    /* Switch Lite (HOAG) ships without the BH1730 — Nintendo omitted
     * the ambient-light sensor on Lite (which is why auto-brightness
     * is absent from Lite settings). Reading the I2C bus there gets
     * NACK -> 0x00, which would otherwise false-FAIL the Inputs
     * verdict. Short-circuit on HOAG with a clear "N/A" line and a
     * neutral verdict. */
    if (fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG) {
        log_color(COL_DEFAULT,
            "  N/A          : Switch Lite has no ALS (auto-brightness omitted)\n");
        dx_set("als_id", DX_PASS, "N/A on Lite");
        return;
    }
    als_ctxt_t ctxt = {0};
    u8 id = als_power_on(&ctxt);
    bool id_ok = (id & 0xF0) == 0x70;
    log_color(id_ok ? COL_OK : COL_ERR,
        "  ID           : 0x%02X%s\n", id,
        id_ok ? " (BH1730 verified)" : " (UNEXPECTED, no chip / I2C fault?)");
    dx_set("als_id", id_ok ? DX_PASS : DX_FAIL,
        id_ok ? "" : "ID 0x%02X != BH1730", id);
    if (!id_ok)
        return;

    LOG("  Gain / cycle : %dx / %d\n",
        (int[]){1,2,64,128}[ctxt.gain & 0x3], ctxt.cycle);

    /* Wait one integration window (~103 ms at default cycle=38) so the
     * first ADC sample is meaningful. The driver's continuous-conversion
     * mode will keep updating after this. */
    msleep_poll(110);
    get_als_lux(&ctxt);
    log_color(ctxt.over_limit ? COL_WARN : COL_OK,
        "  Visible      : %d counts%s\n", ctxt.vi_light,
        ctxt.over_limit ? " (saturated)" : "");
    LOG("  IR           : %d counts\n", ctxt.ir_light);
    LOG("  Lux          : %d\n", ctxt.lux);
}

