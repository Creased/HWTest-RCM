/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: bt probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* Bluetooth radio - CYW4356 HCI over Tegra UART-D                          */
/*                                                                          */
/* Several independent blockers stack here, so no single fix brings the     */
/* link up. Two of them are in BDK and are verifiable without hardware:     */
/*                                                                          */
/*  1. uart_empty_fifo() writes UART_MCR = 0 as its first act and never     */
/*     restores it (bdk/soc/uart.c:173; contrast uart_init, which zeroes    */
/*     MCR at :57 and writes `mode` back at :64). Calling it after         */
/*     uart_init(..., RTS_EN|CTS_EN) leaves MCR at 0x00: RTS deasserted,   */
/*     chip flow-control muted and auto-CTS wiped. TX then "succeeds"      */
/*     and nothing ever comes back. Set MCR by hand after any fifo flush.  */
/*  2. uart_recv()'s timeout is get_tmr_us() + 250, i.e. 250 MICROseconds   */
/*     (uart.c:102, reloaded at :118), against a part that needs            */
/*     milliseconds. It also ignores the buffer bound when len == 0         */
/*     (`if (len && len <= i) break;`, uart.c:110), and it CLEARS           */
/*     UART_MCR_RTS for the duration of the read (uart.c:101-106),          */
/*     inverted-RTS behaviour that exists for the Joy-Con link and would    */
/*     undo our RTS assertion precisely while we listen. Nothing below      */
/*     calls either function.                                               */
/*                                                                          */
/* The rest is sequencing. The CYW4356 latches its transport select from    */
/* BT_HOST_WAKE during power-up (datasheet p28 s7.2) and samples straps "a  */
/* few milliseconds after" POR deassertion (p90 s13.4), where POR is        */
/* released by BT_REG_ON going high (p27 s6.2) and the internal POR can     */
/* hold the part up to 110 ms (p143 s21.1.1). Driving PH4 HIGH alone gives  */
/* no edge on a re-run, so no POR and no re-sample: the part keeps whatever */
/* mode it latched first. PH1/WL_REG_ON must go low too - p143's 10 ms      */
/* dwell only counts "where both signals have been driven low".             */
/*                                                                          */
/* The probe runs full cold power cycles, freezes port H across the whole   */
/* strap window, uses masked GPIO writes so a port-H update can never       */
/* read-modify-write PH5 by accident, and measures rather than assumes at   */
/* every step. Silence is a FAIL, but only once the UART-D loopback has     */
/* proved our own path works - otherwise it stays inconclusive, because a   */
/* FAIL we have not earned would send someone to reball a working module.   */

/* ---- knobs -------------------------------------------------------------
 * Bench-session bounds and timings, each independent: how many power-cycle
 * arms run, the POR and HCI deadlines, the MCR value the HCI arms use, and
 * whether the MCR fallback sweep runs at all. The host baud is not a knob -
 * every arm talks at 115200.                                              */
#define BT_CFG_ARMS        3   /* 1 = normal only, 2 = +autobaud, 3 = +keeper */
#define BT_CFG_POR_MS      200 /* port-H freeze + POR wait after the edge     */
#define BT_CFG_MCR_HCI     UART_MCR_RTS   /* 0x02: force the RTS pin LOW      */
#define BT_CFG_MCR_SWEEP   1   /* on total failure, sweep MCR                 */
#define BT_CFG_HCI_MS      700 /* per-send response deadline, ms              */
#define BT_CFG_HCI_TRIES   3   /* HCI_Reset sends per arm                     */

#define BT_UART      UART_D
#define BT_UART_REGS ((volatile uart_t *)(UART_BASE + 0x300))

/* PINMUX_AUX offsets. The "0x1B4 + 4n" run is only valid up to PH5 - BDK's
 * own header has PINMUX_AUX_AP_WAKE_NFC at 0x1CC (that pad is PH7, which the
 * Switch reuses as BT_GPIO5) and PINMUX_AUX_GPIO_PH6 at 0x250, nowhere near
 * the run. PK0..PK2 are derived backwards from BDK's PINMUX_AUX_GPIO_PK3,
 * so they cannot drift either. */
#define PMX_PH3_DEV_WAKE   0x1C0
#define PMX_PH4_BT_REG_ON  0x1C4
#define PMX_PH5_HOST_WAKE  0x1C8
#define PMX_PH7_BT_GPIO5   PINMUX_AUX_AP_WAKE_NFC      /* 0x1CC, NOT PH6 */
#define PMX_PK0_BT_GPIO2   (PINMUX_AUX_GPIO_PK3 - 0xC) /* 0x254 */
#define PMX_PK1_BT_GPIO3   (PINMUX_AUX_GPIO_PK3 - 0x8) /* 0x258 */
#define PMX_PK2_BT_GPIO4   (PINMUX_AUX_GPIO_PK3 - 0x4) /* 0x25C */

/* bdk/soc/uart.h stops at RDR/THRE/TMTY/FIFOE. */
#define UART_LSR_OVRF BIT(1)
#define UART_LSR_PERR BIT(2)
#define UART_LSR_FERR BIT(3)
#define UART_LSR_BRK  BIT(4)
#define UART_MSR_CTS  BIT(4)   /* set = the chip drove its RTS_N low */
/* INFERRED: FCR[7:6] is the standard 16550 RX trigger level, 3 = 16 chars.
 * Only matters in the RTS_EN arms of the sweep; harmless with manual RTS. */
#define BT_FCR_TRIG16 (3u << 6)

/* --- bounded primitives -------------------------------------------------
 * Everything here polls registers directly and every loop has a deadline.
 * BDK's uart_init() is also avoided: it ends with an unbounded RX drain
 * (uart.c:68 -> :81, `while (LSR & RDR) read THR;`) and, for a mode without
 * CTS_EN/DTR, an unbounded TX-idle spin at :31 - and we deliberately run it
 * with the radio unpowered and a Tegra pull-down on RX, which is a standing
 * break condition. A hang there would produce no log at all. */

