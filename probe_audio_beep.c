/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Audio test: drive a melody through I2S1 into the ALC5639, one output
 * at a time.
 */
#include "hwtest.h"

/* ======================================================================== */
/* Tone playback: CPU (PIO) -> ADMAIF -> AXBAR -> I2S1 -> codec             */
/*                                                                          */
/* The CPU pushes samples straight into the ADMAIF TX FIFO (FIFO_CTRL bit31 */
/* = PIO mode), the crossbar routes ADMAIF1 to I2S1, and I2S1 clocks them   */
/* out to the codec. ADMA is only powered and reset here, never used to     */
/* move the tone: PIO is how the one known-working bare-metal Switch beep   */
/* (shinyquagsire23 hekate PR #990) makes sound, and it removes the DMA     */
/* buffer and its cache-coherency hazard from the path entirely.            */
/*                                                                          */
/* The register sequence below is HOS's own, recovered from the audio       */
/* sysmodule (audio.elf functions 0x1a610 power/global-enable and 0x1a840   */
/* path config). Three independent things identify I2S1 as the codec's      */
/* port: audio.elf maps exactly one I2S window and it is 0x702D1000; its    */
/* crossbar write routes ADMAIF1 to the I2S1 RX port; and the clock it      */
/* enables resolves to CLK_SOURCE_I2S1 / CLK_L_I2S1. The Switch device tree */
/* cannot answer this - it has no sound node and repurposes the DAP4 pins   */
/* as the touchscreen enable.                                              */
/*                                                                          */
/* ADMAIF1 TX_ENABLE is a register HOS never writes from the CPU, because   */
/* its Audio DSP owns it. A bare-metal payload has no DSP, so it writes it  */
/* itself.                                                                  */

#define APE_ADMAIF_BASE     0x702D0000u
#define APE_AXBAR_BASE      0x702D0800u
#define APE_I2S1_BASE       0x702D1000u
#define APE_ADMA_BASE       0x702E2000u

/* ADMAIF: global block at +0x700, per-channel RX at +0x00, TX at +0x300. */
#define ADMAIF_GLOBAL_ENABLE    (APE_ADMAIF_BASE + 0x700)
#define ADMAIF_GLOBAL_SOFT_RST  (APE_ADMAIF_BASE + 0x704)
#define ADMAIF_CH0_TX_ENABLE    (APE_ADMAIF_BASE + 0x300)
#define ADMAIF_CH0_TX_CIF_CTRL  (APE_ADMAIF_BASE + 0x320)
#define ADMAIF_CH0_TX_FIFO_CTRL (APE_ADMAIF_BASE + 0x328)
/* PIO port: with FIFO_CTRL bit31 set, the CPU pushes samples straight into
 * the ADMAIF TX FIFO here, bypassing ADMA entirely. This is how the only
 * known bare-metal Switch beep (shinyquagsire23 hekate PR #990) actually
 * makes sound - its DMA feed never worked, and CTCaer traced that to the
 * ADMAIF config, not clocks or the codec. */
#define ADMAIF_CH0_TX_FIFO_WRITE (APE_ADMAIF_BASE + 0x32C)
#define ADMAIF_TX_FIFO_CTRL_PIO  BIT(31)
/* Per-stage FIFO occupancy, one bit per channel. Together these say where
 * the samples stop: the DMA-side FIFO filling but the ACIF side staying
 * empty means ADMAIF is being fed but not draining into the crossbar,
 * which is a different fault from the DMA never delivering at all. */
#define ADMAIF_TX_ACIF_FIFO_FULL (APE_ADMAIF_BASE + 0x744)

/* Crossbar: one register per destination, source as a one-hot bitmask.
 * +0x40 is the I2S1 RX port; bit 0 selects ADMAIF1 as its source. */
#define AXBAR_I2S1_RX           (APE_AXBAR_BASE + 0x40)
#define AXBAR_SRC_ADMAIF1       BIT(0)

/* I2S1, offsets per TRM 23.3.3. RX is the playback direction: the port
 * receives from the crossbar and transmits on the wire. */
#define I2S1_ENABLE             (APE_I2S1_BASE + 0x80)
#define I2S1_CTRL_REG           (APE_I2S1_BASE + 0xA0)
#define I2S1_TIMING             (APE_I2S1_BASE + 0xA4)

/* ADMA: global at +0xC00, channels every 0x80 from the base. The block is
 * powered and reset so the hub comes up in a known state, but the tone
 * itself goes in by PIO - no channel ever runs. */
#define ADMA_GLOBAL_CMD         (APE_ADMA_BASE + 0xC00)
#define ADMA_GLOBAL_SOFT_RST    (APE_ADMA_BASE + 0xC04)
#define ADMA_CH0_CMD            (APE_ADMA_BASE + 0x00)

#define APE(a)  (*(vu32 *)(a))

/* Clock recipe, from the one bare-metal Switch beep that works (shinyquagsire
 * hekate PR #990, commit 1cf55e2), rebuilt with Erista's PLLA register layout
 * - the PR's own PLLA writes are the Mariko/T210b01 layout and cannot be
 * copied literally. What matters and differs from the obvious 48 kHz recipe:
 *   - AUD_MCLK comes from PLLP, not PLLA. PLLP is up since boot and rock
 *     solid; a PLLA-sourced MCLK would be the one clock this payload
 *     configures itself.
 *   - MCLK = 24 MHz = PLLP_OUT0 / 17 exactly.
 *   - PLLA only makes the 1.5 MHz bit clock (120 MHz / 80), as in the PR.
 *
 * The real sample rate follows from the frame, not the other way round:
 * BCLK 1.5 MHz / 64 bclk-per-frame (TIMING = 31) = 23437.5 Hz. Only the
 * sample-domain math (note pitch and durations) uses TONE_RATE_HZ, so it must
 * be the ACTUAL fs or the melody comes out at the wrong pitch and speed.
 * MCLK is then 1024 x fs, which the codec's 0x73 = 0x1114 accepts. */
#define TONE_RATE_HZ    23437u
#define PLLA_OUT0_HZ    120000000u
#define AUD_MCLK_HZ     24000000u          /* PLLP_OUT0 / 17                */
/* PLLA_OUT0 = 120 MHz: VCO 38.4/2 x 50 = 960 MHz (inside the 500-1000 MHz
 * range Linux records in clk-tegra210.c pll_a_params), then PLLA_OUT0
 * divides by 8. ENABLE | DIVP 0 | DIVN 50 | DIVM 2. DIVP is an exponent,
 * not a plain divider - a DIVP of 1 silently halves everything downstream. */
