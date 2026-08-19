/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: audio probes
 */
#include "hwtest.h"


/* ======================================================================== */
/* ALC5639 audio codec (Realtek RT5639), I2C1 @ 0x1C                        */
/*                                                                          */
/* Bus, address and control pins come from the Switch device tree           */
/* (fail0verflow switch-linux, tegra210-nintendo-switch.dts): the codec is  */
/* audio-codec@1c on i2c@7000c000, which is I2C_1. The same node names PZ4  */
/* as realtek,ldo1-en-gpios - the enable for the codec's own internal LDO1  */
/* - and PV6 as its interrupt line.                                         */
/*                                                                          */
/* Nothing configures PZ4 under RCM, so the codec's LDO1 is off and the     */
/* part does not answer, which is measurable rather than assumed: a census */
/* of all six I2C controllers finds every other device the device tree      */
/* lists on I2C_1 - 0x18 USB-PD, 0x36 fuel gauge, 0x4C thermal, 0x6B        */
/* charger - and nothing at 0x1C. So this probe drives the enable itself.   */
/*                                                                          */
/* Identification is by vendor ID. MX-FEh reads 0x10EC (Realtek's vendor    */
/* code) on every part in the family, so a device answering with it at this */
/* address is the codec and nothing else can be mistaken for it.            */

#define ALC5639_I2C_ADDR    0x1Cu
/* MX-FEh, vendor ID: 0x10EC on every part of the family. */
#define ALC5639_REG_VENDOR  0xFEu
/* MX-00h carries the device id in bits 2:1. Read only, never written: a
 * write to this register resets every register in the codec to its
 * default (ALC5640 datasheet, table 22). */
#define ALC5639_REG_DEVID   0x00u
#define ALC5639_VENDOR_ID   0x10ECu
/* The register Linux's rt5640 driver detects on, and the value it expects. */
#define RT5639_REG_ID2      0xFFu
#define RT5639_DEVICE_ID    0x6231u

/* PZ4 = realtek,ldo1-en-gpios, driven to a physical HIGH to enable.
 *
 * The device tree flags the pin GPIO_ACTIVE_LOW, but that flag is inert for
 * this part: Linux's rt5640 driver reads the property with the legacy
 * of_get_named_gpio(), which passes a NULL flags pointer and discards the
 * cell, then drives the pad with gpiod_direction_output_raw(desc, 1) -
 * documented as bypassing ACTIVE_LOW. HOS agrees: its audio sysmodule
 * asserts the pad high and only drives it low on shutdown.
 *
 * Both implementations then wait 400 ms before the first access, and HOS
 * treats an early NoAck as expected, retrying the first transaction up to
 * 2000 times at 5 ms. So the codec is slow to wake rather than absent, and
 * a single probe right after the drive proves nothing. */
#define PMX_PZ4_CODEC_LDO1  PINMUX_AUX_GPIO_PZ4
#define CODEC_LDO1_SETTLE_MS  400   /* Linux msleep(400); HOS sleeps the same */
#define CODEC_POLL_MS         1000  /* then poll, as HOS does, before judging */
#define CODEC_POLL_STEP_MS    10

/* Registers are 16 bit behind an 8-bit register address, high byte first
 * (ALC5640 datasheet, "Read WORD Protocol"). */
static bool _codec_read16(u32 reg, u16 *out)
{
    u8 buf[2] = { 0, 0 };

    if (i2c_recv_buf_small(buf, sizeof(buf), I2C_1, ALC5639_I2C_ADDR, reg) != 0)
        return false;
    *out = ((u16)buf[0] << 8) | buf[1];
    return true;
}

/* Either identity register is proof enough. MX-FEh is the family vendor code
 * from the datasheet; 0xFF is what Linux's rt5640 driver reads to detect the
 * part. Accepting both means the probe does not depend on which of the two
 * this particular die populates. */
static void _probe_i2c1_census(void);

static bool _codec_id_ok(u16 *id, u32 *reg)
{
    if (_codec_read16(ALC5639_REG_VENDOR, id) && *id == ALC5639_VENDOR_ID) {
        *reg = ALC5639_REG_VENDOR;
        return true;
    }
    if (_codec_read16(RT5639_REG_ID2, id) && *id == RT5639_DEVICE_ID) {
        *reg = RT5639_REG_ID2;
        return true;
    }
    return false;
}

