/*
 * The part of the PCIe bring-up that has to run on a CPU-complex master.
 *
 * The BPMP provably cannot reach the PCIe apertures (see cpu_mbox.h), so
 * this stub runs the MSELECT/AFI half of the probe on CPU0 while the BPMP
 * supervises through the mailbox: it enables the apertures, brings up the
 * controller, trains the link on root port 1 (the WLAN x1 link), and runs
 * the endpoint configuration enumeration - the proof that the CYW4356
 * front-end is alive and on the bus. The die-level ChipCommon ChipID is
 * firmware-gated in the chip's ROM phase (the backplane window only maps
 * once firmware runs), so the endpoint's VID/DID/class answers are the
 * verdict.
 *
 * Rules for anything added here:
 *   - Store the breadcrumb BEFORE the access it describes. If the access
 *     never returns, that store is the only evidence anyone gets.
 *   - Bump `heartbeat` on every step. The BPMP distinguishes "stuck" from
 *     "slow" by watching it move, not by guessing a timeout.
 *   - No loops without a bound. A stall here costs the CPU cluster, which is
 *     recoverable, but a spin costs the whole run.
 */

#include "../cpu_mbox.h"

/* The assembly fault handler addresses these by hand-written byte offset,
 * because it cannot see the struct. Prove the two agree at build time. */
#define MBOX_OFFSET_OF(f) __builtin_offsetof(cpu_mbox_t, f)
_Static_assert(MBOX_OFFSET_OF(stage)     == MBOX_OFF_STAGE,     "mbox stage");
_Static_assert(MBOX_OFFSET_OF(exc_taken) == MBOX_OFF_EXC_TAKEN, "mbox exc_taken");
_Static_assert(MBOX_OFFSET_OF(exc_esr)   == MBOX_OFF_EXC_ESR,   "mbox exc_esr");
_Static_assert(MBOX_OFFSET_OF(exc_far)   == MBOX_OFF_EXC_FAR,   "mbox exc_far");
_Static_assert(MBOX_OFFSET_OF(exc_elr)   == MBOX_OFF_EXC_ELR,   "mbox exc_elr");
_Static_assert(MBOX_OFFSET_OF(exc_stage) == MBOX_OFF_EXC_STAGE, "mbox exc_stage");

/* Same addresses main.c uses. Repeated rather than shared because bdk's
 * headers are not buildable by the AArch64 toolchain, and a four-line
 * duplicate is cheaper than making them so. */
#define MSELECT_BASE     0x50060000u
#define MSELECT_CONFIG   0x00u
/*  Real bit layout, from the Tegra X1 TRM section 16.3.1 (reset 0x07ff4020).
 *  Note bit 27 is WRAP_TO_INCR_SLAVE0 (APC) - hekate's header calls it
 *  WRAP_TO_INCR_BPMP, which is a misnomer. */
#define MSELECT_ERR_RESP_EN_PCIE   (1u << 24)
#define MSELECT_ENABLE_PCIE_APER   (1u << 5)

#define PCIE_AFI_BASE    0x01003800u
#define AFI_PCIE_CONFIG  0xF8u
/* Witness register for "is this aperture live". AFI_AXI_BAR5_START is a
 * plain read/write address register for a BAR the driver NULLs out anyway,
 * so a write-read-restore here proves the aperture without disturbing
 * anything that matters.
 *
 * The witness has to be a genuinely writable register: bit 3 of
 * AFI_PCIE_CONFIG is the disable bit for a THIRD root port this SoC does
 * not have, and never changes when written. Reading that register returns
 * 0x00103025 - a perfectly good value, with xbar = X4_X1 in bits 23:20 -
 * so an unwritable witness makes a live aperture look dead while the read
 * beside it looks entirely plausible. */
#define AFI_AXI_BAR5_START 0x2Cu
#define AFI_WITNESS_PAT    0xA5A50000u

#define PCIE_PADS_BASE   0x01003000u
#define PCIE_RP1_BASE    0x01001000u      /* root port 1 = the WLAN x1 link */
#define PCIE_RP0_BASE    0x01000000u      /* root port 0 = the x4 bundle    */
/* Configuration aperture: the device tree's `cs` window at 0x02000000,
 * exactly as U-Boot's tegra_pcie_setup_translations() programs it (AXI_BAR0
 * = cs.start, FPCI_BAR0 = 0xFE100000). This is the configuration the
 * endpoint answers on. Relocating the window inside the A1 aperture with
 * FPCI_BAR0 at 0xFE100100 still trains the link, but the endpoint never
 * answers a configuration read through it. */
#define PCIE_CS_BASE     0x02000000u
#define PCIE_CS_SIZE     0x10000000u

/* Type-1 config address for bus 1, device 0, function 0. */
#define EP_CFG(reg)      (PCIE_CS_BASE + (1u << 16) + ((reg) & 0xFFCu))

#define AFI_AXI_BAR0_SZ      0x00u
#define AFI_AXI_BAR0_START   0x18u
#define AFI_FPCI_BAR0        0x30u
#define AFI_MSI_BAR_SZ       0x60u
#define AFI_MSI_FPCI_BAR_ST  0x64u
#define AFI_MSI_AXI_BAR_ST   0x68u
#define AFI_CONFIGURATION    0xACu
#define  AFI_CONFIGURATION_EN_FPCI (1u << 0)
#define AFI_FPCI_ERROR_MASKS 0xB0u
#define AFI_INTR_MASK        0xB4u
#define AFI_INTR_CODE         0xB8u
#define AFI_INTR_SIGNATURE    0xBCu
#define AFI_SM_INTR_ENABLE   0xC4u
#define AFI_AFI_INTR_ENABLE  0xC8u
#define  AFI_AFI_INTR_EN_FPCI_TIMEOUT (1u << 7)
#define  AFI_PCIE_CONFIG_DISABLE(x)  (1u << ((x) + 1))
#define  AFI_PCIE_CONFIG_DISABLE_ALL 0xEu
#define  AFI_PCIE_CONFIG_XBAR_MASK   (0xFu << 20)
#define  AFI_PCIE_CONFIG_XBAR_X4_X1  (0x1u << 20)
#define AFI_FUSE             0x104u
#define  AFI_FUSE_PCIE_T0_GEN2_DIS   (1u << 2)
#define AFI_PEX0_CTRL        0x110u
#define AFI_PEX1_CTRL        0x118u
#define  AFI_PEX_CTRL_RST         (1u << 0)
#define  AFI_PEX_CTRL_CLKREQ_EN   (1u << 1)
#define  AFI_PEX_CTRL_REFCLK_EN   (1u << 3)
#define  AFI_PEX_CTRL_OVERRIDE_EN (1u << 4)
#define AFI_PLLE_CONTROL     0x160u
#define  AFI_PLLE_CONTROL_PADS2PLLE_EN     (1u << 1)
#define AFI_PEXBIAS_CTRL     0x168u

/* FPCI completion timeout (TRM 34.5.17, 0xD8). Reset 0 = disabled.
 * Writing a non-zero value arms it; a hung downstream read then returns
 * rather than stalling the fabric forever. */
#define AFI_FPCI_TIMEOUT          0xD8u
#define  AFI_FPCI_TO_VAL          0x0000FFFFu
#define AFI_REQ_PENDING           0xF4u
#define  AFI_REQ_PENDING_TIMEOUT_EN  (1u << 31)