#define PLLA_BASE_VAL   (BIT(30) | (0u << 20) | (50u << 8) | 2u)
/* OUT0_RATIO 14 -> divide by (14/2)+1 = 8; plus CLKEN and RSTN released */
#define PLLA_OUT_VAL    ((14u << 8) | BIT(1) | BIT(0))
/* Bit 28 is I2S1_MASTER_CLKEN and its reset value is 1. It must survive:
 * clearing it stops the controller generating BCLK and LRCK, so the port
 * never clocks samples out - the symptom is a completely full ADMAIF ACIF
 * FIFO, no underrun, and every other register looking perfectly configured. */
#define I2S1_MASTER_CLKEN  BIT(28)
/* I2S1 BCLK = PLLA_OUT0 (120 MHz) / 80 = 1.5 MHz. The CAR divisor encodes
 * "src / ((N/2)+1)", so /80 needs N = 158 = 0x9E. */
#define I2S1_CLK_SRC    ((0u << 29) | I2S1_MASTER_CLKEN | 0x9Eu) /* /80     */
#define I2S1_BCLK_HZ    (PLLA_OUT0_HZ / 80u)                    /* 1.5 MHz  */
#define I2S1_BITCNT     0x0000001Fu       /* reference TIMING = 31        */
#define CODEC_ADDA_CLK  0x1114u              /* PR #990 ADDA_CLK1             */
/* Bit 10 is MASTER_ENABLE (tegra30_i2s.h TEGRA30_I2S_CTRL_MASTER_ENABLE);
 * the controller generates BCLK and LRCK only while it is set. */
#define I2S1_CTRL_MASTER   BIT(10)
/* Bits 28:24 are FSYNC_WIDTH; TRM Table 118 wants "number of bit clocks in
 * the left channel" for basic I2S, so it tracks CHANNEL_BIT_CNT above. */
#define I2S1_CTRL_VAL      0x10000403u    /* reference: FSYNC_WIDTH 16     */

/* DAP1 carries I2S1. Offsets from the kernel's Tegra210 pingroup table
 * (0x3124 etc, i.e. PINMUX_AUX + 0x124); function 0 is I2S1 on all four. */
#define PMX_DAP1_FS     0x124u
#define PMX_DAP1_DIN    0x128u
#define PMX_DAP1_DOUT   0x12Cu
#define PMX_DAP1_SCLK   0x130u
#define PMX_DAP2_FS     0x134u
#define PMX_DAP2_DIN    0x138u
#define PMX_DAP2_DOUT   0x13Cu
#define PMX_DAP2_SCLK   0x140u

/* The codec hangs off DAP1 / I2S1, which the working audio path confirms.
 * Neither reference states it outright: the device tree has no sound node,
 * and audio.elf delegates the AHUB to the embedded ADSP firmware rather
 * than programming I2S itself.
 *
 * These are the two ports whose pads this board leaves to audio. Of the
 * rest, I2S4B (dap4) is the touchscreen enable and I2S5B (pk0-pk3) is
 * claimed by another group in the device tree, so driving them would poke
 * unrelated hardware; I2S3 and I2S5A live on the dmic pins. Each port's
 * registers are one 0x100 block apart, and its crossbar mux one word apart.
 */
typedef struct {
    const char *name;
    u32 base;                       /* I2S block registers                 */
    u32 axbar_rx;                   /* crossbar source select for this port*/
    u32 clk_src;                    /* CLK_SOURCE_I2Sn - not contiguous    */
    u32 pmx_fs, pmx_din, pmx_dout, pmx_sclk;
} i2s_port_t;

static const i2s_port_t kI2sPorts[] = {
    { "I2S1/DAP1", APE_I2S1_BASE,          AXBAR_I2S1_RX,
      CLK_RST_CONTROLLER_CLK_SOURCE_I2S1,
      PMX_DAP1_FS, PMX_DAP1_DIN, PMX_DAP1_DOUT, PMX_DAP1_SCLK },
    { "I2S2/DAP2", APE_I2S1_BASE + 0x100u, AXBAR_I2S1_RX + 4u,
      CLK_RST_CONTROLLER_CLK_SOURCE_I2S2,
      PMX_DAP2_FS, PMX_DAP2_DIN, PMX_DAP2_DOUT, PMX_DAP2_SCLK },
};

/* Per-port register offsets, so the same sequence can drive either block.
 * t210.h defines I2S_CG/I2S_CTRL as plain offsets; override them here with
 * the base-relative accessors this file uses. */
#undef I2S_CG
#undef I2S_CTRL
#define I2S_RX_ENABLE(b)     ((b) + 0x00)
#define I2S_RX_CIF_CTRL(b)   ((b) + 0x20)
#define I2S_RX_CTRL(b)       ((b) + 0x24)
#define I2S_RX_SLOT_CTRL(b)  ((b) + 0x28)
#define I2S_ENABLE(b)        ((b) + 0x80)
#define I2S_SOFT_RESET(b)    ((b) + 0x84)
#define I2S_CG(b)            ((b) + 0x88)
#define I2S_CTRL(b)          ((b) + 0xA0)
#define I2S_TIMING(b)        ((b) + 0xA4)
#define I2S_SLOT_CTRL(b)     ((b) + 0xA8)

#define CODEC_ADDR      0x1Cu

static void _codec_w(u32 reg, u16 val)
{
    u8 buf[2] = { (u8)(val >> 8), (u8)(val & 0xFF) };
    i2c_send_buf_small(I2C_1, CODEC_ADDR, reg, buf, sizeof(buf));
}

static u16 _codec_r(u32 reg)
{
    u8 rb[2] = { 0, 0 };
    if (i2c_recv_buf_small(rb, 2, I2C_1, CODEC_ADDR, reg) != 0)
        return 0;
    return (u16)((rb[0] << 8) | rb[1]);
}

/* HOS never writes these registers wholesale - every runtime change is a
 * read, a mask and a write back, which is what keeps the bits the init
 * table established from being lost. */
static void _codec_rmw(u32 reg, u16 and_mask, u16 or_mask)
{
    _codec_w(reg, (u16)((_codec_r(reg) & and_mask) | or_mask));
}

/* Realtek's private register file, reached through an index/data pair. */
static void _codec_pr_w(u16 idx, u16 val)
{
    _codec_w(0x6A, idx);
    _codec_w(0x6C, val);
}