void probe_audio(void)
{
    HEADER("[ALC5639 audio codec, I2C1 @ 0x1C]");

    u32 pz4_entry = gpio_read(GPIO_PORT_Z, GPIO_PIN_4);
    u16 vid = 0;
    u32 id_reg = 0;
    u32 woke_ms = 0;
    int drove = -1;         /* level that worked, -1 = none, -2 = no drive */

    if (_codec_id_ok(&vid, &id_reg)) {
        drove = -2;
    } else {
        /* Take PZ4 as a plain GPIO output. The GPIO controller's CNF bit
         * wins over the pinmux function, so the pad answers here whatever
         * it was muxed to; clearing TRISTATE is what actually matters.
         * INPUT_ENABLE is set as well, purely so the pad can be read back -
         * with the input buffer off, GPIO_IN reads 0 whatever the pin is
         * actually doing, which makes the readback below a lie. */
        PINMUX_AUX(PMX_PZ4_CODEC_LDO1) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
        gpio_config(GPIO_PORT_Z, GPIO_PIN_4, GPIO_MODE_GPIO);

        /* HIGH first and for the full budget - that is what both Linux and
         * HOS do. LOW is tried afterwards only because the device tree's
         * annotation leaves a doubt, and it is cheap once HIGH has failed. */
        for (int lvl = 1; lvl >= 0 && drove < 0; lvl--) {
            gpio_direction_output(GPIO_PORT_Z, GPIO_PIN_4,
                                  lvl ? GPIO_HIGH : GPIO_LOW);
            /* Read the pad back. "Drove it and nothing answered" and "the
             * pad never moved" are different faults, and only the readback
             * separates them. */
            LOG("  PZ4 drive %s : pad reads %d  pinmux %08X\n",
                lvl ? "HIGH" : "LOW ", gpio_read(GPIO_PORT_Z, GPIO_PIN_4),
                PINMUX_AUX(PMX_PZ4_CODEC_LDO1));

            msleep(CODEC_LDO1_SETTLE_MS);
            if (_codec_id_ok(&vid, &id_reg)) {
                drove = lvl;
                woke_ms = CODEC_LDO1_SETTLE_MS;
                break;
            }
            /* Still quiet. HOS keeps asking rather than concluding, so do
             * the same for a bounded window and record when it answered -
             * a part that wakes late is a different finding from one that
             * never wakes at all. */
            for (u32 t = 0; t < CODEC_POLL_MS; t += CODEC_POLL_STEP_MS) {
                msleep(CODEC_POLL_STEP_MS);
                if (_codec_id_ok(&vid, &id_reg)) {
                    drove = lvl;
                    woke_ms = CODEC_LDO1_SETTLE_MS + t + CODEC_POLL_STEP_MS;
                    break;
                }
            }
        }
    }

    LOG("  PZ4 ldo1_en  : %d on entry (%s)\n", pz4_entry,
        drove == -2 ? "codec already answering" :
        drove <   0 ? "driven both ways, no answer" :
        drove ==  1 ? "enabled by driving HIGH" : "enabled by driving LOW");

    if (drove == -1) {
        /* Silent after the full sequence Linux and HOS both use: LDO1_EN
         * high, 400 ms, then polled rather than probed once. With the pad
         * readback above confirming the pin moved, and neither reference
         * naming any rail, reset or clock for this part, there is nothing
         * further this probe knows to try.
         *
         * Warn rather than fail: nothing here separates a faulty codec from
         * one that is simply not populated, and HOS treats the part as
         * optional per board (its has_codec_ic setting). The bus is known
         * good, which is what makes the silence attributable to the codec
         * side at all. */
        log_color(COL_WARN, "  Codec ID     : no answer at 0x1C after %d ms\n",
                  CODEC_LDO1_SETTLE_MS + CODEC_POLL_MS);
        LOG("  (bus is up - every other I2C_1 device answers - so this is\n");
        LOG("   the codec side: not populated, unpowered or faulty)\n");
        dx_set("audio_codec", DX_WARN, "silent at 0x1C after LDO1_EN + %d ms",
               CODEC_LDO1_SETTLE_MS + CODEC_POLL_MS);
        return;
    }

    log_color(COL_OK, "  Codec ID     : 0x%04X at reg 0x%02X (Realtek verified)\n",
              vid, id_reg);
    if (woke_ms)
        LOG("  Wake time    : answered %d ms after LDO1_EN went high\n", woke_ms);

    u16 devid = 0;
    if (_codec_read16(ALC5639_REG_DEVID, &devid))
        LOG("  Device ID    : 0x%04X (id %d)\n", devid, (devid >> 1) & 3);

    dx_set("audio_codec", DX_PASS, "");
    LOG("  Result       : codec is powered and answering\n");

    _probe_i2c1_census();
}

