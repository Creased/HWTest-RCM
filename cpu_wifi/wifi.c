/*
 * CPU0 Wi-Fi scan: a bare-metal port of the parts of Linux's brcmfmac PCIe
 * driver a scan needs, run after the health check's own bring-up.
 *
 * The stub (../cpu_stub/stub.c, compiled in below) trains the link and
 * enumerates the CYW4356, then calls wifi_main() instead of parking. From
 * there, in brcmfmac's order (drivers/net/wireless/broadcom/brcm80211/
 * brcmfmac: pcie.c, chip.c, msgbuf.c, commonring.c, fweh.c, cfg80211.c):
 *
 *   1. BAR0 (32 KiB register window) and BAR2 (the TCM) sized and placed in
 *      the root port's memory window, which is widened to cover them;
 *   2. ChipCommon's chip ID through BAR0 and its backplane window (config
 *      0x80), then the EROM walked for the cores and their wrappers;
 *   3. ARM CR4 halted and the D11 cores disabled (brcmf_chip_set_passive),
 *      the chip's watchdog reset and PCIe2 config restore
 *      (brcmf_pcie_reset_device), the BAR1 sizing rewrite (attach);
 *   4. the RAM size from the CR4 bank registers, the firmware and the NVRAM
 *      written into TCM, the reset vector at TCM 0, CR4 released, and the
 *      firmware's shared-RAM pointer awaited at the top of RAM;
 *   5. the five common msgbuf rings in an uncached DMA block, TCM ring
 *      indices, the scratch buffers, host-ready;
 *   6. event and ioctl-response buffers posted, then ioctls: "ver",
 *      "cur_etheraddr", "mpc" 0, "event_msgs", UP; the receive buffers
 *      once the firmware owns its ring indices; then "escan", its
 *      ESCAN_RESULT events collected until the final one, retried twice
 *      when it hears nothing;
 *   7. or, when the BPMP names a network, a join instead: wsec and
 *      wpa_auth, the passphrase, SET_SSID, and on a link the four-way
 *      handshake run here (Nintendo's firmware has no supplicant) over a
 *      flow ring, then the pairwise key into the chip;
 *   8. an address from DHCP and an echo off the internet (net.c).
 *
 * Everything is polled: no interrupts, no MSI. Every wait is bounded and
 * bumps the heartbeat, and every step is named in the mailbox before it
 * starts (wifi_mbox.h), the stub's rules. Where brcmfmac's behaviour
 * depends on a chip revision this port takes the 4356's branch only.
 */

#include <stdbool.h>

#define CPU_STUB_THEN wifi_main
#include "../cpu_stub/stub.c"
#include "../wifi_mbox.h"
#include "net.h"
#include "wpa.h"

/* The stack's states are reported through the mailbox as they are. */
_Static_assert(NET_IDLE == WIFI_NET_IDLE && NET_ARP == WIFI_NET_ARP &&
               NET_DHCP == WIFI_NET_DHCP && NET_BOUND == WIFI_NET_BOUND &&
               NET_PING == WIFI_NET_PING && NET_REACHED == WIFI_NET_REACHED,
               "net.h's states and wifi_mbox.h's WIFI_NET_* differ");

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef short              s16;

/* ---- the few libc functions the compiler may emit calls to ------------ */

void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *dp = d;
    const u8 *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, unsigned long n)
{
    u8 *dp = d;
    while (n--)
        *dp++ = (u8)c;
    return d;
}

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

/* ---- mailbox ------------------------------------------------------------ */

static wifi_mbox_t *const W = (wifi_mbox_t *)WIFI_MBOX_ADDR;

static void hb(void)
{
    W->heartbeat++;
    /* The supervisor calls a heartbeat that stops moving a stall, so this
     * one word has to reach DRAM on its own (see mbox_flush below). */
    __asm__ volatile("dc cvac, %0" :: "r"(&W->heartbeat) : "memory");
}

/* The BPMP reads the mailbox from DRAM and is not coherent with CPU0's
 * caches. wifi.S maps the block uncached, which should be enough, yet the
 * BSS table reached the BPMP with holes in it until the mailbox was also
 * cleaned by range here - about a hundred lines, cheap enough for every
 * step. The stub cleans each word it writes the same way. */
