/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: power probes
 */
#include "hwtest.h"


void probe_pmic(void)
{
    HEADER("[MAX77620 main PMIC, I2C5 @ 0x3C]");
    u8 cid3 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CID3);
    u8 cid4 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CID4);
    u8 cid5 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CID5);
    LOG("  CID3 Si rev  : 0x%02X (max77620 v%d)\n", cid3, cid3 & 0xF);

    /* OTP 0x35 = Erista, 0x53 = Mariko. Compare against SoC HIDREV major:
     * a mismatch hints at a swapped PMIC or a wired-in module. */
    u32 chip_major = (APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF;
    bool otp_ok = (chip_major == 2 && cid4 == 0x53) ||
                  (chip_major == 1 && cid4 == 0x35);
    log_color(otp_ok ? COL_OK : COL_ERR,
        "  CID4 OTP     : 0x%02X (%s)%s\n", cid4,
        cid4 == 0x35 ? "Erista" : cid4 == 0x53 ? "Mariko" : "Unknown",
        otp_ok ? "" : " - mismatch with SoC!");
    dx_set("soc_pmic_otp", otp_ok ? DX_PASS : DX_FAIL,
        otp_ok ? "" : "chip M%d, otp 0x%02X", chip_major, cid4);
    LOG("  CID5 ES rev  : 0x%02X\n", cid5);

    /* MAX77621 is the Erista CPU/GPU buck regulator (two single-phase
     * chips on I2C5 at 0x1B/0x1C). Mariko / OLED / Lite replace it with
     * the multi-phase MAX77812 — handled by probe_max77812 below.
     * Trying to read MAX77621 on Mariko returns NACK -> 0xFF on most
     * lines and prints garbage. Gate by SoC chip-major. */
    if (chip_major == 1) {
        HEADER("[MAX77621 CPU/GPU regulators, I2C5 @ 0x1B/0x1C]");
        u8 cpu_id = i2c_recv_byte(I2C_5, MAX77621_CPU_I2C_ADDR, MAX77621_REG_CHIPID1);
        u8 gpu_id = i2c_recv_byte(I2C_5, MAX77621_GPU_I2C_ADDR, MAX77621_REG_CHIPID1);
        LOG("  CPU CHIPID1  : 0x%02X (max77621 v%d)\n", cpu_id, cpu_id & 0xF);
        LOG("  GPU CHIPID1  : 0x%02X (max77621 v%d)\n", gpu_id, gpu_id & 0xF);
        /* Output voltage from VOUT register (0x00) and VOUT_DVS register
         * (0x01). Format: bit7 = enable, bits6:0 = voltage = 606250 + N * 6250 uV.
         * VOUT is the active setting; VOUT_DVS is the pre-loaded DVS target
         * (selected via the GPIO_DVS pin, see Hekate's max7762x.c). HOS
         * uses both for fast CPU/GPU DVFS. We surface both per chip for the
         * tech to compare. */
        struct max77621_rail {
            u32 i2c_addr;
            const char *name;
            u32 vmin_uv;
            u32 vmax_uv;
        };
        static const struct max77621_rail bc_rails[] = {
            {MAX77621_CPU_I2C_ADDR, "CPU", 1000000, 1400000},
            {MAX77621_GPU_I2C_ADDR, "GPU", 1200000, 1400000},
        };
        for (size_t r = 0; r < sizeof(bc_rails)/sizeof(bc_rails[0]); r++) {
            u8 vout     = i2c_recv_byte(I2C_5, bc_rails[r].i2c_addr, MAX77621_REG_VOUT);
            u8 vout_dvs = i2c_recv_byte(I2C_5, bc_rails[r].i2c_addr, MAX77621_REG_VOUT_DVS);
            bool en     = (vout & MAX77621_VOUT_ENABLE_MASK) != 0;
            bool en_dvs = (vout_dvs & MAX77621_VOUT_ENABLE_MASK) != 0;
            u32 uv      = ((vout     & MAX77621_DVC_DVS_VOLT_MASK) * 6250) + 606250;
            u32 uv_dvs  = ((vout_dvs & MAX77621_DVC_DVS_VOLT_MASK) * 6250) + 606250;
            bool ok     = en && uv >= bc_rails[r].vmin_uv && uv <= bc_rails[r].vmax_uv;
            log_color(ok ? COL_OK : COL_WARN,
                "  %s VOUT     : %s %d.%03d V  (range %d.%03d - %d.%03d V)\n",
                bc_rails[r].name,
                en ? "ON " : "off",
                uv / 1000000, (uv / 1000) % 1000,
                bc_rails[r].vmin_uv / 1000000, (bc_rails[r].vmin_uv / 1000) % 1000,
                bc_rails[r].vmax_uv / 1000000, (bc_rails[r].vmax_uv / 1000) % 1000);
            log_color(COL_DEFAULT,
                "  %s VOUT_DVS : %s %d.%03d V  (DVFS pre-load)\n",
                bc_rails[r].name,
                en_dvs ? "ON " : "off",
                uv_dvs / 1000000, (uv_dvs / 1000) % 1000);
        }
    }
}

/* MAX77812 — Mariko / OLED / Lite multi-phase buck regulator that
 * replaces the dual MAX77621 setup found on Erista. Two retail
 * variants exist:
 *   PHASE211 @ I2C5 0x33 : 2-phase M1 (GPU) + 1-phase M3 (DRAM) +
 *                          1-phase M4 (CPU). This is what every
 *                          consumer Switch ships with.
 *   PHASE31  @ I2C5 0x31 : 3-phase M1 + 1-phase M4. Dev-kit / high-power
 *                          GPU variant; M3 is unused.
 * Both probe with no harm — only one ACKs on a real board. We try
 * 0x33 first (retail likelihood) and fall back to 0x31. If neither
 * answers we mark the probe FAIL and skip; on Erista we don't run
 * this probe at all.
 *
 * VOUT register encoding (M1..M4): vout_mv = 250 + N * 5, range
 * 250..1525 mV (LV variant — Switch uses LV). Values quoted in
 * the BDK's max77812.h match this. EN_CTRL holds per-phase enable
 * bits at 0/2/4/6. BUCK_STAT reflects per-rail power-good /
 * over-current latches; non-zero is a fault signal. */
static u8 _max77812_probe_addr(u8 addr)
{
    /* Read VERSION register; valid silicon returns ES2_VERSION (0x04)
     * or QS_VERSION (0x05) in the low 3 bits. A NACK on the bus
     * surfaces as 0xFF; an unpowered chip as 0x00. Either way the
     * masked-low-3-bits don't equal 4 or 5 so we reject. */
    u8 ver = i2c_recv_byte(I2C_5, addr, MAX77812_REG_VERSION) & MAX77812_VERSION_MASK;
    return (ver == MAX77812_ES2_VERSION || ver == MAX77812_QS_VERSION) ? ver : 0xFF;
}