/* ======================================================================== */
/* Audio clock domain: AUD partition, APE/AHUB/I2S1 clocks, AUD_MCLK        */
/*                                                                          */
/* Everything the codec needs to be fed samples sits behind this. hekate     */
/* leaves it all off: hw_init() gates every audio clock and holds APE in     */
/* reset, and the AUD power partition is whatever the boot ROM left.         */
/*                                                                          */
/* This stage brings the domain up and reports it, and touches nothing       */
/* inside APE until the APE reset is released. On this SoC a block that is   */
/* clocked but still held in reset does not fault on a read, it never        */
/* completes, and the BPMP hangs with no way back - the same hang MSELECT    */
/* and the PCIe apertures produce, and which the Tegra210 sources warn       */
/* about for AHUB specifically. Everything touched before that point is CAR, */
/* PMC or APB_MISC, all of which always answer, so the state is verified by  */
/* readback without ever addressing the block being enabled. */

#define CAR_CLK_ENB_L_SET   CLK_RST_CONTROLLER_CLK_ENB_L_SET
#define CAR_RST_DEV_L_CLR   CLK_RST_CONTROLLER_RST_DEV_L_CLR
#define CAR_CLK_ENB_V_SET   CLK_RST_CONTROLLER_CLK_ENB_V_SET
#define CAR_RST_DEV_V_CLR   CLK_RST_CONTROLLER_RST_DEV_V_CLR
#define CAR_CLK_ENB_Y_SET   CLK_RST_CONTROLLER_CLK_ENB_Y_SET
#define CAR_RST_DEV_Y_CLR   CLK_RST_CONTROLLER_RST_DEV_Y_CLR