/* 0=off; >0=us gap between paired config reads. */
#define CFG_READ_DELAY_US         1000u
#if CFG_READ_DELAY_US > 0
#define CFG_DELAY() udelay(CFG_READ_DELAY_US)
#else
#define CFG_DELAY() ((void)0)
#endif

#define PADS_REFCLK_CFG0      0xC8u
#define PADS_REFCLK_CFG0_T210 0x90B890B8u

#define RP_COMMAND               0x004u
#define  RP_COMMAND_MEM          (1u << 1)
#define  RP_COMMAND_BME          (1u << 2)
#define RP_BUS_NUMBERS           0x018u
#define RP_ROOT_CTRL             0x09Cu
#define  RP_ROOT_CTRL_CRS_SW_VIS (1u << 4)
#define RP_LINK_CONTROL_STATUS   0x090u
#define  RP_LINK_DL_ACTIVE       (1u << 29)
#define RP_VEND_XP               0xF00u
#define  RP_VEND_XP_DL_UP        (1u << 30)
#define RP_VEND_CTL2             0xFA8u
#define  RP_VEND_CTL2_PCA_ENABLE (1u << 7)
#define RP_PRIV_MISC             0xFE0u
#define  RP_PRIV_MISC_PRSNT_MAP_EP_PRSNT (0xEu << 0)
#define  RP_PRIV_MISC_PRSNT_MAP_EP_ABSNT (0xFu << 0)

#define BRCM_BAR0_WINDOW   0x80u

/* Tegra's free-running 1 MHz counter, the same one the BPMP times with, so
 * the datasheet waits below mean the same thing on both sides. Reached from
 * here through MSELECT's APC port like any other APB register. */
#define TIMERUS_CNTR_1US 0x60005010u

/* Clock and reset controller. Reachable from CPU0 through MSelect's APC
 * port, same as the timer above. */
#define BIT(n)              (1u << (n))
#define CAR_BASE            0x60006000u
#define CAR_RST_DEVICES_U   0x00Cu
#define CAR_RST_DEV_U_CLR   0x314u
#define CLK_U_PCIEXCLK      10u
#define AFI_CACHE_BAR0_SZ   0x48u
#define AFI_CACHE_BAR0_ST   0x4Cu
#define AFI_CACHE_BAR1_SZ   0x50u
#define AFI_CACHE_BAR1_ST   0x54u

static void udelay(unsigned int us)
{
    unsigned int start = *(volatile unsigned int *)TIMERUS_CNTR_1US;
    while ((*(volatile unsigned int *)TIMERUS_CNTR_1US - start) < us)
        ;
}

static inline unsigned int rd32(unsigned long a)
{
    return *(volatile unsigned int *)a;
}

/* Mailbox store with cache push. With SCTLR.C on, the stub's DRAM stores
 * live in the D-cache; the supervisor on the BPMP only sees DRAM, so every
 * word it reads must be cleaned to the point of coherence first. */
static inline void mbox_w(volatile unsigned int *p, unsigned int v)
{
    *p = v;
    __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
}

static inline void wr32(unsigned long a, unsigned int v)
{
    *(volatile unsigned int *)a = v;
}

/* ---- PHY bring-up executed HERE, on CPU0, exactly as U-Boot's driver
 * runs it: the A57 runs the same instruction stream at the same pace as
 * U-Boot's working enumeration. */
#define XUSB_PADCTL_BASE 0x7009F000u
#define PADCTL_UPHY_PLL_P0_CTL1 0x360u
#define PADCTL_UPHY_PLL_P0_CTL2 0x364u
#define PADCTL_UPHY_PLL_P0_CTL4 0x36Cu
#define PADCTL_UPHY_PLL_P0_CTL5 0x370u
#define PADCTL_UPHY_PLL_P0_CTL8 0x37Cu
#define PADCTL_UPHY_PLL_P0_CTL10 0x384u
#define  UPHY_CTL1_PWR_OVRD      BIT(4)
#define  UPHY_CTL1_ENABLE        BIT(3)
#define  UPHY_CTL1_SLEEP_MSK     (0x3u << 1)
#define  UPHY_CTL1_IDDQ          BIT(0)
#define  UPHY_CTL1_FREQ_NDIV_MSK (0xFFu << 20)
#define  UPHY_CTL1_FREQ_MDIV_MSK (0x3u << 16)
#define  UPHY_CTL1_LOCKDET       BIT(15)
#define  UPHY_CTL2_CAL_CTRL_MSK  (0xFFFFFFu << 4)
#define  UPHY_CTL2_CAL_OVRD      BIT(2)
#define  UPHY_CTL2_CAL_DONE      BIT(1)
#define  UPHY_CTL2_CAL_EN        BIT(0)
#define  UPHY_CTL4_TXCLKREF_EN   BIT(15)
#define  UPHY_CTL4_TXCLKREF_MSK  (0x3u << 12)
#define  UPHY_CTL4_REFCLK_SEL_MSK (0xFu << 4)
#define  UPHY_CTL4_REFCLKBUF_EN  BIT(8)
#define  UPHY_CTL5_DCO_CTRL_MSK  (0xFFu << 16)
#define  UPHY_CTL8_RCAL_OVRD     BIT(15)
#define  UPHY_CTL8_RCAL_CLK_EN   BIT(13)
#define  UPHY_CTL8_RCAL_EN       BIT(12)
#define  UPHY_CTL8_RCAL_DONE     BIT(31)
#define  UPHY_CFG_ADDR(x)        (((x) & 0xFFu) << 16)
#define  UPHY_CFG_WDATA(x)       ((x) & 0xFFFFu)
/* The CTL10 indirect port's strobe bits: BIT(27) reset / BIT(24) write
 * strobe, same protocol as the lane tables through PX_CTL8. With the
 * wrong strobe bits the Mariko brick config never loads and the cal FSM
 * never asserts CAL_DONE. */
#define  UPHY_CFG_RESET          BIT(27)
#define  UPHY_CFG_WS             BIT(24)

#define CAR_PLLREFE_BASE  0x4C4u
#define CAR_PLLREFE_MISC  0x4C8u
#define  PLLREFE_MISC_LOCK  BIT(27)
#define  PLLREFE_MISC_IDDQ  BIT(24)
#define CAR_PLLE_BASE     0x0E8u
#define  PLLE_BASE_ENABLE      BIT(31)
#define CAR_PLLE_MISC     0x0ECu
#define  PLLE_MISC_LOCK         BIT(11)
#define  PLLE_MISC_IDDQ_OVERRIDE BIT(13)
#define  PLLE_MISC_IDDQ_SW_CTRL BIT(14)
#define CAR_PLLE_AUX      0x48Cu
#define  PLLE_AUX_SEQ_ENABLE     BIT(24)
/* CLK_SOURCE_PCIE: the PCIe refclk mux. U-Boot's tegra_plle_enable switches
 * it to the PLLE (0x23010021) after the lock, in two glitchless steps.
 * U-Boot's probe leaves the spread-spectrum registers untouched, so no SSC
 * programming happens here either. */