void probe_max77812(void)
{
    u32 chip_major = (APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF;
    HEADER("[MAX77812 CPU/GPU/DRAM buck (Mariko), I2C5 @ 0x33/0x31]");
    if (chip_major != 2) {
        /* Erista has no MAX77812 - its CPU/GPU bucks are the dual MAX77621
         * shown on the PMIC page. The header is printed before this early
         * return so the pager page says N/A explicitly instead of coming
         * out blank, which would read like a crashed probe. */
        log_color(COL_DEFAULT,
            "  N/A          : Erista uses dual MAX77621 (see PMIC page)\n");
        return;
    }

    u8 addr = MAX77812_PHASE211_CPU_I2C_ADDR;
    u8 ver  = _max77812_probe_addr(addr);
    bool phase211 = true;
    if (ver == 0xFF) {
        addr = MAX77812_PHASE31_CPU_I2C_ADDR;
        ver  = _max77812_probe_addr(addr);
        phase211 = false;
    }
    if (ver == 0xFF) {
        log_color(COL_ERR, "  No MAX77812 at either 0x33 or 0x31\n");
        dx_set("max77812", DX_FAIL, "no MAX77812 ack");
        return;
    }
    static const char *ver_str[] = {
        [MAX77812_ES2_VERSION] = "ES2", [MAX77812_QS_VERSION] = "QS"
    };
    log_color(COL_OK,
        "  Variant      : %s @ 0x%02X (%s silicon)\n",
        phase211 ? "PHASE211 (retail)" : "PHASE31 (dev kit)",
        addr, ver_str[ver]);

    u8 en_ctrl   = i2c_recv_byte(I2C_5, addr, MAX77812_REG_EN_CTRL);
    u8 buck_stat = i2c_recv_byte(I2C_5, addr, MAX77812_REG_BUCK_STAT);
    u8 topsys    = i2c_recv_byte(I2C_5, addr, MAX77812_REG_TOPSYS_STAT);

    /* Per-phase config: name + register offset + sane voltage range.
     * On PHASE211 only M1 / M3 / M4 are populated; M2 reads back as
     * the M1 master because of internal phase tying. We only enumerate
     * the populated rails per variant. */
    struct rail {
        u8 vout_reg;
        u8 en_mask;
        const char *name;
        u32 vmin_mv;
        u32 vmax_mv;
    };
    /* PHASE211 retail layout: M1 = GPU, M3 = LPDDR4 VDD2 (1.1 V),
     * M4 = CPU. Sane operating bands chosen to span the DVFS range
     * documented for HOS plus a safety margin. */
    static const struct rail rails_phase211[] = {
        {MAX77812_REG_M1_VOUT, MAX77812_EN_CTRL_EN_M1_MASK, "M1 (GPU) ",  500, 1300},
        {MAX77812_REG_M3_VOUT, MAX77812_EN_CTRL_EN_M3_MASK, "M3 (DRAM)",  900, 1200},
        {MAX77812_REG_M4_VOUT, MAX77812_EN_CTRL_EN_M4_MASK, "M4 (CPU) ",  600, 1300},
    };
    static const struct rail rails_phase31[] = {
        {MAX77812_REG_M1_VOUT, MAX77812_EN_CTRL_EN_M1_MASK, "M1 (GPU) ",  500, 1300},
        {MAX77812_REG_M4_VOUT, MAX77812_EN_CTRL_EN_M4_MASK, "M4 (CPU) ",  600, 1300},
    };
    const struct rail *rails  = phase211 ? rails_phase211 : rails_phase31;
    size_t       n_rails = phase211 ? sizeof(rails_phase211)/sizeof(rails_phase211[0])
                                    : sizeof(rails_phase31)/sizeof(rails_phase31[0]);

    int n_oob = 0, n_on = 0;
    for (size_t i = 0; i < n_rails; i++) {
        u8 vout = i2c_recv_byte(I2C_5, addr, rails[i].vout_reg);
        u32 mv  = 250 + (vout & MAX77812_BUCK_VOLT_MASK) * 5;
        bool en = (en_ctrl & rails[i].en_mask) != 0;
        bool oob = en && (mv < rails[i].vmin_mv || mv > rails[i].vmax_mv);
        if (en) n_on++;
        if (oob) n_oob++;
        u32 col = oob ? COL_ERR : (en ? COL_OK : COL_DEFAULT);
        log_color(col,
            "  %s    : %s  %d.%03d V  (range %d.%03d - %d.%03d V)%s\n",
            rails[i].name,
            en ? "ON " : "off",
            mv / 1000, mv % 1000,
            rails[i].vmin_mv / 1000, rails[i].vmin_mv % 1000,
            rails[i].vmax_mv / 1000, rails[i].vmax_mv % 1000,
            oob ? " WRONG" : "");
    }

    /* In RCM the BPMP runs hwtest with the A57 CPU and GPU offline and
     * DRAM clocked through MAX77620 SD1 directly. MAX77812 is therefore
     * expected to be present-but-dormant: chip ACKs, all rails off,
     * EN_CTRL=0, no fault latches. Once HOS boots it brings the chip up.
     * Surface this as a friendly "dormant" line instead of three "off"
     * rails the tech might misread as faults. */
    if (n_on == 0)
        log_color(COL_DEFAULT,
            "                 (all rails dormant - normal in RCM, HOS brings them up)\n");

    LOG("  EN_CTRL      : 0x%02X\n", en_ctrl);
    log_color(buck_stat ? COL_WARN : COL_OK,
        "  BUCK_STAT    : 0x%02X%s\n", buck_stat,
        buck_stat ? " (per-rail fault latch set - check rails above)" : "");
    log_color(topsys ? COL_WARN : COL_OK,
        "  TOPSYS_STAT  : 0x%02X%s\n", topsys,
        topsys ? " (system-level fault: thermal / OV / UV)" : "");

    /* Verdict: only fault on real signals. Rail-off in RCM is expected,
     * not a fault. We FAIL when a rail is ON but VOUT is out of band
     * (mis-programmed regulator) or when BUCK/TOPSYS latches are set. */
    dx_set("max77812",
        (n_oob || buck_stat || topsys) ? DX_FAIL : DX_PASS,
        n_oob     ? "%d rails out of band" :
        buck_stat ? "BUCK_STAT 0x%02X"     :
        topsys    ? "TOPSYS_STAT 0x%02X"   : "",
        n_oob ? n_oob : (buck_stat ? buck_stat : topsys));
}

void probe_battery(void)
{
    int v = 0;
    HEADER("[MAX17050 fuel gauge, I2C1 @ 0x36]");

    if (max17050_get_property(MAX17050_RepSOC, &v) == 0) {
        int soc_int = (v >> 8) & 0xFF;
        log_color(health_color(soc_int, 30, HEALTH_NONE, 10, HEALTH_NONE),
            "  SOC          : %d.%d %%\n", soc_int, ((v & 0xFF) * 10) / 256);
    }
    if (max17050_get_property(MAX17050_VCELL, &v) == 0) {
        log_color(health_color(v, 3700, 4250, 3500, 4350),
            "  VCELL        : %d mV\n", v);
    }
    if (max17050_get_property(MAX17050_OCVInternal, &v) == 0)
        LOG("  OCV          : %d mV\n", v);
    if (max17050_get_property(MAX17050_TEMP, &v) == 0) {
        int t10 = v;
        log_color(health_color(t10 / 10, 5, 40, 0, 50),
            "  Temp         : %d.%d C\n",
            t10 / 10, (t10 >= 0 ? t10 : ~t10) % 10);
    }
    if (max17050_get_property(MAX17050_AvgCurrent, &v) == 0)
        LOG("  AvgCurrent   : %d mA\n", v / 1000);
    if (max17050_get_property(MAX17050_Current, &v) == 0)
        LOG("  Current      : %d mA\n", v / 1000);

    int rep = 0, full = 0, design = 0;
    max17050_get_property(MAX17050_RepCap,    &rep);
    max17050_get_property(MAX17050_FullCAP,   &full);
    max17050_get_property(MAX17050_DesignCap, &design);
    LOG("  RepCap       : %d mAh\n", rep);
    /* Battery health = full / design. Below 80 % means cells are aging. */
    int health_pct = design ? (full * 100 / design) : 0;
    log_color(health_color(health_pct, 80, HEALTH_NONE, 60, HEALTH_NONE),
        "  FullCap      : %d mAh (%d%% of design)\n", full, health_pct);
    LOG("  DesignCap    : %d mAh\n", design);
    dx_set("batt_health",
        health_pct < 60 ? DX_FAIL :
        health_pct < 80 ? DX_WARN : DX_PASS,
        health_pct < 80 ? "%d%% of design" : "", health_pct);

    /* Verdict signal from the temp read above (`v` was last assigned
     * MAX17050_TEMP). Negative = NTC disconnected, > 45 C = heat stress. */
    int t10 = 0;
    max17050_get_property(MAX17050_TEMP, &t10);
    dx_set("batt_ntc",
        t10 < 0      ? DX_FAIL :
        t10 / 10 >= 45 ? DX_WARN : DX_PASS,
        t10 < 0      ? "%d.%d C (NTC disconnected?)" :
        t10 / 10 >= 45 ? "%d.%d C hot" : "",
        t10 / 10, t10 < 0 ? -t10 % 10 : t10 % 10);

    if (max17050_get_property(MAX17050_MinVolt, &v) == 0)
        LOG("  Min volt     : %d mV\n", v);
    if (max17050_get_property(MAX17050_MaxVolt, &v) == 0)
        LOG("  Max volt     : %d mV\n", v);
    if (max17050_get_property(MAX17050_V_empty, &v) == 0)
        LOG("  V_empty      : %d mV\n", v);
    if (max17050_get_property(MAX17050_Age, &v) == 0)
        LOG("  Age          : %d %%\n", v);
    if (max17050_get_property(MAX17050_Cycles, &v) == 0) {
        /* Cycle count is purely informational: a high count on an otherwise
         * healthy pack is normal, not a fault. FullCap-vs-DesignCap above is
         * the actual wear signal. */
        LOG("  Cycles       : %d\n", v);
    }

    /* Predictive / learning fields not exposed by max17050_get_property:
     *   TTE  (REG11) : time-to-empty, LSB = 5.625 s. 0xFFFF = invalid /
     *                  charging. A live, settled gauge under load reports
     *                  a real number here; persistent 0 / 0xFFFF on a
     *                  discharging unit means the gauge can't predict.
     *   QH   (REG4D) : coulomb counter high word.
     *   QL   (REG4E) : coulomb counter low word.
     *   dQacc (REG45)/dPacc (REG46) : learning slew accumulators - non-
     *                  zero means the gauge is still relearning capacity.
     * The full QH:QL read is the absolute coulomb count since gauge POR;
     * we expose both halves so a tech can spot a stuck counter (both
     * always-zero) without doing the per-LSB unit math at this stage. */
    u16 tte = 0, qh = 0, ql = 0, dqacc = 0, dpacc = 0;
    i2c_recv_buf_small((u8 *)&tte,   2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_TTE);
    i2c_recv_buf_small((u8 *)&qh,    2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_QH);
    i2c_recv_buf_small((u8 *)&ql,    2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_QL);
    i2c_recv_buf_small((u8 *)&dqacc, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_dQacc);
    i2c_recv_buf_small((u8 *)&dpacc, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_dPacc);
    if (tte == 0xFFFF) {
        LOG("  TTE          : invalid / charging\n");
    } else {
        /* tte * 5.625 sec -> minutes = tte * 5625 / (60 * 1000) = tte * 9 / 96. */
        u32 min = ((u32)tte * 9) / 96;
        LOG("  TTE          : 0x%04X (%d min)\n", tte, min);
    }
    /* Slash-separated pair name in the key column: the host parser
     * splits each row on the first colon, so "QH:QL" would put "QL ..."
     * into the value column and "QH" into the key. Slash is safe. */
    LOG("  QH/QL        : 0x%04X / 0x%04X (coulomb counter, raw)\n", qh, ql);
    log_color((dqacc | dpacc) ? COL_DEFAULT : COL_WARN,
        "  dQacc/dPacc  : 0x%04X / 0x%04X%s\n", dqacc, dpacc,
        (dqacc | dpacc) ? " (gauge learning)" : " (no learning activity)");

    /* Identity + STATUS + FSTAT come from raw I2C reads, Hekate's
     * max17050_get_property only handles the curated charging fields
     * above. STATUS bit definitions (per the datasheet REG00):
     *   bit 15 BR  : battery-remove since last clear
     *   bit 14 SMX : SOC max alert
     *   bit 13 TMX : temp max alert
     *   bit 12 VMX : volt max alert
     *   bit 11 BI  : battery-insert since last clear
     *   bit 10 SMN : SOC min alert
     *   bit  9 TMN : temp min alert
     *   bit  8 VMN : volt min alert
     *   bit  3 BST : battery status (1 = absent)
     *   bit  1 POR : power-on reset latch
     *
     * FSTAT (REG3D) reflects the fuel-gauge internal state machine
     * (per Maxim MAX17050 datasheet + Linux max17042_battery driver):
     *   bit 0 DNR    : data not ready (still booting up)
     *   bit 6 RelDt2 : long-relax mode achieved (current near 0 for
     *                   extended time, gauge re-calibrated)
     *   bit 7 FQ     : full-charge detected during last cycle
     *   bit 8 EDet   : empty detection
     *   bit 9 RelDt  : relax mode active (current near 0 right now)
     * Bits 10-15 are reserved internal state, may read non-zero.
     *
     * DevName (REG21) returns 0x00AC for genuine MAX17050. */
    u16 status = 0, fstat = 0, devname = 0, manname = 0;
    i2c_recv_buf_small((u8 *)&status,  2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_STATUS);
    i2c_recv_buf_small((u8 *)&fstat,   2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_FSTAT);
    i2c_recv_buf_small((u8 *)&devname, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_DevName);
    i2c_recv_buf_small((u8 *)&manname, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_ManName);

    log_color(devname == 0x00AC ? COL_OK : COL_WARN,
        "  DevName      : 0x%04X%s\n", devname,
        devname == 0x00AC ? " (MAX17050 verified)" : " (UNEXPECTED, not MAX17050?)");
    dx_set("fuel_devname", devname == 0x00AC ? DX_PASS : DX_FAIL,
        devname == 0x00AC ? "" : "DevName 0x%04X != MAX17050", devname);
    LOG("  ManName      : 0x%04X\n", manname);

    /* STATUS decode, POR alone is normal at first boot after pack
     * reset/battery swap. BR/BI alone are normal at insertion. Persistent
     * VMN/TMN/SMN/VMX/TMX/SMX would be the actually-interesting alarms. */
    char sbuf[96] = {0};
    if (status == 0) {
        log_color(COL_OK, "  STATUS       : 0x0000 (no alerts)\n");
    } else {
        if (status & 0x8000) strcat(sbuf, "BR,");
        if (status & 0x4000) strcat(sbuf, "SMX,");
        if (status & 0x2000) strcat(sbuf, "TMX,");
        if (status & 0x1000) strcat(sbuf, "VMX,");
        if (status & 0x0800) strcat(sbuf, "BI,");
        if (status & 0x0400) strcat(sbuf, "SMN,");
        if (status & 0x0200) strcat(sbuf, "TMN,");
        if (status & 0x0100) strcat(sbuf, "VMN,");
        if (status & 0x0008) strcat(sbuf, "BST(absent),");
        if (status & 0x0002) strcat(sbuf, "POR,");
        u32 n = strlen(sbuf);
        if (n) sbuf[n - 1] = 0;
        /* POR-only or BI-only is benign; anything else flagged warn. */
        bool benign = !(status & ~(0x0002 | 0x0800));
        if (status & 0x0002 && benign)
            strcat(sbuf, " (relearn pending, readings advisory)");
        log_color(benign ? COL_DEFAULT : COL_WARN,
            "  STATUS       : 0x%04X (%s)\n", status, sbuf);
    }
    /* The POR latch is the gauge's own power-cycle marker - expected
     * after any battery swap/disconnect and on bench consoles, so it
     * does not weigh on the verdict. The STATUS line above already
     * carries the "relearn pending" note for the report. */

    /* FSTAT: RelDt set means current is near zero and the fuel-gauge
     * values you're reading are fully trustworthy. DNR set means the
     * gauge is still warming up. We mask off the reserved high bits
     * (10-15) before scanning so noise in those bits doesn't get
     * misclassified. */
    char fbuf[80] = {0};
    if (fstat & 0x0001) strcat(fbuf, "DNR,");
    if (fstat & 0x0040) strcat(fbuf, "RelDt2(long-relax),");
    if (fstat & 0x0080) strcat(fbuf, "FQ(full-charge),");
    if (fstat & 0x0100) strcat(fbuf, "EDet(empty),");
    if (fstat & 0x0200) strcat(fbuf, "RelDt(relaxed),");
    u32 fn = strlen(fbuf);
    if (fn) fbuf[fn - 1] = 0;
    log_color((fstat & 0x0001) ? COL_WARN : COL_DEFAULT,
        "  FSTAT        : 0x%04X (%s)\n", fstat,
        fn ? fbuf : "no documented flags set");
}

void probe_charger(void)
{
    int v = 0;
    HEADER("[BQ24193 charger, I2C1 @ 0x6B]");

    u8 stat   = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, 0x08); /* status */
    /* BQ24193 FAULT_REG (0x09) is double-read latched: per the datasheet
     * "the first read reports any latched faults since the last read;
     *  subsequent reads report the current fault state". So we read twice
     *, first read shows what HAPPENED (any sticky events from prior PD
     * negotiation, last shutdown, etc.), second read shows what's HAPPENING
     * RIGHT NOW. A latched-but-cleared event (first != 0, second == 0) is
     * benign and tech-normal, it usually just means VBUS dipped during
     * a PD handshake and recovered. A persistent fault (second != 0)
     * means something is actively wrong. */
    u8 fault1 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, 0x09);
    u8 fault2 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, 0x09);
    static const char *vbus[]  = {"none", "USB-SDP", "Adapter", "OTG"};
    static const char *charg[] = {"not charging", "pre", "fast", "done"};
    u8 vb = (stat >> 6) & 3;
    u8 ch = (stat >> 4) & 3;
    u8 pg = (stat >> 2) & 1;

    LOG("  VBUS_STAT    : %d (%s)\n", vb, vbus[vb]);
    LOG("  CHRG_STAT    : %d (%s)\n", ch, charg[ch]);
    /* Power-good must follow VBUS: VBUS present but PG=0 means a charge
     * IC fault, likely BQ or surrounding circuitry. */
    log_color((vb != 0 && pg == 0) ? COL_ERR : COL_OK,
        "  PG_STAT      : %d\n", pg);
    /* Verdict signals: VBUS-vs-Power-Good consistency, and any sticky
     * fault bit on the now-stable read. Bits 7..3 of FAULT_REG are
     * WDOG, BOOST, CHRG[1:0], BAT, NTC[2:0]. */
    dx_set("charger_pg",
        (vb != 0 && pg == 0) ? DX_FAIL : DX_PASS,
        (vb != 0 && pg == 0) ? "VBUS present but PG=0" : "");
    /* Bit 7 is the I2C watchdog, and it is excluded on purpose. The BQ's
     * watchdog exists to return the charger to defaults when the host stops
     * talking to it; in RCM nothing ever writes to the BQ, so it expires by
     * design after ~40 s and latches on any healthy console once a sweep
     * runs longer than that. Flagging it would fail every good unit. The
     * real fault bits - BOOST, CHRG, BAT, NTC - are still checked. */
    u8 fault_real = fault2 & 0x78;
    dx_set("charger_fault",
        fault_real ? DX_FAIL : DX_PASS,
        fault_real ? "FAULT 0x%02X latched" : "", fault_real);
    if (fault2 & 0x80)
        LOG("  (watchdog bit set - expected in RCM, not a fault)\n");

    /* Decode all FAULT_REG bits. Bit map (per Hekate's bq24193.h, which
     * matches the BQ24193 datasheet REG09 layout):
     *   bit 7    : WATCHDOG_FAULT
     *   bit 6    : BOOST_FAULT (5V boost / OTG over-current)
     *   bits 5:4 : CHRG_FAULT  (00=normal, 01=input, 10=thermal, 11=safety-timer)
     *   bit 3    : BATT_OVP    (battery over-voltage)
     *   bits 2:0 : NTC_FAULT   (000=normal, 010=warm, 011=cool, 101=cold, 110=hot) */
    static const char *chrg_fault_str[4] = {
        "", "input", "thermal", "safety-timer"};
    static const char *ntc_fault_str[8] = {
        "", "?", "warm", "cool", "?", "cold", "hot", "?"};
    for (int pass = 0; pass < 2; pass++) {
        u8 f = pass ? fault2 : fault1;
        const char *label = pass ? "now      " : "since last";
        char buf[96];
        buf[0] = 0;
        if (f) {
            char tmp[24];
            if (f & 0x80) { strcat(buf, "watchdog,"); }
            if (f & 0x40) { strcat(buf, "boost,"); }
            u8 cf = (f >> 4) & 0x3;
            if (cf)       { s_printf(tmp, "%s,", chrg_fault_str[cf]);
                            strcat(buf, tmp); }
            if (f & 0x08) { strcat(buf, "batt-OVP,"); }
            u8 nf = f & 0x7;
            if (nf)       { s_printf(tmp, "NTC-%s,", ntc_fault_str[nf]);
                            strcat(buf, tmp); }
            u32 n = strlen(buf);
            if (n) buf[n - 1] = 0;            /* strip trailing comma */
        }
        u32 col = (f == 0) ? COL_OK
                : (pass    ? COL_ERR
                           : COL_WARN);       /* latched-only is yellow */
        log_color(col, "  FAULT (%s) : 0x%02X (%s)\n",
            label, f, f == 0 ? "none" : buf);
    }

    if (bq24193_get_property(BQ24193_InputCurrentLimit, &v) == 0)
        LOG("  IN current   : %d mA\n", v);
    if (bq24193_get_property(BQ24193_InputVoltageLimit, &v) == 0)
        LOG("  IN voltage   : %d mV\n", v);
    if (bq24193_get_property(BQ24193_SystemMinimumVoltage, &v) == 0)
        LOG("  System min   : %d mV\n", v);
    if (bq24193_get_property(BQ24193_FastChargeCurrentLimit, &v) == 0)
        LOG("  FastCharge   : %d mA\n", v);
    if (bq24193_get_property(BQ24193_ChargeVoltageLimit, &v) == 0)
        LOG("  CV target    : %d mV\n", v);
    if (bq24193_get_property(BQ24193_ThermalRegulation, &v) == 0)
        LOG("  Therm thresh : %d C (charge-throttle cutoff, IC default 120)\n", v);

    /* BQ24193_TempStatus decodes the NTC fault register: 0 normal,
     * 2 warm, 3 cool, 5 cold, 6 hot. THIS is the real-time battery
     * temperature class. Anything other than 0 means the IC is
     * actively limiting charge based on the battery's NTC sensor. */
    if (bq24193_get_property(BQ24193_TempStatus, &v) == 0) {
        const char *ts = "?";
        u32 tcol = COL_OK;
        switch (v) {
        case 0: ts = "normal"; break;
        case 2: ts = "warm";   tcol = COL_WARN; break;
        case 3: ts = "cool";   tcol = COL_WARN; break;
        case 5: ts = "cold";   tcol = COL_ERR;  break;
        case 6: ts = "hot";    tcol = COL_ERR;  break;
        }
        log_color(tcol, "  Batt NTC     : %d (%s)\n", v, ts);
    }

    if (bq24193_get_property(BQ24193_DevID, &v) == 0)
        LOG("  DevID        : 0x%02X\n", v);

    /* Configuration fields not exposed by bq24193_get_property. These are
     * printed INFORMATIONAL-ONLY (no pass/fail): in RCM the charger has not
     * yet been configured by HOS, so REG03/05/07 may still hold POR defaults
     * rather than the operational config. No fixed threshold can tell those
     * two states apart, so we surface the raw decode and let a tech interpret
     * it against a known-good unit instead.
     *   REG03 IPRECHG  : pre-charge current (high nibble). 128 + N*128 mA.
     *   REG03 ITERM    : termination current (low nibble). Same encoding.
     *   REG05 ENTIMER  : safety timer enable / CHGTIMER duration.
     *   REG05 WATCHDOG : I2C watchdog (off/40s/80s/160s).
     *   REG07 BATFET_DI: BATFET disable latch (battery isolated when set).
     * Note: a genuine BATFET-off fault is only reliably observable once HOS
     * has configured the charger; that check belongs in an in-HOS test. */
    u8 reg03 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_PreChrgTerm);
    u8 reg05 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_ChrgTermTimer);
    u8 reg07 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_Misc);
    u32 iprechg = ((reg03 >> 4) & 0xF) * 128 + 128;
    u32 iterm   = ((reg03     ) & 0xF) * 128 + 128;
    LOG("  IPRECHG      : %d mA (REG03 high nibble, RCM default)\n", iprechg);
    LOG("  ITERM        : %d mA (REG03 low nibble, RCM default)\n", iterm);
    bool entimer = (reg05 & BQ24193_CHRGTERM_ENTIMER_MASK) != 0;
    static const char *chgtimer_str[4] = {"5h", "8h", "12h", "20h"};
    static const char *watchdog_str[4] = {"disabled", "40s", "80s", "160s"};
    u8  chgtimer_idx = (reg05 & BQ24193_CHRGTERM_CHGTIMER_MASK) >> 1;
    u8  wdog_idx     = (reg05 & BQ24193_CHRGTERM_WATCHDOG_MASK) >> 4;
    LOG("  Safety timer : %s (CHGTIMER=%s)\n",
        entimer ? "ON" : "DISABLED", chgtimer_str[chgtimer_idx]);
    LOG("  I2C watchdog : %s\n", watchdog_str[wdog_idx]);
    bool batfet_off = (reg07 & BQ24193_MISC_BATFET_DI_MASK) != 0;
    LOG("  BATFET       : %s (REG07, informational in RCM)\n",
        batfet_off ? "DISABLED" : "enabled");

    /* Cross-check via Hekate's bq24193_get_version helper, it reads
     * VendorPart (reg 0x0A) and confirms it equals 0x2F for the genuine
     * BQ24193 part. A mismatch here on a unit that ACKs at 0x6B means
     * a non-genuine replacement chip has been swapped in. */
    u32 bqver = 0;
    if (bq24193_get_version(&bqver) == 0) {
        log_color(COL_OK,
            "  Vendor part  : 0x%02X (BQ24193 verified)\n", bqver);
    } else {
        log_color(COL_WARN,
            "  Vendor part  : 0x%02X (UNEXPECTED, not BQ24193?)\n", bqver);
    }
}