void probe_audio_clocks(void)
{
    HEADER("[Audio clocks - APE / AHUB / AUD_MCLK]");

    /* Latch the audio-HV pad rail as 1.8 V, and route the DAS. Both come
     * from the one bare-metal Switch driver that makes sound (shinyquagsire
     * hekate PR #990, misc_audio_init / rt5639_init):
     *
     *  - PWR_DET tells the pad cells what voltage their rail is. The DAP1 and
     *    AUD_MCLK pads sit on AUDIO_HV; without the detect latched they can
     *    drive at the wrong level, or not at all.
     *  - The DAS (Digital Audio Switch) is the block that connects the DAP1
     *    pin group to the I2S1 controller. Unselected, I2S data can be
     *    perfectly serialised inside the SoC and still never reach the
     *    physical DAP1 pins. */
#define PMC_PWR_DET_AUDIO_HV_BIT  BIT(18)
#define APB_MISC_DAS_DAP_CTRL_SEL_REG            0xC00u
#define APB_MISC_DAS_DAC_INPUT_DATA_CLK_SEL_REG  0xC40u
    PMC(APBDEV_PMC_PWR_DET)     |= PMC_PWR_DET_AUDIO_HV_BIT;
    PMC(APBDEV_PMC_PWR_DET_VAL) &= ~PMC_PWR_DET_AUDIO_HV_BIT;   /* 1.8 V     */
    usleep(130);
    PMC(APBDEV_PMC_NO_IOPOWER)  &= ~PMC_NO_IOPOWER_AUDIO_HV;
    APB_MISC(APB_MISC_DAS_DAP_CTRL_SEL_REG)           = BIT(31);
    APB_MISC(APB_MISC_DAS_DAC_INPUT_DATA_CLK_SEL_REG) = 0;
    LOG("  Audio rail   : PWR_DET %08X VAL %08X ; DAS DAP %08X DAC %08X\n",
        PMC(APBDEV_PMC_PWR_DET), PMC(APBDEV_PMC_PWR_DET_VAL),
        APB_MISC(APB_MISC_DAS_DAP_CTRL_SEL_REG),
        APB_MISC(APB_MISC_DAS_DAC_INPUT_DATA_CLK_SEL_REG));

    /* The AUD power partition gates the whole APE island. Ungate it before
     * any clock work, exactly as the PCIe probe ungates PCIE first. */
    u32 pg_before = PMC(APBDEV_PMC_PWRGATE_STATUS) & BIT(POWER_RAIL_AUD);
    int pg_rc = pmc_domain_pwrgate_set(POWER_RAIL_AUD, 1);
    u32 pg_after = PMC(APBDEV_PMC_PWRGATE_STATUS) & BIT(POWER_RAIL_AUD);
    log_color(pg_after ? COL_OK : COL_ERR,
        "  AUD partition: %s -> %s (rc %d)\n",
        pg_before ? "on" : "off", pg_after ? "on" : "off", pg_rc);
    if (!pg_after) {
        /* Without the island powered, every clock below is pointless and
         * any later APE access would hang. Stop here rather than set up
         * a trap for the next stage. */
        dx_set("audio_clocks", DX_FAIL, "AUD power partition would not ungate");
        return;
    }

    /* Clocks first, resets after - coreboot separates them by a 2 us settle
     * and so does hekate. Enable every block that hangs off AHUB together:
     * NVIDIA's own guidance, quoted in coreboot, is that reading an AHUB
     * device hangs unless ALL of them are clocked. */
    CLOCK(CAR_CLK_ENB_Y_SET) = BIT(CLK_Y_APE);
    CLOCK(CAR_CLK_ENB_V_SET) = BIT(CLK_V_AHUB) | BIT(CLK_V_APB2APE) |
                               BIT(CLK_V_I2S4) | BIT(CLK_V_I2S5);
    CLOCK(CAR_CLK_ENB_L_SET) = BIT(CLK_L_I2S1) | BIT(CLK_L_I2S2) |
                               BIT(CLK_L_I2S3) | BIT(CLK_L_SPDIF);
    usleep(2);

    /* SWR_APE_RST, RST_DEVICES_Y bit 6, gates the AHUB/I2S aperture.
     *
     * I2S1 and AHUB genuinely do come out of power-on reset already
     * released - RST_DEVICES_L's POR value clears I2S1's bit and
     * RST_DEVICES_V's clears AHUB's - which is why those blocks work here
     * without being touched. EXTPERIPH1 does NOT: RST_DEVICES_V resets to
     * 0xff81808x, so bit 24 is asserted, and it stays asserted until
     * something clears it. See the AUD_MCLK block below. */
    CLOCK(CAR_RST_DEV_Y_CLR) = BIT(CLK_Y_APE);
    usleep(10);

    bool ape_out = !(CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_Y) & BIT(CLK_Y_APE));
    log_color(ape_out ? COL_OK : COL_ERR,
        "  APE reset    : %s (SWR_APE_RST, the only audio reset on T210)\n",
        ape_out ? "released" : "STILL ASSERTED");

    /* AUD_MCLK. The codec's DAC needs a master clock even though its I2C
     * side does not.
     *
     * Do not use hekate's clock_enable_extperiph1() unmodified: it points
     * EXTPERIPH1 at PLLA_OUT0, and nothing in this payload or in hekate ever
     * enables PLLA, so the pad would carry no clock. Source it from
     * PLLP_OUT0 (408 MHz) with divisor 66 - the encoding is "divide by
     * (N/2)+1", so 408/34 = 12.000 MHz, which is what coreboot computes for
     * the same pad. */
    /* EXTPERIPH1 has a module reset, and it is ASSERTED out of power-on
     * reset: RST_DEVICES_V (CAR 0x358) resets to 0xff81808x, so bit 24 -
     * CLK_V_EXTPERIPH1 - starts set. A block held in reset emits nothing
     * and complains about nothing, so the clock source, the divider, the
     * enable bit, the PMC gate and the pad mux all read back exactly as
     * programmed while AUD_MCLK stays dead. The codec then has no SYSCLK
     * and its DAC never converts.
     *
     * Bracket it the way hekate's own clock_enable() does (bdk/soc/clock.c
     * around 163: assert reset, disable, set source, enable, 2 us, release
     * reset) and the way HOS does in audio.elf 0x1A610. hekate's
     * _clock_extperiph1 descriptor carries RST_DEV_V_SET with index
     * CLK_V_EXTPERIPH1, which corroborates it from the other side. */
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_V_SET) = BIT(CLK_V_EXTPERIPH1);
    CLOCK(CAR_CLK_ENB_V_SET + CLK_CLR_OFFSET) = BIT(CLK_V_EXTPERIPH1);
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_EXTPERIPH1) = (2u << 29) | 66u;
    CLOCK(CAR_CLK_ENB_V_SET) = BIT(CLK_V_EXTPERIPH1);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_V_SET + CLK_CLR_OFFSET) =
        BIT(CLK_V_EXTPERIPH1);
    (void)CLOCK(CLK_RST_CONTROLLER_RST_DEV_V_SET);   /* commit             */

    bool mclk_out = !(CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_V) &
                      BIT(CLK_V_EXTPERIPH1));
    log_color(mclk_out ? COL_OK : COL_ERR,
        "  EXTPERIPH1   : reset %s (bit 24, asserted at POR)\n",
        mclk_out ? "released" : "STILL ASSERTED");
    PMC(APBDEV_PMC_CLK_OUT_CNTRL) |= PMC_CLK_OUT_CNTRL_CLK1_SRC_SEL(OSC_CAR) |
                                     PMC_CLK_OUT_CNTRL_CLK1_FORCE_EN;
    PINMUX_AUX(PINMUX_AUX_AUD_MCLK) = PINMUX_PULL_NONE;

    /* Drive strength for the AUD_MCLK pad. Its CFGPADCTRL resets to 0, i.e.
     * MINIMUM drive - a clock configured perfectly upstream but driven too
     * weakly to reach the codec looks identical to no MCLK. coreboot's Switch mainboard sets CAL_DRVUP (bits 24:20)
     * and CAL_DRVDN (bits 16:12) to 0x10. Read-modify-write so the schmitt
     * and other reset bits survive. */
    APB_MISC(APB_MISC_GP_AUD_MCLK_CFGPADCTRL) =
        (APB_MISC(APB_MISC_GP_AUD_MCLK_CFGPADCTRL)
         & ~((0x1Fu << 20) | (0x1Fu << 12)))
        | (0x10u << 20) | (0x10u << 12);

    LOG("  AUD_MCLK     : src %08X pad %08X padctl %08X CLK_OUT %08X\n",
        CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_EXTPERIPH1),
        PINMUX_AUX(PINMUX_AUX_AUD_MCLK),
        APB_MISC(APB_MISC_GP_AUD_MCLK_CFGPADCTRL),
        PMC(APBDEV_PMC_CLK_OUT_CNTRL));

    if (!ape_out) {
        dx_set("audio_clocks", DX_FAIL, "APE still in reset");
        return;
    }

    /* First read inside APE. Safe only now, and only because hekate does
     * exactly this on this console: _mbist_workaround_bl() read-modify-writes
     * all five I2S_CTRL registers here right after the same clock/reset
     * sequence. That is also where these base addresses come from.
     *
     * All five on ONE row: the LCD wraps to the top of the screen rather than
     * scrolling once a page passes 45 rows, so five rows of register dump
     * would cost more than they tell anyone. Only I2S1 carries the codec. */
    LOG("  I2S1-5 CTRL  : %08X %08X %08X %08X %08X\n",
        I2S(I2S_CTRL), I2S(I2S_CTRL + (1 << 8)), I2S(I2S_CTRL + (2 << 8)),
        I2S(I2S_CTRL + (3 << 8)), I2S(I2S_CTRL + (4 << 8)));
    dx_set("audio_clocks", DX_PASS, "");
}

/* ======================================================================== */
/* What else sits on the codec's bus                                        */

/* Re-walk I2C_1 once the codec's enable is asserted: anything that shares
 * that enable only answers at this point, and the walk costs a few
 * milliseconds. The expected inhabitants are 0x18 USB-PD, 0x1C the codec
 * itself, 0x36 fuel gauge, 0x4C TMP451 thermal and 0x6B charger. */
static void _probe_i2c1_census(void)
{
    u8 v = 0;
    char line[80];
    u32 n = 0;

    line[0] = 0;
    for (u32 a = 0x08; a <= 0x77; a++) {
        if (i2c_recv_buf_small(&v, 1, I2C_1, a, 0) == 0) {
            char one[8];
            s_printf(one, " %02X", a);
            if (n < 16) strcat(line, one);
            n++;
        }
    }
    LOG("  I2C_1 census :%s\n", n ? line : " (none)");
}
