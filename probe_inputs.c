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
/* Joy-Con detection + version (UART_B / UART_C HID)                        */
/*                                                                          */
/* The Joy-Con MCUs talk to the Switch over UART_B (right) and UART_C       */
/* (left) at 1 Mbaud. Detecting them physically is just two GPIO reads      */
/* (PH6 / PE6) and we already do that in the input census; this probe       */
/* adds the *interactive* layer - bringing up the rails, polling the        */
/* Joy-Cons via Hekate's BDK, and reporting battery + BT MAC + connection   */
/* type.                                                                    */
/*                                                                          */
/* It only does the polling in the JC_PROBE Makefile build because Hekate's */
/* joycon.c body is gated on `#if !defined(DEBUG_UART_PORT) ||              */
/* !(DEBUG_UART_PORT)`, in the default build UART_B carries our debug    */
/* log, which is electrically the same line a real Joy-Con-R would speak    */
/* on, so the two uses are mutually exclusive at compile time.              */
/*                                                                          */
/* Build with `make JC_PROBE=1` to drop UART debug and pick up Joy-Con      */
/* polling for both rails. The default build still surfaces the IsAttached  */
/* bits so you know whether a Joy-Con / chassis adapter is physically       */
/* present, just without protocol-level info. */

#ifdef JC_PROBE
/* ------------------------------------------------------------------------ */
/* Joy-Con HID device-info subcommand (0x02) - firmware version fetcher    */
/*                                                                          */
/* Hekate's joycon.c retrieves MAC + type at init via the wired             */
/* JC_WIRED_CMD_GET_INFO (subcmd 0x01) but never sends the *HID*-level     */
/* REQUEST_DEVICE_INFO (subcmd 0x02) which is what carries the firmware     */
/* version major/minor. The HID transport over UART works after the         */
/* Joy-Con has handshaked (jc->state >= HID_CONN). We rebuild the wire      */
/* packet manually here so we don't have to fork joycon.c just to add this  */
/* one command.                                                             */
/*                                                                          */
/* Wire format (from Hekate's jc_wired_hdr_t / jc_hid_out_rpt_t):           */
/*                                                                          */
/*   send (host -> Joy-Con):                                                */
/*     19 01 03                  jc_uart_hdr_t magic                        */
/*     12 00                     total_size LE = sizeof(wired_hdr) - 5 + 11 */
/*     92                        cmd = JC_WIRED_HID                         */
/*     00                        subcmd (header-level)                      */
/*     0B 00                     payload_size LE = 11                       */
/*     00                        status                                     */
/*     00                        crc_payload (not enforced)                 */
/*     XX                        crc_hdr = CRC8(cmd..crc_payload, 6 bytes) */
/*    , HID output report payload (11 bytes),                           */
/*     01                        cmd = JC_HID_OUTPUT_RPT                    */
/*     00                        pkt_id (rolling, 0..F)                     */
/*     00 01 40 40 00 01 40 40   rumble (mute pattern)                      */
/*     02                        subcmd = REQUEST_DEVICE_INFO               */
/*                                                                          */
/*   receive (Joy-Con -> host):                                             */
/*     19 81 03                  RX magic                                   */
/*     ...wired header...        cmd 0x92                                   */
/*    , HID input subcmd reply payload,                                 */
/*     21                        cmd = JC_HID_SUBMCD_RPT                    */
/*     pkt_id, conn+batt, btn x3, lstick x3, rstick x3, vib  (12 bytes)    */
/*     82                        submcd_ack (= 0x80 | subcmd)               */
/*     02                        subcmd                                     */
/*     fw_major fw_minor jc_type 02 mac[6:reversed] 01 spi_color  (subcmd_data) */
/*                                                                          */
/* CRC8 polynomial 0x8D, MSB-first - copied from Hekate's _jc_crc.         */
static u8 _jc_crc8(const u8 *data, u16 len)
{
    u8 crc = 0;
    for (u16 i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (u8)((crc << 1) ^ 0x8D) : (u8)(crc << 1);
    }
    return crc;
}

/* Send REQUEST_DEVICE_INFO subcmd to one Joy-Con UART, read reply, extract
 * firmware version. Returns 0 on success, 1 on no-reply / parse failure.
 * Caller is responsible for ensuring the Joy-Con on this UART is already
 * in HID-connected state (joycon_poll has been driven to completion). */