void probe_usbpd(void)
{
    HEADER("[BM92T36 USB-PD, I2C1 @ 0x18]");
    u32 ver = 0;
    int ver_res = bm92t36_get_version(&ver);
    if (ver_res == 0)
        LOG("  Version      : 0x%08X\n", ver);
    else
        log_color(COL_ERR, "  Version read FAILED\n");
    dx_set("usb_pd", (ver_res == 0 && ver != 0) ? DX_PASS : DX_FAIL,
        (ver_res == 0 && ver != 0) ? "" : "BM92T36 not responding");

    bool inserted = false;
    usb_pd_objects_t pd = {0};
    bm92t36_get_source_info(&inserted, &pd);
    LOG("  Cable        : %s\n", inserted ? "inserted" : "not inserted");
    LOG("  PDO count    : %d\n", pd.pdo_no);
    for (u32 i = 0; i < pd.pdo_no && i < 7; i++) {
        LOG("    PDO[%d]    : %d V  %d mA\n", i,
            pd.pdos[i].voltage, pd.pdos[i].amperage);
    }
    if (inserted) {
        LOG("  Selected     : %d V  %d mA\n",
            pd.selected_pdo.voltage, pd.selected_pdo.amperage);
    }

    /* STATUS2 register (0x04) carries the live cable / orientation
     * state. The BM92T36 datasheet exposes:
     *   bit 0..3 : VBUS state, current PD power role, etc.
     *   bit 4    : data-role (UFP=0 / DFP=1)
     *   bit 5    : CC orientation (0 = CC1 normal, 1 = CC2 flipped)
     *   bit 6..7 : SOP'/SOP'' presence (cable-marker chip)
     * A single bad CC pin is one of the most common dock-fault complaints
     * (the user's USB-C port works one way but not the other), so this
     * single bit is worth surfacing alongside the inserted/PDO state. */
    u8 status2[2] = {0};
    if (i2c_recv_buf_big(status2, 2, I2C_1, BM92T36_I2C_ADDR, 0x04) == 0) {
        u8 s2 = status2[0];
        LOG("  STATUS2      : 0x%02X 0x%02X\n", status2[0], status2[1]);
        log_color(COL_OK,
            "  Data role    : %s\n", (s2 & (1u << 4)) ? "DFP (host)" : "UFP (device)");
        log_color(COL_OK,
            "  CC orient.   : %s\n", (s2 & (1u << 5)) ? "CC2 (cable flipped)"
                                                      : "CC1 (cable normal)");
    } else {
        log_color(COL_WARN, "  STATUS2 read FAILED (cable disconnected?)\n");
    }
}