/* HOS polls these soft-reset bits rather than asserting them, and forces
 * the register to 0 if it never clears. Same shape here. */
static bool _wait_soft_reset(u32 addr)
{
    for (u32 i = 0; i < 5; i++) {
        if (!(APE(addr) & BIT(0)))
            return true;
        msleep(1);
    }
    APE(addr) = 0;
    return !(APE(addr) & BIT(0));
}

static void _audio_clocks_setup(void)
{
    /* PLLA up first - both MCLK and the I2S bit clock hang off it. */
    CLOCK(CLK_RST_CONTROLLER_PLLA_BASE) &= ~BIT(30);      /* disable while
                                                           * reprogramming */
    CLOCK(CLK_RST_CONTROLLER_PLLA_MISC) &= ~PLLA_BASE_IDDQ;
    usleep(5);
    CLOCK(CLK_RST_CONTROLLER_PLLA_BASE) = PLLA_BASE_VAL;
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_PLLA_OUT) = PLLA_OUT_VAL;
    /* Wait for lock rather than assuming: an unlocked PLL still reads back
     * the dividers, so the configuration alone proves nothing. */
    u32 lock = 0;
    for (u32 i = 0; i < 100; i++) {
        if (CLOCK(CLK_RST_CONTROLLER_PLLA_BASE) & BIT(27)) { lock = 1; break; }
        usleep(100);
    }
    LOG("  PLLA         : base %08X out %08X %s\n",
        CLOCK(CLK_RST_CONTROLLER_PLLA_BASE),
        CLOCK(CLK_RST_CONTROLLER_PLLA_OUT),
        lock ? "LOCKED -> 120 MHz OUT0" : "NO LOCK");

    /* Re-point AUD_MCLK at PLLP / 17 = 24 MHz = 512 x 46875, the reference
     * recipe. PLLP is always-on and rock solid; the codec never seemed to
     * receive the PLLA-sourced MCLK. */
    /* Gate EXTPERIPH1 across the parent switch. The audio-clock probe left
     * it enabled and running off PLLP; changing the source mux of a live
     * peripheral divider can leave the output dead or glitched while every
     * register still reads back exactly as programmed, and nothing
     * downstream of this pin is observable from the SoC. */
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_V_SET) = BIT(CLK_V_EXTPERIPH1);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_V_CLR) = BIT(CLK_V_EXTPERIPH1);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_EXTPERIPH1) = (2u << 29) | 0x20u; /* PLLP/17 = 24 MHz */
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_V_SET) = BIT(CLK_V_EXTPERIPH1);
    usleep(2);
    /* Release the module reset, which is asserted at power-on and which
     * nothing else in this payload or in hekate ever clears. Without it the
     * pad carries no clock however the mux and divider are programmed. */
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_V_CLR) = BIT(CLK_V_EXTPERIPH1);
    (void)CLOCK(CLK_RST_CONTROLLER_RST_DEV_V_SET);
    LOG("  AUD_MCLK     : src %08X pad %08X -> %d Hz (1024 x fs)\n",
        CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_EXTPERIPH1),
        PINMUX_AUX(PINMUX_AUX_AUD_MCLK), AUD_MCLK_HZ);

    /* Global blocks first: ADMA, then ADMAIF. */
    APE(ADMA_GLOBAL_SOFT_RST) = 1;
    _wait_soft_reset(ADMA_GLOBAL_SOFT_RST);
    APE(ADMA_GLOBAL_CMD) = 1;
    APE(ADMAIF_GLOBAL_SOFT_RST) = 1;
    _wait_soft_reset(ADMAIF_GLOBAL_SOFT_RST);
    APE(ADMAIF_GLOBAL_ENABLE) = 1;

    /* ADMAIF playback channel, shared by whichever port is driven. */
    /* FIFO_THRESHOLD = 3, not 0. The working bare-metal reference builds this
     * as ahub_get_cif(2ch, 16-bit, fifo_threshold=4) | UNPACK16, i.e.
     * (4-1)<<24 in the threshold field -> 0x43113300. Ours had 0x40113300,
     * threshold 0. CTCaer's note on this exact codebase is that "the secret
     * sauce is in the admaif config" - and a zero FIFO threshold lets the
     * ADMAIF forward to the crossbar before it holds real data, so the port
     * clocks out stale/empty words: the FIFO drains (PIO sees no stalls)
     * yet no waveform reaches the codec. This is the one ADMAIF value that
     * differs from the reference. */
    APE(ADMAIF_CH0_TX_CIF_CTRL) = 0x43113300;
    /* Per-channel FIFO allocation. The driver writes this on every stream -
     * DMA_FIFO_SIZE 3, START_ADDR 0 for ADMAIF1 - rather than trusting the
     * reset value, because the ten TX channels carve one shared FIFO and a
     * wrong size or an overlapping start address moves data internally while
     * emitting nothing. The payload had been relying on a value it never set
     * and that hekate, HOS or the ADSP could have left mis-sized. */
    APE(ADMAIF_CH0_TX_FIFO_CTRL) = 0x02000300;
}

/* Bring one serial port up and point the crossbar at it. Ports not named
 * here keep their crossbar mux at reset, so only one drives the pins. */