static void mbox_flush(void)
{
    const unsigned char *p = (const unsigned char *)W;
    for (u32 o = 0; o < sizeof(wifi_mbox_t); o += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(p + o) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void set_step(u32 s)
{
    W->step = s;
    hb();
    mbox_flush();
}

static void msleep(u32 ms)
{
    while (ms--) {
        udelay(1000);
        if (!(ms & 15))
            hb();
    }
}

/* Tiny string building for the one-line reason. */
static char *put_s(char *p, const char *s)
{
    while (*s && p < W->why + sizeof(W->why) - 1)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_x(char *p, u32 v)
{
    char t[11];
    t[0] = '0'; t[1] = 'x';
    for (int i = 0; i < 8; i++)
        t[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xF];
    t[10] = 0;
    return put_s(p, t);
}

/* ---- PCIe ----------------------------------------------------------------
 *
 * Memory BARs go in the root port's non-prefetchable window, which the
 * stub's AFI BAR3 maps 1:1 from AXI 0x13000000 (208 MiB). The stub opens
 * the bridge window at 0x13000000-0x137FFFFF; it is widened below. */

#define PCI_BAR0_BASE   0x13000000u   /* 32 KiB register window         */
#define PCI_BAR2_BASE   0x14000000u   /* TCM, up to 64 MiB              */
#define PCI_BAR2_MAX    0x04000000u

static u32 s_tmp_a, s_tmp_b;          /* readback scratch for cfg writes */

static u32 cfg_rd(u32 off) { return rd32(EP_CFG(off)); }
static void cfg_wr(u32 off, u32 v) { wr32(EP_CFG(off), v); }

/* A config write that must stick, verified as the stub verifies its own
 * (paired readbacks, rewritten, 25 rounds 20 ms apart). */
static int cfg_set(u32 off, u32 v, u32 mask)
{
    return cfg_wr_verify(g_mb, EP_CFG(off), v, mask, &s_tmp_a, &s_tmp_b)
           < 25 ? 0 : -1;
}

static inline u16 rd16(unsigned long a) { return *(volatile u16 *)a; }
static inline void wr16(unsigned long a, u16 v) { *(volatile u16 *)a = v; }
static inline u8 rd8(unsigned long a) { return *(volatile u8 *)a; }

static u32 reg32(u32 off) { return rd32(PCI_BAR0_BASE + off); }
static void reg32_w(u32 off, u32 v) { wr32(PCI_BAR0_BASE + off, v); }
static u32 tcm32(u32 off) { return rd32(PCI_BAR2_BASE + off); }
static void tcm32_w(u32 off, u32 v) { wr32(PCI_BAR2_BASE + off, v); }
static u16 tcm16(u32 off) { return rd16(PCI_BAR2_BASE + off); }
static void tcm16_w(u32 off, u16 v) { wr16(PCI_BAR2_BASE + off, v); }

/* A config read the "every second read is stale" quirk cannot fool: read
 * until two in a row agree (at most 6 reads). */
static u32 cfg_rd_stable(u32 off)
{
    u32 a = cfg_rd(off);
    for (int i = 0; i < 5; i++) {
        u32 b = cfg_rd(off);
        if (a == b)
            return a;
        a = b;
    }
    return a;
}

static bool is_size_mask(u32 v)
{
    u32 m = v & 0xFFFFFFF0u;
    if (!m)
        return false;
    u32 sz = ~m + 1u;
    return (sz & (sz - 1u)) == 0;
}

/* Size a memory BAR and give it `base`. Returns its size, 0 on failure. */
static u32 bar_assign(u32 off, u32 base)
{
    u32 orig = cfg_rd_stable(off);
    bool is64 = (orig & 0x6u) == 0x4u;
    u32 m = 0;

    cfg_wr(off, 0xFFFFFFFFu);
    for (int i = 0; i < 6 && !is_size_mask(m); i++)
        m = cfg_rd(off);
    if (!is_size_mask(m))
        return 0;
    u32 size = ~(m & 0xFFFFFFF0u) + 1u;
    if (is64) {
        cfg_wr(off + 4, 0xFFFFFFFFu);
        u32 hi = cfg_rd_stable(off + 4);
        if (hi != 0 && hi != 0xFFFFFFFFu)
            return 0;
        /* A BAR bigger than 4 GiB would read back all ones here; this
         * one cannot be, so all ones is the stale-read quirk at work. */
        if (cfg_set(off + 4, 0, 0xFFFFFFFFu))
            return 0;
    }
    if (base & (size - 1u))
        return 0;
    if (cfg_set(off, base | (orig & 0xFu), 0xFFFFFFF0u))
        return 0;
    return size;
}

/* ---- the backplane through BAR0 ----------------------------------------
 *
 * Config 0x80 picks which 4 KiB of the chip's backplane BAR0's first 4 KiB
 * show (brcmf_pcie_buscore_prep_addr). Written only when it changes, and
 * verified, since the stub records this chip dropping window writes. */

static u32 s_win = 0xFFFFFFFFu;

static int bp_window(u32 addr)
{
    u32 win = addr & ~0xFFFu;
    if (win == s_win)
        return 0;
    if (cfg_set(BRCM_BAR0_WINDOW, win, 0xFFFFF000u)) {
        s_win = 0xFFFFFFFFu;
        return -1;
    }
    s_win = win;
    return 0;
}

static bool s_bp_err;

static u32 bp_rd(u32 addr)
{
    if (bp_window(addr)) {
        s_bp_err = true;
        return 0xFFFFFFFFu;
    }
    return reg32(addr & 0xFFFu);
}

static void bp_wr(u32 addr, u32 v)
{
    if (bp_window(addr)) {
        s_bp_err = true;
        return;
    }
    reg32_w(addr & 0xFFFu, v);
}

/* ---- cores (chip.c) -------------------------------------------------- */

#define CORE_CC     0x800u
#define CORE_80211  0x812u
#define CORE_PMU    0x827u
#define CORE_PCIE2  0x83Cu
#define CORE_CR4    0x83Eu
#define CORE_GCI    0x840u

#define BCMA_IOCTL           0x0408u
#define  BCMA_IOCTL_CLK      0x0001u
#define  BCMA_IOCTL_FGC      0x0002u
#define BCMA_RESET_CTL       0x0800u
#define  BCMA_RESET_CTL_RESET 0x0001u
#define CR4_IOCTL_CPUHALT    0x0020u
#define D11_IOCTL_PHYCLOCKEN 0x0004u
#define D11_IOCTL_PHYRESET   0x0008u

#define ARMCR4_CAP      0x04u
#define ARMCR4_BANKIDX  0x40u
#define ARMCR4_BANKINFO 0x44u

static const wifi_core_t *core_get(u16 id, u32 nth)
{
    for (u32 i = 0; i < W->ncores; i++)
        if (W->cores[i].id == id && nth-- == 0)
            return &W->cores[i];
    return 0;
}

static void core_add(u16 id, u8 rev, u32 base, u32 wrap)
{
    if (W->ncores >= WIFI_CORES_MAX)
        return;
    wifi_core_t *c = &W->cores[W->ncores++];
    c->id = id;
    c->rev = rev;
    c->base = base;
    c->wrap = wrap;
}

/* EROM descriptors (chip.c, brcmf_chip_dmp_*). */
#define DMP_DESC_TYPE_MSK      0x0000000Fu
#define DMP_DESC_EMPTY         0x00000000u
#define DMP_DESC_VALID         0x00000001u
#define DMP_DESC_COMPONENT     0x00000001u
#define DMP_DESC_MASTER_PORT   0x00000003u
#define DMP_DESC_ADDRESS       0x00000005u
#define DMP_DESC_ADDRSIZE_GT32 0x00000008u
#define DMP_DESC_EOT           0x0000000Fu
#define DMP_COMP_PARTNUM       0x000FFF00u
#define DMP_COMP_PARTNUM_S     8
#define DMP_COMP_REVISION      0xFF000000u
#define DMP_COMP_REVISION_S    24
#define DMP_COMP_NUM_SWRAP     0x00F80000u
#define DMP_COMP_NUM_SWRAP_S   19
#define DMP_COMP_NUM_MWRAP     0x0007C000u
#define DMP_COMP_NUM_MWRAP_S   14
#define DMP_SLAVE_ADDR_BASE    0xFFFFF000u
#define DMP_SLAVE_TYPE         0x000000C0u
#define DMP_SLAVE_TYPE_S       6
#define  DMP_SLAVE_TYPE_SLAVE  0
#define  DMP_SLAVE_TYPE_SWRAP  2
#define  DMP_SLAVE_TYPE_MWRAP  3
#define DMP_SLAVE_SIZE_TYPE    0x00000030u
#define DMP_SLAVE_SIZE_TYPE_S  4
#define  DMP_SLAVE_SIZE_4K     0
#define  DMP_SLAVE_SIZE_8K     1
#define  DMP_SLAVE_SIZE_DESC   3

static u32 erom_desc(u32 *erom, u8 *type)
{
    u32 v = bp_rd(*erom);
    *erom += 4;
    if (type) {
        *type = v & DMP_DESC_TYPE_MSK;
        if ((*type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
            *type = DMP_DESC_ADDRESS;
    }
    return v;
}

static int erom_regaddr(u32 *erom, u32 *regbase, u32 *wrapbase)
{
    u8 desc, wraptype;
    u32 val;

    *regbase = 0;
    *wrapbase = 0;
    val = erom_desc(erom, &desc);
    if (desc == DMP_DESC_MASTER_PORT) {
        wraptype = DMP_SLAVE_TYPE_MWRAP;
    } else if (desc == DMP_DESC_ADDRESS) {
        *erom -= 4;
        wraptype = DMP_SLAVE_TYPE_SWRAP;
    } else {
        *erom -= 4;
        return -1;
    }

    for (int guard = 0; guard < 64; guard++) {
        do {
            val = erom_desc(erom, &desc);
            if (desc == DMP_DESC_EOT) {
                *erom -= 4;
                return -1;
            }
        } while (desc != DMP_DESC_ADDRESS && desc != DMP_DESC_COMPONENT);

        if (desc == DMP_DESC_COMPONENT) {
            *erom -= 4;
            return 0;
        }
        if (val & DMP_DESC_ADDRSIZE_GT32)
            erom_desc(erom, 0);
        u32 sztype = (val & DMP_SLAVE_SIZE_TYPE) >> DMP_SLAVE_SIZE_TYPE_S;
        if (sztype == DMP_SLAVE_SIZE_DESC) {
            u32 szdesc = erom_desc(erom, 0);
            if (szdesc & DMP_DESC_ADDRSIZE_GT32)
                erom_desc(erom, 0);
        }
        if (sztype != DMP_SLAVE_SIZE_4K && sztype != DMP_SLAVE_SIZE_8K)
            continue;
        u32 stype = (val & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_S;
        if (*regbase == 0 && stype == DMP_SLAVE_TYPE_SLAVE)
            *regbase = val & DMP_SLAVE_ADDR_BASE;
        if (*wrapbase == 0 && stype == wraptype)
            *wrapbase = val & DMP_SLAVE_ADDR_BASE;
        if (*regbase && *wrapbase)
            return 0;
    }
    return -1;
}

static void erom_scan(u32 erom)
{
    u8 type = 0;
    /* An EROM is a few hundred descriptors; 2048 bounds a corrupt one. */
    for (int n = 0; n < 2048 && type != DMP_DESC_EOT; n++) {
        u32 val = erom_desc(&erom, &type);
        if (!(val & DMP_DESC_VALID) || type == DMP_DESC_EMPTY ||
            type != DMP_DESC_COMPONENT)
            continue;
        u16 id = (val & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_S;
        val = erom_desc(&erom, &type);
        if ((val & DMP_DESC_TYPE_MSK) != DMP_DESC_COMPONENT)
            return;
        u32 nmw = (val & DMP_COMP_NUM_MWRAP) >> DMP_COMP_NUM_MWRAP_S;
        u32 nsw = (val & DMP_COMP_NUM_SWRAP) >> DMP_COMP_NUM_SWRAP_S;
        u8 rev = (val & DMP_COMP_REVISION) >> DMP_COMP_REVISION_S;
        if (nmw + nsw == 0 && id != CORE_PMU && id != CORE_GCI)
            continue;
        u32 base, wrap;
        if (erom_regaddr(&erom, &base, &wrap))
            continue;
        core_add(id, rev, base, wrap);
        hb();
    }
}

/* brcmf_chip_ai_coredisable / _resetcore, the 4356's one-D11 path. */
static void core_disable(const wifi_core_t *c, u32 prereset, u32 reset)
{
    u32 v = bp_rd(c->wrap + BCMA_RESET_CTL);
    if (!(v & BCMA_RESET_CTL_RESET)) {
        bp_wr(c->wrap + BCMA_IOCTL, prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
        (void)bp_rd(c->wrap + BCMA_IOCTL);
        bp_wr(c->wrap + BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);
        udelay(20);
        for (int i = 0; i < 30 &&
             bp_rd(c->wrap + BCMA_RESET_CTL) != BCMA_RESET_CTL_RESET; i++)
            udelay(10);
    }
    bp_wr(c->wrap + BCMA_IOCTL, reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
    (void)bp_rd(c->wrap + BCMA_IOCTL);
}

static void core_reset(const wifi_core_t *c, u32 prereset, u32 reset,
                       u32 postreset)
{
    core_disable(c, prereset, reset);
    for (int i = 0; i < 50 &&
         (bp_rd(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET); i++) {
        bp_wr(c->wrap + BCMA_RESET_CTL, 0);
        udelay(50);
    }
    bp_wr(c->wrap + BCMA_IOCTL, postreset | BCMA_IOCTL_CLK);
    (void)bp_rd(c->wrap + BCMA_IOCTL);
}

/* brcmf_chip_cr4_set_passive. */
static void chip_passive(void)
{
    const wifi_core_t *cr4 = core_get(CORE_CR4, 0);
    u32 v = bp_rd(cr4->wrap + BCMA_IOCTL) & CR4_IOCTL_CPUHALT;
    core_reset(cr4, v, CR4_IOCTL_CPUHALT, CR4_IOCTL_CPUHALT);
    for (u32 i = 0; i < 2; i++) {
        const wifi_core_t *d11 = core_get(CORE_80211, i);
        if (d11)
            core_disable(d11, D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN,
                         D11_IOCTL_PHYCLOCKEN);
    }
}

/* PCIe2 core registers, through BAR0 with the window on that core. */
#define PCIE2_INTMASK      0x24u
#define PCIE2_MAILBOXINT   0x48u
#define PCIE2_MAILBOXMASK  0x4Cu
#define PCIE2_CONFIGADDR   0x120u
#define PCIE2_CONFIGDATA   0x124u
#define PCIE2_H2D_MB0      0x140u
#define PCIE2_H2D_MB1      0x144u
#define PCIE2_64_MAILBOXINT 0xC30u
#define PCIE2_64_H2D_MB0   0xA20u
#define PCIE2_64_H2D_MB1   0xA24u

/* Endpoint config register the chip calls LINK_STATUS_CTRL. */
#define CFG_LINK_STATUS_CTRL 0xBCu

/* brcmf_pcie_reset_device. */
static void chip_reset(void)
{
    static const u16 cfg_offset[] = {
        0x004, 0x04C, 0x058, 0x05C, 0x060, 0x064, 0x0DC, 0x228, 0x248,
        0x4E0, 0x4F4
    };
    const wifi_core_t *cc = core_get(CORE_CC, 0);
    const wifi_core_t *p2 = core_get(CORE_PCIE2, 0);

    u32 lsc = cfg_rd_stable(CFG_LINK_STATUS_CTRL);
    cfg_wr(CFG_LINK_STATUS_CTRL, lsc & ~3u);          /* ASPM off */
    bp_wr(cc->base + 0x80, 4);                        /* watchdog */
    msleep(100);
    /* The watchdog resets the backplane, the window register with it. */
    s_win = 0xFFFFFFFFu;
    cfg_wr(CFG_LINK_STATUS_CTRL, lsc);

    if (p2->rev <= 13) {
        for (u32 i = 0; i < sizeof(cfg_offset) / sizeof(cfg_offset[0]); i++) {
            bp_wr(p2->base + PCIE2_CONFIGADDR, cfg_offset[i]);
            u32 v = bp_rd(p2->base + PCIE2_CONFIGDATA);
            bp_wr(p2->base + PCIE2_CONFIGDATA, v);
        }
    }
}

/* ---- DMA memory: a bump allocator over the uncached block ------------- */

static u32 s_dma_next = WIFI_DMA_ADDR;

static void *dma_alloc(u32 size, u32 align)
{
    u32 a = (s_dma_next + align - 1) & ~(align - 1);
    if (a + size > WIFI_MBOX_ADDR)
        return 0;
    s_dma_next = a + size;
    memset((void *)(unsigned long)a, 0, size);
    return (void *)(unsigned long)a;
}

/* ---- msgbuf rings (commonring.c, pcie.c) ------------------------------- */

#define RING_CTRL_SUBMIT   0
#define RING_RXPOST        1
#define RING_CTRL_COMPLETE 2
#define RING_TX_COMPLETE   3
#define RING_RX_COMPLETE   4
#define NRINGS             5

typedef struct {
    u8  *buf;
    u16  depth, item;
    u32  w_idx_addr, r_idx_addr;   /* TCM offsets of the indices */
    u16  w, r;
} ring_t;

static ring_t s_ring[NRINGS];
/* Where the firmware put the index arrays: the flow ring's indices go in
 * the same ones, at its ring id. */
static u32 s_h2d_w, s_h2d_r;
static u32 s_mb0 = PCIE2_H2D_MB0, s_mb1 = PCIE2_H2D_MB1;
static u32 s_rx_dataoffset;
static u32 s_max_rxbufpost;

static const u16 ring_depth[NRINGS] = { 64, 1024, 64, 1024, 1024 };
static const u16 ring_item_v7[NRINGS]  = { 40, 32, 24, 24, 40 };
static const u16 ring_item_pre7[NRINGS] = { 40, 32, 24, 16, 32 };

static void doorbell(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
    reg32_w(s_mb0, 1);
}

static net_t  s_net;
static bool   s_net_up;      /* net_init has run: frames go to it     */

/* A free slot in an H2D ring, zeroed, or 0 when it is full. */
static u8 *ring_reserve(ring_t *rg)
{
    rg->r = tcm16(rg->r_idx_addr);
    u16 avail = (rg->r <= rg->w) ? rg->depth - rg->w + rg->r : rg->r - rg->w;
    if (avail <= 1)
        return 0;
    u8 *slot = rg->buf + (u32)rg->w * rg->item;
    memset(slot, 0, rg->item);
    if (++rg->w == rg->depth)
        rg->w = 0;
    return slot;
}

static void ring_commit(ring_t *rg)
{
    __asm__ volatile("dsb sy" ::: "memory");
    tcm16_w(rg->w_idx_addr, rg->w);
    doorbell();
}

static inline void put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static inline void put32(u8 *p, u32 v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static inline u16 get16(const u8 *p) { return p[0] | (p[1] << 8); }
static inline u32 get32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}
static inline u16 get16be(const u8 *p) { return (p[0] << 8) | p[1]; }
static inline u32 get32be(const u8 *p)
{
    return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* ---- message buffers ------------------------------------------------- */

#define MSG_GEN_STATUS        0x01
#define MSG_RING_STATUS       0x02
#define MSG_IOCTLPTR_REQ      0x09
#define MSG_IOCTLPTR_REQ_ACK  0x0A
#define MSG_IOCTLRESP_BUF_POST 0x0B
#define MSG_IOCTL_CMPLT       0x0C
#define MSG_EVENT_BUF_POST    0x0D
#define MSG_WL_EVENT          0x0E
#define MSG_TX_STATUS         0x10
#define MSG_RXBUF_POST        0x11
#define MSG_RX_CMPLT          0x12

#define IOCTL_REQ_PKTID   0xFFFEu
#define IOCTL_MAX_TX      1518u        /* BRCMF_TX_IOCTL_MAX_MSG_SIZE    */
#define CTL_BUF_SIZE      8192u        /* BRCMF_MSGBUF_MAX_CTL_PKT_SIZE  */
#define RX_BUF_SIZE       2048u
#define N_IOCTLRESP       8u
#define N_EVENT           8u
#define N_RXDATA          32u
#define PKT_IOCTLRESP     0x100u
#define PKT_EVENT         0x200u
#define PKT_RXDATA        0x300u

static u8 *s_ioctbuf;
static u8 *s_resp_buf[N_IOCTLRESP];
static u8 *s_event_buf[N_EVENT];
static u8 *s_rx_buf[N_RXDATA];

static void post_ctrl_buf(u32 msgtype, u32 pktid, u8 *buf)
{
    ring_t *rg = &s_ring[RING_CTRL_SUBMIT];
    u8 *it = ring_reserve(rg);
    if (!it)
        return;
    it[0] = msgtype;
    put32(it + 4, pktid);
    put16(it + 8, CTL_BUF_SIZE);
    put32(it + 16, (u32)(unsigned long)buf);
    put32(it + 20, 0);
    ring_commit(rg);
}

static bool post_rx_buf(u32 pktid, u8 *buf)
{
    ring_t *rg = &s_ring[RING_RXPOST];
    u8 *it = ring_reserve(rg);
    /* A full ring here is not a detail to swallow: with no buffer posted
     * the firmware drops every data frame, silently, and the handshake
     * waits for a message that was thrown away before it reached us. */
    if (!it)
        return false;
    it[0] = MSG_RXBUF_POST;
    put32(it + 4, pktid);
    put16(it + 8, 0);                  /* no metadata */
    put16(it + 10, RX_BUF_SIZE);
    put32(it + 24, (u32)(unsigned long)buf);
    put32(it + 28, 0);
    ring_commit(rg);
    W->rx_posted++;
    return true;
}

/* ---- the four-way handshake -------------------------------------------
 *
 * Nintendo's firmware has no supplicant of its own: WLC_SET_WSEC_PMK
 * answers BCME_UNSUPPORTED, because HOS does the handshake in software.
 * So this does too - which means transmitting, and transmitting on
 * msgbuf means a flow ring, created for the one destination there is.
 *
 * What is implemented is a station's half of IEEE 802.11-2016 12.7.6 for
 * WPA2-PSK with CCMP: message 1 carries the AP's nonce, message 2 answers
 * with ours and a MIC over the frame, message 3 is checked and message 4
 * acknowledges it, after which the pairwise key goes into the chip. The
 * group key in message 3 arrives wrapped and is left alone: nothing here
 * listens to broadcast traffic, and unwrapping it would want an AES the
 * CPU0 image does not carry.
 */
static void poll_d2h(void);      /* the rings, which the handshake drives */

#define ETHERTYPE_EAPOL   0x888Eu
#define EAPOL_KEY         3u
#define KEY_INFO_PAIRWISE 0x0008u
#define KEY_INFO_INSTALL  0x0040u
#define KEY_INFO_ACK      0x0080u
#define KEY_INFO_MIC      0x0100u
#define KEY_INFO_SECURE   0x0200u
#define KEY_INFO_ENC_DATA 0x1000u
#define KEY_DESC_VER_SHA1 2u

/* The flow ring: brcmfmac numbers them from 2, after the two common
 * submit rings, and its indices sit in the same arrays. */
#define FLOWRING_ID      2u
#define FLOWRING_DEPTH   64u
#define FLOWRING_ITEM    48u
#define MSG_FLOW_CREATE       0x03
#define MSG_FLOW_CREATE_CMPLT 0x04
#define MSG_TX_POST           0x0F
#define PKT_TX            0x400u

static ring_t s_txring;
static bool   s_txring_up;
static u8    *s_txbuf;            /* the frame body a TX post points at */

/* The handshake's state, for the poll loop below. */
static struct {
    bool  have_m1, done, failed;
    /* The AP's first message lands while the link is still coming up,
     * before there is anywhere to transmit: what to send is decided
     * here and sent from the loop once the flow ring exists. */
    bool  m2_pending, m4_pending;
    u8    anonce[32], snonce[32];
    u8    ptk[48];                /* KCK | KEK | TK */
    u8    replay[8];
    u8    bssid[6];
    u32   m3_seen;
} s_hs;

static u32 s_rand = 0x5A4356A5;

static u32 prand(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand ^ (W->heartbeat << 13);
}

static int flowring_create(const u8 *bssid)
{
    ring_t *rg = &s_txring;
    rg->depth = FLOWRING_DEPTH;
    rg->item = FLOWRING_ITEM;
    rg->buf = dma_alloc(rg->depth * rg->item, 4096);
    s_txbuf = dma_alloc(2048, 64);
    if (!rg->buf || !s_txbuf)
        return -1;
    rg->w = rg->r = 0;
    /* Its indices live beside the common rings', at its ring id. */
    rg->w_idx_addr = s_h2d_w + 4u * FLOWRING_ID;
    rg->r_idx_addr = s_h2d_r + 4u * FLOWRING_ID;
    tcm16_w(rg->w_idx_addr, 0);
    tcm16_w(rg->r_idx_addr, 0);

    ring_t *cr = &s_ring[RING_CTRL_SUBMIT];
    u8 *it = ring_reserve(cr);
    if (!it)
        return -1;
    it[0] = MSG_FLOW_CREATE;
    put32(it + 4, PKT_TX);
    memcpy(it + 8, bssid, 6);          /* to the AP */
    memcpy(it + 14, (const void *)W->mac, 6);
    it[20] = 0;                        /* TID 0: best effort */
    put16(it + 22, FLOWRING_ID);
    put16(it + 28, (u16)rg->depth);
    put16(it + 30, (u16)rg->item);
    put32(it + 32, (u32)(unsigned long)rg->buf);
    put32(it + 36, 0);
    ring_commit(cr);

    for (u32 t = 0; t < 2000 && !s_txring_up; t++) {
        poll_d2h();
        udelay(1000);
        if (!(t & 15))
            hb();
    }
    return s_txring_up ? 0 : -1;
}

/* One frame out: the ethernet header travels in the descriptor, the rest
 * in a buffer the firmware reads by DMA. */
static int tx_frame_et(const u8 *dst, u16 ethertype, const u8 *body, u32 len)
{
    if (!s_txring_up || len > 1500)
        return -1;
    memcpy(s_txbuf, body, len);
    u8 *it = ring_reserve(&s_txring);
    if (!it)
        return -1;
    it[0] = MSG_TX_POST;
    put32(it + 4, PKT_TX);
    memcpy(it + 8, dst, 6);
    memcpy(it + 14, (const void *)W->mac, 6);
    /* The descriptor carries the ethernet header as it goes on the air,
     * so the ethertype is big-endian here and not a host halfword. */
    it[20] = (u8)(ethertype >> 8);
    it[21] = (u8)ethertype;
    it[23] = 1;                        /* one segment */
    put32(it + 32, (u32)(unsigned long)s_txbuf);
    put32(it + 36, 0);
    put16(it + 42, (u16)len);
    ring_commit(&s_txring);
    return 0;
}

static int tx_frame(const u8 *dst, const u8 *body, u32 len)
{
    return tx_frame_et(dst, ETHERTYPE_EAPOL, body, len);
}

/* ---- what the little IP stack needs from this side ------------------ */

/* Who the echo goes to. A public resolver, because it answers echoes,
 * is reachable from anywhere a consumer network routes, and is not
 * somebody's machine. */
#define PING_TARGET   0x08080808u      /* 8.8.8.8 */
#define NET_LIMIT_MS  15000u

static u32 s_net_ms;                   /* the stack's own clock */

static int net_tx(void *ctx, const u8 *dst, u16 et, const u8 *body, u32 len)
{
    (void)ctx;
    return tx_frame_et(dst, et, body, len);
}
static u32 net_ms(void *ctx)  { (void)ctx; return s_net_ms; }
static u32 net_rnd(void *ctx) { (void)ctx; return prand(); }

/* The mailbox's own strings are fixed arrays, so this is the one copy
 * that will not run off the end of one. */
static void scopy(volatile char *dst, const char *src, u32 cap)
{
    u32 i = 0;
    for (; src[i] && i + 1 < cap; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* An EAPOL-Key frame: the fixed header, then the MIC computed over the
 * whole of it with the MIC field zeroed, as 12.7.2 has it. */
static void eapol_send(u16 key_info, const u8 *nonce, const u8 *kdata,
                       u32 klen, bool mic)
{
    u8 f[4 + 95 + 64];
    u32 n = 4 + 95 + klen;
    memset(f, 0, sizeof(f));
    f[0] = 2;                          /* 802.1X-2004 */
    f[1] = EAPOL_KEY;
    f[2] = (u8)((95 + klen) >> 8);
    f[3] = (u8)(95 + klen);
    u8 *k = f + 4;
    k[0] = 2;                          /* RSN key descriptor */
    k[1] = (u8)(key_info >> 8);
    k[2] = (u8)key_info;
    k[3] = 0; k[4] = 16;               /* key length: CCMP */
    memcpy(k + 5, s_hs.replay, 8);
    if (nonce)
        memcpy(k + 13, nonce, 32);
    k[93] = (u8)(klen >> 8);
    k[94] = (u8)klen;
    if (klen)
        memcpy(k + 95, kdata, klen);
    if (mic)
        wpa_mic(s_hs.ptk, f, n, k + 77);
    W->tx_eapol++;
    tx_frame(s_hs.bssid, f, n);
}

/* The RSN element message 2 carries must be the one the station put in
 * its association request, byte for byte: the AP compares them and says
 * nothing when they differ, which reads exactly like a wrong passphrase.
 * The firmware built it, so this asks for its copy, brcmfmac's way:
 * assoc_info gives the lengths, assoc_req_ies the bytes. */
static int iovar_get(const char *name, void *out, u32 len);

static u8  s_rsn[40];
static u32 s_rsn_len;

static void fetch_assoc_rsn(void)
{
    /* The firmware writes its answer into a buffer of its own choosing
     * and refuses a short one, so this asks for the whole 512 bytes
     * brcmfmac asks for, not just the two lengths it reads out of it. */
    static u8 info[512];
    s_rsn_len = 0;
    if (iovar_get("assoc_info", info, sizeof(info)) < 0)
        return;
    u32 req_len = get32(info);          /* brcmf_cfg80211_assoc_ielen_le */
    if (!req_len || req_len > 512)
        return;
    static u8 ies[512];
    if (iovar_get("assoc_req_ies", ies, req_len) < 0)
        return;
    for (u32 o = 0; o + 2 <= req_len; ) {
        u32 l = ies[o + 1];
        if (o + 2 + l > req_len)
            break;
        if (ies[o] == 48 && l + 2 <= sizeof(s_rsn)) {   /* RSN */
            memcpy(s_rsn, ies + o, l + 2);
            s_rsn_len = l + 2;
            break;
        }
        o += 2 + l;
    }
}

/* What to fall back on when the firmware will not say: the plain
 * WPA2-PSK element, CCMP both ways. */
static const u8 kRsnIe[] = {
    0x30, 0x14, 0x01, 0x00,
    0x00, 0x0F, 0xAC, 0x04,              /* group: CCMP        */
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,  /* pairwise: CCMP     */
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,  /* AKM: PSK           */
    0x00, 0x00
};

static void handshake_rx(const u8 *f, u32 len)
{
    if (len < 4 + 95 || f[1] != EAPOL_KEY)
        return;
    const u8 *k = f + 4;
    u16 info = (u16)((k[1] << 8) | k[2]);
    u32 klen = (u32)((k[93] << 8) | k[94]);
    if (4 + 95 + klen > len)
        return;

    if ((info & KEY_INFO_ACK) && !(info & KEY_INFO_MIC)) {
        /* Message 1: the AP's nonce. Derive the pairwise key and answer
         * with ours, the station's RSN element and a MIC. */
        W->m1_info = info;
        memcpy(s_hs.anonce, k + 13, 32);
        memcpy(s_hs.replay, k + 5, 8);
        for (u32 i = 0; i < 32; i += 4)
            put32(s_hs.snonce + i, prand());
        u8 pmk[32];
        wpa_pmk((const char *)W->join_psk, slen((const char *)W->join_psk),
                (const u8 *)W->join_ssid, slen((const char *)W->join_ssid), pmk);
        wpa_ptk(pmk, s_hs.bssid, (const u8 *)W->mac, s_hs.anonce, s_hs.snonce,
                s_hs.ptk);
        s_hs.have_m1 = true;
        s_hs.m2_pending = true;
        set_step(WIFI_ST_4WAY);
        return;
    }

    if ((info & KEY_INFO_MIC) && (info & KEY_INFO_ACK) && s_hs.have_m1) {
        /* Message 3. Check its MIC with the key just derived - a wrong
         * passphrase shows up here and nowhere earlier - then answer. */
        u8 want[16], got[16], tmp[4 + 95 + 256];
        u32 n = 4 + 95 + klen;
        if (n > sizeof(tmp))
            return;
        memcpy(tmp, f, n);
        memcpy(got, tmp + 4 + 77, 16);
        memset(tmp + 4 + 77, 0, 16);
        wpa_mic(s_hs.ptk, tmp, n, want);
        for (int i = 0; i < 16; i++)
            if (want[i] != got[i]) {
                s_hs.failed = true;
                W->join_reason = 1;        /* the passphrase is wrong */
                return;
            }
        memcpy(s_hs.replay, k + 5, 8);
        s_hs.m3_seen++;
        s_hs.m4_pending = true;
    }
}

/* ---- completions -------------------------------------------------------- */

static struct {
    bool  done;
    u16   trans_id;
    s16   status;
    u16   len;
    u32   pktid;
} s_ioctl;
static u16 s_trans_id;
static bool s_scan_done;
static u32 s_scan_status;

static void bss_add(const u8 *b, u32 len)
{
    if (len < 82)
        return;
    const u8 *bssid = b + 8;
    u32 slot = W->nbss;
    for (u32 i = 0; i < W->nbss; i++) {
        bool same = true;
        for (int k = 0; k < 6; k++)
            if (W->bss[i].bssid[k] != bssid[k])
                same = false;
        if (same) {
            slot = i;
            break;
        }
    }
    if (slot == WIFI_BSS_MAX)
        return;
    wifi_bss_t *e = &W->bss[slot];
    short rssi = (short)get16(b + 78);
    if (slot == W->nbss) {
        memset(e, 0, sizeof(*e));
        for (int k = 0; k < 6; k++)
            e->bssid[k] = bssid[k];
        W->nbss = slot + 1;
        e->rssi = rssi;
    } else if (rssi > e->rssi) {
        e->rssi = rssi;
    }
    u8 sl = b[18];
    if (sl > 32)
        sl = 32;
    e->ssid_len = sl;
    for (u32 k = 0; k < sl; k++)
        e->ssid[k] = b[19 + k];
    e->ssid[sl] = 0;
    e->chanspec = get16(b + 72);
    u8 ch = (b[81] && b[88]) ? b[88] : (u8)(e->chanspec & 0xFF);
    e->channel = ch;
    e->flags = ((get16(b + 16) & 0x10) ? 1 : 0) | (ch > 14 ? 2 : 0);
    e->seen++;
}

#define E_SET_SSID       0u
#define E_DEAUTH_IND     6u
#define E_DISASSOC_IND  12u
#define E_LINK          16u
#define E_EAPOL_MSG     25u
#define E_PSK_SUP       46u
#define E_IF            54u
#define E_ESCAN_RESULT  69u
#define E_STATUS_SUCCESS 0u
#define E_STATUS_PARTIAL 8u
/* brcmfmac: the flags word's bit 0 on an E_LINK means the link came up. */
#define E_FLAG_LINK      1u
#define WLC_SUP_KEYED    6u   /* the supplicant finished the handshake    */

static bool s_link_up, s_keyed, s_join_failed;

static void handle_event(const u8 *p, u32 len)
{
    /* ether header, brcm header (OUI 00:10:18, user subtype 1), event. */
    if (len < 72 || get16be(p + 12) != 0x886C ||
        p[19] != 0x00 || p[20] != 0x10 || p[21] != 0x18 ||
        get16be(p + 22) != 1)
        return;
    u32 flags = get16be(p + 26);
    u32 type = get32be(p + 28);
    u32 status = get32be(p + 32);
    u32 reason = get32be(p + 36);
    u32 dlen = get32be(p + 44);
    const u8 *d = p + 72;
    if (dlen > len - 72)
        dlen = len - 72;
    /* Which types this firmware actually sends, so an event we are not
     * listening for stops being invisible. */
    switch (type) {
    case E_EAPOL_MSG:
        /* Some firmware hands the key frames up as an event rather than as
         * a data frame on the receive ring; both are taken. */
        W->rx_eapol++;
        handshake_rx(d, dlen);
        return;
    case E_LINK:
        if (flags & E_FLAG_LINK) {
            s_link_up = true;
            for (int i = 0; i < 6; i++)
                W->join_bssid[i] = p[48 + i];
            /* The pairwise key is derived over this address, and the
             * AP's first key frame can arrive in the same breath as the
             * link event - so take it here, not when the join loop gets
             * round to it. */
            memcpy(s_hs.bssid, (const void *)W->join_bssid, 6);
        } else if (s_link_up) {
            s_link_up = false;            /* it went away again */
            s_join_failed = true;
            W->join_reason = reason;
        }
        return;
    case E_SET_SSID:
        W->join_status = (int)status;
        if (status != E_STATUS_SUCCESS)
            s_join_failed = true;
        return;
    case E_PSK_SUP:
        if (status == WLC_SUP_KEYED)
            s_keyed = true;
        else if (status > WLC_SUP_KEYED)  /* the failure codes are above it */
            s_join_failed = true;
        return;
    case E_DEAUTH_IND:
    case E_DISASSOC_IND:
        W->join_reason = reason;
        s_join_failed = true;
        return;
    default:
        break;
    }
    if (type != E_ESCAN_RESULT)
        return;
    W->events++;
    if (status == E_STATUS_PARTIAL) {
        if (dlen < 12)
            return;
        u32 count = get16(d + 10);
        const u8 *b = d + 12;
        u32 left = dlen - 12;
        for (u32 i = 0; i < count && left >= 128; i++) {
            u32 blen = get32(b + 4);
            if (blen < 128 || blen > left)
                break;
            bss_add(b, blen);
            b += blen;
            left -= blen;
        }
    } else {
        s_scan_done = true;
        s_scan_status = status;
    }
}

static void handle_msg(const u8 *m)
{
    u32 pktid = get32(m + 4);
    switch (m[0]) {
    case MSG_IOCTL_CMPLT:
        if (get16(m + 14) != s_trans_id) {
            /* A late answer to an ioctl that already timed out: give
             * its buffer back and wait on. */
            u32 idx = pktid - PKT_IOCTLRESP;
            if (idx < N_IOCTLRESP)
                post_ctrl_buf(MSG_IOCTLRESP_BUF_POST, pktid, s_resp_buf[idx]);
            break;
        }
        s_ioctl.status = (s16)get16(m + 8);
        s_ioctl.len = get16(m + 12);
        s_ioctl.trans_id = get16(m + 14);
        s_ioctl.pktid = pktid;
        s_ioctl.done = true;
        break;
    case MSG_WL_EVENT: {
        u32 idx = pktid - PKT_EVENT;
        if (idx < N_EVENT) {
            u32 elen = get16(m + 12);
            if (elen > CTL_BUF_SIZE - s_rx_dataoffset)
                elen = CTL_BUF_SIZE - s_rx_dataoffset;
            handle_event(s_event_buf[idx] + s_rx_dataoffset, elen);
            post_ctrl_buf(MSG_EVENT_BUF_POST, pktid, s_event_buf[idx]);
        }
        break;
    }
    case MSG_FLOW_CREATE_CMPLT:
        /* Status 0 and the ring is ours to post on. */
        if (get16(m + 8) == 0)
            s_txring_up = true;
        break;
    case MSG_RX_CMPLT: {
        u32 idx = pktid - PKT_RXDATA;
        if (idx < N_RXDATA) {
            u32 dlen = get16(m + 14);
            u32 doff = get16(m + 16);
            if (!doff)
                doff = s_rx_dataoffset;
            /* The only frame this payload has any use for is EAPOL: the
             * handshake the firmware will not do for us. */
            if (dlen > 14 && doff + dlen <= RX_BUF_SIZE) {
                const u8 *fr = s_rx_buf[idx] + doff;
                W->rx_frames++;
                /* Ethernet II, and the LLC/SNAP form some firmware hands
                 * up instead: AA AA 03 00 00 00 and then the ethertype. */
                u32 hdr = 0;
                if (get16be(fr + 12) == ETHERTYPE_EAPOL)
                    hdr = 14;
                else if (dlen > 22 && get16be(fr + 12) <= 1500 &&
                         fr[14] == 0xAA && fr[15] == 0xAA && fr[16] == 0x03 &&
                         fr[17] == 0 && fr[18] == 0 && fr[19] == 0 &&
                         get16be(fr + 20) == ETHERTYPE_EAPOL)
                    hdr = 22;
                if (hdr) {
                    W->rx_eapol++;
                    handshake_rx(fr + hdr, dlen - hdr);
                } else if (s_net_up) {
                    net_rx(&s_net, fr, dlen);
                }
            }
            (void)post_rx_buf(pktid, s_rx_buf[idx]);
        }
        break;
    }
    default:
        break;     /* REQ_ACK, GEN/RING_STATUS, TX_STATUS: nothing to do */
    }
}

static void poll_d2h(void)
{
    for (u32 i = RING_CTRL_COMPLETE; i < NRINGS; i++) {
        ring_t *rg = &s_ring[i];
        u16 w = tcm16(rg->w_idx_addr);
        /* For a failed join's report: a ring the firmware never wrote is
         * a different fault from one this side failed to read. */
        W->ring_idx[2 * (i - RING_CTRL_COMPLETE)] = w;
        W->ring_idx[2 * (i - RING_CTRL_COMPLETE) + 1] = rg->r;
        if (w >= rg->depth)
            continue;
        u32 n = 0;
        while (rg->r != w && n++ < rg->depth) {
            /* The item itself: rx_dataoffset is into a receive buffer,
             * and MSG_RX_CMPLT applies it there. */
            handle_msg(rg->buf + (u32)rg->r * rg->item);
            if (++rg->r == rg->depth)
                rg->r = 0;
        }
        if (n)
            tcm16_w(rg->r_idx_addr, rg->r);
    }
    /* And the two this side submits on: whether the chip took what was
     * posted at all. */
    W->ring_idx[6] = s_ring[RING_CTRL_SUBMIT].w;
    W->ring_idx[7] = tcm16(s_ring[RING_CTRL_SUBMIT].r_idx_addr);
    W->ring_idx[8] = s_ring[RING_RXPOST].w;
    W->ring_idx[9] = tcm16(s_ring[RING_RXPOST].r_idx_addr);
}

/* ---- ioctls (msgbuf.c, fwil.c) ------------------------------------------ */

#define C_UP            2u
#define C_SET_INFRA     20u
#define C_SET_AUTH      22u
#define C_SET_SSID      26u
#define C_SET_KEY       45u
#define C_SET_WPA_AUTH  165u
#define C_GET_VAR       262u
#define C_SET_VAR       263u
#define C_SET_WSEC_PMK  268u
#define WPA2_AUTH_PSK   0x0080u
#define WSEC_AES        0x0004u
#define WSEC_PASSPHRASE 1u
#define BCME_UNSUPPORTED 23   /* the firmware's -23, as ioctl() returns it */

/* One ioctl: `len` bytes of buf out, up to `len` back into it. 0 is
 * success, -1 no completion, >0 the firmware's error. */
static int ioctl(u32 cmd, u8 *buf, u32 len)
{
    ring_t *rg = &s_ring[RING_CTRL_SUBMIT];
    u8 *it = ring_reserve(rg);
    if (!it)
        return -1;
    u32 in = len < IOCTL_MAX_TX ? len : IOCTL_MAX_TX;
    memcpy(s_ioctbuf, buf, in);
    s_trans_id++;
    it[0] = MSG_IOCTLPTR_REQ;
    put32(it + 4, IOCTL_REQ_PKTID);
    put32(it + 8, cmd);
    put16(it + 12, s_trans_id);
    put16(it + 14, in);
    put16(it + 16, len);
    put32(it + 24, (u32)(unsigned long)s_ioctbuf);
    put32(it + 28, 0);
    s_ioctl.done = false;
    ring_commit(rg);

    for (u32 t = 0; t < 2000 && !s_ioctl.done; t++) {
        poll_d2h();
        if (s_ioctl.done)
            break;
        udelay(1000);
        if (!(t & 15))
            hb();
    }
    if (!s_ioctl.done)
        return -1;
    u32 idx = s_ioctl.pktid - PKT_IOCTLRESP;
    if (idx < N_IOCTLRESP) {
        u32 n = s_ioctl.len < len ? s_ioctl.len : len;
        memcpy(buf, s_resp_buf[idx], n);
        post_ctrl_buf(MSG_IOCTLRESP_BUF_POST, s_ioctl.pktid, s_resp_buf[idx]);
    }
    W->fwerr = s_ioctl.status;
    /* The firmware's error codes are negative (BCME_*); report the size. */
    return s_ioctl.status < 0 ? -s_ioctl.status : s_ioctl.status;
}

static u8 s_iov[1536];

static int iovar_get(const char *name, void *out, u32 len)
{
    u32 nl = slen(name) + 1;
    u32 tot = nl > len ? nl : len;
    if (tot > sizeof(s_iov))
        return -1;
    memset(s_iov, 0, tot);
    memcpy(s_iov, name, nl);
    int r = ioctl(C_GET_VAR, s_iov, tot);
    if (!r)
        memcpy(out, s_iov, len);
    return r;
}

static int iovar_set(const char *name, const void *data, u32 len)
{
    u32 nl = slen(name) + 1;
    if (nl + len > sizeof(s_iov))
        return -1;
    memcpy(s_iov, name, nl);
    memcpy(s_iov + nl, data, len);
    return ioctl(C_SET_VAR, s_iov, nl + len);
}

static int cmd_int(u32 cmd, u32 v)
{
    u8 b[4];
    put32(b, v);
    return ioctl(cmd, b, 4);
}

/* ---- the firmware's console (pcie.c, console_read) ------------------ */

static u32 s_console_base;

static void console_grab(void)
{
    if (!s_console_base)
        return;
    u32 buf = tcm32(s_console_base + 8);
    u32 size = tcm32(s_console_base + 12);
    u32 widx = tcm32(s_console_base + 16);
    if (!size || size > 0x10000 || widx >= size || buf + size > W->bar2_size)
        return;
    u32 n = size < WIFI_CONSOLE_MAX - 1 ? size : WIFI_CONSOLE_MAX - 1;
    u32 start = (widx + size - n) % size;
    u32 o = 0;
    for (u32 i = 0; i < n; i++) {
        u8 c = rd8(PCI_BAR2_BASE + buf + (start + i) % size);
        if (c == '\r' || (c == 0 && o == 0))
            continue;
        W->console[o++] = c ? (char)c : '\n';
    }
    W->console[o] = 0;
    W->console_len = o;
}

/* ---- finishing -------------------------------------------------------- */

static void __attribute__((noreturn)) finish(u32 result, u32 detail,
                                             const char *why)
{
    W->result = result;
    W->detail = detail;
    char *p = W->why;
    p = put_s(p, why);
    if (detail || result) {
        p = put_s(p, " (");
        p = put_x(p, detail);
        p = put_s(p, ")");
    }
    console_grab();
    if (result == WIFI_OK)
        set_step(WIFI_ST_DONE);
    /* Everything the supervisor is about to read, out of the cache before
     * the flag that tells it to read. */
    mbox_flush();
    W->done = WIFI_MBOX_DONE;
    __asm__ volatile("dc cvac, %0" :: "r"(&W->done) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    for (;;)
        __asm__ volatile("wfe");
}

/* ---- the run -------------------------------------------------------- */

#define SHARED_DMA_INDEX       0x10000u
#define SHARED_HOSTRDY_DB1     0x10000000u
#define RAMSIZE_MAGIC          0x534D4152u   /* SMAR */

void wifi_main(cpu_mbox_t *m)
{
    W->magic = WIFI_MBOX_MAGIC;
    set_step(WIFI_ST_ENUM);
    if (m->port != 1u || m->ep_id != 0x43EC14E4u)
        finish(WIFI_E_NOLINK, m->ep_id,
               "stub did not enumerate it");
    if (W->in_magic != WIFI_MBOX_MAGIC || !W->fw_len || !W->nvram_len ||
        W->fw_len > WIFI_FW_MAX || W->nvram_len > WIFI_NVRAM_MAX)
        finish(WIFI_E_INPUT, W->fw_len, "no firmware handed over");

    /* 1. BARs, the bridge window over them, memory and bus master on. */
    set_step(WIFI_ST_BARS);
    u32 bar0 = bar_assign(0x10, PCI_BAR0_BASE);
    if (bar0 < 0x8000u)
        finish(WIFI_E_BAR, bar0, "BAR0 would not take a base");
    u32 bar2 = bar_assign(0x18, PCI_BAR2_BASE);
    if (bar2 < 0x200000u || bar2 > PCI_BAR2_MAX)
        finish(WIFI_E_BAR, bar2, "BAR2 (TCM) took no base");
    W->bar0 = PCI_BAR0_BASE;
    W->bar2 = PCI_BAR2_BASE;
    W->bar2_size = bar2;
    wr32(PCIE_RP1_BASE + 0x20u,
         ((PCI_BAR2_BASE + bar2 - 1u) & 0xFFF00000u) |
         ((PCI_BAR0_BASE >> 16) & 0xFFF0u));
    if (cfg_set(0x04, RP_COMMAND_MEM | RP_COMMAND_BME, 0x0006u))
        finish(WIFI_E_BAR, 0, "cmd reg took no MEM|BME");

    /* 2. Chip ID and the cores. */
    set_step(WIFI_ST_CHIPID);
    u32 cid = bp_rd(0x18000000u);
    W->chipid = cid;
    if (s_bp_err)
        finish(WIFI_E_WINDOW, 0x18000000u, "backplane window stuck");
    u32 chip = cid & 0xFFFFu;
    if (cid == 0xFFFFFFFFu || ((cid >> 28) & 0xF) != 1u ||
        (chip != 0x4356u && chip != 0x4354u && chip != 43567u &&
         chip != 43569u))
        finish(WIFI_E_CHIPID, cid, "chip ID is not a 4356");

    set_step(WIFI_ST_EROM);
    erom_scan(bp_rd(0x18000000u + 0xFCu));
    const wifi_core_t *cr4 = core_get(CORE_CR4, 0);
    const wifi_core_t *p2 = core_get(CORE_PCIE2, 0);
    if (!cr4 || !p2 || !core_get(CORE_CC, 0) || s_bp_err)
        finish(WIFI_E_EROM, W->ncores, "EROM: cores missing");
    W->pcie2_rev = p2->rev;
    if (p2->rev >= 64) {
        s_mb0 = PCIE2_64_H2D_MB0;
        s_mb1 = PCIE2_64_H2D_MB1;
    }

    /* 3. Passive, reset, passive again, BAR1 sizing rewrite. */
    set_step(WIFI_ST_HALT);
    chip_passive();
    set_step(WIFI_ST_RESET);
    chip_reset();
    {
        u32 r = p2->rev >= 64 ? PCIE2_64_MAILBOXINT : PCIE2_MAILBOXINT;
        u32 v = bp_rd(p2->base + r);
        if (v != 0xFFFFFFFFu)
            bp_wr(p2->base + r, v);
    }
    chip_passive();
    bp_wr(p2->base + PCIE2_CONFIGADDR, 0x4E0);
    bp_wr(p2->base + PCIE2_CONFIGDATA, bp_rd(p2->base + PCIE2_CONFIGDATA));
    if (s_bp_err)
        finish(WIFI_E_WINDOW, 0, "window lost after reset");

    /* 4. RAM, download, release, shared RAM. */
    set_step(WIFI_ST_RAMINFO);
    const u32 *fw = (const u32 *)(unsigned long)WIFI_FW_ADDR;
    u32 cap = bp_rd(cr4->base + ARMCR4_CAP);
    u32 nb = (cap & 0xFu) + ((cap >> 4) & 0xFu);
    u32 ramsize = 0;
    for (u32 i = 0; i < nb && i < 16; i++) {
        bp_wr(cr4->base + ARMCR4_BANKIDX, i);
        u32 info = bp_rd(cr4->base + ARMCR4_BANKINFO);
        u32 blk = (info & 0x200u) ? 1024u : 8192u;
        ramsize += ((info & 0x7Fu) + 1u) * blk;
    }
    u32 rambase = 0x180000u;
    if (W->fw_len >= 0x78 && fw[0x6C / 4] == RAMSIZE_MAGIC)
        ramsize = fw[0x70 / 4];
    W->rambase = rambase;
    W->ramsize = ramsize;
    if (ramsize < 0x40000u || ramsize > 0x400000u ||
        rambase + ramsize > bar2 || W->fw_len + W->nvram_len > ramsize)
        finish(WIFI_E_RAM, ramsize, "RAM size not plausible");

    set_step(WIFI_ST_DOWNLOAD);
    u32 nw = (W->fw_len + 3) / 4;
    for (u32 i = 0; i < nw; i++) {
        u32 v = fw[i];
        if (i == nw - 1 && (W->fw_len & 3))
            v &= 0xFFFFFFFFu >> (8 * (4 - (W->fw_len & 3)));
        tcm32_w(rambase + 4 * i, v);
        if (!(i & 1023))
            hb();
    }
    /* Read back: the first and last KiB and every 64th word between. A
     * word that reads wrong twice is a write the chip did not take. */
    u32 bad = 0;
    for (u32 i = 0; i < nw; i++) {
        if (i >= 256 && i + 256 < nw && (i & 63))
            continue;
        u32 v = fw[i];
        if (i == nw - 1 && (W->fw_len & 3))
            v &= 0xFFFFFFFFu >> (8 * (4 - (W->fw_len & 3)));
        if (tcm32(rambase + 4 * i) != v && tcm32(rambase + 4 * i) != v)
            bad++;
    }
    W->verify_bad = bad;
    if (bad)
        finish(WIFI_E_VERIFY, bad, "firmware readback failed");

    tcm32_w(rambase + ramsize - 4, 0);
    const u32 *nv = (const u32 *)(unsigned long)WIFI_NVRAM_ADDR;
    u32 nvaddr = rambase + ramsize - W->nvram_len;
    for (u32 i = 0; i < W->nvram_len / 4; i++)
        tcm32_w(nvaddr + 4 * i, nv[i]);
    u32 written = tcm32(rambase + ramsize - 4);
    u32 resetintr = fw[0];
    W->resetintr = resetintr;

    set_step(WIFI_ST_BOOT);
    tcm32_w(0, resetintr);
    core_reset(cr4, CR4_IOCTL_CPUHALT, 0, 0);
    u32 shared = written;
    u32 t;
    for (t = 0; t < 5000 && shared == written; t += 10) {
        msleep(10);
        shared = tcm32(rambase + ramsize - 4);
    }
    W->fw_up_ms = t;
    if (shared == written)
        finish(WIFI_E_FWUP, written, "firmware did not start");
    W->shared_addr = shared;
    if (shared < rambase || shared >= rambase + ramsize)
        finish(WIFI_E_SHARED, shared, "shared RAM out of range");

    set_step(WIFI_ST_SHARED);
    u32 flags = tcm32(shared);
    W->shared_flags = flags;
    u32 ver = flags & 0xFFu;
    if (ver < 5 || ver > 7)
        finish(WIFI_E_SHARED, flags, "unsupported shared ver");
    /* How many receive buffers the firmware will hold. brcmfmac never
     * posts more than this, and overfilling a ring the chip is sizing
     * itself is not a thing to do blind. 0 means it has no preference. */
    s_max_rxbufpost = tcm16(shared + 34);
    W->max_rxbufpost = s_max_rxbufpost;
    s_rx_dataoffset = tcm32(shared + 36);
    if (s_rx_dataoffset > 256)
        finish(WIFI_E_SHARED, s_rx_dataoffset, "rx data offset not plausible");
    s_console_base = tcm32(shared + 20);
    if (s_console_base >= rambase + ramsize)
        s_console_base = 0;
    u32 ri = tcm32(shared + 48);
    if (ri < rambase || ri >= rambase + ramsize)
        finish(WIFI_E_RINGS, ri, "ring info out of range");

    /* 5. Rings (TCM indices: brcmfmac's fallback, which every firmware
     * of this protocol supports), scratch buffers, host-ready. */
    set_step(WIFI_ST_RINGS);
    u32 ringmem = tcm32(ri + 0);
    u32 h2d_w = tcm32(ri + 4), h2d_r = tcm32(ri + 8);
    s_h2d_w = h2d_w;
    s_h2d_r = h2d_r;
    u32 d2h_w = tcm32(ri + 12), d2h_r = tcm32(ri + 16);
    if (ringmem < rambase || ringmem >= rambase + ramsize ||
        h2d_w >= rambase + ramsize || d2h_w >= rambase + ramsize)
        finish(WIFI_E_RINGS, ringmem, "ring pointers out of range");
    const u16 *isz = ver < 7 ? ring_item_pre7 : ring_item_v7;
    for (u32 i = 0; i < NRINGS; i++) {
        ring_t *rg = &s_ring[i];
        rg->depth = ring_depth[i];
        rg->item = isz[i];
        rg->buf = dma_alloc((u32)rg->depth * rg->item, 4096);
        if (!rg->buf)
            finish(WIFI_E_RINGS, i, "out of DMA memory");
        u32 desc = ringmem + 16u * i;
        tcm32_w(desc + 8, (u32)(unsigned long)rg->buf);
        tcm32_w(desc + 12, 0);
        tcm16_w(desc + 4, rg->depth);
        tcm16_w(desc + 6, rg->item);
        if (i < 2) {
            rg->w_idx_addr = h2d_w + 4u * i;
            rg->r_idx_addr = h2d_r + 4u * i;
        } else {
            rg->w_idx_addr = d2h_w + 4u * (i - 2);
            rg->r_idx_addr = d2h_r + 4u * (i - 2);
        }
        rg->w = rg->r = 0;
    }
    u8 *scratch = dma_alloc(8, 64);
    u8 *ringupd = dma_alloc(1024, 64);
    s_ioctbuf = dma_alloc(2048, 64);
    if (!scratch || !ringupd || !s_ioctbuf)
        finish(WIFI_E_RINGS, 0, "out of DMA memory");
    tcm32_w(shared + 56, (u32)(unsigned long)scratch);
    tcm32_w(shared + 60, 0);
    tcm32_w(shared + 52, 8);
    tcm32_w(shared + 68, (u32)(unsigned long)ringupd);
    tcm32_w(shared + 72, 0);
    tcm32_w(shared + 64, 1024);

    /* From here BAR0 stays on the PCIe2 core: the doorbells live there. */
    if (bp_window(p2->base))
        finish(WIFI_E_WINDOW, p2->base, "PCIe2 window stuck");
    if (flags & SHARED_HOSTRDY_DB1)
        reg32_w(s_mb1, 1);

    for (u32 i = 0; i < N_RXDATA; i++) {
        s_rx_buf[i] = dma_alloc(RX_BUF_SIZE, 64);
        if (!s_rx_buf[i])
            finish(WIFI_E_RINGS, i, "out of DMA memory");
    }
    for (u32 i = 0; i < N_EVENT; i++) {
        s_event_buf[i] = dma_alloc(CTL_BUF_SIZE, 64);
        if (!s_event_buf[i])
            finish(WIFI_E_RINGS, i, "out of DMA memory");
        post_ctrl_buf(MSG_EVENT_BUF_POST, PKT_EVENT + i, s_event_buf[i]);
    }
    for (u32 i = 0; i < N_IOCTLRESP; i++) {
        s_resp_buf[i] = dma_alloc(CTL_BUF_SIZE, 64);
        if (!s_resp_buf[i])
            finish(WIFI_E_RINGS, i, "out of DMA memory");
        post_ctrl_buf(MSG_IOCTLRESP_BUF_POST, PKT_IOCTLRESP + i, s_resp_buf[i]);
    }

    /* 6. The firmware's own view, then the scan. */
    set_step(WIFI_ST_IOCTL);
    int r = iovar_get("ver", W->fw_ver, sizeof(W->fw_ver) - 1);
    if (r < 0)
        finish(WIFI_E_IOCTL, 0, "no reply to ioctl ver");
    W->fw_ver[sizeof(W->fw_ver) - 1] = 0;
    for (u32 i = 0; i < sizeof(W->fw_ver); i++)
        if (W->fw_ver[i] == '\n' || W->fw_ver[i] == '\r')
            W->fw_ver[i] = ' ';
    (void)iovar_get("cur_etheraddr", W->mac, 6);

    set_step(WIFI_ST_UP);
    {
        u8 mpc[4];
        put32(mpc, 0);
        (void)iovar_set("mpc", mpc, 4);
        u8 mask[16];
        memset(mask, 0, sizeof(mask));
        static const u8 want[] = { E_ESCAN_RESULT, E_IF, E_LINK, E_SET_SSID,
                                   E_PSK_SUP, E_DEAUTH_IND, E_DISASSOC_IND,
                                   E_EAPOL_MSG };
        for (u32 i = 0; i < sizeof(want); i++)
            mask[want[i] / 8] |= 1u << (want[i] % 8);
        r = iovar_set("event_msgs", mask, sizeof(mask));
        if (r)
            finish(r < 0 ? WIFI_E_IOCTL : WIFI_E_FWERR, (u32)r,
                   "the firmware refused event_msgs");
        (void)cmd_int(C_SET_INFRA, 1);
        r = cmd_int(C_UP, 1);
        if (r)
            finish(r < 0 ? WIFI_E_IOCTL : WIFI_E_FWERR, (u32)r,
                   "the firmware refused UP");
    }

    /* The receive buffers go in now, not with the rings: posting reads the
     * firmware's read index, which is stale until it has started, and a
     * stale one makes the ring look full and drops every post. */
    {
        u32 want = s_max_rxbufpost ? s_max_rxbufpost : N_RXDATA;
        if (want > N_RXDATA)
            want = N_RXDATA;
        for (u32 i = 0; i < want; i++)
            if (!post_rx_buf(PKT_RXDATA + i, s_rx_buf[i]))
                break;
    }

    /* The radio has just come up: a scan issued in the same breath can
     * come back with nothing at all on a chip that is still settling,
     * which is what an empty result usually turns out to be. */
    msleep(300);

    /* A join skips the escan: WLC_SET_SSID scans for the BSSID itself. */
    bool joining = (W->in_magic == WIFI_MBOX_MAGIC && W->join_req &&
                    W->join_ssid[0]);
    u32 limit = W->scan_time_ms ? W->scan_time_ms : 12000;
    if (joining)
        goto join;
    set_step(WIFI_ST_SCAN);
    /* An empty scan is retried: a single escan that hears no beacon
     * proves nothing, and three say something. */
    for (W->scan_tries = 1; ; W->scan_tries++) {
        /* brcmf_escan_params_le, v1, no SSID, every channel: 72 bytes. */
        u8 p[72];
        memset(p, 0, sizeof(p));
        put32(p + 0, 1);                 /* BRCMF_ESCAN_REQ_VERSION   */
        put16(p + 4, 1);                 /* WL_ESCAN_ACTION_START     */
        put16(p + 6, 0x1234);            /* sync id                   */
        for (int i = 0; i < 6; i++)
            p[44 + i] = 0xFF;            /* broadcast BSSID           */
        p[50] = 2;                       /* DOT11_BSSTYPE_ANY         */
        p[51] = 0;                       /* active                    */
        put32(p + 52, 0xFFFFFFFFu);      /* nprobes: default          */
        put32(p + 56, 0xFFFFFFFFu);      /* active time               */
        put32(p + 60, 0xFFFFFFFFu);      /* passive time              */
        put32(p + 64, 0xFFFFFFFFu);      /* home time                 */
        put32(p + 68, 0);                /* every channel             */
        put16(p + 6, 0x1234 + W->scan_tries);   /* a sync id per try */
        s_scan_done = false;
        r = iovar_set("escan", p, sizeof(p));
        if (r)
            finish(r < 0 ? WIFI_E_IOCTL : WIFI_E_FWERR, (u32)r,
                   "the firmware refused escan");

        for (t = 0; t < limit && !s_scan_done; t++) {
            poll_d2h();
            udelay(1000);
            if (!(t & 15))
                hb();
        }
        W->scan_ms += t;
        /* Anything heard, or out of tries: take what there is. */
        if (W->nbss || W->scan_tries >= 3)
            break;
        msleep(500);
    }
    if (!s_scan_done && !W->nbss)
        finish(WIFI_E_SCANTIME, t, "no final escan event in time");
    if (!W->nbss && s_scan_status != E_STATUS_SUCCESS)
        finish(WIFI_E_SCAN, s_scan_status, "escan ended with an error");

    /* ---- the join ------------------------------------------------------
     *
     * The scan exercises the receive path only. Associating adds the
     * transmitter, the rates and, with a passphrase, the four-way
     * handshake; the network below then proves the chip carries data. */
join:
    if (joining) {
        set_step(WIFI_ST_JOIN);
        u32 nl = slen(W->join_ssid);
        if (nl > 32)
            nl = 32;
        bool psk = W->join_psk[0] != 0;
        bool sw_sup = false;
        W->rx_frames = W->rx_eapol = W->tx_eapol = 0;
        u32 pl = slen(W->join_psk);
        if (pl > 63)
            pl = 63;

        if (psk) {
            u8 v[4];
            put32(v, WSEC_AES);
            W->join_rc[0] = iovar_set("wsec", v, 4);
            W->join_rc[1] = cmd_int(C_SET_WPA_AUTH, WPA2_AUTH_PSK);
            put32(v, 1);
            W->join_rc[2] = iovar_set("sup_wpa", v, 4); /* its own one */
            /* wsec_pmk_t: length, flags, then the passphrase itself. */
            u8 pmk[68];
            memset(pmk, 0, sizeof(pmk));
            put16(pmk, (u16)pl);
            put16(pmk + 2, WSEC_PASSPHRASE);
            memcpy(pmk + 4, (const void *)W->join_psk, pl);
            r = ioctl(C_SET_WSEC_PMK, pmk, sizeof(pmk));
            W->join_rc[3] = r;
            /* Unsupported: no supplicant in this firmware (Nintendo's has
             * none), so the handshake is ours once the link is up. Any
             * other refusal is real. */
            if (r == BCME_UNSUPPORTED) {
                sw_sup = true;
                /* And sup_wpa back off: left on, the firmware keeps the
                 * AP's key frames for a supplicant that is not there. */
                put32(v, 0);
                W->join_rc[4] = iovar_set("sup_wpa", v, 4);
            } else if (r)
                finish(r < 0 ? WIFI_E_IOCTL : WIFI_E_FWERR, (u32)r,
                       "the firmware refused the key");
        } else {
            u8 v[4];
            put32(v, 0);
            (void)iovar_set("wsec", v, 4);
            (void)cmd_int(C_SET_WPA_AUTH, 0);
        }
        (void)cmd_int(C_SET_AUTH, 0);           /* open system */
        (void)cmd_int(C_SET_INFRA, 1);

        u8 j[36];
        memset(j, 0, sizeof(j));
        put32(j, nl);
        memcpy(j + 4, (const void *)W->join_ssid, nl);
        r = ioctl(C_SET_SSID, j, sizeof(j));
        if (r)
            finish(r < 0 ? WIFI_E_IOCTL : WIFI_E_FWERR, (u32)r,
                   "the firmware refused the join");

        for (t = 0; t < 15000 && !s_join_failed; t++) {
            poll_d2h();
            if (s_link_up) {
                if (!W->link_up) {
                    W->link_up = 1;
                    W->join_ms = t;
                    set_step(WIFI_ST_LINKED);
                }
                if (!psk || s_keyed)
                    break;
            }
            udelay(1000);
            if (!(t & 15))
                hb();
        }
        W->keyed = s_keyed ? 1 : 0;
        if (!W->link_up)
            finish(WIFI_E_JOIN, W->join_reason ? W->join_reason : (u32)t,
                   s_join_failed ? "the network refused us" :
                                   "no link to that network");

        /* Associated, and the key is ours to agree: a flow ring to talk
         * on, then the four messages. The AP starts, so this is a poll
         * for its first one and then for the rest. */
        if (psk && !s_keyed && sw_sup) {
            W->sw_sup = 1;
            fetch_assoc_rsn();
            memcpy(s_hs.bssid, (const void *)W->join_bssid, 6);
            if (flowring_create(s_hs.bssid))
                finish(WIFI_E_4WAY, 0, "no flow ring to the AP");
            for (t = 0; t < 8000 && !s_hs.done && !s_hs.failed; t++) {
                poll_d2h();
                /* Whatever the last frame asked for, now that there is
                 * a ring to answer on. */
                if (s_hs.m2_pending) {
                    s_hs.m2_pending = false;
                    eapol_send((u16)((W->m1_info & 7u) | KEY_INFO_PAIRWISE |
                                     KEY_INFO_MIC),
                               s_hs.snonce,
                               s_rsn_len ? s_rsn : kRsnIe,
                               s_rsn_len ? s_rsn_len : sizeof(kRsnIe), true);
                } else if (s_hs.m4_pending) {
                    s_hs.m4_pending = false;
                    eapol_send((u16)((W->m1_info & 7u) | KEY_INFO_PAIRWISE |
                                     KEY_INFO_MIC | KEY_INFO_SECURE),
                               0, 0, 0, true);
                    s_hs.done = true;
                }
                udelay(1000);
                if (!(t & 15))
                    hb();
            }
            W->join_ms += t;
            if (s_hs.failed)
                finish(WIFI_E_PSK, W->join_reason, "wrong passphrase");
            if (!s_hs.done)
                /* An AP that does not like message 2's MIC says nothing
                 * at all - a wrong passphrase looks exactly like this,
                 * and it is the likeliest reason by far. */
                finish(WIFI_E_4WAY, s_hs.have_m1 ? 3 : 1,
                       s_hs.have_m1 ? "no answer to msg 2: bad key?"
                                    : "the AP sent no key frame");
            /* The pairwise key into the chip; the group key stays wrapped
             * and unused. brcmf_wsec_key_le is not as packed as it looks:
             * index 0, len 4, data 8, algo 112, flags 116 (0 for a pairwise
             * key; BRCMF_PRIMARY_KEY is the group key's) and the peer's
             * address at 156, past iv_initialized, the receive IV and two
             * pad words. */
            u8 key[164];
            memset(key, 0, sizeof(key));
            put32(key + 0, 0);                      /* key index         */
            put32(key + 4, 16);                     /* CCMP key length   */
            memcpy(key + 8, s_hs.ptk + 32, 16);     /* the TK            */
            put32(key + 112, 4);                    /* CRYPTO_ALGO_AES_CCM */
            put32(key + 116, 0);                    /* pairwise          */
            memcpy(key + 156, s_hs.bssid, 6);       /* ea: the AP        */
            r = ioctl(C_SET_KEY, key, sizeof(key));
            if (r)
                finish(r < 0 ? WIFI_E_IOCTL : WIFI_E_FWERR, (u32)r,
                       "the key would not install");
            s_keyed = true;
            W->keyed = 1;
        } else if (psk && !s_keyed) {
            finish(WIFI_E_PSK, W->join_reason, "handshake did not finish");
        }

        /* ---- the network itself ------------------------------------
         *
         * Associated and keyed proves the radio and the crypto, not that
         * the chip carries a packet: an address from DHCP and an echo
         * off the internet do. */
        set_step(WIFI_ST_NET);
        if (!s_txring_up) {
            /* An open network never needed a ring until now. */
            memcpy(s_hs.bssid, (const void *)W->join_bssid, 6);
            (void)flowring_create(s_hs.bssid);
        }
        if (!s_txring_up) {
            W->net_step = WIFI_NET_IDLE;
            scopy(W->net_why, "no flow ring to send on", sizeof(W->net_why));
        } else {
            static const net_io_t io = { net_tx, net_ms, net_rnd, 0 };
            s_net_ms = 0;
            net_init(&s_net, &io, (const u8 *)W->mac, PING_TARGET);
            W->ping_target = PING_TARGET;
            s_net_up = true;
            for (t = 0; t < NET_LIMIT_MS; t++) {
                poll_d2h();
                int st = net_poll(&s_net);
                if (st <= NET_REACHED)
                    W->net_step = (u32)st;
                if (st == NET_REACHED || st == NET_FAILED)
                    break;
                udelay(1000);
                s_net_ms++;
                if (!(t & 15))
                    hb();
            }
            s_net_up = false;
            W->ip_addr = s_net.ip;
            W->ip_mask = s_net.mask;
            W->ip_gw = s_net.gw;
            W->ip_dns = s_net.dns;
            W->dhcp_ms = s_net.dhcp_ms;
            W->ping_sent = s_net.ping_sent;
            W->ping_recv = s_net.ping_recv;
            W->ping_ms = s_net.ping_best_ms;
            W->net_step = s_net.state == NET_REACHED ? WIFI_NET_REACHED
                        : s_net.ip ? WIFI_NET_BOUND : WIFI_NET_DHCP;
            if (s_net.state != NET_REACHED)
                scopy(W->net_why, s_net.why ? s_net.why : "the network timed out",
                      sizeof(W->net_why));
        }
    }

    finish(WIFI_OK, 0, "");
}