void probe_5v(void)
{
    HEADER("[5V regulator]");
    /* The 5V regulator drives three downstream loads: the cooling FAN
     * (T210B01 only), and the Joy-Con left / right rails. Each is
     * independently enabled by the bdk on demand. */
    LOG("  FAN          : %s\n",
        regulator_5v_get_dev_enabled(REGULATOR_5V_FAN)  ? "enabled" : "off");
    LOG("  Joy-Con L    : %s\n",
        regulator_5v_get_dev_enabled(REGULATOR_5V_JC_L) ? "enabled" : "off");
    LOG("  Joy-Con R    : %s\n",
        regulator_5v_get_dev_enabled(REGULATOR_5V_JC_R) ? "enabled" : "off");
}

/* MAX77620 internal GPIO state. The PMIC has 8 GPIO pins (GPIO0..GPIO7)
 * exposed at config registers 0x36..0x3D. Each register encodes:
 *   bit 0 : drive type   (0=open-drain, 1=push-pull)
 *   bit 1 : direction    (0=output, 1=input)
 *   bit 2 : input value  (live readback when direction=input)
 *   bit 3 : output value (latch when direction=output)
 *
 * On the Switch, GPIO3/5/6/7 participate in the MAX77620 Flexible
 * Power Sequencer (FPS), the PMIC's autonomous rail-up/rail-down
 * state machine. They are NOT simple "rail enable" outputs in the
 * GPIO-driven sense; they're FPS source/sink signals coupling the
 * PMIC to external enables and to the SoC. The rails themselves
 * (CPU/GPU bucks, DSI 1.2 V, 3.3 V general) come up via FPS timer
 * slots configured in OTP, with these GPIO pins acting as
 * coordinating signals.
 *
 * Hekate's bdk/power/max7762x.h has a brief comment associating
 * these pins with the rails they coordinate with, annotations
 * below mirror that, but treat them as informational rather than
 * "if it's low, the rail is off". The actual rail status is in the
 * Regulators page (probe_regulators). */
