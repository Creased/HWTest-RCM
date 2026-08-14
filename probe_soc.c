/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: soc probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* Probes                                                                   */

void probe_soc(void)
{
    HEADER("[SoC]");
    u32 hidrev = APB_MISC(APB_MISC_GP_HIDREV);
    u32 chip_major = (hidrev >> 4) & 0xF;
    u32 chip_minor = (hidrev >> 16) & 0xF;
    u32 chip_id    = (hidrev >> 8)  & 0xFF;
    LOG("  HIDREV       : 0x%08X\n", hidrev);
    LOG("  Chip ID      : 0x%02X (%s)\n", chip_id,
        chip_major == 2 ? "T210B01 Mariko" : "T210 Erista");
    LOG("  Major.Minor  : %d.%02d\n", chip_major, chip_minor);
}

void probe_fuses(void)
{
    HEADER("[Fuses - identity]");
    LOG("  PRODUCTION   : 0x%08X\n", FUSE(FUSE_PRODUCTION_MODE));
    LOG("  SKU_INFO     : 0x%08X\n", FUSE(FUSE_SKU_INFO));
    LOG("  ODM4         : 0x%08X (DRAM ID %d)\n",
        fuse_read_odm(4), fuse_read_dramid(true));
    LOG("  HW state     : %s\n",
        fuse_read_hw_state() == FUSE_NX_HW_STATE_PROD ? "Prod" : "Dev");
    LOG("  HW type      : %d\n", fuse_read_hw_type());
    LOG("  Keygen rev   : %d\n", fuse_read_odm_keygen_rev());

    bool patched = fuse_check_patched_rcm();
    /* SoC-aware colouring. On Mariko (T210B01) every unit ships with the
     * BootROM RCM bug patched, it's the expected state, not an anomaly,
     * so we keep it neutral. On Erista (T210), patched=true means the
     * RCM exploit is gone (factory ipatch burnt later in the lifecycle)
     * which is interesting but not a hardware fault either; flag yellow.
     * Erista with patched=false is the exploitable state we expect when
     * the user is running this payload in the first place, green. */
    u32 chip_major = (APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF;
    bool is_mariko = (chip_major == 2);
    u32 patched_color = !patched   ? COL_OK
                      : is_mariko  ? COL_DEFAULT
                                   : COL_WARN;
    log_color(patched_color,
        "  Patched RCM  : %s\n",
        patched ? (is_mariko ? "yes (Mariko - expected)" : "yes (ipatch burnt)")
                : "no (exploitable)");

    /* Burnt anti-downgrade fuses (ODM7). 255 means over-burnt. */
    u32 odm7 = fuse_read_odm(7);
    u32 odm6 = fuse_read_odm(6);
    int burnt = 0;
    for (u32 b = odm7; b; b &= b - 1) burnt++;
    log_color(odm7 == 0xFFFFFFFF ? COL_ERR : COL_OK,
        "  Burnt fuses  : %d | %d (ODM7 | ODM6)\n", burnt,
        __builtin_popcount(odm6));

    HEADER("[Fuses - speedo / IDDQ]");
    LOG("  CPU SPEEDO 0 : %d\n",   FUSE(FUSE_CPU_SPEEDO_0_CALIB));
    LOG("  CPU SPEEDO 1 : %d\n",   FUSE(FUSE_CPU_SPEEDO_1_CALIB));
    LOG("  CPU SPEEDO 2 : %d\n",   FUSE(FUSE_CPU_SPEEDO_2_CALIB));
    LOG("  SOC SPEEDO 0 : %d\n",   FUSE(FUSE_SOC_SPEEDO_0_CALIB));
    LOG("  SOC SPEEDO 2 : %d\n",   FUSE(FUSE_SOC_SPEEDO_2_CALIB));
    LOG("  CPU IDDQ     : %d\n",   FUSE(FUSE_CPU_IDDQ_CALIB) * 4);
    LOG("  GPU IDDQ     : %d\n",   FUSE(FUSE_GPU_IDDQ_CALIB) * 5);
    LOG("  SOC IDDQ     : %d\n",   FUSE(FUSE_SOC_IDDQ_CALIB) * 4);
    LOG("  BROM rev     : 0x%02X\n", FUSE(FUSE_SOC_SPEEDO_1_CALIB));
    LOG("  FT rev       : %d.%02d (0x%X)\n",
        (FUSE(FUSE_OPT_FT_REV) >> 5) & 0x3F,
         FUSE(FUSE_OPT_FT_REV) & 0x1F, FUSE(FUSE_OPT_FT_REV));
    LOG("  CP rev       : %d.%02d (0x%X)\n",
        (FUSE(FUSE_OPT_CP_REV) >> 5) & 0x3F,
         FUSE(FUSE_OPT_CP_REV) & 0x1F, FUSE(FUSE_OPT_CP_REV));
    LOG("  USB ctrl     : %s\n",
        (FUSE(FUSE_RESERVED_SW) & 0x80) ? "XUSB" : "USB2");

    HEADER("[Fuses - lot / wafer]");
    static const char base36[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    u32 lot_enc = FUSE(FUSE_OPT_LOT_CODE_0);
    char lot_bcd[6] = {0};
    for (int i = 0; i < 5; i++) {
        u32 d = (lot_enc & 0x3F000000) >> 24;
        lot_bcd[i] = (d < 36) ? base36[d] : '?';
        lot_enc <<= 6;
    }
    u32 fab = FUSE(FUSE_OPT_FAB_CODE);
    int die_x = FUSE(FUSE_OPT_X_COORDINATE);
    int die_y = FUSE(FUSE_OPT_Y_COORDINATE);
    if (die_x & (1 << 8)) die_x -= 512;            /* 9-bit signed */
    LOG("  Vendor code  : %d\n", FUSE(FUSE_OPT_VENDOR_CODE));
    LOG("  FAB / LOT    : %c%s\n",
        (fab < 36) ? base36[fab] : '?', lot_bcd);
    LOG("  Wafer ID     : %d\n", FUSE(FUSE_OPT_WAFER_ID));
    LOG("  X / Y coord  : %d / %d\n", die_x, die_y);

    HEADER("[Fuses - Public Key SHA-256]");
    LOG("  PK0..3       : %08X%08X%08X%08X\n",
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY0)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY1)),
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY2)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY3)));
    LOG("  PK4..7       : %08X%08X%08X%08X\n",
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY4)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY5)),
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY6)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY7)));

    HEADER("[Fuses - SBK / DK]");
    /* If all five private-key fuses read 0xFFFFFFFF, the SBK/DK have been
     * locked out by the bootrom, that's normal post-pkg1. */
    bool locked =
        FUSE(FUSE_PRIVATE_KEY0) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY1) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY2) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY3) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY4) == 0xFFFFFFFF;
    if (locked) {
        /* SBK/DK reading all-0xFFFFFFFF is the bootrom locking them out
         * post-pkg1: the normal, expected state, not a fault or warning. */
        log_color(COL_DEFAULT, "  SBK / DK     : locked out (bootrom)\n");
    } else {
        LOG("  SBK          : %08X%08X%08X%08X\n",
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY0)),
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY1)),
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY2)),
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY3)));
        LOG("  DK           : %08X\n",
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY4)));
    }
}