static void bt_uart_cfg(u32 baud, u32 mcr)
{
    volatile uart_t *u = BT_UART_REGS;

    /* clock_uart_use_src_div returns 1 for every rate except 1M/3M, where it
     * sets UART_SRC_CLK_DIV_EN and the block runs from 48 MHz with div = 1.
     * At 115200 it leaves bit 24 clear, so PLLP_OUT0 arrives undivided at
     * 408 MHz and DLL = 221 -> 115385 baud, +0.16%. The chip's own 115200 is
     * also +0.16% (p38 Table 15) and it tolerates +/-2% combined. */
    u32 div = clock_uart_use_src_div(BT_UART, baud)
                ? ((8 * baud + 408000000) / (16 * baud)) : 1;

    u->UART_IER_DLAB = 0;
    u->UART_LCR      = UART_LCR_DLAB | UART_LCR_WORD_LENGTH_8;
    u->UART_THR_DLAB = (u8)div;
    u->UART_IER_DLAB = (u8)(div >> 8);
    /* 8N1 at every rate. uart_init() would set 2 stop bits above 1 Mbaud
     * (uart.c:37-38) - fine for a debug console, wrong for H4. */
    u->UART_LCR      = UART_LCR_WORD_LENGTH_8;
    (void)u->UART_SPR;

    u->UART_IIR_FCR = UART_IIR_FCR_EN_FIFO;
    (void)u->UART_SPR;
    usleep(20);
    u->UART_MCR = 0;
    usleep(96);
    u->UART_IIR_FCR = BT_FCR_TRIG16 | UART_IIR_FCR_EN_FIFO |
                      UART_IIR_FCR_TX_CLR | UART_IIR_FCR_RX_CLR;
    u->UART_MCR = mcr;
    (void)u->UART_SPR;
    usleep(200);
}

/* Clear the FIFOs without touching MCR. */
static u32 bt_flush(void)
{
    volatile uart_t *u = BT_UART_REGS;
    u32 n = 0;

    u->UART_IIR_FCR = BT_FCR_TRIG16 | UART_IIR_FCR_EN_FIFO |
                      UART_IIR_FCR_RX_CLR | UART_IIR_FCR_TX_CLR;
    (void)u->UART_SPR;
    usleep(100);
    while ((u->UART_LSR & UART_LSR_RDR) && n < 512) {
        (void)u->UART_THR_DLAB;
        n++;
    }
    (void)u->UART_LSR;   /* reading LSR clears latched BI/FE/PE/OE */
    return n;
}

static bool bt_send(const u8 *buf, u32 len)
{
    volatile uart_t *u = BT_UART_REGS;

    for (u32 i = 0; i < len; i++) {
        u32 spin = 20000;   /* 20 ms/byte cap vs 87 us actual at 115200 */
        while (!(u->UART_LSR & UART_LSR_THRE)) {
            if (!--spin)
                return false;   /* auto-CTS gated: chip never released us */
            usleep(1);
        }
        u->UART_THR_DLAB = buf[i];
    }
    return true;
}

/* One byte or -1. Subtraction against a signed delta so a get_tmr_us()
 * wrap mid-wait cannot turn into a multi-hour spin. */
static int bt_getb(u32 deadline_us)
{
    volatile uart_t *u = BT_UART_REGS;

    for (;;) {
        if (u->UART_LSR & UART_LSR_RDR)
            return (int)(u->UART_THR_DLAB & 0xFF);
        if ((s32)(deadline_us - get_tmr_us()) <= 0)
            return -1;
    }
}

/* Length-driven H4 reader. Scanning forward for a 0x04 byte is not safe:
 * Broadcom parts emit packet types the H4 spec does not define, and
 * BCM_LM_DIAG_PKT (0x07) carries a FIXED 63-byte payload with no length
 * field (hci_bcm.c:36-46, :682-691) which can easily contain a 0x04. So
 * frame properly: skip the zero-length vendor types 0x00/0x31/0x34, consume
 * exactly 63 for 0x07, and only then trust a 0x04 event header.
 *
 * Returns the event length (type byte included) in `ev`, 0 on timeout.
 * Everything seen goes into `raw` so a failed run can still be read. */
static u32 bt_wait_event(u8 *ev, u32 max, u32 ms, u8 *raw, u32 raw_max,
                         u32 *raw_n)
{
    u32 dl = get_tmr_us() + ms * 1000;
    int c;

    for (;;) {
        c = bt_getb(dl);
        if (c < 0)
            return 0;
        if (*raw_n < raw_max)
            raw[(*raw_n)++] = (u8)c;

        if (c == 0x00 || c == 0x31 || c == 0x34)
            continue;                       /* zero-length vendor packets */

        if (c == 0x07) {                    /* LM diagnostic, 63 bytes */
            for (u32 i = 0; i < 63; i++) {
                int d = bt_getb(dl);
                if (d < 0)
                    return 0;
                if (*raw_n < raw_max)
                    raw[(*raw_n)++] = (u8)d;
            }
            continue;
        }

        if (c == 0x04) {                    /* HCI event: hdr 2, len at [1] */
            u8 hdr[2];
            for (u32 i = 0; i < 2; i++) {
                int d = bt_getb(dl);
                if (d < 0)
                    return 0;
                if (*raw_n < raw_max)
                    raw[(*raw_n)++] = (u8)d;
                hdr[i] = (u8)d;
            }
            u32 plen = hdr[1];
            u32 n = 0;
            if (max >= 3) {
                ev[0] = 0x04; ev[1] = hdr[0]; ev[2] = hdr[1];
                n = 3;
            }
            for (u32 i = 0; i < plen; i++) {
                int d = bt_getb(dl);
                if (d < 0)
                    break;
                if (*raw_n < raw_max)
                    raw[(*raw_n)++] = (u8)d;
                if (n < max)
                    ev[n++] = (u8)d;
            }
            return n;
        }

        /* 0x02 ACL / 0x03 SCO / 0x05 ISO, or garbage. Resync by continuing;
         * the raw dump above is what tells you which it was. */
    }
}

/* H4 HCI_Reset, opcode 0x0C03, no parameters. */
static const u8 bt_hci_reset[] = { 0x01, 0x03, 0x0C, 0x00 };