void probe_pmic_gpios(void)
{
    HEADER("[MAX77620 GPIOs]");
    /* GPIO5/GPIO6 are FPS (Flexible Power Sequencer) source pins to the
     * external buck regulator. On Erista that's the dual MAX77621
     * (separate CPU and GPU chips); on Mariko/OLED/Lite it's the
     * multi-phase MAX77812 (M1=GPU, M4=CPU). Pick the label per SoC. */
    bool mariko = (((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2);
    const char *cpu_role = mariko ? "FPS - MAX77812 M4 (CPU)"
                                  : "FPS - CPU MAX77621";
    const char *gpu_role = mariko ? "FPS - MAX77812 M1 (GPU)"
                                  : "FPS - GPU MAX77621";
    const struct {
        u8 idx;
        const char *role;
    } pins[] = {
        {0, "generic"},
        {1, "generic"},
        {2, "generic"},
        {3, "FPS - 3.3V rail"},
        {4, "generic"},
        {5, cpu_role},
        {6, gpu_role},
        {7, "FPS - LDO0 (DSI 1.2V)"},
    };
    for (size_t i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) {
        u8 reg = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR,
                               MAX77620_REG_GPIO0 + pins[i].idx);
        const char *dir = (reg & 0x02) ? "input " : "output";
        const char *drv = (reg & 0x01) ? "push-pull " : "open-drain";
        int in_val  = (reg & 0x04) ? 1 : 0;
        int out_val = (reg & 0x08) ? 1 : 0;
        LOG("  GPIO%d        : 0x%02X  %s %s  IN=%d OUT=%d  (%s)\n",
            pins[i].idx, reg, dir, drv, in_val, out_val, pins[i].role);
    }

    /* FPS (Flexible Power Sequencer) configuration. The MAX77620 has
     * three FPS masters (FPS0, FPS1, FPS2) and each rail / GPIO can be
     * assigned to one of them with a per-rail slot in the sequence.
     * Reading FPS_CFGx surfaces the master config:
     *   bit 0   ENFPS_SW   - SW-controlled enable
     *   bits 2:1 EN_SRC    - which event triggers this FPS master
     *   bits 5:3 TIME_PERIOD - per-slot dwell time, encoded as
     *               40 us << N (so 0=40us .. 6=2560us, 7=reserved).
     * Switch HOS programs FPS0 to a 1280-2560 us slot period; a
     * mis-configured (very short) period can race rail ramp times
     * and explain otherwise-mysterious boot stalls. */
    static const u8 fps_regs[3] = {
        MAX77620_REG_FPS_CFG0, MAX77620_REG_FPS_CFG1, MAX77620_REG_FPS_CFG2
    };
    for (int f = 0; f < 3; f++) {
        u8 cfg = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, fps_regs[f]);
        u8 tp_code = (cfg & MAX77620_FPS_TIME_PERIOD_MASK)
                     >> MAX77620_FPS_TIME_PERIOD_SHIFT;
        u8 en_src  = (cfg & MAX77620_FPS_EN_SRC_MASK)
                     >> MAX77620_FPS_EN_SRC_SHIFT;
        u32 tp_us  = tp_code < 7 ? (40u << tp_code) : 0;
        LOG("  FPS%d cfg     : 0x%02X (slot=%d us, src=%d, sw_en=%d)\n",
            f, cfg, tp_us, en_src, cfg & MAX77620_FPS_ENFPS_SW_MASK);
    }
}

/* Cooling fan probe.
 *
 * Every Switch model is actively cooled: Erista, Mariko, Lite and OLED all
 * have a fan on Tegra PWM channel 1 with tach feedback on GPIO_PORT_S pin 7.
 * The only SoC difference is that the tach line needs an internal pull-up on
 * T210 and not on T210B01, which fan_set_duty() applies for us. (OLED/AULA
 * additionally needs the PWM clock enabled, also handled there.)
 *
 * Two readings:
 *   - PWM duty: instantaneous, derived from PWM_CSR_1 bits 16:23.
 *     0 = fan off, ~236 = max speed (Hekate inverts the polarity
 *     so the register value 0xEC = 0% to the fan).
 *   - Tach RPM: counted from edges on the tach line (PORT_S pin 7).
 *
 * This is the one probe that deliberately DRIVES the hardware instead of
 * only reading it, because a passive fan check cannot work: in RCM the fan
 * is off, so a healthy stopped fan and a dead fan both read 0 RPM and are
 * indistinguishable. To get an answer we have to command a duty and see
 * whether the tach responds.
 *
 * The probe therefore runs in two parts:
 *   1. Report the state the fan was FOUND in (passive).
 *   2. Spin it up at a fixed duty, measure, then restore the found state.
 *
 * fan_set_duty() is self-contained: it sets up the tach pinmux/GPIO, enables
 * the PWM clock on AULA, and brings REGULATOR_5V_FAN up (and back down on
 * duty 0), so the 5 V supply can't be the reason for a false stall.
 *
 * Repair-tech use case: 0 RPM while we are commanding a duty is a clear
 * "fan dead / seized / unplugged, or tach line broken" verdict. A few
 * thousand RPM is healthy. */

/* Commanded duty for the active test, and the kick that gets there.
 *
 * Duty 100 holds a fan that is ALREADY TURNING, but it does not reliably
 * break stiction from a dead stop: a cold fan can sit at 0 rpm at that duty
 * and only start turning once something else gets it moving, after which it
 * reads a few thousand rpm, climbing run over run while it stays turning,
 * until it coasts to a stop again. The absolute RPM is therefore not
 * repeatable between runs - only "turning at all" is. That is stiction, not
 * a broken tachometer.
 *
 * So: kick at full duty to get it moving, then drop to the measurement duty
 * and let it settle before sampling. This is the ordinary way to start a
 * brushless fan and it makes the reading independent of how recently the
 * console last ran. */
#define FAN_KICK_DUTY   236   /* max, to break stiction from standstill */
#define FAN_KICK_MS     600
/* Extra current draw that counts as "the motor is actually turning". The
 * Switch fan pulls well over this at duty 100; the gauge's sample-to-sample
 * jitter is a few mA, so this clears the noise with room to spare. */
#define FAN_CURRENT_MIN_MA  25
#define FAN_TEST_DUTY   100
/* Time allowed to reach steady state at the test duty before sampling. */
#define FAN_SPINUP_MS   2000
/* Tach sampling window. At the test duty this is a few hundred edges,
 * far more resolution than the alive/dead question needs. */