#define CAR_CLK_SRC_PCIE  0x068u
#define CAR_XUSBIO_PLL_CFG0 0x51Cu
#define  XUSBIO_SEQ_ENABLE         BIT(24)
#define  XUSBIO_PADPLL_SLEEP_IDDQ  BIT(13)
#define  XUSBIO_PADPLL_USE_LOCKDET BIT(6)
#define  XUSBIO_CLK_ENABLE_SWCTL   BIT(2)
#define  XUSBIO_PADPLL_RESET_SWCTL BIT(0)

static int wait_bit(unsigned long reg, unsigned int bit, unsigned int on,
                    unsigned int ms)
{
    unsigned int start = *(volatile unsigned int *)TIMERUS_CNTR_1US;
    do {
        unsigned int v = rd32(reg);
        if (on && (v & bit))
            return 1;
        if (!on && !(v & bit))
            return 1;
        udelay(1000);
    } while ((*(volatile unsigned int *)TIMERUS_CNTR_1US - start) < ms * 1000u);
    return 0;
}

/* PLLREFE + PLLE, byte-identical port of U-Boot's tegra_plle_enable()
 * (tegra210/clock.c) - the 100 MHz low-jitter PCIe reference clock,
 * including the CLK_SOURCE_PCIE mux switch and the PLLE_AUX tail. Every
 * mask and constant is U-Boot's. */
static int stub_plle_enable(void)
{
    unsigned int v;

    wr32(CAR_BASE + CAR_PLLREFE_MISC,
         rd32(CAR_BASE + CAR_PLLREFE_MISC) & ~PLLREFE_MISC_IDDQ);
    udelay(5);
    wr32(CAR_BASE + CAR_PLLREFE_BASE, 0x40002804u);
    if (!wait_bit(CAR_BASE + CAR_PLLREFE_MISC, PLLREFE_MISC_LOCK, 1, 250))
        return 0;

    /* XTAL as the PLLE reference. */
    wr32(CAR_BASE + CAR_PLLE_AUX, rd32(CAR_BASE + CAR_PLLE_AUX) & ~BIT(28));
    wr32(CAR_BASE + CAR_PLLE_MISC,
         rd32(CAR_BASE + CAR_PLLE_MISC) & ~PLLE_MISC_IDDQ_OVERRIDE);
    udelay(5);

    /* 100 MHz, low jitter. */
    wr32(CAR_BASE + CAR_PLLE_BASE,
         (rd32(CAR_BASE + CAR_PLLE_BASE) & 0xE0FF0000u) | 0x0E007D02u);
    wr32(CAR_BASE + CAR_PLLE_MISC,
         (rd32(CAR_BASE + CAR_PLLE_MISC) & 0xFFFFFE32u) | 0x100u);

    wr32(CAR_BASE + CAR_PLLE_BASE,
         rd32(CAR_BASE + CAR_PLLE_BASE) | PLLE_BASE_ENABLE);
    if (!wait_bit(CAR_BASE + CAR_PLLE_MISC, PLLE_MISC_LOCK, 1, 250))
        return 0;

    /* The PCIe refclk source mux: PLLE, glitchless two-step switch. */
    wr32(CAR_BASE + CAR_CLK_SRC_PCIE,
         (rd32(CAR_BASE + CAR_CLK_SRC_PCIE) & 0xC0002A00u) | 0x23010021u);
    udelay(1);
    wr32(CAR_BASE + CAR_CLK_SRC_PCIE,
         (rd32(CAR_BASE + CAR_CLK_SRC_PCIE) & 0xC0002200u) | 0x23010021u);

    /* Hand PLLE to its hardware sequencer, like Linux/U-Boot. */
    wr32(CAR_BASE + CAR_PLLE_MISC,
         rd32(CAR_BASE + CAR_PLLE_MISC) & ~PLLE_MISC_IDDQ_SW_CTRL);
    v = rd32(CAR_BASE + CAR_PLLE_AUX);
    v = (v & 0xFFFFFFAFu) | 0x80000008u;
    wr32(CAR_BASE + CAR_PLLE_AUX, v);
    udelay(1);
    wr32(CAR_BASE + CAR_PLLE_AUX, v | PLLE_AUX_SEQ_ENABLE);
    return 1;
}

/* The UPHY PLL P0 cal, ported from the probe's pcie_uphy_enable() -
 * U-Boot's pcie_phy_enable sequence, executed on the A57. */
static int stub_uphy_enable(void)
{
    unsigned int v;

    /* T210 vs T210B01 configure the brick differently. */
    unsigned int hidrev = *(volatile unsigned int *)(0x70000000u + 0x804u);
    if (((hidrev >> 4) & 0xF) == 2) {
        static const struct { unsigned char addr; unsigned short data; }
        pll_g1[] = {
            { 0x02, 0x0000 }, { 0x03, 0x7051 },
            { 0x25, 0x0130 }, { 0x1E, 0x0017 },
        };
        for (unsigned int i = 0; i < 4; i++)
            wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL10,
                 UPHY_CFG_ADDR(pll_g1[i].addr) |
                 UPHY_CFG_WDATA(pll_g1[i].data) | UPHY_CFG_RESET | UPHY_CFG_WS);
    } else {
        v = rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2);
        v = (v & ~UPHY_CTL2_CAL_CTRL_MSK) | (0x136u << 4);
        wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2, v);

        v = rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL5);
        v = (v & ~UPHY_CTL5_DCO_CTRL_MSK) | (0x2Au << 16);
        wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL5, v);
    }

    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1) | UPHY_CTL1_PWR_OVRD);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2) | UPHY_CTL2_CAL_OVRD);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8) | UPHY_CTL8_RCAL_OVRD);

    v = rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL4);
    v &= ~(UPHY_CTL4_TXCLKREF_MSK | UPHY_CTL4_REFCLK_SEL_MSK);
    v |= (2u << 12) | UPHY_CTL4_TXCLKREF_EN;
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL4, v);

    /* U-Boot's pcie_phy_enable CTL1 programming between the CTL4 write
     * and the cal: power-down latch config, then the IDDQ override
     * clears - byte-identical masks. */
    v = rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1);
    v = (v & 0xF00CFFFFu) | 0x01900000u;
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1, v);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1) & ~1u);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1) & ~6u);

    v = rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1);
    v &= ~(UPHY_CTL1_FREQ_MDIV_MSK | UPHY_CTL1_FREQ_NDIV_MSK);
    v |= 25u << 20;
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1, v);

    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1) & ~UPHY_CTL1_IDDQ);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1) & ~UPHY_CTL1_SLEEP_MSK);
    udelay(1);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL4,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL4) | UPHY_CTL4_REFCLKBUF_EN);

    /* Frequency calibration: raise CAL_EN, wait CAL_DONE, lower, wait
     * CAL_DONE clear. */
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2) | UPHY_CTL2_CAL_EN);
    if (!wait_bit(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2,
                  UPHY_CTL2_CAL_DONE, 1, 250))
        return 0;
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2) & ~UPHY_CTL2_CAL_EN);
    if (!wait_bit(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2,
                  UPHY_CTL2_CAL_DONE, 0, 250))
        return 0;

    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1) | UPHY_CTL1_ENABLE);
    if (!wait_bit(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
                  UPHY_CTL1_LOCKDET, 1, 250))
        return 0;

    /* Resistor calibration. */
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8) |
         UPHY_CTL8_RCAL_CLK_EN | UPHY_CTL8_RCAL_EN);
    if (!wait_bit(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8,
                  UPHY_CTL8_RCAL_DONE, 1, 250))
        return 0;
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8) & ~UPHY_CTL8_RCAL_EN);
    if (!wait_bit(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8,
                  UPHY_CTL8_RCAL_DONE, 0, 250))
        return 0;
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8) & ~UPHY_CTL8_RCAL_CLK_EN);

    /* U-Boot's pcie_phy_enable tail: release the overrides, hand the
     * PLL to the hardware sequencer. */
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL1) & ~UPHY_CTL1_PWR_OVRD);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL2) & ~UPHY_CTL2_CAL_OVRD);
    wr32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8,
         rd32(XUSB_PADCTL_BASE + PADCTL_UPHY_PLL_P0_CTL8) & ~UPHY_CTL8_RCAL_OVRD);
    udelay(1);

    {
        unsigned int xio = rd32(CAR_BASE + CAR_XUSBIO_PLL_CFG0);
        xio &= ~XUSBIO_PADPLL_RESET_SWCTL;
        xio &= ~XUSBIO_CLK_ENABLE_SWCTL;
        xio |= XUSBIO_PADPLL_USE_LOCKDET;
        xio |= XUSBIO_PADPLL_SLEEP_IDDQ;
        wr32(CAR_BASE + CAR_XUSBIO_PLL_CFG0, xio);
        wr32(CAR_BASE + CAR_XUSBIO_PLL_CFG0, xio | XUSBIO_SEQ_ENABLE);
    }

    /* The x1 lane out of IDDQ, now that the PLL is up. */
    wr32(XUSB_PADCTL_BASE + 0x028u,
         rd32(XUSB_PADCTL_BASE + 0x028u) | BIT(1));
    return 1;
}