static int probe_jc_fw_version(u32 uart, u8 *fw_major, u8 *fw_minor)
{
    /* Build request (23 bytes total) */
    u8 pkt[23] = {0};
    /* jc_uart_hdr_t */
    pkt[0]  = 0x19; pkt[1] = 0x01; pkt[2] = 0x03;
    pkt[3]  = 18;   pkt[4] = 0;          /* total_size LE = 12 - 5 + 11 = 18 */
    /* jc_wired_hdr_t (cmd, subcmd, payload_size, status, crc_payload, crc_hdr) */
    pkt[5]  = 0x92;                       /* JC_WIRED_HID */
    pkt[6]  = 0;                          /* subcmd (header-level) */
    pkt[7]  = 11;   pkt[8] = 0;          /* payload_size LE = 11 */
    pkt[9]  = 0;                          /* status */
    pkt[10] = 0;                          /* crc_payload */
    pkt[11] = _jc_crc8(&pkt[5], 6);       /* crc_hdr over cmd..crc_payload */
    /* HID output report */
    pkt[12] = 0x01;                       /* JC_HID_OUTPUT_RPT */
    pkt[13] = 0;                          /* pkt_id */
    /* rumble[2] = mute pattern (8 bytes) */
    pkt[14] = 0x00; pkt[15] = 0x01; pkt[16] = 0x40; pkt[17] = 0x40;
    pkt[18] = 0x00; pkt[19] = 0x01; pkt[20] = 0x40; pkt[21] = 0x40;
    pkt[22] = 0x02;                       /* subcmd = REQUEST_DEVICE_INFO */

    uart_send(uart, pkt, sizeof(pkt));
    uart_wait_xfer(uart, UART_TX_IDLE);

    /* Drain UART for ~150 ms collecting bytes into a scratch buffer */
    static u8 rx[256];
    u32 rx_pos = 0;
    u32 deadline = get_tmr_us() + 150000;
    while ((s32)(deadline - get_tmr_us()) > 0 && rx_pos < sizeof(rx)) {
        u8 b;
        if (uart_recv(uart, &b, 1) == 1)
            rx[rx_pos++] = b;
    }

    /* Scan for the wired RX magic followed by a SUBMCD_RPT for 0x02 */
    /* Reply layout: rx[i..i+11] = jc_wired_hdr_t (12 bytes), then the HID
     * input report. The HID report's submcd_ack is at +13 from payload
     * start (after cmd, pkt_id, conn/batt, 3 stick groups, vib_decider).
     * We're looking for: cmd=0x21, submcd_ack=0x82, subcmd=0x02. */
    for (u32 i = 0; i + 30 < rx_pos; i++) {
        if (rx[i] != 0x19 || rx[i+1] != 0x81 || rx[i+2] != 0x03)
            continue;
        if (rx[i+5] != 0x92)               /* not JC_WIRED_HID */
            continue;
        const u8 *hid = &rx[i + 12];        /* HID input report starts here */
        if (hid[0]  != 0x21)                /* not JC_HID_SUBMCD_RPT */
            continue;
        if (hid[13] != 0x82 || hid[14] != 0x02)  /* not the 0x02 reply */
            continue;
        /* subcmd_data starts at hid + 15 */
        *fw_major = hid[15];
        *fw_minor = hid[16];
        return 0;
    }
    return 1;
}
#endif  /* JC_PROBE */