/* Does `ev` hold the Command Complete for HCI_Reset, status 0x00?
 *   [0]04 [1]0E [2]plen [3]ncmd [4..5]opcode LE [6]status */
static bool bt_is_reset_cc(const u8 *ev, u32 n)
{
    return n >= 7 && ev[1] == 0x0E && ev[4] == 0x03 &&
           ev[5] == 0x0C && ev[6] == 0x00;
}

static u8 bt_at(const u8 *b, u32 n, u32 i) { return i < n ? b[i] : 0; }

static void bt_dump(const u8 *b, u32 n)
{
    if (!n) {
        LOG("  raw           : (nothing on the wire)\n");
        return;
    }
    for (u32 i = 0; i < n && i < 32; i += 8)
        LOG("  raw+%02d       : %02X %02X %02X %02X %02X %02X %02X %02X\n",
            i, bt_at(b, n, i), bt_at(b, n, i + 1), bt_at(b, n, i + 2),
            bt_at(b, n, i + 3), bt_at(b, n, i + 4), bt_at(b, n, i + 5),
            bt_at(b, n, i + 6), bt_at(b, n, i + 7));
}

/* --- pad state ---------------------------------------------------------- */

/* Park every pad the CYW4356 can sample, with both BT_REG_ON and WL_REG_ON
 * driven LOW. Whole-value pinmux writes, never OR, so PINMUX
 * PARK (bit 5) is cleared as a side effect - a parked pad ignores
 * TRISTATE/E_INPUT/PUPD and holds its latched level while reading back
 * exactly what you wrote.
 *
 * ph5_kick: pulse BT_HOST_WAKE high, then release it to high-Z, all while
 * both REG_ON pins are low. Datasheet p93 Table 29 marks this pad group
 * "Keeper: Y", so a level previously driven onto it is weakly latched and
 * survives the Tegra releasing the pad - which means fixing the pinmux
 * alone does not guarantee the strap reads high. Flipping the keeper before
 * the edge biases it without any contention during the sample window. */
static void bt_pads_park(bool ph5_kick)
{
    /* PH5 = BT_HOST_WAKE = BT_GPIO_1, the SPI-vs-UART transport strap.
     * 0x50 = no pull + TRISTATE + E_INPUT, which is what NVIDIA's board
     * pinmux and Horizon both program. The no-pull is deliberate: the
     * structurally identical WifiWakeAp pad next door gets an explicit
     * pull-down on every board revision and this one never does. TRISTATE
     * matters as well as OE = 0, so the pad's reserved SFIO function cannot
     * drive the ball either. Figure 9 (p24) draws BT_GPIO_1 as "Pulled",
     * never "Driven" - the host is not supposed to hold this line. */
    if (ph5_kick) {
        PINMUX_AUX(PMX_PH5_HOST_WAKE) = PINMUX_INPUT_ENABLE;
        GP_MWR(GPH_MOUT, GPIO_PIN_5, 1);
        GP_MWR(GPH_MCNF, GPIO_PIN_5, 1);
        GP_MWR(GPH_MOE,  GPIO_PIN_5, 1);
        usleep(2000);
        GP_MWR(GPH_MOE,  GPIO_PIN_5, 0);   /* release, keeper holds high */
    }
    PINMUX_AUX(PMX_PH5_HOST_WAKE) =
        PINMUX_INPUT_ENABLE | PINMUX_TRISTATE | PINMUX_PULL_NONE;
    GP_MWR(GPH_MCNF, GPIO_PIN_5, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_5, 0);
    /* GPIO_OUT bit 5 is never written, in either branch except the kick. */

    /* PH3 = BT_DEV_WAKE = BT_GPIO_0, held LOW. Three sources agree from two
     * different directions: Horizon parks ApWakeBt Output/Low, the Switch DT
     * declares device-wakeup-gpios ACTIVE_LOW (so low is also the asserted
     * state), and Figure 9 keeps the trace low for the entire startup. A
     * high here is deasserted under either reading, and if SPI ever latches
     * this pad becomes SPI_INT, a chip OUTPUT (p28 Table 7), so driving it
     * push-pull high would be a sustained drive fight. */
    PINMUX_AUX(PMX_PH3_DEV_WAKE) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_3, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_3, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_3, 1);

    /* PH7 = BT_GPIO5, output low, as every Switch GPIO hog has it. */
    PINMUX_AUX(PMX_PH7_BT_GPIO5) = PINMUX_INPUT_ENABLE | PINMUX_PULL_NONE;
    GP_MWR(GPH_MOUT, GPIO_PIN_7, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_7, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_7, 1);

    /* PK0/PK1/PK2 = BT_GPIO2/3/4, inputs. BT_GPIO4 is the one strap the
     * datasheet actually tabulates (p90 Table 25): 1 = BT serial flash
     * present, 0 = absent, default 0. Driven high across the edge, the ROM
     * boots hunting for a flash that is not on this board instead of
     * presenting HCI. Function field is rsvd2 (0b10) on these three, not 0,
     * and the pull differs per pin - a blanket 0x40 is wrong twice over. */
    PINMUX_AUX(PMX_PK0_BT_GPIO2) = 2 | PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    PINMUX_AUX(PMX_PK1_BT_GPIO3) = 2 | PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    PINMUX_AUX(PMX_PK2_BT_GPIO4) = 2 | PINMUX_INPUT_ENABLE | PINMUX_PULL_NONE;
    GP_MWR(GPK_MCNF, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2, 1);
    GP_MWR(GPK_MOE,  GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2, 0);

    /* PH4 = BT_REG_ON low. OUT before OE, so a second run of the payload
     * cannot glitch the pad high for a few hundred ns before it goes low -
     * an unintended pulse that would consume the CBUCK discharge dwell. */
    PINMUX_AUX(PMX_PH4_BT_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_4, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_4, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_4, 1);

    /* PH1 = WL_REG_ON low as well. It is OR-gated with BT_REG_ON into the
     * shared CBUCK/CLDO (p143 s21.1.1), so with PH1 high the regulators
     * never collapse and a PH4 toggle is a warm poke, not a cold POR - the
     * 10 ms rule is explicitly qualified "where both signals have been
     * driven low". BT does not need WLAN: Figure 50 (p145) is literally
     * "WLAN = OFF, Bluetooth = ON". Restored high before we return. */
    PINMUX_AUX(PMX_PH1_WL_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_1, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_1, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_1, 1);

    /* PH0 is left exactly as found: it is Wi-Fi RF Disable on Erista but
     * the Samsung panel IRQ on some OLED units, and it gates the WLAN RF
     * path rather than anything on the BT side. */
}