static void step(cpu_mbox_t *m, unsigned int s)
{
    mbox_w(&m->stage, s);
    mbox_w(&m->heartbeat, m->heartbeat + 1);
    /* The breadcrumb is worthless if it can sit in a store buffer behind
     * the access it is supposed to precede: order it explicitly. */
    __asm__ volatile("dsb sy" ::: "memory");
}

#define RP_SEC_STATUS  0x01Cu   /* secondary status: RMA/RTA for downstream */

/* Write v to an endpoint config register, then verify with a pair of
 * readbacks per round, rewriting when neither agrees. The compare is
 * masked: a BAR owns its low flag bits, and the command register shares
 * its dword with status bits the endpoint drives.
 *
 * Rounds are spaced 20 ms: the endpoint accepts the command write
 * immediately but silently drops BAR0 and backplane window writes early
 * in its boot, so the retry spans the rest of its firmware init. Returns
 * the rounds used (0 = first try), also bounded because a register that
 * never verifies must not spin the CPU cluster. */
static unsigned int cfg_wr_verify(cpu_mbox_t *m, unsigned long addr,
                                  unsigned int v, unsigned int mask,
                                  volatile unsigned int *ra,
                                  volatile unsigned int *rb)
{
    unsigned int round = 0;

    for (round = 0; round < 25; round++) {
        wr32(addr, v);
        mbox_w(ra, rd32(addr));
        mbox_w(rb, rd32(addr));
        if ((*ra & mask) == (v & mask) || (*rb & mask) == (v & mask))
            break;
        /* The wait is for the endpoint's firmware init, and it can add up
         * to 500 ms per register - keep the supervisor's heartbeat moving
         * or it reads the wait as a wedge. */
        mbox_w(&m->heartbeat, m->heartbeat + 1);
        __asm__ volatile("dsb sy" ::: "memory");
        udelay(20000);
    }
    return round;
}