/* hekate polls this signal for 2 s and notes 5 s is needed for an accurate
 * count. The window sets the resolution: rpm comes out in steps of
 * 60000/FAN_SAMPLE_MS, so 1 s quantises to 60 rpm while a 100 ms window
 * would quantise to 600. 1 s keeps the probe quick and the step small
 * enough that the reading is worth printing. */
#define FAN_SAMPLE_MS   1000

/* Count debounced rising tach pulses on PORT_S pin 7 for `ms`, and convert
 * to RPM. Each revolution yields 2 tach pulses, so
 * rpm = (pulses / 2) * (60000 / ms). */
/* Put the tachometer pad in a state where it can actually be read.
 *
 * bdk's fan_set_duty() does this, but only inside a `if (!fan_init)` one-shot
 * that also early-returns when the requested duty equals the current one - so
 * whether the pad is configured by the time we sample depends on the call
 * history, not on anything this probe controls. Configure it here, every
 * time, exactly as bdk does: CAM1_PWDN to SFIO 1, input receiver on,
 * tristated, and pulled up on Erista (T210B01 drives it, so no pull), then
 * the GPIO to input.
 *
 * This is the same trap as UART-B RX elsewhere in this file: a pad whose
 * input receiver was never enabled reads a constant, and a constant reads as
 * "no edges", which reads as "dead fan". */
