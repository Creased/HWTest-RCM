/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: gamecard reader (Lotus3 ASIC on SDMMC2)
 */
#include "hwtest.h"

/* ======================================================================== */
/* Lotus3 gamecard ASIC - the Tegra's SDMMC2 device                         */
/*                                                                          */
/* The game card slot does not hang off the SoC. A separate ASIC (switchbrew */
/* calls it Lotus3; the board marking is GCBRG) sits between them, and the   */
/* Tegra reaches it as an MMC device on SDMMC2 (0x700B0200). Horizon's FS    */
/* then drives the card through it with vendor commands CMD60-63 layered on  */
/* top of MMC.                                                              */
/*                                                                          */
/* This probe stops once the ASIC demonstrably drives the bus, which is     */
/* enough to answer what a repair needs - is the reader controller alive? -  */
/* and stays clear of the rest. The ASIC's bootrom accepts exactly one       */
/* operation, SendFirmware, and loading a firmware blob BURNS OTP FUSES to   */
/* enforce anti-downgrade. That is irreversible, so no vendor command is     */
/* ever sent from here.                                                      */
/*                                                                          */
/* The sequence below is FS's own, read out of the extracted sysmodule       */
/* rather than guessed, because guessing produced a silent bus for a long    */
/* time. FS's gamecard path touches exactly four devices - the two GPIOs and */
/* the two rails used here - and writes no pinmux and no clock registers.    */

/* Rails. pcv names both LDO3 and LDO5 "GcCard": 3.1 V for the core and 1.8 V
 * for the interface. ORDER MATTERS - FS enables LDO3 first, then LDO5, then
 * settles 1.5 ms (SetGcPower). LDO5 also boots at the PMIC's 3.1 V OTP
 * default against a 1.8 V interface, so it is set down and read back before
 * being enabled rather than trusted. */
#define GC_ASIC_UV        3100000
#define GC_IO_UV          1800000
#define GC_POWER_SETTLE_MS  2      /* FS sleeps 1.5 ms after LDO5 */

/* Card detect: slot pin 2 (CD#) is strapped to ground in the cartridge shell,
 * so the line reads low when one is seated - confirmed on hardware both ways,
 * 0 with a cartridge in and 1 with the slot empty. It is wired straight to the
 * connector, so it reports insertion with the reader unpowered, which
 * separates "empty slot" from "dead reader" before any rail is touched. The
 * pad boots muxed to I2CVI and has to be reclaimed. */
#define PMX_PS3_GC_CD     PINMUX_AUX_CAM_I2C_SDA
#define GPIO_S_MSK_CNF    0x488
#define GPIO_S_MSK_OE     0x498

/* GameCardReset (PBB3, switchbrew pad 0x05). FS drives it to 1 to run the
 * ASIC and to 0 to hold it in reset; boot leaves it 0 from cold. Masked GPIO
 * writes throughout - PBB4 next door is the codec's alert line.
 *
 * PowGc (PE5, pad 0x07) is deliberately NOT driven here. Horizon never raises
 * it: boot configures it as an output at 0 and no sysmodule, FS included,
 * touches it again. Driving it high is exactly the kind of guess that kept
 * this bus silent, so the pad is left as the console's own boot code sets it. */
#define PMX_PBB3_GC_RST   PINMUX_AUX_GPIO_X1_AUD   /* 0x18C */
#define GPIO_BB_MSK_CNF   0x68C
#define GPIO_BB_MSK_OE    0x69C
#define GPIO_BB_MSK_OUT   0x6AC

static sdmmc_t         gc_sdmmc;
static sdmmc_storage_t gc_storage;