void probe_joycon(void)
{
    HEADER("[Joy-Con detection + version]");

#ifdef JC_PROBE
    /* In JC_PROBE builds jc_init_hw + initial polling already ran in
     * ipl_main, so by the time this probe runs:
     *   - The PH6/PE6 GPIO buffer was unlatched by _jc_rail_detect
     *     (Hekate's "HW BUG" workaround in joycon.c) and now reads true.
     *   - The Joy-Con MCUs have completed handshake (rail detect ->
     *     wake -> baud-rate-up -> HID input mode) and joycon_poll()
     *     returns valid conn_l / conn_r and bt_conn_* fields.
     * We run a handful more polls here to refresh the report - cheap. */
    for (u32 i = 0; i < 10; i++) {
        joycon_poll();
        msleep(5);
    }
#endif

    /* IsAttached pins (PH6 = R, PE6 = L), active low. In default builds
     * these reads can be stale until something runs the unlatch dance,
     * which we don't do (it would interfere with UART_B debug). The
     * status here is best-effort in default mode; for accurate Joy-Con
     * detection switch to the JC_PROBE build. */
    int jc_r_attached = !gpio_read(GPIO_PORT_H, GPIO_PIN_6);
    int jc_l_attached = !gpio_read(GPIO_PORT_E, GPIO_PIN_6);
    log_color(jc_r_attached ? COL_OK : COL_DEFAULT,
        "  JC-R rail  : %s   (PH6 IsAttached)\n",
        jc_r_attached ? "ATTACHED" : "empty   ");
    log_color(jc_l_attached ? COL_OK : COL_DEFAULT,
        "  JC-L rail  : %s   (PE6 IsAttached)\n",
        jc_l_attached ? "ATTACHED" : "empty   ");

#ifdef JC_PROBE
    LOG("  Joy-Con poll : enabled (JC_PROBE=1 build)\n");
    jc_gamepad_rpt_t *rpt = joycon_poll();
    if (rpt) {
        if (rpt->conn_l) {
            log_color(COL_OK,
                "  JC-L state  : connected, batt=0x%02X chrg=0x%02X type=0x%02X\n",
                rpt->batt_info_l, rpt->batt_chrg_l, rpt->bt_conn_l.type);
        } else if (jc_l_attached) {
            log_color(COL_WARN,
                "  JC-L state  : attached but did not respond over UART (timeout)\n");
        }
        if (rpt->conn_r) {
            log_color(COL_OK,
                "  JC-R state  : connected, batt=0x%02X chrg=0x%02X type=0x%02X\n",
                rpt->batt_info_r, rpt->batt_chrg_r, rpt->bt_conn_r.type);
        } else if (jc_r_attached) {
            log_color(COL_WARN,
                "  JC-R state  : attached but did not respond over UART (timeout)\n");
        }
        /* Pull pairing info (BT MAC of each Joy-Con + the host MAC it
         * was paired to). Two flags come back: is_l_hos / is_r_hos
         * indicating whether each Joy-Con's pairing slot points at this
         * console (HOS = current Switch). Only meaningful when at least
         * one Joy-Con responded. */
        if (rpt->conn_l || rpt->conn_r) {
            bool is_l_hos = false, is_r_hos = false;
            jc_gamepad_rpt_t *pair = jc_get_bt_pairing_info(&is_l_hos, &is_r_hos);
            if (pair) {
                if (rpt->conn_l) {
                    LOG("  JC-L MAC    : %02X:%02X:%02X:%02X:%02X:%02X (paired to this Switch: %s)\n",
                        pair->bt_conn_l.mac[5], pair->bt_conn_l.mac[4],
                        pair->bt_conn_l.mac[3], pair->bt_conn_l.mac[2],
                        pair->bt_conn_l.mac[1], pair->bt_conn_l.mac[0],
                        is_l_hos ? "yes" : "no");
                }
                if (rpt->conn_r) {
                    LOG("  JC-R MAC    : %02X:%02X:%02X:%02X:%02X:%02X (paired to this Switch: %s)\n",
                        pair->bt_conn_r.mac[5], pair->bt_conn_r.mac[4],
                        pair->bt_conn_r.mac[3], pair->bt_conn_r.mac[2],
                        pair->bt_conn_r.mac[1], pair->bt_conn_r.mac[0],
                        is_r_hos ? "yes" : "no");
                }
            }
        }
        /* Firmware version via HID subcmd 0x02 (REQUEST_DEVICE_INFO).
         * Hekate's joycon stack doesn't fetch this - we send the wired
         * HID packet manually. JC-L lives on UART_C, JC-R on UART_B. */
        if (rpt->conn_l) {
            u8 fw_maj = 0, fw_min = 0;
            if (probe_jc_fw_version(UART_C, &fw_maj, &fw_min) == 0)
                log_color(COL_OK,
                    "  JC-L firmware: %d.%02d\n", fw_maj, fw_min);
            else
                log_color(COL_WARN,
                    "  JC-L firmware: read FAILED (no reply within 150 ms)\n");
        }
        if (rpt->conn_r) {
            u8 fw_maj = 0, fw_min = 0;
            if (probe_jc_fw_version(UART_B, &fw_maj, &fw_min) == 0)
                log_color(COL_OK,
                    "  JC-R firmware: %d.%02d\n", fw_maj, fw_min);
            else
                log_color(COL_WARN,
                    "  JC-R firmware: read FAILED (no reply within 150 ms)\n");
        }
    } else {
        log_color(COL_WARN, "  joycon_poll() returned NULL (jc_init_hw failed?)\n");
    }
#else
    log_color(COL_DEFAULT,
        "  Joy-Con poll : disabled (UART_B owned by debug log)\n");
#endif
}


void probe_touch(void)
{
    HEADER("[Touch panel (FTS4, I2C3 @ 0x49)]");

    /* The touch IC is unpowered at boot and I2C3's clock is gated. If we
     * called touch_get_*() now, the i2c_xfer_packet poll loop would spin
     * forever and the watchdog would reset the SoC after ~5 s. Hekate's
     * `touch_power_on()` configures the GPIO reset pin, enables LDO6 (the
     * AVDD/DVDD supply for the panel), pinmux/clock-init's I2C3, releases
     * the reset, waits for the controller-ready event, and runs the
     * sense-enable handshake. Returns 0 on full success, 1 if init
     * retries gave up - we report either way and keep probing for the
     * static info populated during the attempt. */
    int power_rc = touch_power_on();
    log_color(power_rc ? COL_ERR : COL_OK,
        "  power-on     : %s\n", power_rc ? "FAILED (panel cable / IC fault?)" : "OK");

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
 * als_power_on() enables LDO6 (already enabled by the regulator probe
 * but idempotent), pinmuxes I2C2, and triggers a continuous-conversion
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