static void _fan_tach_pad_init(void)
{
    u32 pull = (((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 1)
                 ? PINMUX_PULL_UP : 0;
    PINMUX_AUX(PINMUX_AUX_CAM1_PWDN) =
        PINMUX_TRISTATE | PINMUX_INPUT_ENABLE | pull | 1;
    gpio_direction_input(GPIO_PORT_S, GPIO_PIN_7);
    (void)GPIO(0x004);   /* commit */
}

/* Count tach pulses the way hekate does: debounced rising pulses over a
 * window long enough to matter. Its own comment - "Poll irqs for 2 seconds
 * (5 seconds for accurate count)" - is the tell that this signal is far
 * slower than a naive sample window assumes, so the sample window here is a
 * full second.
 *
 * The Switch fan is 4-pole: two tach pulses per revolution, hence /2. */
static u32 _fan_measure_rpm(u32 ms, u32 *out_edges, u32 *out_trans, u32 *out_lo)
{
    u32 count = 0, trans = 0, lo = 0;
    int last = gpio_read(GPIO_PORT_S, GPIO_PIN_7);
    /* Arm from the line's ACTUAL starting level, not unconditionally.
     * Starting armed means a line sitting permanently high scores its very
     * first sample as a pulse and reports "1 pulse", which then reads as
     * "the tach is toggling, just slowly" - the exact opposite of the truth.
     * A line that never moves must count zero. */
    bool armed = !last;
    u32 deadline = get_tmr_us() + ms * 1000;

    while ((s32)(deadline - get_tmr_us()) > 0) {
        int v = gpio_read(GPIO_PORT_S, GPIO_PIN_7);
        if (v) {
            if (armed) { count++; armed = false; }
        } else {
            lo++;
            armed = true;
        }
        if (v != last)
            trans++;
        last = v;
    }

    if (out_edges) *out_edges = count;
    /* Transitions and low-sample count separate "the pad never saw the line
     * move" from anything to do with this function's arithmetic. */
    if (out_trans) *out_trans = trans;
    if (out_lo)    *out_lo    = lo;

    /* pulses -> rev/min: count over `ms`, two pulses per revolution. */
    return (count / 2) * (60000 / ms);
}

/* Battery current in mA, averaged. Negative = discharging. The fuel gauge
 * reports in microamps and jitters by a few mA sample to sample, so take
 * several and mean them - the fan's own draw is tens of mA and has to be
 * separable from that noise. */
static int _fan_current_ma(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        if (max17050_get_property(MAX17050_Current, &v) == 0) {
            sum += v / 1000;
            n++;
        }
        msleep(25);
    }
    return n ? sum / n : 0;
}

void probe_fan(void)
{
    HEADER("[Cooling fan]");

    /* PWM_CSR_1 layout for the fan channel:
     *   bit 31    : PWM_CSR_EN , channel enable
     *   bit 24    : PWM_CSR_M  , absolute 0% override (Hekate uses
     *                             this to disable the fan without
     *                             touching the duty bits)
     *   bits 23:16: duty (inverted polarity per Hekate fan_set_duty:
     *               register 0 = 100%, register 236 = ~0%)
     *
     * Hekate's own fan_get_speed has a bug where it doesn't check the
     * enable / absolute-0 bits before back-calculating duty, so it
     * reports "duty 236 = max" when the fan is in fact disabled. We
     * decode all three properly. */
    u32 csr      = PWM(PWM_CONTROLLER_PWM_CSR_1);
    bool ch_en   = (csr & (1u << 31)) != 0;
    bool abs_off = (csr & (1u << 24)) != 0;
    u32 inv_duty = (csr >> 16) & 0xFF;
    u32 duty     = (!ch_en || abs_off) ? 0
                 : (inv_duty >= 236)   ? 0
                                       : (236 - inv_duty);
    int duty_pct = (duty * 100) / 236;
    log_color(duty == 0 ? COL_DEFAULT : COL_OK,
        "  PWM duty     : %d/236 (%d%%)%s\n",
        duty, duty_pct,
        !ch_en  ? "  [PWM channel disabled]"
        /* Plain "%", not "%%": this string is a printf ARGUMENT, not part of
         * the format, so the escape is never consumed and "%%" would reach
         * the console verbatim. */
        : abs_off ? "  [absolute 0% override]"
                  : "");

    /* If the fan was already commanded before we ran (some launch paths do
     * this), report what it's doing right now without disturbing it. */
    if (duty > 0) {
        u32 idle_rpm = _fan_measure_rpm(FAN_SAMPLE_MS, NULL, NULL, NULL);
        log_color(idle_rpm ? COL_OK : COL_ERR,
            "  As found     : ~%d RPM at duty %d\n", idle_rpm, duty);
    }

    /* --- Active test: command a known duty and see if the tach answers. --- */
    log_color(COL_INFO,
        "  Spin-up test : kick %d for %d ms, then %d for %d ms...\n",
        FAN_KICK_DUTY, FAN_KICK_MS,
        FAN_TEST_DUTY, FAN_SPINUP_MS + FAN_SAMPLE_MS);

    /* Baseline current before the fan is driven. A turning fan draws real
     * current, so comparing before/after tells us whether the motor is
     * running INDEPENDENTLY of the tachometer. That distinction matters: a
     * fan can spin audibly while its tach line stays flat, and calling that
     * "fan seized" is a false failure. Averaged over a few samples because
     * the charger makes a single reading noisy. */
    int i_before = _fan_current_ma();

    /* Kick first (see FAN_KICK_DUTY): full duty to break stiction, then the
     * measurement duty. Without the kick a stopped fan can simply never start
     * at duty 100 and the tach correctly reads zero - which looks exactly
     * like a dead tachometer. */
    fan_set_duty(FAN_KICK_DUTY);
    _fan_tach_pad_init();         /* after bdk, so ours is what sticks */
    msleep_poll(FAN_KICK_MS);
    fan_set_duty(FAN_TEST_DUTY);
    msleep_poll(FAN_SPINUP_MS);   /* stays reboot-responsive */

    /* Measure with bdk's own fan_get_speed(), not a hand-rolled counter.
     *
     * fan_get_speed() polls the tach for a full 2 s and counts debounced
     * rising edges - and its own comment notes 5 s is needed for an accurate
     * count. Against a signal that slow, a window of a few hundred ms buys
     * speed at the cost of a coarse reading.
     *
     * Our own counter is kept, but only as a secondary line: two independent
     * numbers make "the tach is silent" and "our arithmetic is wrong" tell
     * themselves apart. */
    u32 bdk_duty = 0, rpm = 0;
    fan_get_speed(&bdk_duty, &rpm);

    u32 edges = 0;
    u32 tach_trans = 0, tach_lo = 0;
    u32 own_rpm = _fan_measure_rpm(FAN_SAMPLE_MS, &edges,
                                   &tach_trans, &tach_lo);
    LOG("  Tach detail  : bdk %d rpm | own %d rpm (%d pulses in %d ms)\n",
        rpm, own_rpm, edges, FAN_SAMPLE_MS);

    /* Current with the fan running, against the baseline taken before it was
     * driven. A fan that is actually turning draws current; one that is
     * seized or unplugged does not. This is what separates "the tachometer
     * cannot be read" from "the fan is not running", which the tach signal
     * alone cannot do. */
    int i_after = _fan_current_ma();
    int i_delta = i_before - i_after;   /* more discharge => positive */
    bool drawing = i_delta >= FAN_CURRENT_MIN_MA;
    LOG("  Fan current  : %d -> %d mA (delta %d mA, %s)\n",
        i_before, i_after, i_delta,
        drawing ? "motor is loaded" : "no extra draw");

    /* If the count is zero, say whether the pad could even have seen a pulse.
     * A tristated pad with its input receiver off reads a constant, and a
     * constant is indistinguishable from a stopped fan unless the pad state
     * is on the page next to it. */
    {
        u32 pmx = PINMUX_AUX(PINMUX_AUX_CAM1_PWDN);
        LOG("  Tach pad     : CAM1_PWDN=%04X E_INPUT=%d TRI=%d pull=%d lvl=%d\n",
            pmx & 0xFFFF, (pmx & PINMUX_INPUT_ENABLE) ? 1 : 0,
            (pmx & PINMUX_TRISTATE) ? 1 : 0, (pmx >> 2) & 3,
            gpio_read(GPIO_PORT_S, GPIO_PIN_7));

        /* bdk keeps the GPIO register offsets private to gpio.c. Same
         * formula. CNF says whether the GPIO controller actually owns the
         * pad - the pinmux above selects SFIO function 1, so without CNF
         * set this would read a constant no matter what the fan did. */
        u32 pofs = ((GPIO_PORT_S >> 2) << 8) + ((GPIO_PORT_S % 4) << 2);
        LOG("  Tach line    : %d transitions, %d low samples in %d ms\n",
            tach_trans, tach_lo, FAN_SAMPLE_MS);
        LOG("  Tach gpio    : CNF=%08X OE=%08X (PS7 cnf=%d oe=%d)\n",
            GPIO(0x00 + pofs), GPIO(0x10 + pofs),
            (GPIO(0x00 + pofs) & GPIO_PIN_7) ? 1 : 0,
            (GPIO(0x10 + pofs) & GPIO_PIN_7) ? 1 : 0);
    }

    /* Read the duty back through the same decode, so a PWM block that
     * silently ignored the write is distinguishable from a dead fan. */
    u32 csr_test      = PWM(PWM_CONTROLLER_PWM_CSR_1);
    u32 inv_duty_test = (csr_test >> 16) & 0xFF;
    bool test_en      = (csr_test & (1u << 31)) != 0;
    bool test_abs_off = (csr_test & (1u << 24)) != 0;
    u32 duty_readback = (!test_en || test_abs_off) ? 0
                      : (inv_duty_test >= 236)     ? 0
                                                   : (236 - inv_duty_test);

    /* Restore whatever we found. In RCM that is almost always 0, which also
     * drops REGULATOR_5V_FAN and parks the pinmux. */
    fan_set_duty(duty);

    log_color(duty_readback == FAN_TEST_DUTY ? COL_OK : COL_ERR,
        "  PWM readback : %d/236 (commanded %d)%s\n",
        duty_readback, FAN_TEST_DUTY,
        duty_readback == FAN_TEST_DUTY ? "" : "  - PWM did not take!");

    if (duty_readback != FAN_TEST_DUTY) {
        /* The PWM controller itself is the fault; the fan never got a
         * signal, so we cannot judge the fan or its tach. */
        log_color(COL_ERR,
            "  Tach RPM     : %d - inconclusive, PWM never applied\n", rpm);
        dx_set("fan_stalled", DX_FAIL, "PWM readback %d", duty_readback);
    } else if (rpm == 0) {
        /* Report the raw pulse count, always. "0 RPM" with the count hidden
         * is ambiguous: a single pulse halves and floors to zero, and zero
         * reads as "dead". The count separates "no signal at all" from
         * "signal, but under one revolution in the window". */
        log_color(tach_trans ? COL_WARN : COL_ERR,
            "  Tach RPM     : 0 at duty %d (%d pulses, %d transitions)\n",
            FAN_TEST_DUTY, edges, tach_trans);
        if (tach_trans) {
            LOG("  (the tach IS toggling, so the fan is turning - the pulse\n");
            LOG("   count is just too low for this window's arithmetic)\n");
            dx_set("fan_stalled", DX_WARN, "only %d tach pulses", edges);
        } else if (drawing) {
            /* The measurement that stops this being a false failure: the
             * motor is drawing current, so the fan IS running and only its
             * tachometer is unreadable. That is a real finding worth a
             * warning - the cooling still works - but it is not the hard
             * failure a seized fan would be. */
            log_color(COL_WARN,
                "  Fan verdict  : SPINNING (draws %d mA) but tach unreadable\n",
                i_delta);
            LOG("  (cooling works; only the tach signal is missing, so RPM\n");
            LOG("   cannot be measured on this unit - a broken tach wire or\n");
            LOG("   a replacement fan without one)\n");
            dx_set("fan_stalled", DX_WARN,
                   "spins (%d mA) but tach silent", i_delta);
        } else {
            /* No pulses AND no extra current: nothing is turning. This is
             * a hard failure regardless of cause (seized, unplugged or
             * unpowered): the console cannot measure the fan speed, which
             * is exactly the state that must not be reported as fine. */
            LOG("  (tach line never moved: it sat at %d for the whole\n",
                gpio_read(GPIO_PORT_S, GPIO_PIN_7));
            LOG("   window, and the motor drew no extra current - so the fan\n");
            LOG("   really is not turning: seized, unplugged or unpowered)\n");
            dx_set("fan_stalled", DX_FAIL, "no tach and no current draw");
        }
    } else {
        log_color(COL_OK,
            "  Tach RPM     : ~%d at duty %d (%d edges in %d ms)\n",
            rpm, FAN_TEST_DUTY, edges, FAN_SAMPLE_MS);
        dx_set("fan_stalled", DX_PASS, "");
    }
}

void probe_thermal(void)
{
    HEADER("[TMP451 thermal sensor, I2C1 @ 0x4C]");
    u16 soc = tmp451_get_soc_temp(false);
    u16 pcb = tmp451_get_pcb_temp(false);
    int soc_int = soc >> 8;
    int pcb_int = pcb >> 8;
    log_color(health_color(soc_int, HEALTH_NONE, 50, 0, 70),
        "  SoC die temp : %02d.%d C\n", soc_int, (soc & 0xFF) / 10);
    log_color(health_color(pcb_int, HEALTH_NONE, 40, HEALTH_NONE, 55),
        "  PCB temp     : %02d.%d C\n", pcb_int, (pcb & 0xFF) / 10);

    /* Status register (0x02) and conversion rate. TMP451 status:
     *   bit 7 BUSY, bit 6 LHIGH, bit 5 LLOW, bit 4 RHIGH, bit 3 RLOW,
     *   bit 2 OPEN (remote diode open circuit - SoC sensor unhooked),
     *   bit 1 RTHRM, bit 0 LTHRM (THERM2 trip flags).
     * OPEN is the failure mode that matters most for repair: a mechanically
     * loose or unsoldered diode line on the SoC under-fill.
     *
     * Note on register addresses: TMP451 uses split read / write
     * addresses for the configuration and limit registers (the BDK's
     * TMP451_CNV_RATE_REG = 0x0A is the *write* address). To read
     * them back we use the corresponding read addresses:
     *   0x03 = Config (read)        | 0x09 (write)
     *   0x04 = Cnv rate (read)      | 0x0A (write)
     *   0x05 = Local high limit     | 0x0B (write)
     *   0x06 = Local low limit      | 0x0C (write)
     *   0x07 = Remote high limit MSB| 0x0D (write)
     *   0x08 = Remote low limit MSB | 0x0E (write) */
    u8 status   = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x02);
    u8 cnv      = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x04);
    u8 lhigh    = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x05);
    u8 llow     = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x06);
    u8 rhigh    = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x07);
    u8 rlow     = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x08);
    bool open = (status & 0x04) != 0;
    log_color(open ? COL_ERR : COL_OK,
        "  Status       : 0x%02X%s%s%s%s%s%s\n", status,
        open            ? " OPEN(remote-diode)" : "",
        (status & 0x80) ? " BUSY"   : "",
        (status & 0x40) ? " LHIGH"  : "",
        (status & 0x20) ? " LLOW"   : "",
        (status & 0x10) ? " RHIGH"  : "",
        (status & 0x08) ? " RLOW"   : "");
    /* Conversion rate: 2^cnv / 16 Hz (0x06 = 4 Hz, default after init).
     *   cnv >= 4: rate = 1 << (cnv - 4) Hz (1, 2, 4, 8, 16, 32, 64)
     *   cnv <  4: rate = 1 / (1 << (4 - cnv)) Hz (1/16, 1/8, 1/4, 1/2) */
    static const char *cnv_str[] = {
        "1/16 Hz", "1/8 Hz", "1/4 Hz", "1/2 Hz",
        "1 Hz", "2 Hz", "4 Hz", "8 Hz",
        "16 Hz", "32 Hz", "64 Hz"
    };
    LOG("  Cnv rate     : 0x%02X (%s)\n", cnv,
        cnv < (sizeof(cnv_str)/sizeof(cnv_str[0])) ? cnv_str[cnv] : "?");

    /* Alert thresholds. The IC raises THERM and ALERT lines when
     * the corresponding limit is crossed. Limits are signed degrees
     * Celsius in the 8-bit MSB (the LSB sub-degree limits are not
     * used for thresholding on Switch). HOS / Hekate program these
     * to bracket the operating range; a unit with WRONG limits (or
     * default 0/85 from POR) is interesting because it means
     * software hasn't configured the sensor — we'd see a missing
     * thermal-management init somewhere. */
    LOG("  PCB limits   : low %d C / high %d C\n", (s8)llow, (s8)lhigh);
    LOG("  SoC limits   : low %d C / high %d C\n", (s8)rlow, (s8)rhigh);

    /* There is deliberately no SoC-die / PCB / battery "sensor agreement"
     * cross-check. The three sensors measure physically different points
     * with different thermal masses and response times; in a cold-boot RCM
     * state (no sustained load) they legitimately disagree, and the battery
     * NTC path in particular doesn't behave in a way that makes a spread
     * threshold meaningful. */

    /* Verdict signals. Tegra X1 throttles at 85 C; >70 C in handheld
     * idle is suspicious. PCB skin shouldn't exceed ~45 C even under
     * sustained load, so the threshold is tighter on channel 0. An OPEN
     * remote diode invalidates the SoC reading regardless of value. */
    dx_set("soc_die_temp",
        open          ? DX_FAIL :
        soc_int >= 85 ? DX_FAIL :
        soc_int >= 70 ? DX_WARN : DX_PASS,
        open          ? "remote diode OPEN" :
        soc_int >= 85 ? "%d C >= throttle" :
        soc_int >= 70 ? "%d C warm" : "", soc_int);
    dx_set("pcb_temp",
        pcb_int >= 60 ? DX_FAIL :
        pcb_int >= 45 ? DX_WARN : DX_PASS,
        pcb_int >= 60 ? "%d C (thermal pad?)" :
        pcb_int >= 45 ? "%d C warm" : "", pcb_int);
}