void probe_gamecard(void)
{
    HEADER("[Gamecard reader - Lotus3 ASIC on SDMMC2]");

    /* ---- card detect, before anything is powered ---- */
    u32 sv_ps3 = PINMUX_AUX(PMX_PS3_GC_CD);
    PINMUX_AUX(PMX_PS3_GC_CD) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP |
                                PINMUX_TRISTATE;
    /* Masked writes: port S also carries the fan tacho (PS7) and the charger
     * status lines (PS1/PS6). */
    GP_MWR(GPIO_S_MSK_CNF, GPIO_PIN_3, 1);
    GP_MWR(GPIO_S_MSK_OE,  GPIO_PIN_3, 0);
    usleep(200);
    u32 cd_raw = gpio_read(GPIO_PORT_S, GPIO_PIN_3);
    PINMUX_AUX(PMX_PS3_GC_CD) = sv_ps3;

    LOG("  Card detect  : %s\n",
        cd_raw ? "slot empty" : "cartridge seated");

    /* ---- hold the ASIC in reset while the rails come up ---- */
    u32 sv_pbb3 = PINMUX_AUX(PMX_PBB3_GC_RST);
    PINMUX_AUX(PMX_PBB3_GC_RST) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPIO_BB_MSK_OUT, GPIO_PIN_3, 0);
    GP_MWR(GPIO_BB_MSK_CNF, GPIO_PIN_3, 1);
    GP_MWR(GPIO_BB_MSK_OE,  GPIO_PIN_3, 1);

    /* ---- rails, in FS's order: core first, then interface ---- */
    u8 sv_ldo3 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO3_CFG);
    u8 sv_ldo5 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO5_CFG);
    bool ldo3_was_on = max77620_regulator_get_status(REGULATOR_LDO3);
    bool ldo5_was_on = max77620_regulator_get_status(REGULATOR_LDO5);

    max7762x_regulator_set_voltage(REGULATOR_LDO3, GC_ASIC_UV);
    max7762x_regulator_enable(REGULATOR_LDO3, true);

    max7762x_regulator_set_voltage(REGULATOR_LDO5, GC_IO_UV);
    u32 ldo5_uv = ((u32)(i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR,
                                       MAX77620_REG_LDO5_CFG) & 0x3F))
                  * 50000u + 800000u;
    bool ldo5_on = false;
    if (ldo5_uv == GC_IO_UV) {
        max7762x_regulator_enable(REGULATOR_LDO5, true);
        ldo5_on = true;
    }
    msleep(GC_POWER_SETTLE_MS);

    bool ldo3_ok = max77620_regulator_get_status(REGULATOR_LDO3);
    ldo5_on = ldo5_on && max77620_regulator_get_status(REGULATOR_LDO5);
    log_color(ldo3_ok && ldo5_on ? COL_OK : COL_ERR,
        "  Rails        : LDO3 %s 3.100 V, LDO5 %s 1.800 V\n",
        ldo3_ok ? "on" : "OFF", ldo5_on ? "on" : "OFF");
    if (!ldo3_ok || !ldo5_on) {
        dx_set("gc_asic", DX_FAIL, "gamecard rails would not come up");
        goto restore;
    }

    /* Release reset. Read the pad back rather than assume the write landed -
     * "drove it and nothing answered" and "the pad never moved" are different
     * faults and only the readback separates them. */
    GP_MWR(GPIO_BB_MSK_OUT, GPIO_PIN_3, 1);
    usleep(10);
    LOG("  GC reset     : PBB3 reads %d (1 = running)\n",
        gpio_read(GPIO_PORT_BB, GPIO_PIN_3));
    msleep(50);

    /* ---- controller ----
     * This port is never run at an identification clock. FS's Activate brings
     * it straight up 8-bit, 1.8 V, at its own "GcAsicSpeed" - 200 MHz from the
     * CAR divided by two, so a 100 MHz card clock - and the very first thing on
     * the bus is CMD21 tuning, which is what arms SAMPLING_CLOCK_SELECT. There
     * is no CMD0 and no 400 kHz phase anywhere in it.
     *
     * That is exactly what BDK's sdmmc_storage_init_gc() does (SDMMC_2,
     * POWER_1_8, BUS_WIDTH_8, HS100, then tuning on MMC_SEND_TUNING_BLOCK_HS200),
     * which is dead code in hekate with no callers. Earlier attempts here
     * failed against it because the rails were brought up in the wrong order,
     * PowGc was being driven high, and a 1-bit identification-speed init had
     * already claimed the controller. With those fixed it is the right call.
     *
     * Tuning is the test: it only completes if the ASIC drives data back. */
    /* On T210B01 the SDMMC2 pads are ordinary muxed pads, and nothing in RCM
     * has configured them: BDK's bring-up only ORs the Schmitt bit into them
     * (_sdmmc_config_sdmmc2_schmitt), which cannot clear TRISTATE or PARKED
     * nor set INPUT_ENABLE, while the Erista leg of the same switch un-parks
     * its pad brick outright. hekate never drives SDMMC2, so that leg has no
     * user to notice. Assign the pads the way BDK configures SDMMC1 - a plain
     * write clears the function, tristate and park bits as a side effect.
     *
     * Measured on a Mariko in RCM: CLK reads 0xA064 before this runs, which
     * is PARKED (bit 5) set, so the bus could never have come up. */
    if ((((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2)) {
        /* Values are HOS's own initial pad table for this board, decoded:
         * DAT0-7 pull-up, CLK/CMD no pull, all three input-enabled and out
         * of tristate; the DDR-only strobes stay parked. BDK stops short of
         * defining the last three, so their offsets are named here. */
        #define PINMUX_AUX_SDMMC2_CLKB 0x2B8
        #define PINMUX_AUX_SDMMC2_DQS  0x2C0
        #define PINMUX_AUX_SDMMC2_DQSB 0x2C4
        const u32 dat  = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP;
        const u32 ctl  = PINMUX_INPUT_ENABLE | PINMUX_PULL_NONE;
        const u32 park = PINMUX_TRISTATE | PINMUX_PULL_DOWN;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_CLK)  = ctl;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_CMD)  = ctl;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT0) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT1) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT2) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT3) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT4) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT5) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT6) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DAT7) = dat;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_CLKB) = park;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DQS)  = park;
        PINMUX_AUX(PINMUX_AUX_SDMMC2_DQSB) = park;
        (void)PINMUX_AUX(PINMUX_AUX_SDMMC2_CLK); /* commit */
        LOG("  SDMMC2 pads  : assigned (T210B01)\n");
    }

    if (sdmmc_storage_init_gc(&gc_storage, &gc_sdmmc)) {
        log_color(COL_WARN, "  ASIC         : bus would not come up\n");
        dx_set("gc_asic", DX_WARN, "SDMMC2 bus did not come up");
        goto stop;
    }

    /* Tuning completing is NOT proof the reader is there. sdmmc_tuning_execute
     * judges only on SDHCI_CTRL_TUNED_CLK, and the Tegra's tuning state machine
     * sets that after sweeping its taps whether or not anything answered - it
     * throws away the per-iteration result that actually tracks data arriving.
     * Measured with the reader module unplugged, tuning "passes".
     *
     * So ask for real data instead: one CMD21 tuning block. The controller
     * raises SDHCI_INT_DATA_AVAIL only when a device clocks bytes back, so this
     * fails on an absent or dead reader and succeeds on a live one. */
    /* 128 bytes in 8-bit mode - BDK's own tuning path sets blksize 128 for
     * SDMMC_BUS_WIDTH_8 and 64 only for 4-bit. */
    u8 blk[128] __attribute__((aligned(8)));
    sdmmc_cmd_t tc;
    sdmmc_req_t tr;

    memset(blk, 0, sizeof(blk));
    sdmmc_init_cmd(&tc, MMC_SEND_TUNING_BLOCK_HS200, 0, SDMMC_RSP_TYPE_1, 0);
    tr.buf              = blk;
    tr.blksize          = 128;
    tr.num_sectors      = 1;
    tr.is_write         = 0;
    tr.is_multi_block   = 0;
    tr.is_auto_stop_trn = 0;

    /* BDK convention: non-zero is FAILURE. */
    if (sdmmc_execute_cmd(&gc_sdmmc, &tc, &tr, NULL)) {
        log_color(COL_WARN, "  ASIC         : bus up, but no data returned\n");
        LOG("  (reader module absent, unseated or dead)\n");
        dx_set("gc_asic", DX_WARN, "no data from the reader on SDMMC2");
        goto stop;
    }

    log_color(COL_OK, "  ASIC         : returned data at %d kHz, 8-bit\n",
              gc_sdmmc.card_clock);

    /* Show what actually came back. CMD21 returns a fixed tuning pattern, so
     * the bytes are checkable: structured data means a device drove the bus,
     * while all-00 or all-FF means the transfer completed locally and proves
     * nothing about anything being out there. */
    LOG("  Data[0..7]   : %02X %02X %02X %02X %02X %02X %02X %02X\n",
        blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7]);
    /* Judge on the CONTENT, not on the transfer completing. CMD21 returns the
     * JEDEC tuning block, whose 8-bit form opens FF FF 00 FF FF FF 00 00 and
     * carries a mix of 0xCC/0x33/0xEE/0xDD/0xBB/0x77 further in. A host
     * controller cannot invent that, so matching it is device-sourced proof -
     * whereas "the transfer returned success" is not: BDK's own tuning judges
     * only SDHCI_CTRL_TUNED_CLK, which the Tegra sets after sweeping its taps
     * even with nothing on the bus. */
    static const u8 kTuneHead[8] = { 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00 };
    u32 varied = 0;
    bool head_ok = true;
    for (u32 i = 0; i < 8; i++)
        if (blk[i] != kTuneHead[i])
            head_ok = false;
    for (u32 i = 0; i < 128; i++)
        if (blk[i] != 0x00 && blk[i] != 0xFF)
            varied++;

    if (!head_ok || varied < 16) {
        log_color(COL_WARN,
            "  Tuning block : not the JEDEC pattern (%d varied bytes)\n", varied);
        dx_set("gc_asic", DX_WARN, "SDMMC2 data is not the tuning pattern");
        goto stop;
    }
    log_color(COL_OK,
        "  Tuning block : JEDEC pattern, %d varied bytes of 128\n", varied);

    log_color(COL_OK, "  Result       : reader present and answering\n");
    dx_set("gc_asic", DX_PASS, "");

stop:
    sdmmc_end(&gc_sdmmc);

restore:
    /* Reset asserted, rails down, pad as found - and in FS's order, which
     * powers down 2 ms after the reset goes low. FS also enforces 100 ms off
     * before the next power-up; nothing here re-enters, but the delay keeps
     * that contract if the pager re-runs this page. */
    GP_MWR(GPIO_BB_MSK_OUT, GPIO_PIN_3, 0);
    msleep(2);
    if (ldo5_on && !ldo5_was_on)
        max7762x_regulator_enable(REGULATOR_LDO5, false);
    if (!ldo3_was_on)
        max7762x_regulator_enable(REGULATOR_LDO3, false);
    i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO5_CFG, sv_ldo5);
    i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO3_CFG, sv_ldo3);
    PINMUX_AUX(PMX_PBB3_GC_RST) = sv_pbb3;
    msleep(100);


}