static void _i2s_port_setup(const i2s_port_t *p)
{
    /* Pads to their I2S function. FS/SCLK/DOUT are driven by the controller
     * (it is the master), DIN is an input. */
    /* Keep DRV_TYPE at 0b11. These are AUDIO_HV-rail pads, and the TRM is
     * explicit that rails without COMP pads "should stay in 0x03 only" -
     * their reset value is 0x00006074, with bits 14:13 set. Writing the
     * whole word as PULL_NONE forced them to DRIVE_1X and also cleared the
     * Schmitt trigger, which is exactly the kind of change that leaves every
     * register reading back as programmed while the pins stop being able to
     * drive a load. hekate's own pinmux_config_i2s() uses DRIVE_4X for all
     * four, with the data input tristated and receiving. */
    PINMUX_AUX(p->pmx_fs)   = PINMUX_DRIVE_4X | PINMUX_PULL_DOWN;
    PINMUX_AUX(p->pmx_sclk) = PINMUX_DRIVE_4X | PINMUX_PULL_DOWN;
    PINMUX_AUX(p->pmx_dout) = PINMUX_DRIVE_4X | PINMUX_PULL_DOWN;
    PINMUX_AUX(p->pmx_din)  = PINMUX_DRIVE_4X | PINMUX_INPUT_ENABLE |
                              PINMUX_TRISTATE | PINMUX_PULL_DOWN;

    CLOCK(p->clk_src) = I2S1_CLK_SRC;
    usleep(2);

    /* MASTER_EN goes in before the reset wait, as HOS does - but carry the
     * real BIT_SIZE with it. Writing MASTER alone leaves bits 2:0 at 0,
     * which is the reserved BIT_SIZE encoding, on a port that is about to be
     * enabled; the correct value follows two lines later, so the window is
     * short, but there is no reason to pass through an illegal one. */
    APE(I2S_CG(p->base)) = 0;
    APE(I2S_CTRL(p->base)) = I2S1_CTRL_MASTER | (I2S1_CTRL_VAL & 0x7u);
    _wait_soft_reset(I2S_SOFT_RESET(p->base));

    /* Reset the RX datapath itself, not just poll the block reset.
     *
     * The working driver soft-resets the receive path before every stream
     * (tegra210_i2s_sw_reset: assert SOFT_RESET_EN in the RX register at
     * +0x04, poll it back to 0). The payload only ever polled the block
     * reset at +0x84 and never asserted anything - and the capture probe
     * that now runs before the beeps drives the TX side and leaves the RX
     * FIFOs and state machine in an arbitrary state. Without this, the port
     * starts from wherever the last probe left it. */
    APE(p->base + 0x04u) = 1;
    _wait_soft_reset(p->base + 0x04u);

    APE(I2S_ENABLE(p->base)) = 1;
    APE(I2S_CTRL(p->base)) = I2S1_CTRL_VAL; /* master, 64-bclk, 16-bit     */
    APE(I2S_TIMING(p->base)) = I2S1_BITCNT; /* 63 -> 128 bclk/frame, as HOS */
    APE(I2S_RX_CTRL(p->base)) = 0x00000100; /* DATA_OFFSET 1 = standard I2S */
    APE(I2S_SLOT_CTRL(p->base)) = 0;   /* TOTAL_SLOTS 0, per TRM Table 118 */

    /* SLOT_ENABLES = 0, matching the working reference and the L4T driver,
     * which both leave I2S_AXBAR_RX_SLOT_CTRL at 0 in basic (LRCK) mode and
     * only touch it for TDM. An earlier build set it to 1 on the strength of
     * TRM Table 118, but two known-good implementations disagree, so follow
     * them.
     *
     * (Historic note kept as a warning: the register blurb calls the field
     * "used in TDM mode", but Table 118 lists SLOT_ENABLES = 0x01 for basic
     * I2S. The HIGHZ_CTRL text about tristating an invalid slot suggested the
     * serializer consults slot validity in all modes - but the reference
     * plays audio with this at 0, so that reading was wrong.)
     *
     * Left at 0 no slot is ever valid, and because HIGHZ_CTRL here is
     * NOHIGHZ the pad is driven anyway - at a constant zero. Every
     * measurement upstream still looks perfect: BCLK and LRCK run, the RX
     * CIF drains, ADMA completes buffers at exactly the programmed rate and
     * no status bit complains. The only missing thing is the audio. */
    APE(I2S_RX_SLOT_CTRL(p->base)) = 0x00000000;
    APE(I2S_RX_CIF_CTRL(p->base)) = 0x03113300; /* 2ch 16-bit, thresh 3    */

    /* The crossbar mux for one destination spans THREE registers, one per
     * partition, 0x200 apart, and the selected source is a single bit across
     * all three (tegra210_ahub.c writes reg + 0x200*i for i in 0..2, zeroing
     * the parts it is not selecting from). Writing only partition 0 leaves
     * any stale bit in partition 1 or 2 summing a second source into this
     * port - and nothing in this payload ever resets the crossbar, so a bit
     * left by the bootloader or by an earlier probe would be invisible here
     * and would corrupt the stream. The capture path already does this. */
    APE(p->axbar_rx + 0x200u) = 0;
    APE(p->axbar_rx + 0x400u) = 0;
    APE(p->axbar_rx) = AXBAR_SRC_ADMAIF1;

    /* Enable the I2S's AXBAR RX path - the port's receive side, which is
     * what actually accepts samples from the crossbar. This is a separate
     * register from the block enable at 0x80, and the TRM's I2S programming
     * guidelines call it out as its own step: "Enable the I2S TX/RX: set
     * I2S_AXBAR_TX/RX_ENABLE to 1". HOS's CPU-side code never writes it
     * because its audio DSP owns per-channel run control, so it was missing
     * from the sequence recovered from audio.elf. */
    APE(I2S_RX_ENABLE(p->base)) = 1;
}

/* Codec: reset, Realtek's errata patches, bias, clocking, format, and the
 * DAC -> headphone-mixer path. MCLK direct, no PLL (see TONE_RATE_HZ). */