void probe_kfuse(void)
{
    HEADER("[KFUSE (HDCP keys)]");
    /* Hekate's kfuse_wait_ready() is an UNBOUNDED `while (!STATE_DONE);`
     * loop. If the KFUSE block isn't clocked - or the silicon is in a
     * bad state - the payload hangs forever, the BPMP watchdog fires,
     * and modchip-injected setups (picofly) re-inject hwtest in a
     * reset-loop. We replicate kfuse_read()'s clock-enable dance and
     * bound the wait ourselves. */
    clock_enable_kfuse();

    bool done = false, crc_pass = false;
    u32 timeout_us = get_tmr_us() + 100000;   /* 100 ms cap */
    while ((s32)(timeout_us - get_tmr_us()) > 0) {
        u32 state = KFUSE(KFUSE_STATE);
        if (state & KFUSE_STATE_DONE) {
            done     = true;
            crc_pass = (state & KFUSE_STATE_CRCPASS) != 0;
            break;
        }
    }
    u32 state = KFUSE(KFUSE_STATE);
    clock_disable_kfuse();

    /* KFUSE_STATE bit map:
     *   bits  5:0  CURBLOCK  - current block being decoded (0..143)
     *   bits 13:8  ERRBLOCK  - block index where CRC failed (only meaningful
     *                          when CRCPASS=0 at completion)
     *   bit  16    DONE      - decode complete
     *   bit  17    CRCPASS   - CRC verified across all 144 blocks
     *   bit  24    RESTART   - controller in restart state
     *   bit  25    STOP      - controller stopped
     *   bit  31    SOFTRESET - block held in soft reset
     *
     * On a healthy unit you'll see ~0x00030030 = DONE | CRCPASS | CURBLOCK=48
     * (CURBLOCK lands wherever the FSM finished; pattern varies by silicon
     * rev). On a broken HDCP block, ERRBLOCK pinpoints the failing word
     * which is the only useful diagnostic short of replacing the SoC. */
    LOG("  KFUSE_STATE  : 0x%08X\n", state);
    LOG("  CURBLOCK     : %d\n", state & KFUSE_STATE_CURBLOCK_MASK);
    if (done && !crc_pass) {
        u32 errblk = (state & KFUSE_STATE_ERRBLOCK_MASK) >> KFUSE_STATE_ERRBLOCK_SHIFT;
        log_color(COL_ERR, "  ERRBLOCK     : %d (CRC failure at this block)\n", errblk);
    }
    if (state & KFUSE_STATE_RESTART)   log_color(COL_WARN, "  RESTART set  : controller in restart\n");
    if (state & KFUSE_STATE_STOP)      log_color(COL_WARN, "  STOP set     : controller stopped\n");
    if (state & KFUSE_STATE_SOFTRESET) log_color(COL_WARN, "  SOFTRESET set: block held in reset\n");

    if (!done) {
        log_color(COL_ERR, "  status       : TIMEOUT (block did not assert DONE)\n");
        dx_set("kfuse", DX_WARN, "timeout");
    } else {
        log_color(crc_pass ? COL_OK : COL_ERR,
            "  status       : %s\n", crc_pass ? "OK (CRC pass)" : "FAILED (CRC bad)");
        dx_set("kfuse", crc_pass ? DX_PASS : DX_FAIL,
            crc_pass ? "" : "CRC bad");
    }
}