/* uart4_tx_pi4 / rx_pi5 / rts_pi6 / cts_pi7.
 *
 * NVIDIA's board pinmux puts PULL_DOWN on all four; hekate's
 * pinmux_config_uart() puts none on any. We deviate from both on the two
 * inputs, deliberately:
 *
 *   CTS (our input, the chip's RTS_N, active low) gets PULL_UP. With the
 *   vendor pull-down the pin idles LOW, which reads in UART_MSR bit 4 as
 *   "CTS asserted" whether or not anything is out there - so the chip's
 *   "transport is ready" signal (Figure 9, T4) would be a constant and the
 *   one measurement this whole probe leans on would be worthless. PULL_UP
 *   parks it deasserted so an assertion means the chip really drove it.
 *
 *   RX gets whatever `rx_pull` says. PULL_DOWN while the radio is dark is a
 *   genuine diagnostic - it manufactures a permanent break so LSR shows
 *   BRK/FERR and you can tell "nothing driving" from "driving idle-high".
 *   PULL_UP is the correct idle for HCI traffic and is what this tree
 *   already does for UART-B RX in ipl_main. So: pull-down for the
 *   line-state snapshot, pull-up for the exchange. */
static void bt_uart_pads(u32 rx_pull)
{
    PINMUX_AUX(PINMUX_AUX_UARTX_TX(BT_UART))  = PINMUX_PULL_NONE;
    PINMUX_AUX(PINMUX_AUX_UARTX_RX(BT_UART))  =
        PINMUX_INPUT_ENABLE | PINMUX_TRISTATE | rx_pull;
    PINMUX_AUX(PINMUX_AUX_UARTX_RTS(BT_UART)) = PINMUX_PULL_NONE;
    PINMUX_AUX(PINMUX_AUX_UARTX_CTS(BT_UART)) =
        PINMUX_INPUT_ENABLE | PINMUX_TRISTATE | PINMUX_PULL_UP;

    /* A Tegra pad only reaches the UART while its GPIO_CNF bit is 0, and
     * pinmux_config_uart() writes PINMUX_AUX only. BDK hand-writes this
     * handback for UART-B/C in hw_init (hw_init.c:458-462) and this tree
     * does it for UART-B RX in ipl_main, but there is no UART-D case
     * anywhere. Cheap insurance: nothing in BDK or main.c claims port I, so
     * CNF should already read 0 - which is exactly why we log it. Note the
     * 16550 loopback self-test below is structurally incapable of catching a
     * fault here, because loopback closes inside the controller. */
    GPIO(GPI_CNF) &= ~(GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);
    (void)GPIO(GPI_CNF);
}

/* Is anything driving PH3? In UART transport BT_DEV_WAKE is a pure
 * host-to-chip input the chip never drives; in SPI transport the same ball
 * is SPI_INT, a chip OUTPUT that the part uses to negotiate transactions
 * (p28 s7.1 / Table 7). Release the pad and see whether it follows our own
 * pull in both directions. Returns 1 if the chip is driving it. */
static int bt_probe_spi_int(int *hi, int *lo)
{
    GP_MWR(GPH_MOE, GPIO_PIN_3, 0);
    PINMUX_AUX(PMX_PH3_DEV_WAKE) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP;
    usleep(1000);
    *hi = gpio_read(GPIO_PORT_H, GPIO_PIN_3);
    PINMUX_AUX(PMX_PH3_DEV_WAKE) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    usleep(1000);
    *lo = gpio_read(GPIO_PORT_H, GPIO_PIN_3);

    /* Restore: driven low for the rest of the session. */
    GP_MWR(GPH_MOUT, GPIO_PIN_3, 0);
    GP_MWR(GPH_MOE,  GPIO_PIN_3, 1);

    return (*hi == *lo);   /* pin ignored our pull -> something drives it */
}

/* --- one cold power cycle + HCI attempt --------------------------------- */

typedef struct {
    const char *name;
    bool rts_across_edge;   /* assert the chip's CTS_N over the edge      */
    bool ph5_kick;          /* flip the BT_HOST_WAKE bus keeper high      */
} bt_arm_t;