static void _codec_playback_setup(bool use_bclk_pll)
{
    /* HOS's bring-up order, from audio.elf:0x1B3F0-0x1B5E0. The analogue
     * bias comes up BEFORE the register table, not after it: the table's
     * last five entries are the power registers (0x61, 0x62, 0x64, 0x65,
     * 0x66), so they are meant to land on an already-biased part. */
    _codec_w(0x00, 0x0000);            /* software reset                   */
    msleep(2);
    _codec_pr_w(0x77, 0x9F00);
    _codec_w(0x63, 0xA810);
    msleep(50);
    _codec_w(0x63, 0xA810 | 0x4008);   /* + PWR_FV1 | PWR_FV2              */
    msleep(10);

    /* HOS's own initialisation table for this board, lifted verbatim from
     * the audio sysmodule (audio.elf:0x145A50, walked by the helper at
     * 0x1B830). Each entry is {kind, reg, value}: kind 1 is an ordinary
     * register, kind 2 a Realtek private register.
     *
     * The length is 53, taken from the `MOV W2, #0x35` at audio.elf:0x1B574
     * that feeds the walker. It matters: the shutdown table butts directly
     * against the end of this one at audio.elf:0x145B24, so overrunning
     * lands on 0x01=0xC8C8, 0x02=0xC8C8, 0x61=0x0000 - muting both outputs
     * and powering off I2S1, both DACs and the class-D amplifier, while
     * leaving the analogue rails up. That fails exactly like a wiring fault:
     * mute transitions still click, no audio ever arrives.
     *
     * This replaces the sequence assembled from Linux's rt5640 driver. The
     * driver is generic across a whole family of boards; this is what the
     * console's own firmware programs into the part it actually has, and it
     * touches two dozen registers the driver-derived version never wrote. */
    static const struct { u8 pr; u8 reg; u16 val; } kHosInit[] = {
        { 1, 0x1B, 0x0200 }, { 0, 0xFA, 0x3E01 }, { 0, 0x73, 0x8814 },
        { 1, 0x1D, 0x0347 }, { 1, 0x3D, 0x3600 }, { 1, 0x12, 0x0AA8 },
        { 1, 0x14, 0x8AAA }, { 1, 0x20, 0x6110 }, { 1, 0x23, 0x0804 },
        { 0, 0x70, 0x8000 }, { 0, 0x71, 0x8000 }, { 0, 0xBD, 0x4000 },
        { 0, 0xBE, 0x8000 }, { 0, 0xC0, 0x8400 }, { 0, 0xC2, 0x0004 },
        { 0, 0xBB, 0x0000 }, { 0, 0xFB, 0x0000 }, { 0, 0x8D, 0xA800 },
        { 0, 0x8C, 0x0328 }, { 0, 0x46, 0x0036 }, { 0, 0x47, 0x0036 },
        { 0, 0x01, 0x8484 }, { 0, 0x4A, 0x0004 }, { 0, 0x48, 0xE800 },
        { 0, 0x49, 0x2800 }, { 0, 0x19, 0x0000 }, { 0, 0x4E, 0x0000 },
        { 0, 0x51, 0x0000 }, { 0, 0x4F, 0x01FE }, { 0, 0x52, 0x01FE },
        { 0, 0x02, 0x1111 }, { 0, 0x45, 0xC000 }, { 0, 0x2A, 0x1414 },
        { 1, 0x90, 0x2000 }, { 1, 0x91, 0x1000 }, { 0, 0x8F, 0x1100 },
        { 0, 0x90, 0x0646 }, { 1, 0x37, 0xFC00 }, { 0, 0x91, 0x0C00 },
        { 0, 0x0D, 0x5000 }, { 0, 0x0E, 0x0000 }, { 0, 0x3C, 0x007F },
        { 0, 0x3E, 0x007F }, { 0, 0x27, 0x3060 }, { 0, 0x93, 0x3800 },
        { 0, 0x2F, 0x2000 }, { 0, 0x1C, 0x1F80 }, { 0, 0xD3, 0x2A20 },
        { 0, 0x61, 0x8001 }, { 0, 0x62, 0x0000 }, { 0, 0x64, 0x0000 },
        { 0, 0x65, 0x3000 }, { 0, 0x66, 0xC000 },
    };
    for (u32 i = 0; i < sizeof(kHosInit) / sizeof(kHosInit[0]); i++) {
        if (kHosInit[i].pr)
            _codec_pr_w(kHosInit[i].reg, kHosInit[i].val);
        else
            _codec_w(kHosInit[i].reg, kHosInit[i].val);
    }

    /* Drop MCLK_DET, GEN_CTRL1 (0xFA) bit 11: an interlock that watches the
     * MCLK pin and mutes the DAC when it does not see a clock there. HOS
     * leaves it set (0xFA = 0x3E01) because its own MCLK always arrives.
     * Clearing it removes a failure mode that no register readback can tell
     * apart from a dead converter. */
    _codec_rmw(0xFA, (u16)~0x0800u, 0x0000);
    LOG("  MCLK_DET     : cleared, 0xFA now %04X (was 3E01)\n",
        _codec_r(0xFA));

    /* Keep 0x73 at the table's 64FS value; CODEC_ADDA_CLK equals it. The
     * write is left here so the frame is set in one obvious place if it ever
     * changes again. */
    _codec_w(0x73, CODEC_ADDA_CLK);
    LOG("  ADDA_CLK     : 0x73 now %04X (64FS)\n", _codec_r(0x73));

    /* SYSCLK source. Two options, and which one works is itself the test.
     *
     * MCLK-direct is the simpler path, but nothing measured proves AUD_MCLK
     * actually arrives at the codec pin - the clock is set up on the Tegra
     * side and the pad is muxed, and that is the whole of the evidence.
     * BCLK1 by contrast is proven present: the drain-rate measurement shows
     * the port clocking samples out at exactly the programmed rate, which
     * cannot happen without a bit clock on the wire.
     *
     * So the codec's PLL can be run off BCLK1 instead and sidestep MCLK
     * entirely. With BCLK = 128 x fs = 6.000 MHz, N+2 = 16 and M+2 = 2 give
     * FOUT = 6 x 16 / (2 x 4) = 12.000 MHz = 256 x 46875, the exact ratio
     * the codec requires. */
    if (use_bclk_pll) {
        /* An alternative to MCLK: run the codec's own PLL from BCLK1, a
         * clock it demonstrably receives. Unused by the shipped path, which
         * takes the MCLK-direct leg below.
         *
         * Driver-exact constants, from mainline rl6231_pll_calc(3072000,
         * 12288000): FOUT = FIN x (N+2) / ((M+2) x (K+2)) with N+2 = 24,
         * K+2 = 6, M bypassed -> FVCO = 3.072 x 24 = 73.728 MHz, FOUT =
         * 73.728 / 6 = 12.288 MHz = 256 x fs. FVCO sits inside the codec's
         * PLL VCO range. 0x73 keeps the value set above: the OSR and
         * pre-divider are
         * relative to fs and do not change with the SYSCLK source. */
        _codec_rmw(0x64, 0xFFFF, 0x0200);  /* Pow_pll - RMW, keep IN1/IN2   */
        _codec_w(0x81, 0x0B04);        /* N_code 22 (bits 15:7), K_code 4  */
        _codec_w(0x82, 0x0800);        /* M bypass (bit 11), M_code 0      */
        _codec_w(0x80, 0x1000);        /* PLL1 <- BCLK1, SYSCLK still MCLK */
        msleep(10);                    /* no lock bit; fixed settle        */
        _codec_w(0x80, 0x5000);        /* SYSCLK <- PLL1 | PLL1 <- BCLK1   */
        msleep(2);
        LOG("  BCLK1-PLL    : 0x80 %04X 0x81 %04X 0x82 %04X 0x64 %04X\n",
            _codec_r(0x80), _codec_r(0x81), _codec_r(0x82), _codec_r(0x64));
    } else {
        /* SYSCLK straight from MCLK - the codec's reset default, and what
         * HOS relies on: its init table never writes 0x80.
         *
         * 0x73 is deliberately NOT rewritten here. HOS sets it to 0x8814 in
         * the table above, and that register carries the I2S pre-divider and
         * the DAC oversampling ratio, i.e. what the DAC's digital section
         * actually runs on. Overriding it with a hand-computed 0x0114 was
         * guesswork layered on top of the console's own known-good value. */
        _codec_w(0x80, 0x0000);
    }
    /* Everything the digital path needs is already in the table above:
     * 0x70/0x71 (serial format), 0x73 (clocking), 0x2A (DAC1 into the stereo
     * mixer), 0x46/0x47 (DAC1 into SPKMIX), 0x48/0x49 (SPKVOL into SPO),
     * 0x45, 0x4A. None of HOS's runtime functions rewrite any of them, so
     * neither does this. What follows is only what HOS does on top.
     *
     * The speaker enable proper, audio.elf sub_1BA20 (0x1BA40-0x1BB08): the
     * table deliberately parks the part with 0x61 = 0x8001, i.e. I2S1 and
     * the class-D amplifier powered but both DACs off, and 0x01 = 0x8484,
     * i.e. SPKOUT muted at -6 dB. These two read-modify-writes are the whole
     * of what turns it on. */
    _codec_rmw(0x61, 0xFFFF, 0x1800);  /* + PWR_DAC_L1 | PWR_DAC_R1        */
    msleep(10);                        /* 0x989680 ns at audio.elf:0x1BAA8 */
    _codec_rmw(0x01, 0x7F7F, 0x0000);  /* unmute SPKOUT L and R            */

    /* The speaker route, audio.elf sub_1BB50 entered at 0x1C594. It walks
     * the depop machine, then explicitly powers the headphone side DOWN
     * (0x65, 0x66 and 0x63 all lose their HP bits) before bringing the
     * speaker mixers and the class-D amp up. The 21 private-register EQ and
     * DRC coefficients it also writes come from its context structure, which
     * is board calibration this payload does not have, and they shape the
     * response rather than gate it - so they are left out. */
    _codec_w(0x90, 0x0646);
    _codec_pr_w(0x37, 0xFC00);
    _codec_rmw(0x8E, 0xFFFF, 0x0004);
    _codec_rmw(0x8E, 0xFFFF, 0x0020);
    _codec_rmw(0x8E, (u16)~0x0020, 0x0300);
    _codec_rmw(0x02, 0xFFFF, 0x8080);  /* mute HPOUT - speaker case        */
    msleep(30);                        /* 0x1C9C380 ns at audio.elf:0x1C7F8*/
    _codec_rmw(0x8E, 0x7CFF, 0x0000);
    _codec_w(0x91, 0x0C00);
    _codec_rmw(0x65, 0x3FFF, 0x0000);  /* drop OUT_MIXL/R power            */
    _codec_rmw(0x66, 0xF3FF, 0x0000);  /* drop HPOVOL L/R power            */
    _codec_rmw(0x63, 0xFF1F, 0x0000);  /* drop the analogue HP bits 7:5    */
    _codec_rmw(0x8E, 0xFF66, 0x0080);
    _codec_w(0x8F, 0x1100);
    _codec_rmw(0x61, 0xFFFF, 0x0001);  /* class-D amplifier power          */
    _codec_rmw(0x65, 0xFFFF, 0x3000);  /* SPKMIXL | SPKMIXR power          */
    _codec_rmw(0x66, 0xFFFF, 0xC000);  /* SPKVOLL | SPKVOLR power          */
    _codec_rmw(0x01, 0x7F7F, 0x0000);  /* unmute SPKOUT again, as HOS does */

    /* DAC1 digital volume. The table leaves 0x19 at 0x0000, which on this
     * part is the MINIMUM, not the maximum - the real level only ever
     * arrives at runtime through loc_1D1D0. Replaying the table and stopping
     * gives a powered, unmuted, silent DAC. 0xAFAF is 0 dB. */
    _codec_w(0x19, 0xAFAF);

    /* HOS powers the headphone side down for speaker playback, above,
     * because on the console a plugged jack is what selects it. Bring it
     * back up here, because this probe plays a phrase out the jack as well
     * as the speakers.
     *
     * Restoring the power bits sub_1BB50 cleared is not enough on its own -
     * the output is capless, so its negative rail comes from an on-chip
     * charge pump, and with the pump down the amplifier cannot move the pin
     * however many power bits are set. The pump and depop machine have to be
     * walked in this order. */
    _codec_rmw(0x63, 0xFFFF, 0x00E0);  /* analogue HP bits 7:5             */
    _codec_rmw(0x65, 0xFFFF, 0xC000);  /* OUT_MIXL | OUT_MIXR power        */
    _codec_rmw(0x66, 0xFFFF, 0x0C00);  /* HPOVOL L | HPOVOL R power        */

    /* Route DAC1 straight into the HPO mixer for a strong headphone output.
     * HPO_MIXER (0x45) bit14 is M_DAC1_HM, the DAC1-to-HPO mute; the init
     * table leaves it 0xC000 (bit14 set = DAC1 muted), so the only path to
     * the jack was the long DAC -> OUT MIX -> HPOVOL -> HPO one, which comes
     * out ~35 dB down. Clearing bit14 gives DAC1 -> HPO MIX directly, at the
     * same level the speaker gets. Also unmute the DAC into the OUT mixers
     * (0x4D/0x50 bit0) so the HPOVOL leg carries signal too. */
    _codec_rmw(0x45, (u16)~0x4000u, 0x0000);   /* unmute DAC1 -> HPO MIX    */
    _codec_rmw(0x4D, (u16)~0x0001u, 0x0000);   /* DAC L1 -> OUT MIXL        */
    _codec_rmw(0x50, (u16)~0x0001u, 0x0000);   /* DAC R1 -> OUT MIXR        */

    _codec_pr_w(0x0024, 0x0200);       /* charge-pump internal reg 1       */
    _codec_w(0x8F, 0x3100);            /* depop mode 2 (manual)            */
    _codec_w(0x8E, 0x0009);            /* Pow_pump_hp | Pow_capless        */
    _codec_w(0x63, 0xA8D0);            /* fast VREF while the amp comes up */
    _codec_w(0x63, 0xA8F0);            /* En_amp_hp                        */
    msleep(10);
    _codec_w(0x63, 0xE8F8);            /* back to slow VREF                */
    _codec_w(0x8F, 0x1140);            /* depop mode 1 (auto)              */
    _codec_w(0x91, 0x0E00);            /* charge pump high-voltage mode    */
    _codec_w(0x90, 0x0737);            /* pump frequencies                 */
    _codec_pr_w(0x0037, 0x1C00);
    /* Pow_pump_hp (bit3), En_out_hp (bit4) and the soft generator: a capless
     * output cannot drive its pin with the charge pump down, however many
     * other bits are set. */
    _codec_w(0x8E, 0x001D);
    _codec_pr_w(0x0024, 0x0400);

    /* Clear HP_SG_EN (0x8E bit2), the soft generator. L4T's set_bias_level
     * clears it together with the HP L/R soft-mutes whenever the headphone
     * is actually unmuted. On a capless output the soft generator holds the
     * pin at its reference and lets only transients through.
     * 0x001D & ~0x0004 = 0x0019. */
    _codec_rmw(0x8E, (u16)~0x0004u, 0x0000);
}


