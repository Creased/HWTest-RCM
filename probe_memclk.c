/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: memclk probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* DRAM identity (LPDDR4 / LPDDR4X via EMC mode-register reads)             */
/*                                                                          */
/* The two LPDDR chips on the SoM identify themselves via JEDEC mode        */
/* registers. Hekate's gui_info reads:                                      */
/*   MR5 : manufacturer ID (1 = Samsung, 6 = Hynix, 255 = Micron, ...)      */
/*   MR6 : revision ID 1                                                    */
/*   MR7 : revision ID 2                                                    */
/*   MR8 : density encoding (bits 5:2)                                      */
/* sdram_read_mrx() drives the EMC bus to issue a mode-register read on     */
/* both chips and both ranks; we surface chip0/chip1 separately so a board  */
/* with one bad LPDDR shows a clear asymmetry. Erista is LPDDR4, Mariko is  */
/* LPDDR4X - the only practical difference here is naming.                  */
static const char *_dram_vendor_name(u8 v)
{
    switch (v) {
    case 1:   return "Samsung";
    case 5:   return "Nanya";
    case 6:   return "Hynix";
    case 8:   return "Winbond";
    case 19:  return "CXMT";
    case 255: return "Micron";
    default:  return NULL;  /* caller prints numeric */
    }
}

static const char *_dram_density_str(u8 mr8)
{
    switch ((mr8 & 0x3C) >> 2) {
    case 2: return "512MB";
    case 3: return "768MB";
    case 4: return "1GB";
    case 5: return "1.5GB";
    case 6: return "2GB";
    default: return NULL;
    }
}