static bool bt_attempt(const bt_arm_t *arm, u8 *ev, u32 ev_max, u32 *ev_n)
{
    volatile uart_t *u = BT_UART_REGS;
    u8  raw[48];
    u32 raw_n = 0;

    log_color(COL_INFO, "  -- arm %s --\n", arm->name);

    /* 1. Straps parked, both REG_ON low. */
    bt_pads_park(arm->ph5_kick);

    /* 2. Host I/Os configured BEFORE the edge (Figure 9, T2), with the RX
     *    pull-down in place so the dark-line diagnostic is available, and
     *    with the RTS pin at whichever level this arm is testing.
     *
     *    Both arms are legitimate chip modes, and the kernel has shipped
     *    both: commit 3347a80965b3 added drive-RTS-after-power-on for a
     *    CYW43455 that "doesn't react to HCI requests", e601daed271e then
     *    narrowed it to BCM43438 only because it regressed other boards,
     *    and af35e28f0fea documents RTS-low-across-the-edge as the way you
     *    deliberately enter autobaud. Autobaud still answers a plain
     *    HCI_Reset - there is no training pattern anywhere in hci_bcm.c or
     *    btbcm.c - so this is a real A/B, not a trap to avoid. */
    bt_uart_pads(PINMUX_PULL_DOWN);
    bt_uart_cfg(115200, arm->rts_across_edge ? UART_MCR_RTS : 0);

    LOG("  pre-edge      : MCR=%02X LSR=%02X MSR=%02X CNF_I=%02X\n",
        u->UART_MCR & 0xFF, u->UART_LSR & 0xFF, u->UART_MSR & 0xFF,
        GPIO(GPI_CNF) & 0xFF);
    LOG("  pads low      : PH1=%d PH3=%d PH4=%d PH5=%d PH7=%d\n",
        gpio_read(GPIO_PORT_H, GPIO_PIN_1), gpio_read(GPIO_PORT_H, GPIO_PIN_3),
        gpio_read(GPIO_PORT_H, GPIO_PIN_4), gpio_read(GPIO_PORT_H, GPIO_PIN_5),
        gpio_read(GPIO_PORT_H, GPIO_PIN_7));

    /* 3. Datasheet minimum is 10 ms with both REG_ON low so the internal
     *    CBUCK can discharge; skipping it risks a ~36 mA VDDIO in-rush on
     *    the next PMU cold start and an unreliable POR (p143 s21.1.1). */
    msleep_poll(20);

    /* 4. THE EDGE. One masked store; nothing else on port H moves. */
    GP_MWR(GPH_MOUT, GPIO_PIN_4, 1);
    (void)GPIO(GPH_OUT);

    /* 5. Absolute port-H silence. PH5 is sampled a few milliseconds
     *    AFTER POR deassertion (p90 s13.4), and POR itself can hold the part
     *    up to 110 ms after its rails cross threshold - rails that only
     *    start at this edge, since VDDC comes from the internal CBUCK. So
     *    the worst-case sample instant is ~115 ms from here, not "a few ms".
     *    No GPIO_CNF/OE/OUT writes, no PINMUX 0x1C8 writes, in this window.
     *    Silence on the UART here is EXPECTED: p93 Table 29 lists all four
     *    BT UART pins as "I: PU" until the ROM configures its I/Os. */
    msleep_poll(BT_CFG_POR_MS);

    u32 in_h = GPIO(GPH_IN);
    log_color((in_h & GPIO_PIN_4) ? COL_OK : COL_ERR,
        "  PH4 BT_REG_ON : %d%s\n", (in_h & GPIO_PIN_4) ? 1 : 0,
        (in_h & GPIO_PIN_4) ? "" : "  (write did not latch)");
    /* Informational only. BT_HOST_WAKE is a programmable-polarity chip I/O
     * once the ROM is up (p23 Table 6, p93 Table 29), and it is NOT remapped
     * in SPI mode - so this level is not a transport verdict either way. */
    LOG("  PH5 HOST_WAKE : %d (info only, not a transport verdict)\n",
        (in_h & GPIO_PIN_5) ? 1 : 0);
    LOG("  PK0/1/2 in    : %d %d %d  (BT_GPIO2/3/4)\n",
        gpio_read(GPIO_PORT_K, GPIO_PIN_0),
        gpio_read(GPIO_PORT_K, GPIO_PIN_1),
        gpio_read(GPIO_PORT_K, GPIO_PIN_2));

    /* 6. Line-state snapshot while RX still carries the vendor pull-down.
     *    BRK/FERR latched here means nothing is driving BT_UART_TXD at all
     *    (unpowered, or in SPI mode where that ball becomes SPI_MISO). A
     *    clean LSR means the chip is driving RX idle-high, i.e. alive and
     *    choosing not to answer - a completely different fault, and this
     *    snapshot is what tells the two apart. */
    u32 lsr = u->UART_LSR;
    LOG("  line state    : LSR=%02X%s%s%s%s\n", lsr & 0xFF,
        (lsr & UART_LSR_BRK)  ? " BRK"  : "",
        (lsr & UART_LSR_FERR) ? " FERR" : "",
        (lsr & UART_LSR_PERR) ? " PERR" : "",
        (lsr & UART_LSR_OVRF) ? " OVRF" : "");

    /* 7. Is the chip driving SPI_INT on PH3? This is the decisive
     *    transport measurement and it costs two register writes. */
    {
        int hi = 0, lo = 0;
        int driven = bt_probe_spi_int(&hi, &lo);
        log_color(driven ? COL_ERR : COL_OK,
            "  PH3 SPI_INT   : pu=%d pd=%d -> %s\n", hi, lo,
            driven ? "CHIP DRIVING - SPI latched" : "not driven (UART/dead)");
    }

    /* 8. Swap RX to idle-high, then flush. The chip only starts driving its
     *    TXD at the end of Figure 9's T3 ("BTH I/Os configured"), so up to
     *    this point the FIFO has been filling with break-generated 0x00s. */
    bt_uart_pads(PINMUX_PULL_UP);
    usleep(2000);
    u32 junk = bt_flush();

    /* 9. NOW assert the chip's BT_UART_CTS_N (our RTS pin, active low).
     *    Bit 1 is the manual force; bit 6 (RTS_EN) would instead hand the
     *    pin to the RX-FIFO watermark logic. CTS_EN stays off for the first
     *    pass so a dead part shows up as chip silence rather than as a
     *    gated transmitter - the sweep below tries the other combinations. */
    uart_set_mode(BT_UART, BT_CFG_MCR_HCI);
    usleep(200);

    /* 10. Figure 9's T4: the chip answers by driving BT_UART_RTS_N low,
     *     which lands on our CTS input as UART_MSR bit 4. With the PULL_UP
     *     from step 8 this is a real measurement. Note it is a live
     *     per-byte flow-control output, not a latch (p39 Table 16 ref 3),
     *     so it is evidence, not a gate - we send HCI either way. */
    bool cts = false;
    for (u32 i = 0; i < 500 && !cts; i++) {
        cts = (u->UART_MSR & UART_MSR_CTS) != 0;
        if (!cts)
            usleep(1000);
    }
    log_color(cts ? COL_OK : COL_WARN,
        "  transport rdy : chip RTS_N %s (MSR=%02X, junk=%d)\n",
        cts ? "LOW" : "never low", u->UART_MSR & 0xFF, junk);

    /* 11. HCI_Reset is unconditionally the first command on the wire for
     *     every Broadcom UART part, in normal and autobaud mode alike
     *     (btbcm.c:582-585 -> :272-289). No firmware download is needed
     *     first: the part runs its lower-layer stack from 668 KB of on-die
     *     ROM (p27 s6) and Horizon pushes its patchram in-band over an
     *     already-working HCI link.
     *
     *     Sent up to three times. The first four bytes can land while the
     *     ROM is still claiming its pads at the end of T3, and Reset is
     *     idempotent, so retrying costs nothing and covers that race. */
    *ev_n = 0;
    for (u32 t = 0; t < BT_CFG_HCI_TRIES && !*ev_n; t++) {
        if (!bt_send(bt_hci_reset, sizeof(bt_hci_reset))) {
            log_color(COL_WARN, "  HCI_Reset     : TX gated (MCR=%02X)\n",
                u->UART_MCR & 0xFF);
            break;
        }
        *ev_n = bt_wait_event(ev, ev_max, BT_CFG_HCI_MS,
                              raw, sizeof(raw), &raw_n);
    }

    if (!*ev_n) {
        LOG("  HCI_Reset     : no event, %d raw bytes, LSR=%02X\n",
            raw_n, u->UART_LSR & 0xFF);
        bt_dump(raw, raw_n);
        return false;
    }

    bool cc = bt_is_reset_cc(ev, *ev_n);
    log_color(cc ? COL_OK : COL_WARN,
        "  HCI_Reset     : %d bytes %02X %02X %02X %02X %02X %02X %02X\n",
        *ev_n, ev[0], bt_at(ev, *ev_n, 1), bt_at(ev, *ev_n, 2),
        bt_at(ev, *ev_n, 3), bt_at(ev, *ev_n, 4), bt_at(ev, *ev_n, 5),
        bt_at(ev, *ev_n, 6));
    if (!cc)
        bt_dump(raw, raw_n);
    return cc;
}