static void _codec_mute_all(void)
{
    /* Mute the output ports only. Writing 0xC8C8 wholesale would overwrite
     * the volume fields HOS parked (0x8484 on 0x01, 0x1111 on 0x02) and set
     * the volume-channel mutes, so a later "& 0x7F7F" unmute would leave
     * bit14/bit6 set and the channel silent. */
    _codec_rmw(0x02, 0xFFFF, 0x8080);
    _codec_rmw(0x01, 0xFFFF, 0x8080);
}

/* A short tone sequence per speaker, so a dead or weak channel stands out.
 * {frequency Hz, duration ms}; frequency 0 is a rest. */
typedef struct { u16 freq; u16 ms; } note_t;

static const note_t kMelodyL[] = {
    { 587, 150}, { 659, 150}, { 784, 150}, { 587, 150}, { 988, 300},
    {   0, 150}, { 988, 300}, {   0, 150}, { 880, 600}, {   0, 300},
};
static const note_t kMelodyR[] = {
    { 587, 150}, { 659, 150}, { 784, 150}, { 587, 150}, { 880, 300},
    {   0, 150}, { 880, 300}, {   0, 150}, { 784, 300}, { 784, 150},
    { 740, 150}, { 659, 300},
};
/* Played out the headphone jack only, so a dead jack stands out too. */
static const note_t kMelodyJack[] = {
    { 587, 143}, { 659, 143}, { 784, 143}, { 659, 143}, { 784, 571},
    { 880, 286}, { 740, 286}, { 740, 143}, { 659, 143}, { 587, 286},
    { 587, 143}, {   0, 143}, { 587, 143}, {   0, 143}, { 880, 286},
    { 880, 143}, {   0, 143}, { 784, 571},
};