void cpu_stub_main(void)
{
    cpu_mbox_t *m = (cpu_mbox_t *)(unsigned long)CPU_MBOX_ADDR;

    mbox_w(&m->magic, CPU_MBOX_MAGIC);
    step(m, CPU_STAGE_ENTRY);

    /* The PHY bring-up, executed HERE in U-Boot's order: PLLE ->
     * PADS2PLLE -> UPHY cal -> sequencers. This is the ported
     * pcie_plle_enable/pcie_uphy_enable; the A57 runs the same
     * instruction stream U-Boot's driver runs. */
    {
        /* RP0 accessibility map: probe 0 = before the PHY bring-up. */
        mbox_w(&m->raw_cfg[5], rd32(PCIE_RP0_BASE + 0x00u));
        int plle_ok = stub_plle_enable();
        wr32(PCIE_AFI_BASE + AFI_PLLE_CONTROL,
             rd32(PCIE_AFI_BASE + AFI_PLLE_CONTROL) |
             AFI_PLLE_CONTROL_PADS2PLLE_EN);
        int uphy_ok = stub_uphy_enable();
        mbox_w(&m->raw_cfg[19],
               (plle_ok ? 0u : 1u) | (uphy_ok ? 0u : 2u));
        /* Probe 1 = after the PHY bring-up. */
        mbox_w(&m->raw_cfg[6], rd32(PCIE_RP0_BASE + 0x00u));
    }

    /* MSELECT, the block the BPMP reads as a constant at every offset. If a
     * CPU-complex master sees the TRM's 0x07ff4020 reset value here instead,
     * that alone settles the architecture question - which is why the value
     * is captured rather than assumed. */
    step(m, CPU_STAGE_MSELECT_RD);
    mbox_w(&m->mselect_cfg, rd32(MSELECT_BASE + MSELECT_CONFIG));

    step(m, CPU_STAGE_MSELECT_WR);
    /* Only the bits the reset value lacks: aperture enable and error
     * response are already set in the TRM reset value 0x07FF4020, and
     * WRAP_TO_INCR is deliberately NOT written - U-Boot's enumeration
     * works with MSELECT_CONFIG left exactly at its reset value. */
    wr32(MSELECT_BASE + MSELECT_CONFIG,
         m->mselect_cfg | MSELECT_ERR_RESP_EN_PCIE |
         MSELECT_ENABLE_PCIE_APER);
    mbox_w(&m->mselect_cfg2, rd32(MSELECT_BASE + MSELECT_CONFIG));

    /* The AFI window. The BPMP gets 0xFFFFFFFF here no matter what it does
     * first; a real value, and a write that reads back, is proof the
     * aperture is being serviced for this master. */
    step(m, CPU_STAGE_AFI_RD);
    mbox_w(&m->afi_probe0, rd32(PCIE_AFI_BASE + AFI_PCIE_CONFIG));

    step(m, CPU_STAGE_AFI_WR);
    {
        unsigned int saved = rd32(PCIE_AFI_BASE + AFI_AXI_BAR5_START);
        wr32(PCIE_AFI_BASE + AFI_AXI_BAR5_START, AFI_WITNESS_PAT);
        mbox_w(&m->afi_probe1, rd32(PCIE_AFI_BASE + AFI_AXI_BAR5_START));
        wr32(PCIE_AFI_BASE + AFI_AXI_BAR5_START, saved);
    }

    /* Everything past here needs the aperture to be real. If the witness
     * write did not read back there is nothing to talk to, and pressing on
     * would only produce confident nonsense. */
    if (m->afi_probe0 == 0xFFFFFFFFu || m->afi_probe1 != AFI_WITNESS_PAT) {
        mbox_w(&m->port, ~0u);
        step(m, CPU_STAGE_DONE);
        mbox_w(&m->done, CPU_MBOX_DONE);
        __asm__ volatile("dsb sy" ::: "memory");
        for (;;)
            __asm__ volatile("wfe");
    }

    /* ---- AFI address translations ------------------------------------
     * BAR0 is the type-1 config aperture; without it no configuration read
     * downstream of the root port decodes. The memory windows are what make
     * the ChipCommon read at the end possible. Values from tegra210.dtsi's
     * pcie@1003000 ranges, except FPCI_BAR0 which is set to the endpoint's
     * known default BAR0 base (0xFF9F8000) so memory reads hit the TCM
     * without needing to reconfigure BAR0 (BAR0 writes don't stick in ROM
     * phase). */
    step(m, CPU_STAGE_XLATE);
    /* Configuration aperture: the DTS's 256 MiB cs window, exactly as
     * U-Boot's tegra_pcie_setup_translations() leaves it (AXI_BAR0 =
     * 0x02000000, size 0x10000 pages, FPCI_BAR0 = 0xFE100000). Read back
     * below because a window that silently did not take the write decodes
     * nothing. */
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_START, PCIE_CS_BASE);
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_SZ,    PCIE_CS_SIZE >> 12);
    wr32(PCIE_AFI_BASE + AFI_FPCI_BAR0,      0xFE100000u);
    mbox_w(&m->bar0_start, rd32(PCIE_AFI_BASE + AFI_AXI_BAR0_START));
    mbox_w(&m->bar0_sz, rd32(PCIE_AFI_BASE + AFI_AXI_BAR0_SZ));
    mbox_w(&m->bar0_fpci, rd32(PCIE_AFI_BASE + AFI_FPCI_BAR0));

    /* U-Boot's tegra_pcie_setup_translations values for the downstream
     * windows: BAR1 = the IO range, BAR2 = the prefetchable memory,
     * BAR3 = the non-prefetchable memory (the endpoint's BAR0 region),
     * and the cache BAR mapping all upstream transactions uncached.
     * Captured from the working enumeration's AFI dump. */
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_START + 1 * 4, 0x12000000u);
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_SZ    + 1 * 4, 0x00000010u);
    wr32(PCIE_AFI_BASE + AFI_FPCI_BAR0      + 1 * 4, 0xFDFC0000u);
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_START + 2 * 4, 0x20000000u);
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_SZ    + 2 * 4, 0x00020000u);
    wr32(PCIE_AFI_BASE + AFI_FPCI_BAR0      + 2 * 4, 0x00200001u);
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_START + 3 * 4, 0x13000000u);
    wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_SZ    + 3 * 4, 0x0000D000u);
    wr32(PCIE_AFI_BASE + AFI_FPCI_BAR0      + 3 * 4, 0x00130001u);
    wr32(PCIE_AFI_BASE + AFI_CACHE_BAR0_ST, 0x80000000u);
    wr32(PCIE_AFI_BASE + AFI_CACHE_BAR0_SZ, 0);
    wr32(PCIE_AFI_BASE + AFI_CACHE_BAR1_ST, 0);
    wr32(PCIE_AFI_BASE + AFI_CACHE_BAR1_SZ, 0);

    /* NULL the two windows nobody uses. */
    for (unsigned int i = 4; i < 6; i++) {
        wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_START + i * 4, 0);
        wr32(PCIE_AFI_BASE + AFI_AXI_BAR0_SZ    + i * 4, 0);
        wr32(PCIE_AFI_BASE + AFI_FPCI_BAR0      + i * 4, 0);
    }

    wr32(PCIE_AFI_BASE + AFI_MSI_FPCI_BAR_ST, 0);
    wr32(PCIE_AFI_BASE + AFI_MSI_BAR_SZ,      0);
    wr32(PCIE_AFI_BASE + AFI_MSI_AXI_BAR_ST,  0);

    /* ---- controller enable -------------------------------------------- */
    step(m, CPU_STAGE_CTRL_EN);
    wr32(PCIE_AFI_BASE + AFI_PEXBIAS_CTRL, 0);

    {
        unsigned int v = rd32(PCIE_AFI_BASE + AFI_PCIE_CONFIG);
        v &= ~AFI_PCIE_CONFIG_XBAR_MASK;
        v |= AFI_PCIE_CONFIG_DISABLE_ALL | AFI_PCIE_CONFIG_XBAR_X4_X1;
        v &= ~AFI_PCIE_CONFIG_DISABLE(1);       /* port 1 = the WLAN x1 link */
        /* Keep controller 0 enabled, exactly like U-Boot's enable path:
         * it leaves AFI_PCIE_CONFIG at PCIEC0_DISABLE_DEVICE=0 and only
         * disables ports whose link failed at PEX_CTRL level. */
        v &= ~AFI_PCIE_CONFIG_DISABLE(0);
        wr32(PCIE_AFI_BASE + AFI_PCIE_CONFIG, v);
        mbox_w(&m->afi_pcie_cfg, rd32(PCIE_AFI_BASE + AFI_PCIE_CONFIG));
    }
    wr32(PCIE_AFI_BASE + AFI_FUSE,
         rd32(PCIE_AFI_BASE + AFI_FUSE) & ~AFI_FUSE_PCIE_T0_GEN2_DIS);

    /* PCIEXCLK out of reset, and it has to be HERE - after the cross-bar
     * config and before anything touches a root port window. The root ports
     * live behind this clock: with it asserted, reading a root port window
     * does not fault, it never completes - the stub stalls on that read,
     * and the stage breadcrumb is what identifies which one.
     *
     * The BPMP cannot do it: it bails into this handoff as soon as it finds
     * the AFI window unreadable, which is well before this point. CAR is
     * reachable from here anyway - CPU0 goes MSelect -> APC -> ARM7
     * crossbar -> the register bus - which is also why the udelay() below
     * can read TIMERUS. U-Boot's tegra_pcie_enable_controller releases it
     * at the same point, after the xbar and before EN_FPCI. */
    wr32(CAR_BASE + CAR_RST_DEV_U_CLR, 1u << CLK_U_PCIEXCLK);
    mbox_w(&m->car_rst_u, rd32(CAR_BASE + CAR_RST_DEVICES_U));

    wr32(PCIE_AFI_BASE + AFI_CONFIGURATION,
         rd32(PCIE_AFI_BASE + AFI_CONFIGURATION) | AFI_CONFIGURATION_EN_FPCI);
    mbox_w(&m->afi_cfg_en, rd32(PCIE_AFI_BASE + AFI_CONFIGURATION));

    /* Disable all AFI interrupts until the baseline config reads prove
     * the endpoint alive - U-Boot's tegra_pcie_enable_controller writes
     * exactly this, and consoles exist where the endpoint's first config
     * reads hang while the FPCI-timeout interrupt enable is set. What
     * gets armed again does so in the post-baseline block below. */
    wr32(PCIE_AFI_BASE + AFI_AFI_INTR_ENABLE,  0);
    wr32(PCIE_AFI_BASE + AFI_SM_INTR_ENABLE,   0);
    wr32(PCIE_AFI_BASE + AFI_INTR_MASK,        0);
    wr32(PCIE_AFI_BASE + AFI_FPCI_ERROR_MASKS, 0);

    /* U-Boot's tegra_pcie_enable probes port 0 (the x4 bundle) FIRST:
     * a full enable + link-training attempt (3 PERST# retries) + a
     * disable episode on the unconnected lanes, all before port 1.
     * The wait loops heartbeat so the supervisor sees progress. */
    {
        wr32(PCIE_AFI_BASE + AFI_PEX0_CTRL,
             rd32(PCIE_AFI_BASE + AFI_PEX0_CTRL) | AFI_PEX_CTRL_REFCLK_EN |
             AFI_PEX_CTRL_CLKREQ_EN | AFI_PEX_CTRL_OVERRIDE_EN);
        wr32(PCIE_AFI_BASE + AFI_PEX0_CTRL,
             rd32(PCIE_AFI_BASE + AFI_PEX0_CTRL) & ~AFI_PEX_CTRL_RST);
        udelay(2000);
        wr32(PCIE_AFI_BASE + AFI_PEX0_CTRL,
             rd32(PCIE_AFI_BASE + AFI_PEX0_CTRL) | AFI_PEX_CTRL_RST);
        wr32(PCIE_RP0_BASE + RP_VEND_CTL2,
             rd32(PCIE_RP0_BASE + RP_VEND_CTL2) | RP_VEND_CTL2_PCA_ENABLE);
        {
            unsigned int v = rd32(PCIE_RP0_BASE + RP_PRIV_MISC);
            v &= ~RP_PRIV_MISC_PRSNT_MAP_EP_ABSNT;
            v |= RP_PRIV_MISC_PRSNT_MAP_EP_PRSNT;
            wr32(PCIE_RP0_BASE + RP_PRIV_MISC, v);
        }
        for (int retry = 0; retry < 3; retry++) {
            int up = 0;
            for (int i = 0; i < 200 && !up; i++) {
                if (rd32(PCIE_RP0_BASE + RP_VEND_XP) & RP_VEND_XP_DL_UP)
                    up = 1;
                else {
                    step(m, CPU_STAGE_CTRL_EN);
                    udelay(2000);
                }
            }
            if (up)
                break;
            wr32(PCIE_AFI_BASE + AFI_PEX0_CTRL,
                 rd32(PCIE_AFI_BASE + AFI_PEX0_CTRL) & ~AFI_PEX_CTRL_RST);
            udelay(2000);
            wr32(PCIE_AFI_BASE + AFI_PEX0_CTRL,
                 rd32(PCIE_AFI_BASE + AFI_PEX0_CTRL) | AFI_PEX_CTRL_RST);
        }
        /* tegra_pcie_port_disable: assert the reset, kill the refclk. */
        wr32(PCIE_AFI_BASE + AFI_PEX0_CTRL,
             rd32(PCIE_AFI_BASE + AFI_PEX0_CTRL) & ~AFI_PEX_CTRL_RST);
        wr32(PCIE_AFI_BASE + AFI_PEX0_CTRL,
             rd32(PCIE_AFI_BASE + AFI_PEX0_CTRL) & ~AFI_PEX_CTRL_REFCLK_EN);
    }

    /* ---- root port 1: refclk, then PERST# ------------------------------
     * The two waits are the CYW4356's, not the Tegra's: Trefclkstable is
     * 10 ms from reference clock to PERST# release, and Tperst2firstconfig
     * is 6 ms from there to the first configuration access (datasheet Table
     * 62). Upstream drivers wait for neither, because a slot's clock is
     * running long before the driver loads; here the BPMP started it. */
    step(m, CPU_STAGE_PORT_EN);
    /* Probe 2 = after the controller enable, before the port enable. */
    mbox_w(&m->raw_cfg[7], rd32(PCIE_RP0_BASE + 0x00u));
    wr32(PCIE_AFI_BASE + AFI_PEX1_CTRL,
         rd32(PCIE_AFI_BASE + AFI_PEX1_CTRL) | AFI_PEX_CTRL_REFCLK_EN |
         AFI_PEX_CTRL_CLKREQ_EN | AFI_PEX_CTRL_OVERRIDE_EN);
    udelay(12000);

    wr32(PCIE_AFI_BASE + AFI_PEX1_CTRL,
         rd32(PCIE_AFI_BASE + AFI_PEX1_CTRL) & ~AFI_PEX_CTRL_RST);
    udelay(2000);
    wr32(PCIE_AFI_BASE + AFI_PEX1_CTRL,
         rd32(PCIE_AFI_BASE + AFI_PEX1_CTRL) | AFI_PEX_CTRL_RST);

    /* The refclk driver config only sticks once the pads block is
     * clocked: written after the PERST# pulse (U-Boot's port_enable
     * order), readback-verified into the mailbox. */
    wr32(PCIE_PADS_BASE + PADS_REFCLK_CFG0, PADS_REFCLK_CFG0_T210);
    mbox_w(&m->raw_cfg[16], rd32(PCIE_PADS_BASE + PADS_REFCLK_CFG0));

    wr32(PCIE_RP1_BASE + RP_VEND_CTL2,
         rd32(PCIE_RP1_BASE + RP_VEND_CTL2) | RP_VEND_CTL2_PCA_ENABLE);
    udelay(8000);

    /* Tell the port an endpoint is there; with no physical presence-detect
     * pin the LTSSM otherwise never leaves detect. */
    {
        unsigned int v = rd32(PCIE_RP1_BASE + RP_PRIV_MISC);
        v &= ~RP_PRIV_MISC_PRSNT_MAP_EP_ABSNT;
        v |= RP_PRIV_MISC_PRSNT_MAP_EP_PRSNT;
        wr32(PCIE_RP1_BASE + RP_PRIV_MISC, v);
    }

    step(m, CPU_STAGE_LINK_WAIT);
    /* Probe 3 = after the port enable, before the link trains. */
    mbox_w(&m->raw_cfg[8], rd32(PCIE_RP0_BASE + 0x00u));
    mbox_w(&m->rp_id, rd32(PCIE_RP1_BASE + 0x00));

    /* The TRM's official bring-up sequence (34.4.8) is DELIBERATELY not
     * done: none of the public drivers run it - U-Boot's enumeration,
     * which works on this console, never writes RP_VEND_XP1 (0xF04),
     * RP_VEND_XP_BIST (0xF4C) or the RP_ECTL equalization (0xE90/0xE94).
     * With those writes in place the root port's own config space reads
     * back garbage afterwards (VID/DID 00000000 where it otherwise reads
     * 0FAF10DE); the BIST register (0x10000001, start bit 0) is the prime
     * suspect. U-Boot's exact state is the reference. */
    {
        int up = 0;
        for (int i = 0; i < 200 && !up; i++) {           /* ~400 ms cap */
            if (rd32(PCIE_RP1_BASE + RP_VEND_XP) & RP_VEND_XP_DL_UP)
                up = 1;
            else
                udelay(2000);
        }
        for (int i = 0; i < 200 && up; i++) {
            mbox_w(&m->link_stat, rd32(PCIE_RP1_BASE + RP_LINK_CONTROL_STATUS));
            if (m->link_stat & RP_LINK_DL_ACTIVE)
                break;
            udelay(2000);
        }
        mbox_w(&m->link_stat, rd32(PCIE_RP1_BASE + RP_LINK_CONTROL_STATUS));
        mbox_w(&m->port, (m->link_stat & RP_LINK_DL_ACTIVE) ? 1u : ~0u);
    }

    if (m->port == 1u) {
        /* The root port is a bridge: no secondary bus number, no type-0
         * configuration TLPs downstream, no endpoint. */
        step(m, CPU_STAGE_CFG_RD);
        wr32(PCIE_RP1_BASE + RP_BUS_NUMBERS, (1u << 16) | (1u << 8) | 0u);
        wr32(PCIE_RP1_BASE + RP_COMMAND,
             rd32(PCIE_RP1_BASE + RP_COMMAND) | RP_COMMAND_MEM | RP_COMMAND_BME);
        /* Match U-Boot's post-enumeration bridge state exactly: its PCI
         * core leaves memory windows, IO windows and Device Control
         * (MPS 4096 + No Snoop) programmed before the first endpoint
         * config read, and consoles exist where the endpoint only
         * answers with the windows in place. */
        wr32(PCIE_RP1_BASE + 0x88u, 0x00002810u);
        wr32(PCIE_RP1_BASE + 0x20u, 0x13701300u);   /* mem base/limit */
        wr32(PCIE_RP1_BASE + 0x24u, 0x1FF12001u);   /* pref/IO base/limit */
        wr32(PCIE_RP1_BASE + RP_COMMAND,
             rd32(PCIE_RP1_BASE + RP_COMMAND) | 0x1u);  /* IO enable */

        /* Read them BACK. A bridge with no secondary bus number does not
         * forward anything downstream, and a write that silently did not
         * take looks identical from here to one that did. */
        mbox_w(&m->rp_bus, rd32(PCIE_RP1_BASE + RP_BUS_NUMBERS));
        mbox_w(&m->rp_cmd, rd32(PCIE_RP1_BASE + RP_COMMAND));

        /* U-Boot's enumeration choreography, replicated: the PCI core
         * walks the root port's own config window (the bus-0 scan, a
         * burst of reads through the RP window) BEFORE the first
         * downstream CS-aperture access. Mirroring that walk is an
         * unverified candidate for the first-CS-read hang, not a
         * measured behaviour: what the loop does to the RP/FPCI path
         * has never been isolated on hardware. */
        for (unsigned int off = 0; off <= 0x3C; off += 4)
            (void)rd32(PCIE_RP1_BASE + off);

        /* Same stage, another heartbeat: it splits "the root-port writes
         * hung" (hb stops at 10) from "the configuration aperture hung"
         * (hb reaches 11), which the stage number alone cannot. */
        step(m, CPU_STAGE_CFG_RD);

        /* PHY state dump for the supervisor: the UPHY/padctl/PLLE
         * registers as this stub sees them right before the first
         * endpoint read. U-Boot (which enumerates the same chip) leaves
         * a reference state to diff this against. NO PADS reads here -
         * reading the pads block breaks the root port's config path on
         * this console (see the list below). */
        {
            /* u32 entries: every address is below 4 GiB, and a u64 list
             * would add 4 bytes per entry - 84 bytes this image does not
             * have to spare at the layout ceiling. */
            static const unsigned int phy_addrs[] = {
                0x7009F004u, 0x7009F024u, 0x7009F028u, 0x7009F044u,
                0x7009F360u, 0x7009F364u, 0x7009F36Cu, 0x7009F370u,
                0x7009F37Cu, 0x7009F380u, 0x7009F384u,
                0x7009F460u, 0x7009F464u, 0x7009F47Cu,
                0x60006068u, 0x600060E8u, 0x600060ECu, 0x6000648Cu,
                0x600064C4u, 0x600064C8u, 0x6000651Cu,
                /* NO PADS reads here: touching the 0x01003000 block
                 * before the first endpoint access breaks the root
                 * port's config path - the 0x88 readback drops from
                 * 00002810 to 0 and the endpoint read then hangs.
                 * U-Boot never reads the pads before enumerating,
                 * which is why its flow works; even its shell's md.l
                 * of the pads hangs. The PADS state is dumped only in
                 * the failure path. */
            };
            volatile unsigned int *dump =
                (volatile unsigned int *)0xA0008800u;
            for (unsigned int i = 0;
                 i < sizeof(phy_addrs) / sizeof(phy_addrs[0]); i++) {
                dump[i] = rd32(phy_addrs[i]);
                __asm__ volatile("dc cvac, %0" :: "r"(&dump[i]) : "memory");
            }
            /* Bridge-programming readback taken here, before the first
             * endpoint access: if that access wedges, the later [10]
             * store never runs and the 0x88/0x20/0x24 writes would go
             * unverified. */
            mbox_w(&m->raw_cfg[10], rd32(PCIE_RP1_BASE + 0x88u));
        }

        /* Enable CRS software visibility on the root port (PCIe root
         * control, RP1 + 0x9C bit 4). Without it, an endpoint that is
         * still in ROM-phase init answers early config reads with CRS,
         * the root port retries those silently - and the read looks
         * exactly like a never-completing hang. With visibility on, a
         * CRS response completes as 0xFFFF0001 and can be retried on
         * our schedule instead of the RP's. */
        wr32(PCIE_RP1_BASE + RP_ROOT_CTRL,
             rd32(PCIE_RP1_BASE + RP_ROOT_CTRL) | RP_ROOT_CTRL_CRS_SW_VIS);

        /* U-Boot's enumeration pacing and access pattern, replicated
         * exactly: its scan starts ~10-50 ms after link-up (the bridge
         * programming and the bus-0 walk in between), and its FIRST
         * endpoint accesses are, in order (pci_bind_bus_devices):
         *   header type  - read of offset 0x0E, 8-bit
         *   vendor ID    - read of offset 0x00, 16-bit
         *   device ID    - read of offset 0x02, 16-bit
         *   class/rev    - read of offset 0x08, 32-bit
         * with no delays and no FPCI timeout armed.
         *
         * On the wire every one of those is a 32-BIT dword read: the
         * tegra controller driver (pci_tegra_read_config) masks the
         * offset down to the aligned dword in tegra_pcie_conf_address
         * (`where & 0xFC`) and reads `*(u32 *)address`, then extracts
         * the requested slice in software (pci_conv_32_to_size). An
         * 8-bit or 16-bit TLP is NOT what U-Boot sends, and an 8-bit
         * read of the 0x0C dword also samples the wrong byte: the
         * header type is byte 2 of that dword. */
        mbox_w(&m->raw_cfg[14],
               (rd32(EP_CFG(0x0E)) >> 16) & 0xFFu);  /* header type, FIRST */
        mbox_w(&m->afi_intr_code, rd32(PCIE_AFI_BASE + AFI_INTR_CODE));
        mbox_w(&m->afi_intr_sig, rd32(PCIE_AFI_BASE + AFI_INTR_SIGNATURE));
        mbox_w(&m->link_stat, rd32(PCIE_RP1_BASE + RP_LINK_CONTROL_STATUS));
        mbox_w(&m->raw_cfg[10], rd32(PCIE_RP1_BASE + 0x88u));
        mbox_w(&m->raw_cfg[11], rd32(PCIE_RP1_BASE + RP_SEC_STATUS));
        unsigned int id = rd32(EP_CFG(0x00));       /* VID:DID dword, read A */
        mbox_w(&m->raw_cfg[12], id);
        mbox_w(&m->raw_cfg[13], rd32(EP_CFG(0x02))); /* same dword, read B   */
        mbox_w(&m->raw_cfg[15], rd32(EP_CFG(0x08))); /* class/rev           */
        mbox_w(&m->raw_cfg[0], id);                 /* copy of read A: keeps
                                                     * the [0]/[1] pair on
                                                     * the same alternation
                                                     * parity (reads 2 and 6)*/
        CFG_DELAY();
        mbox_w(&m->raw_cfg[1], rd32(EP_CFG(0x00))); /* VID:DID, read D      */
        mbox_w(&m->raw_cfg[2], rd32(EP_CFG(0x04))); /* status/cmd, baseline */
        mbox_w(&m->raw_cfg[3], rd32(EP_CFG(0x08))); /* class/rev, read A    */
        mbox_w(&m->raw_cfg[4], rd32(EP_CFG(0x08))); /* class/rev, read B    */

        /* Every read is a pair: on the Erista test unit every second
         * endpoint config read returns a stale per-boot constant, in
         * strict access-order alternation, independent of the register
         * and of any delay. Take the value that agrees with its pair, or
         * the one that matches the one thing we know: the full ID dword
         * 0x43EC14E4 (DID:VID), which is also what the supervisor checks
         * for the CYW4356 verdict - it must not see VID alone. */
        mbox_w(&m->ep_id,
               ((m->raw_cfg[0] & 0xFFFFu) == 0x14E4u &&
                (m->raw_cfg[0] >> 16)    == 0x43ECu)
               ? m->raw_cfg[0] : m->raw_cfg[1]);
        /* Class code of a network controller (0x028000): the second thing
         * we know, used the same way when the class pair disagrees. */
        mbox_w(&m->ep_class,
               ((m->raw_cfg[3] & 0xFFFFFF00u) == 0x02800000u)
               ? m->raw_cfg[3] :
               ((m->raw_cfg[4] & 0xFFFFFF00u) == 0x02800000u)
               ? m->raw_cfg[4] : m->raw_cfg[3]);

        /* Snap AFI_INTR_CODE mid-sequence: any latched code here is from
         * the baseline config reads alone, before any writes. If bad reads
         * hit the FPCI timeout, code 9 (FPCI_TIMEOUT) should be visible. */
        mbox_w(&m->afi_intr_code, rd32(PCIE_AFI_BASE + AFI_INTR_CODE));
        mbox_w(&m->afi_intr_sig, rd32(PCIE_AFI_BASE + AFI_INTR_SIGNATURE));

        if ((m->ep_id & 0xFFFFu) == 0x14E4u) {
            /* ---- probe the endpoint's BAR state ------------------------
             * Answering config reads with the right ID proves the PCIe
             * front-end is alive; the command-register write below (the
             * one config write the ROM-phase chip accepts) plus the
             * surviving re-reads prove the path. The ChipCommon ChipID
             * itself needs firmware - the ROM never maps the backplane
             * at BAR0 - so it is deliberately not read here.
             *
             * Absolute writes only, no read-modify-write: the read side of
             * this path is exactly what is under test, and an RMW would
             * smear a bad read back into the chip as a write. Every write
             * is verified by a pair of readbacks and rewritten when
             * neither agrees - with alternating read corruption a single
             * readback cannot be trusted, and if the corruption is in the
             * request path rather than the response path, writes need the
             * retry too.
             *
             * Do NOT re-write AFI_FPCI_BAR0 between accesses. It is armed
             * once at the translation stage; re-poking it after config
             * traffic has flowed disturbs the path. */
            step(m, CPU_STAGE_BAR_RD);

            /* Arm the FPCI completion timeout only now, AFTER the baseline
             * reads proved the endpoint alive. U-Boot's driver never arms
             * it, and consoles exist where the first config read hangs with
             * it armed - the baseline reads above run in U-Boot's exact
             * environment on purpose. */
            wr32(PCIE_AFI_BASE + AFI_FPCI_TIMEOUT, AFI_FPCI_TO_VAL);
            wr32(PCIE_AFI_BASE + AFI_REQ_PENDING,
                 AFI_REQ_PENDING_TIMEOUT_EN | AFI_FPCI_TO_VAL);
            /* Let the FPCI-timeout interrupt latch (code 9) now that a
             * timeout is actually armed; it stays 0 through the baseline
             * reads, matching U-Boot. */
            wr32(PCIE_AFI_BASE + AFI_AFI_INTR_ENABLE,
                 AFI_AFI_INTR_EN_FPCI_TIMEOUT);
            /* The flow-control knobs live in RP_VEND_XP1 (0xF04) and
             * belong before the link trains, per the TRM's sequence -
             * nothing to do here. */

            mbox_w(&m->rp_secsts0, rd32(PCIE_RP1_BASE + RP_SEC_STATUS));

            /* Default BAR0 and backplane-window values, paired reads, no
             * writes: in ROM phase the chip accepts the command register
             * but ignores BAR0/window writes, so read what it boots with. */
            mbox_w(&m->raw_cfg[6], rd32(EP_CFG(0x10)));
            mbox_w(&m->raw_cfg[7], rd32(EP_CFG(0x10)));
            mbox_w(&m->ep_bar0, ((m->raw_cfg[6] & 0xFFFFFFF0u) != 0u)
                          ? m->raw_cfg[6] : m->raw_cfg[7]);
            mbox_w(&m->raw_cfg[10], rd32(EP_CFG(BRCM_BAR0_WINDOW)));
            mbox_w(&m->raw_cfg[11], rd32(EP_CFG(BRCM_BAR0_WINDOW)));

            /* Command register: the one config write the ROM-phase chip
             * accepts. MEM + BME, so the endpoint can issue requests. */
            unsigned int r_cmd = cfg_wr_verify(m, EP_CFG(0x04),
                          RP_COMMAND_MEM | RP_COMMAND_BME,
                          0x0007u, &m->raw_cfg[8], &m->raw_cfg[9]);
            mbox_w(&m->rp_secsts1, rd32(PCIE_RP1_BASE + RP_SEC_STATUS));
            mbox_w(&m->raw_cfg[19], r_cmd);

            /* Path still alive at the end? A pair of VID/DID re-reads. */
            mbox_w(&m->raw_cfg[17], rd32(EP_CFG(0x00)));
            mbox_w(&m->raw_cfg[18], rd32(EP_CFG(0x00)));
        }
    }

    /* Read AFI interrupt status after the full config-access dance. The
     * FPCI timeout is armed in the post-baseline block, after the first
     * endpoint config reads, so only accesses from the BAR stage onwards
     * can latch code 9 (FPCI_TIMEOUT) here. */
    mbox_w(&m->afi_intr_code, rd32(PCIE_AFI_BASE + AFI_INTR_CODE));
    mbox_w(&m->afi_intr_sig, rd32(PCIE_AFI_BASE + AFI_INTR_SIGNATURE));
    mbox_w(&m->afi_fpci_to, rd32(PCIE_AFI_BASE + AFI_FPCI_TIMEOUT));

    step(m, CPU_STAGE_DONE);
    mbox_w(&m->done, CPU_MBOX_DONE);
    __asm__ volatile("dsb sy" ::: "memory");

    for (;;)
        __asm__ volatile("wfe");
}