void probe_bt_radio(void)
{
    volatile uart_t *u = BT_UART_REGS;
    u8  ev[72];
    u32 ev_n = 0;
    bool ok = false;

    log_color(COL_INFO, "[Bluetooth radio (" WLAN_CHIP_NAME " HCI over UART-D)]\n");

    /* Entry state, before anything is touched. OE bit 5 set with OUT bit 5
     * clear is the SPI-strap condition - the Tegra itself holding
     * BT_HOST_WAKE low - so this read is what tells you whether the BootROM
     * left the pad that way. */
    LOG("  port H entry  : CNF=%02X OE=%02X OUT=%02X IN=%02X\n",
        GPIO(GPH_CNF) & 0xFF, GPIO(GPH_OE) & 0xFF,
        GPIO(GPH_OUT) & 0xFF, GPIO(GPH_IN) & 0xFF);
    LOG("  PH5 pinmux    : %04X   port I CNF: %02X\n",
        PINMUX_AUX(PMX_PH5_HOST_WAKE) & 0xFFFF, GPIO(GPI_CNF) & 0xFF);

    /* Every input this probe depends on - uart4_rx, uart4_cts and PH5 - is
     * TRISTATE + E_INPUT. Bit 0 here clamps a tristated pad's input receiver
     * to 0, which would kill all three at once. hw_init clears it; confirm
     * it stayed clear rather than assuming. */
    u32 glob = APB_MISC(APB_MISC_PP_PINMUX_GLOBAL);
    log_color((glob & BIT(0)) ? COL_ERR : COL_OK,
        "  PINMUX_GLOBAL : %08X (%s)\n", glob,
        (glob & BIT(0)) ? "INPUTS CLAMPED" : "clear");

    /* Rails. No Switch device tree gives the bluetooth node a regulator,
     * clock or pwrseq property, so which supply reaches BT_VDDIO through the
     * board's load switch is not documented and these are brought up by hand.
     *
     * Only the two that plausibly feed the radio: LDO1 (XUSB+PCIe 1.05 V) and
     * LDO7 (XUSB 1.05 V). This list used to also carry LDO3, LDO5, LDO6 and
     * LDO8 - the gamecard ASIC, the gamecard slot, touch+ALS and DisplayPort -
     * brought up together because this probe could not tell which mattered.
     * None of them reach the radio, and switching them on had two costs.
     * LDO5 is the serious one:
     * pcv runs the cartridge interface at 1.8 V, the PMIC's OTP default is
     * 3.1 V, and nothing programs it under RCM - so enabling it as found put
     * 3.1 V on a 1.8 V interface with a game card possibly seated. The other
     * cost is measurement: rails left on changed what later pages reported,
     * so the same console read "LDO5 off" on a cold sweep and "LDO5 ON" on a
     * refresh.
     *
     * They are put back as found for the same reason - this probe is one of
     * many in the sweep and must not decide the state the rest of them
     * measure. */
    static const struct { u32 id; u8 cfg; const char *name; } rails[] = {
        { REGULATOR_LDO1, 0x25, "LDO1" }, { REGULATOR_LDO7, 0x31, "LDO7" },
    };
    u8 rail_saved[ARRAY_SIZE(rails)];

    for (u32 i = 0; i < ARRAY_SIZE(rails); i++) {
        rail_saved[i] = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].cfg);
        max7762x_regulator_enable(rails[i].id, true);
        msleep(5);
    }
    LOG("  Rails up      : ");
    for (u32 i = 0; i < ARRAY_SIZE(rails); i++)
        LOG("%s=%d ", rails[i].name,
            i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].cfg) >> 6);
    LOG("\n");

    u8 ame = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_AME_GPIO);
    /* Every Switch device tree routes the PMIC's SECOND 32 kHz output to
     * GPIO4 - pin_gpio4 { function = "32k-out1"; } - and on this PMIC an
     * alternate function is selected by AME_GPIO (0x40), whose header
     * comment is literally "Clear bits are Standard GPIO". So the default is
     * plain GPIO and no clock leaves the pin. 32K_OUT0 (CNFG1_32K bit 2) is
     * a different output: it feeds the Tegra's own clk_32k_in.
     *
     * Worth matching the vendor, but do NOT rank this first: p18 s3.3 says
     * "Either the internal low-precision LPO or an external 32.768 kHz
     * precision oscillator is required", internal range ~33 kHz +/-30%. The
     * part boots and answers HCI on its internal LPO. This affects
     * low-power timing accuracy, not whether the chip talks. */
    if (!(ame & BIT(4))) {
        i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_AME_GPIO,
                      ame | BIT(4));
        msleep(10);
    }
    u8 ame2 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_AME_GPIO);
    u8 c32  = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CNFG1_32K);
    if (!(c32 & MAX77620_CNFG1_32K_OUT0_EN)) {
        i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CNFG1_32K,
                      c32 | MAX77620_CNFG1_32K_OUT0_EN);
        msleep(10);
        c32 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CNFG1_32K);
    }
    LOG("  LPO 32k       : AME %02X->%02X CNFG1_32K %02X (32K_OK=%d)\n",
        ame, ame2, c32, (c32 & MAX77620_CNFG1_32K_OK) ? 1 : 0);

    /* vdd_3v3 is gated by PMIC GPIO3, but the vendor tree declares that pin
     * function = "fps-out" with drive-open-drain on FPS_SRC_0: with AME bit
     * 3 set the Flexible Power Sequencer owns it, the CNFG_GPIO3 output bit
     * does not drive the pin, and an open-drain output cannot source a high
     * anyway, so a write here can read back as success on a rail that is
     * still gated. Report, do not write. */
    LOG("  vdd_3v3 gate  : AME.3=%d CNFG_GPIO3=%02X (FPS-owned)\n",
        (ame2 & BIT(3)) ? 1 : 0,
        i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_GPIO3));

    /* Clock the UART exactly once, here. clock_enable() asserts the module
     * reset before releasing it (clock.c:168), so re-running it per retry
     * would reset the block while the radio is already powered and watching
     * its RXD, and would discard the divisor programmed the attempt before. */
    clock_enable_uart(BT_UART);
    bt_uart_pads(PINMUX_PULL_UP);
    bt_uart_cfg(115200, 0);
    LOG("  CAR U         : RST=%08X ENB=%08X SRC_D=%08X\n",
        CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_U),
        CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_U),
        CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_UARTD));
    if (!(CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_U) & BIT(CLK_U_UARTD))) {
        log_color(COL_ERR, "  UART-D        : NOT CLOCKED - aborting\n");
        return;
    }
    /* IRDA_CSR carries per-line inversion (uart_invert), which is used in
     * anger on the Joy-Con ports. An inverted line is symptomless from the
     * register side: TX "succeeds", LSR looks clean, nothing ever decodes. */
    LOG("  IRDA_CSR      : %08X (0 = no line inversion)\n", u->UART_IRDA_CSR);

    /* 16550 internal loopback: MCR bit 4 ties TX back to RX inside the
     * block. This proves the clock, the divisor, the FIFOs and the send /
     * receive helpers without involving a single pad - which also means it
     * PASSES with every pad in GPIO mode, so it can never substitute for the
     * port-I readback above. Restore MCR by hand; uart_empty_fifo() would
     * zero it and never put it back. */
    bool uart_ok = false;
    {
        static const u8 pat[] = { 0xA5, 0x5A, 0x00, 0xFF };
        u8 back[8];
        u32 got = 0;
        u->UART_MCR = BIT(4);
        (void)u->UART_SPR;
        bt_flush();
        if (bt_send(pat, sizeof(pat))) {
            u32 dl = get_tmr_us() + 50000;
            for (u32 i = 0; i < sizeof(pat); i++) {
                int c = bt_getb(dl);
                if (c < 0)
                    break;
                back[got++] = (u8)c;
            }
        }
        bool lb = (got == sizeof(pat)) && !memcmp(back, pat, sizeof(pat));
        uart_ok = lb;
        u->UART_MCR = 0;
        (void)u->UART_SPR;
        bt_flush();
        log_color(lb ? COL_OK : COL_ERR,
            "  UART-D loopbk : %s (%d bytes, LSR=%02X)\n",
            lb ? "OK" : "FAILED", got, u->UART_LSR & 0xFF);
        if (!lb)
            LOG("  (controller-level fault - the radio is not the problem)\n");
    }

    /* Each arm is a full cold power cycle, because the transport strap is
     * latched once per POR and never re-evaluated: a retry that only
     * re-pulses PH4, or that changes a pad without a low period, proves
     * nothing. Arm A is the retail configuration; B and C each change
     * exactly one thing. */
    static const bt_arm_t arms[] = {
        { "A normal",   false, false },
        { "B autobaud", true,  false },
        { "C ph5-keep", false, true  },
    };
    /* Only the first arm draws its full detail on the LCD. Each arm prints
     * about a dozen rows, so a radio that stays silent runs all three and
     * pushes this page past the 45 rows the panel holds - and it wraps to
     * the top rather than scrolling, overwriting its own header exactly when
     * the output matters most. The retries still log in full to UART and to
     * the SD report; _log_uart_only suppresses the LCD sink alone. */
    for (u32 a = 0; a < BT_CFG_ARMS && a < ARRAY_SIZE(arms) && !ok; a++) {
        bool lcd_off = _log_uart_only;
        if (a)
            _log_uart_only = true;
        ok = bt_attempt(&arms[a], ev, sizeof(ev), &ev_n);
        _log_uart_only = lcd_off;
        if (a && !ok)
            log_color(COL_WARN, "  arm %s      : no HCI reply (detail on UART)\n",
                      arms[a].name);
    }