/* Bring the jack up exactly as the reference does (hekate PR#990, commit
 * 1cf55e2, rt5639.c headphone branch). The signal reaches the jack through
 * the HPVOL leg - DAC1 -> STO DAC mixer -> OUT mixer -> HPOVOL -> HPO mixer -
 * not the DAC1-direct-to-HPO tap (0x45 bit14 clear), which this function
 * deliberately re-mutes. First restore the routing registers the
 * reference sets in its common init but the speaker phase clobbered, then
 * replay the reference's headphone branch verbatim; pr = a private
 * index/data write. */
static void _codec_hp_on(void)
{
    _codec_w(0x2A, 0x1414);            /* STO_DAC_MIXER: DAC1 -> stereo mixer */
    _codec_w(0x4F, 0x01FE);            /* OUT_L3_MIXER: DAC MIXL -> OUT MIXL  */
    _codec_w(0x52, 0x01FE);            /* OUT_R3_MIXER: DAC MIXR -> OUT MIXR  */
    _codec_w(0x45, 0xC000);            /* HPO_MIXER: HPVOL -> HPO, DAC1 muted */

    _codec_rmw(0x01, 0xFFFF, 0x8080);  /* mute SPOLP/SPORP                    */
    _codec_rmw(0x61, 0xFFFE, 0x0000);  /* PWR_DIG1: drop bit0                 */
    _codec_rmw(0x65, 0xCFFF, 0x0000);  /* PWR_MIXER: drop SPKMIX L/R          */
    _codec_rmw(0x66, 0x3FFF, 0x0000);  /* PWR_VOL: drop SPKVOL L/R            */
    _codec_w(0x8F, 0x3100);            /* DEPOP_M2                            */
    _codec_w(0x8E, 0x0009);            /* DEPOP_M1                            */
    _codec_rmw(0x63, 0xFFFF, 0x00E0);  /* PWR_ANLG1: HP_L|HP_R|HA             */
    _codec_rmw(0x8E, 0xFFFF, 0x0014);  /* DEPOP_M1 += bit4|bit2               */
    _codec_rmw(0x65, 0xFFFF, 0xC000);  /* PWR_MIXER: OUT MIXL/R power         */
    _codec_rmw(0x66, 0xFFFF, 0x0C00);  /* PWR_VOL: HPOVOL L/R power           */
    _codec_w(0x91, 0x0E00);            /* CHARGE_PUMP high-voltage            */
    _codec_w(0xB1, 0x0000);            /* EQ_CTRL2                            */
    _codec_w(0xB0, 0x6000);            /* EQ_CTRL1                            */
    _codec_w(0xB4, 0x2206);            /* DRC_AGC_1                           */
    _codec_w(0x90, 0x0737);            /* DEPOP_M3                            */
    _codec_pr_w(0x37, 0xFC00);         /* private 0x37                        */
    _codec_rmw(0x8E, 0xFFFF, 0x8000);  /* DEPOP_M1 += bit15                   */
    _codec_rmw(0x8E, 0xFFFF, 0x0040);  /* DEPOP_M1 += bit6                    */
    _codec_rmw(0x8E, 0xFCBF, 0x0000);  /* DEPOP_M1 keep-mask 0xFCBF           */
    _codec_rmw(0x8E, 0xFFFF, 0x0300);  /* DEPOP_M1 += bits9:8                 */
    _codec_rmw(0x02, 0x7F7F, 0x0000);  /* HP_VOL: unmute L/R                  */
    msleep(100);
    _codec_rmw(0x8E, 0xFCFB, 0x0000);  /* DEPOP_M1 keep-mask 0xFCFB           */
    _codec_w(0x02, 0x0000);            /* HP_VOL 0 dB, unmuted                */
    _codec_w(0x19, 0xAFAF);            /* DAC1_DIG_VOL 0 dB                   */
}