void probe_dram(void)
{
    HEADER("[DRAM identity]");

    /* HIDREV major: 1 = Erista (LPDDR4), 2 = Mariko (LPDDR4X). The shift
     * returns the major nibble, not a bool, so we have to compare. */
    bool mariko = ((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) >= 2;
    LOG("  Type         : %s\n", mariko ? "LPDDR4X (Mariko)" : "LPDDR4 (Erista)");

    u8 dram_id = fuse_read_dramid(true);
    LOG("  Fuse DRAM ID : %d\n", dram_id);

    emc_mr_data_t v   = sdram_read_mrx(MR5_MAN_ID);
    emc_mr_data_t r0  = sdram_read_mrx(MR6_REV_ID1);
    emc_mr_data_t r1  = sdram_read_mrx(MR7_REV_ID2);
    emc_mr_data_t den = sdram_read_mrx(MR8_DENSITY);

    /* Per-chip readout. chip0 = first LPDDR, chip1 = second LPDDR. */
    for (int c = 0; c < 2; c++) {
        u8 vid = c ? v.chip1.rank0_ch0   : v.chip0.rank0_ch0;
        u8 rev0 = c ? r0.chip1.rank0_ch0 : r0.chip0.rank0_ch0;
        u8 rev1 = c ? r1.chip1.rank0_ch0 : r1.chip0.rank0_ch0;
        u8 d8  = c ? den.chip1.rank0_ch0 : den.chip0.rank0_ch0;

        const char *vname = _dram_vendor_name(vid);
        const char *dname = _dram_density_str(d8);
        u32 col = (vname && dname) ? COL_OK : COL_WARN;

        if (vname)
            log_color(col, "  Chip %d vend  : %s\n", c, vname);
        else
            log_color(col, "  Chip %d vend  : Unknown (id %d)\n", c, vid);
        log_color(col, "  Chip %d rev   : %X.%02X\n", c, rev0, rev1);
        if (dname)
            log_color(col, "  Chip %d dens  : %s\n", c, dname);
        else
            log_color(col, "  Chip %d dens  : Unknown (MR8 0x%02X)\n", c, d8);
    }

    /* Cross-rank symmetry check. If a stacked chip has both ranks, MR5 of
     * rank 0 and rank 1 should match; otherwise we're seeing only one
     * working die. Channels also should match - mismatch = bad LPDDR. */
    bool sym0 = (v.chip0.rank0_ch0 == v.chip0.rank0_ch1) &&
                (r0.chip0.rank0_ch0 == r0.chip0.rank0_ch1);
    bool sym1 = (v.chip1.rank0_ch0 == v.chip1.rank0_ch1) &&
                (r0.chip1.rank0_ch0 == r0.chip1.rank0_ch1);
    log_color((sym0 && sym1) ? COL_OK : COL_WARN,
        "  Channel sym  : chip0 %s, chip1 %s\n",
        sym0 ? "OK" : "MISMATCH",
        sym1 ? "OK" : "MISMATCH");
    dx_set("dram_sym", (sym0 && sym1) ? DX_PASS : DX_FAIL,
        (sym0 && sym1) ? "" : "channel asymmetry");

    /* MR4 (per-die refresh-rate / thermal class) is deliberately not read
     * here: in the RCM/BPMP context the LPDDR MR4 mode-register read does
     * not return a trustworthy live value, so a threshold test on it only
     * yields false signals. Catching bad DRAM (marginal cells that stall
     * HOS boot) requires a trained controller plus an actual R/W stress
     * march (a real memtester), which is out of scope for this passive
     * single-pass probe. */
}


void probe_clocks(void)
{
    HEADER("[Clocks]");
    /* Raw register dumps - hard to derive exact MHz without replaying the
     * full PLL math, so we just show the register state. The values are
     * meaningful when comparing two consoles or before/after a config
     * change. */
    LOG("  PLLP_BASE    : 0x%08X\n", CLOCK(0xA0));
    LOG("  PLLP_OUTA    : 0x%08X\n", CLOCK(0xA4));
    LOG("  PLLP_OUTB    : 0x%08X\n", CLOCK(0x68));
    LOG("  SCLK_BURST   : 0x%08X\n", CLOCK(0x28));
    LOG("  SUPER_SCLK   : 0x%08X\n", CLOCK(0x2C));
    LOG("  CLK_SYSTEM   : 0x%08X\n", CLOCK(0x30));
    LOG("  CLK_OUT_ENB_L: 0x%08X\n", CLOCK(0x10));
    LOG("  CLK_OUT_ENB_H: 0x%08X\n", CLOCK(0x14));
    LOG("  CLK_OUT_ENB_U: 0x%08X\n", CLOCK(0x18));
    LOG("  CLK_OUT_ENB_X: 0x%08X\n", CLOCK(0x280));

    /* PLL ENABLE / LOCK status. Per the Tegra X1 TRM each PLLx_BASE
     * register has bit 27 = LOCK (read-only, set when the loop is
     * locked) and bit 30 = ENABLE. A PLL that is enabled but not locked
     * means the loop is hunting - "console boots, then hangs after a
     * few seconds" symptoms map onto this. n_unlocked is the verdict
     * signal: an enabled PLL that hasn't locked is always wrong. */
    log_color(COL_INFO, "[Clocks - PLL lock status]\n");
    /* PLLM is special: Hekate enables it as a parallel DRAM-clock
     * candidate but EMC may still be running off PLLP/2 at the moment
     * we sample. PLLM hunting while it's not actually feeding EMC is
     * harmless. We mark PLLM as "transient" rather than failing the
     * verdict so a benign mid-retune state doesn't show as red. */
    struct pll_entry { u32 off; const char *name; bool may_hunt; };
    static const struct pll_entry plls[] = {
        {0x80,  "PLLC ",  false},
        {0x90,  "PLLM ",  true},  /* may transiently hunt - see comment above */
        {0xA0,  "PLLP ",  false},
        {0xB0,  "PLLA ",  false},
        {0xC0,  "PLLU ",  false},
        {0xD0,  "PLLD ",  false},
        {0xE0,  "PLLX ",  false},
        {0x4B8, "PLLD2",  false},
        {0x590, "PLLDP",  false},
        /* PLLRE (the PCIe refclk PLL, 0x4C4) is excluded: it is only
         * enabled by the wifi probe and reported there; including it
         * here produces spurious "ENABLED, NO-LOCK" noise on any
         * run that follows a PCIe bring-up. */
    };
    int n_unlocked = 0;
    for (size_t i = 0; i < sizeof(plls)/sizeof(plls[0]); i++) {
        u32 base   = CLOCK(plls[i].off);
        bool en    = (base & (1u << 30)) != 0;
        bool lock  = (base & (1u << 27)) != 0;
        bool fail  = en && !lock && !plls[i].may_hunt;
        bool transient = en && !lock && plls[i].may_hunt;
        if (fail) n_unlocked++;
        const char *suffix = fail      ? " (loop is hunting)"   :
                             transient ? " (transient - DRAM on PLLP)" : "";
        u32 col = fail      ? COL_ERR     :
                  transient ? COL_DEFAULT :
                  en        ? COL_OK      : COL_DEFAULT;
        log_color(col,
            "  %s        : %s%s\n", plls[i].name,
            en ? (lock ? "ENABLED, LOCKED" : "ENABLED, NO-LOCK") : "disabled",
            suffix);
    }
    dx_set("plls", n_unlocked ? DX_FAIL : DX_PASS,
        n_unlocked ? "%d enabled but not locked" : "", n_unlocked);

    /* Decoded frequencies via the PTO (Pulse Tag Observer) cell. The
     * BPMP couples the named clock to a 32.768 kHz reference for one
     * window and counts ticks; clock_get_dev_freq() returns the result
     * in kHz. We reach for the freqs a tech actually cares about: CPU,
     * SCLK (system / BPMP bus), EMC (DRAM), PLLP_OBS, SDMMC4 (eMMC).
     *
     * Note: hwtest runs on the BPMP, so CCLK_G reads as 0 unless the
     * payload separately fires the A57 cluster - we still print it for
     * completeness and call out the "(BPMP idle)" caveat in the line
     * itself. */
    /* Section header so the host viewer renders the decoded clock rates
     * as their own collapsible table beside the raw register dump. */
    log_color(COL_INFO, "[Clocks - decoded rates]\n");
    u32 osc_khz = clock_get_osc_freq();
    LOG("  OSC          : %d.%03d MHz\n", osc_khz / 1000, osc_khz % 1000);
    struct { clock_pto_id_t id; const char *name; } kClocks[] = {
        {CLK_PTO_PLLP_OBS, "PLLP_OBS    "},
        {CLK_PTO_SCLK,     "SCLK / BPMP "},
        {CLK_PTO_EMC,      "EMC (DRAM)  "},
        {CLK_PTO_SDMMC4,   "SDMMC4(eMMC)"},
        {CLK_PTO_SDMMC1,   "SDMMC1(SD)  "},
        {CLK_PTO_CCLK_G,   "CCLK_G(A57) "},
    };
    for (size_t i = 0; i < sizeof(kClocks)/sizeof(kClocks[0]); i++) {
        u32 khz = clock_get_dev_freq(kClocks[i].id);
        if (khz == 0) {
            LOG("  %s : (idle / not clocked)\n", kClocks[i].name);
        } else {
            LOG("  %s : %d.%03d MHz\n",
                kClocks[i].name, khz / 1000, khz % 1000);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* GPIO pin census - configuration of every pin we touch                    */
/*                                                                          */
/* For repair triage: surface the full state (PINMUX + GPIO CNF/OE/OUT/IN)  */
/* of every pin involved in display / Joy-Con rails / debug UART, in one    */
/* place. A bent pin or solder fault shows up here as an unexpected         */
/* function selector or GPIO mode that doesn't match the role we expect     */
/* the pin to play.                                                         */
/*                                                                          */
/* Pinmux is the same APB_MISC register read in other probes. GPIO state is */
/* the four-byte block per port within its bank (CNF=+0x00, OE=+0x10,       */
/* OUT=+0x20, IN=+0x30 within (port&3)<<2 inside (port>>2)<<8).             */
/*                                                                          */
/* For pads where the relevant function is an alt-function (UART, PWM,      */
/* etc.), GPIO CNF=0 (SPIO mode) is the *correct* state - the alt-function  */
/* controller drives the pin and the GPIO out/in bits become irrelevant.    */
/* We still print them for completeness; readers can ignore CNF/OE/OUT/IN   */
/* whenever the row reads "SPIO". */
const char *_pmx_pull_str(u32 pmx)
{
    u32 p = pmx & PINMUX_PULL_MASK;
    return p == PINMUX_PULL_UP   ? "pull-up"
         : p == PINMUX_PULL_DOWN ? "pull-dn"
                                 : "no-pull";
}