#if BT_CFG_MCR_SWEEP
    /* The Tegra MCR RTS-bit polarity is genuinely self-contradictory in this
     * codebase: uart_recv() clears UART_MCR_RTS while it reads (uart.c:101),
     * implying set = blocked, while the TRM field name is FORCE_RTS_LOW.
     * Sweep rather than reason. The strap is already latched, so no further
     * power cycle is needed between values. */
    if (!ok) {
        static const u8 mcrs[] = { 0x22, 0x60, 0x40, 0x00 };
        /* LCD-suppressed for the same reason as the arms above: this only
         * runs when the radio is already silent, i.e. when the page is
         * already at risk of wrapping. Full sweep still goes to UART. */
        bool lcd_off = _log_uart_only;
        _log_uart_only = true;
        log_color(COL_INFO, "  -- MCR sweep (no re-POR needed) --\n");
        for (u32 i = 0; i < ARRAY_SIZE(mcrs) && !ok; i++) {
            u8 raw[32]; u32 raw_n = 0;
            bt_flush();
            uart_set_mode(BT_UART, mcrs[i]);
            usleep(500);
            if (!bt_send(bt_hci_reset, sizeof(bt_hci_reset))) {
                LOG("  MCR=%02X        : TX gated, MSR=%02X\n",
                    mcrs[i], u->UART_MSR & 0xFF);
                continue;
            }
            ev_n = bt_wait_event(ev, sizeof(ev), BT_CFG_HCI_MS,
                                 raw, sizeof(raw), &raw_n);
            /* Judge on a parsed Command Complete, never on "a byte arrived" -
             * a single stray BCM_NULL 0x00 would otherwise win the sweep. */
            ok = bt_is_reset_cc(ev, ev_n);
            LOG("  MCR=%02X        : ev=%d raw=%d MSR=%02X %s\n",
                mcrs[i], ev_n, raw_n, u->UART_MSR & 0xFF, ok ? "CC!" : "-");
        }
        _log_uart_only = lcd_off;
        log_color(ok ? COL_OK : COL_WARN,
            "  MCR sweep     : %s\n",
            ok ? "answered on a swept MCR" : "no reply on any MCR value");
    }