/* Play a note list onto the chosen channels (do_l / do_r), by PIO into the
 * ADMAIF FIFO - a square wave per note at TONE_RATE_HZ. `amp` sets the level
 * (the headphone amp needs far more than the efficient class-D speakers), and
 * `lead_ms` is a silent run-in so the output's mute-release ramp finishes
 * before the first note instead of clipping it. */
static void _play_notes(const note_t *notes, u32 count, bool do_l, bool do_r,
                        s16 amp, u32 lead_ms)
{
    /* One 32-bit word per FRAME: with the ADMAIF CIF's UNPACK16 set, the low
     * 16 bits are the left channel and the high 16 the right. A 16-bit push
     * per frame would leave the high half as sign extension, so the right DAC
     * would see DC and the pitch would come out an octave low. */
    for (u32 i = 0; i < TONE_RATE_HZ * lead_ms / 1000u; i++) {
        u32 spin = 0;
        while (APE(ADMAIF_TX_ACIF_FIFO_FULL) & BIT(0))
            if (++spin > 200000u) break;
        APE(ADMAIF_CH0_TX_FIFO_WRITE) = 0u;
    }

    for (u32 nidx = 0; nidx < count; nidx++) {
        u32 total = TONE_RATE_HZ * notes[nidx].ms / 1000u;
        u32 half  = notes[nidx].freq ? TONE_RATE_HZ / (2u * notes[nidx].freq)
                                     : 0u;
        for (u32 i = 0; i < total; i++) {
            s16 v = half ? (((i / half) & 1) ? amp : (s16)-amp) : (s16)0;
            u32 word = ((u32)(u16)(do_r ? v : (s16)0) << 16)
                     | (u32)(u16)(do_l ? v : (s16)0);
            u32 spin = 0;
            while (APE(ADMAIF_TX_ACIF_FIFO_FULL) & BIT(0))
                if (++spin > 200000u) break;
            APE(ADMAIF_CH0_TX_FIFO_WRITE) = word;
        }
    }
}

/* The melody test: left phrase out the left speaker, right phrase out the
 * right, then a phrase out the headphone jack, so a dead or quiet output on
 * any one of them is obvious by ear. */
static void _pio_melody(void)
{

    /* Make sure DAC1 reaches the headphone output too: route it straight
     * into the HPO mixer (0x45 bit14 clear) and the OUT mixers that feed
     * HPOVOL, and power those stages. Harmless during the speaker phrases,
     * where the headphone port itself is muted. */
    _codec_rmw(0x45, (u16)~0x4000u, 0x0000);
    _codec_rmw(0x4D, (u16)~0x0001u, 0x0000);
    _codec_rmw(0x50, (u16)~0x0001u, 0x0000);
    _codec_rmw(0x65, 0xFFFF, 0xC000);
    _codec_rmw(0x66, 0xFFFF, 0x0C00);

    APE(ADMA_CH0_CMD) = 0;
    APE(ADMAIF_CH0_TX_FIFO_CTRL) |= ADMAIF_TX_FIFO_CTRL_PIO;
    APE(AXBAR_I2S1_RX) = AXBAR_SRC_ADMAIF1;
    APE(ADMAIF_CH0_TX_ENABLE) = 1;

    /* Speakers: left phrase out the left driver, right phrase out the right,
     * headphone muted so it is purely a speaker check. */
    _codec_w(0x02, 0xC8C8);            /* headphone muted                    */
    _codec_w(0x01, 0x0000);            /* speaker L+R unmuted                */
    LOG("  Melody       : left speaker...\n");
    _play_notes(kMelodyL, sizeof(kMelodyL) / sizeof(kMelodyL[0]),
                true, false, 0x0200, 300u);
    LOG("  Melody       : right speaker...\n");
    _play_notes(kMelodyR, sizeof(kMelodyR) / sizeof(kMelodyR[0]),
                false, true, 0x0200, 150u);

    /* Headphone jack: both channels, speakers muted. The HP amp is far less
     * efficient than the class-D, so it needs a much larger amplitude to
     * reach a comparable level. */
    _codec_w(0x01, 0xC8C8);            /* speaker muted                      */
    _codec_hp_on();                    /* full HP amp bring-up, fresh        */
    LOG("  Melody       : headphone jack...\n");
    _play_notes(kMelodyJack, sizeof(kMelodyJack) / sizeof(kMelodyJack[0]),
                true, true, 0x0A00, 300u);

    APE(ADMAIF_CH0_TX_ENABLE) = 0;
    APE(ADMAIF_CH0_TX_FIFO_CTRL) &= ~ADMAIF_TX_FIFO_CTRL_PIO;
    _codec_mute_all();
}

void probe_audio_beep(void)
{
    HEADER("[Audio test - melody through I2S1]");

    /* The codec probe already proved the part answers and left its enable
     * asserted; without that there is nothing to play into. */
    u8 probe = 0;
    if (i2c_recv_buf_small(&probe, 1, I2C_1, CODEC_ADDR, 0xFE) != 0) {
        LOG("  Skipped      : codec not answering, nothing to drive\n");
        return;
    }

    /* Everything below reads and writes inside APE, which is only reachable
     * once the audio-clocks page has ungated the AUD partition and released
     * SWR_APE_RST. That always holds during the boot sweep, but the pager can
     * re-run this page alone ('r'), and an APE access while the block is
     * clocked-but-in-reset never completes - the BPMP hangs with no way back,
     * needing a power cycle. Checking the reset bit costs one CAR read and
     * turns that hang into a line of text. */
    if (CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_Y) & BIT(CLK_Y_APE)) {
        log_color(COL_WARN,
            "  Skipped      : APE still in reset, run the audio clocks page\n");
        return;
    }

    _audio_clocks_setup();
    _i2s_port_setup(&kI2sPorts[0]);
    _codec_playback_setup(false);
    _codec_mute_all();

    /* Keep this to one line: the LCD wraps to the top of the screen past row
     * 45 instead of scrolling, so every diagnostic row here costs a row the
     * melody's own output needs. The frame length is the one value worth
     * showing: fs = bclk / (2 x (bitcnt + 1)), so a divider mistake shows up
     * as a wrong rate. */
    {
        u32 frame = 2u * ((APE(I2S1_TIMING) & 0x7FFu) + 1u);
        LOG("  I2S1         : %d bclk/frame, fs %d Hz\n",
            frame, I2S1_BCLK_HZ / frame);
    }

    _pio_melody();
    _codec_mute_all();
    LOG("  Result       : melody played - left, right, then jack\n");
    dx_set("audio_beep", DX_PASS, "");
}