void probe_regulators(void)
{
    HEADER("[MAX77620 regulators]");
    int n_wrong = 0;
    /* For each rail surface:
     *   - Power-OK bit (rail crossed ~80% of target during ramp)
     *   - Configured voltage from the per-rail VOLT register
     *   - Exact expected voltage for the FIXED-target rails
     *
     * Voltage registers per Hekate's _pmic_regulators table:
     *   SD0..SD3   = 0x16..0x19 (12.5 mV steps from 600 mV, mask varies)
     *   LDO0..LDO8 = 0x23/0x25/0x27/.../0x33 (LDOx_CFG, low 6 bits = volt)
     *               step 25 mV (LDO0/1/4) or 50 mV (LDO2/3/5/6/7/8)
     * Decoded uV = (reg_val & mask) * uv_step + uv_min.
     *
     * `expect_uv` is the exact value BDK's max77620_config_default() programs
     * (the uv_default column of _pmic_regulators in max7762x.c). A fixed rail
     * that is ON must read this value EXACTLY, with no tolerance band. A band
     * would hide a mis-programmed regulator, so we compare for equality.
     *
     * A handful of rails genuinely move and cannot be checked against a
     * single value, so they carry expect_uv = 0 and are reported
     * informational-only (no verdict contribution):
     *   SD0  (CPU)   - DVFS, 0.6 V BPMP-idle up to 1.4 V under CPU load
     *   LDO2 (SDMMC1)- 1.8 V on UHS (SDR104) <-> 3.3 V on legacy SD
     *   LDO4 (RTC)   - 0.85 V at BDK default, 1.0 V after HOS keygen retune
     *   LDO8 (XUSB/DP)- multi-purpose, 1.05 V .. 2.8 V depending on use
     * Rails that are OFF also get no verdict (the configured voltage is
     * meaningless when the rail is disabled). */
    struct rail {
        int  id;
        const char *name;
        u8   volt_reg;
        u8   mask;
        u32  step_uv;
        u32  base_uv;
        u32  expect_uv;   /* exact expected on Erista; 0 = variable, info-only */
        u32  expect_uv_mrk; /* Mariko override; 0 = same as expect_uv */
    };
    /* Rail labels cross-referenced against Hekate's bdk/power/max7762x.h
     * "Switch Power domains" table. Expected voltages come from the
     * uv_default column of _pmic_regulators (max7762x.c), corrected against
     * hardware where the two disagree; hardware wins:
     *   SD1 (DRAM) : 1.125 V Erista (LPDDR4) vs 1.100 V Mariko (LPDDR4X).
     *   SD2 (LDOsrc): 1.350 V Erista vs 1.325 V Mariko. BDK lists 1.325 V as
     *                 uv_default for both, but Erista runs the table's max
     *                 value, so expecting 1.325 V there flags every healthy
     *                 Erista.
     * Both pairs measured on real unpatched consoles. */
    static const struct rail rails[] = {
        /* id  name              volt_reg mask  step    base   erista   mariko */
        {0,  "SD0  (SoC CPU)",   0x16, 0x7F, 12500, 600000,       0,       0 }, /* DVFS */
        {1,  "SD1  (DRAM)",      0x17, 0x7F, 12500, 600000, 1125000, 1100000 },
        {2,  "SD2  (LDO src)",   0x18, 0xFF, 12500, 600000, 1350000, 1325000 },
        {3,  "SD3  (1V8 gen)",   0x19, 0xFF, 12500, 600000, 1800000,       0 },
        {4,  "LDO0 (Display)",   0x23, 0x3F, 25000, 800000, 1200000,       0 },
        {5,  "LDO1 (XUSB+PCIE)", 0x25, 0x3F, 25000, 800000, 1050000,       0 },
        {6,  "LDO2 (SDMMC1)",    0x27, 0x3F, 50000, 800000,       0,       0 }, /* UHS<->legacy */
        {7,  "LDO3 (GC ASIC)",   0x29, 0x3F, 50000, 800000, 3100000,       0 },
        {8,  "LDO4 (RTC)",       0x2B, 0x3F, 12500, 800000,       0,       0 }, /* 0.8<->1.0 */
        {9,  "LDO5 (GC Card)",   0x2D, 0x3F, 50000, 800000,       0,       0 }, /* OTP/HOS state-dependent */
        {10, "LDO6 (Touch+ALS)", 0x2F, 0x3F, 50000, 800000,       0,       0 }, /* OTP/HOS state-dependent */
        {11, "LDO7 (XUSB)",      0x31, 0x3F, 50000, 800000,       0,       0 }, /* OTP/HOS state-dependent */
        {12, "LDO8 (XUSB/DP)",   0x33, 0x3F, 50000, 800000,       0,       0 }, /* multi-use */
    };
    bool is_mariko = (((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2);
    for (size_t i = 0; i < sizeof(rails)/sizeof(rails[0]); i++) {
        int ok = max77620_regulator_get_status(rails[i].id);
        u8  reg_val = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].volt_reg);
        u32 uv = ((u32)(reg_val & rails[i].mask)) * rails[i].step_uv + rails[i].base_uv;
        u32 expect = (is_mariko && rails[i].expect_uv_mrk)
                        ? rails[i].expect_uv_mrk : rails[i].expect_uv;
        if (!ok) {
            /* Rail off: configured voltage is meaningless, report neutral. */
            log_color(COL_DEFAULT,
                "  %s : off  %d.%03d V\n",
                rails[i].name, uv / 1000000, (uv / 1000) % 1000);
        } else if (expect == 0) {
            /* No fixed target. Two kinds of rail land here:
             *  - SD0 and LDO2/4/8 are DVFS/multi-use and scale at runtime.
             *  - LDO5/6/7 (game-card, touch/ALS, XUSB) are peripheral rails
             *    with no single RCM voltage to check against. Their value is
             *    EITHER the cold OTP default OR whatever HOS last programmed:
             *    a PMIC soft reset (our reboot path) preserves the HOS value,
             *    only a cold power-cycle restores the OTP default. On one
             *    Mariko, LDO6 reads 2.9 V warm-from-HOS but 2.8 V cold; the
             *    Erista reads 2.8 V too. No hardcoded expectation is right in
             *    both states, and none of these rails matters for booting -
             *    so report the live value without a verdict. */
            bool otp_rail = (rails[i].id == REGULATOR_LDO5 ||
                             rails[i].id == REGULATOR_LDO6 ||
                             rails[i].id == REGULATOR_LDO7);
            log_color(COL_OK,
                "  %s : ON   %d.%03d V  (%s)\n",
                rails[i].name, uv / 1000000, (uv / 1000) % 1000,
                otp_rail ? "OTP default; HOS may differ" : "variable / DVFS");
        } else {
            /* Fixed rail: must match the expected value EXACTLY. */
            bool match = (uv == expect);
            if (!match) n_wrong++;
            log_color(match ? COL_OK : COL_ERR,
                "  %s : ON   %d.%03d V  (expect %d.%03d V)%s\n",
                rails[i].name,
                uv / 1000000, (uv / 1000) % 1000,
                expect / 1000000, (expect / 1000) % 1000,
                match ? "" : " WRONG");
        }
    }
    dx_set("pmic_rails", n_wrong ? DX_FAIL : DX_PASS,
        n_wrong ? "%d ON but wrong voltage" : "", n_wrong);
}