#endif

    /* Put WL_REG_ON back where the vendor GPIO hog leaves it. This probe is
     * one of many in the sweep and the WLAN core has to come back. */
    PINMUX_AUX(PMX_PH1_WL_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_1, 1);
    GP_MWR(GPH_MCNF, GPIO_PIN_1, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_1, 1);
    LOG("  PH1 restored  : %d (WL_REG_ON back high)\n",
        gpio_read(GPIO_PORT_H, GPIO_PIN_1));

    /* Rails back as found. The CFG byte carries the power mode and the
     * voltage code together, so one write restores both. Done after the HCI
     * attempts rather than before, since the radio needs them up for the
     * whole exchange. */
    for (u32 i = 0; i < ARRAY_SIZE(rails); i++)
        i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].cfg, rail_saved[i]);
    LOG("  Rails restored: ");
    for (u32 i = 0; i < ARRAY_SIZE(rails); i++)
        LOG("%s=%d ", rails[i].name,
            i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].cfg) >> 6);
    LOG("\n");

    if (!ok) {
        /* Silence is a real finding, but only once the loopback has
         * shown the host path works. The sequence is validated
         * against known-good units, which answer with a valid Command
         * Complete and identify as Broadcom. Every arm here runs at 115200 and the
         * rate is never swept, so what a quiet unit demonstrates is
         * silence at 115200: a part that came up at some other baud
         * looks the same from here, and that case is untested rather
         * than excluded. Even so, a unit that stays quiet through every
         * arm and every MCR variant while its own controller loops back
         * cleanly is not a probe that failed. When the loopback itself
         * fails the fault is on the host side of the pads, so nothing is
         * recorded against the radio. */
        if (uart_ok) {
            log_color(COL_ERR,
                "  Result        : radio does NOT respond - not running\n");
            LOG("  (PRODINFO calibration is judged separately; if that is\n");
            LOG("   intact, this is the module, its supply or its solder)\n");
            dx_set("bt_hci", DX_FAIL, "no HCI response, radio not running");
        } else {
            LOG("  Result        : inconclusive - UART-D loopback failed\n");
            LOG("  (controller-side fault, says nothing about the radio)\n");
        }
        return;
    }

    /* "100 msec delay for module to complete reset process" - the Command
     * Complete arrives before the part is actually ready (btbcm.c:285). */
    msleep_poll(100);

    /* Read_Local_Version_Information, opcode 0x1001. Return parameters are
     * status(1) hci_ver(1) hci_rev(2) lmp_ver(1) manufacturer(2)
     * lmp_subver(2) = 9 bytes, so plen is 0x0C and the event is 15 bytes
     * with the H4 type included:
     *   [0]04 [1]0E [2]0C [3]ncmd [4]01 [5]10 [6]status [7]hci_ver
     *   [8..9]hci_rev [10]lmp_ver [11..12]manufacturer [13..14]lmp_subver
     * Reading manufacturer at [10..11] is one byte early: it yields
     * lmp_ver | (manuf_lo << 8) = 0x0F08 and reports "not Broadcom?" on a
     * perfectly good chip.
     *
     * Purely informational: Linux's `.manufacturer = 15` is a static field
     * the driver sets on hdev, not a value it checks, and btbcm's subver
     * lookup just picks a display name. Horizon's own patchram config string
     * is "BCM4356A3", so 0x230F (A2) is not even the expected subver here.
     * Nothing below can downgrade the verdict. */
    {
        u8 raw[32]; u32 raw_n = 0;
        static const u8 hci_ver[] = { 0x01, 0x01, 0x10, 0x00 };
        bt_flush();
        bt_send(hci_ver, sizeof(hci_ver));
        ev_n = bt_wait_event(ev, sizeof(ev), 2000, raw, sizeof(raw), &raw_n);
        if (ev_n >= 15 && ev[1] == 0x0E && ev[6] == 0x00) {
            u16 manuf = (u16)ev[11] | ((u16)ev[12] << 8);
            u16 subv  = (u16)ev[13] | ((u16)ev[14] << 8);
            LOG("  HCI version   : %d  LMP version: %d\n", ev[7], ev[10]);
            LOG("  Manufacturer  : %04X (%s), lmp_subver %04X\n", manuf,
                manuf == 0x000F ? "Broadcom" : "unknown", subv);
        } else {
            LOG("  Local version : no usable reply (%d bytes)\n", ev_n);
        }
    }

    log_color(COL_OK,
        "  Result        : radio is powered, running and speaking HCI\n");
    LOG("  (so a Wi-Fi fault is the PCIe link, WLAN core or antenna)\n");
    dx_set("bt_hci", DX_PASS, "");
}

