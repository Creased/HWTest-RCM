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
 *   6. receive, event and ioctl-response buffers posted, then ioctls:
 *      "ver", "cur_etheraddr", "mpc" 0, "event_msgs" (ESCAN_RESULT), UP,
 *      and "escan"; the ESCAN_RESULT events collected until the final one.
 *
 * Everything is polled: no interrupts, no MSI. Every wait is bounded and
 * bumps the heartbeat, and every step is named in the mailbox before it
 * starts (wifi_mbox.h), the stub's rules.
 *
 * NOT TESTED ON HARDWARE when written. Where brcmfmac's behaviour depends on
 * a chip revision this port takes the 4356's branch only.
 */

#include <stdbool.h>

#define CPU_STUB_THEN wifi_main
#include "../cpu_stub/stub.c"
#include "../wifi_mbox.h"

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
}

static void set_step(u32 s)
{
    W->step = s;
    hb();
    __asm__ volatile("dsb sy" ::: "memory");
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
static u32 s_mb0 = PCIE2_H2D_MB0, s_mb1 = PCIE2_H2D_MB1;
static u32 s_rx_dataoffset;

static const u16 ring_depth[NRINGS] = { 64, 1024, 64, 1024, 1024 };
static const u16 ring_item_v7[NRINGS]  = { 40, 32, 24, 24, 40 };
static const u16 ring_item_pre7[NRINGS] = { 40, 32, 24, 16, 32 };

static void doorbell(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
    reg32_w(s_mb0, 1);
}

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

static void post_rx_buf(u32 pktid, u8 *buf)
{
    ring_t *rg = &s_ring[RING_RXPOST];
    u8 *it = ring_reserve(rg);
    if (!it)
        return;
    it[0] = MSG_RXBUF_POST;
    put32(it + 4, pktid);
    put16(it + 8, 0);                  /* no metadata */
    put16(it + 10, RX_BUF_SIZE);
    put32(it + 24, (u32)(unsigned long)buf);
    put32(it + 28, 0);
    ring_commit(rg);
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

#define E_IF            54u
#define E_ESCAN_RESULT  69u
#define E_STATUS_SUCCESS 0u
#define E_STATUS_PARTIAL 8u

static void handle_event(const u8 *p, u32 len)
{
    /* ether header, brcm header (OUI 00:10:18, user subtype 1), event. */
    if (len < 72 || get16be(p + 12) != 0x886C ||
        p[19] != 0x00 || p[20] != 0x10 || p[21] != 0x18 ||
        get16be(p + 22) != 1)
        return;
    u32 type = get32be(p + 28);
    u32 status = get32be(p + 32);
    u32 dlen = get32be(p + 44);
    const u8 *d = p + 72;
    if (dlen > len - 72)
        dlen = len - 72;
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
    case MSG_RX_CMPLT: {
        u32 idx = pktid - PKT_RXDATA;
        if (idx < N_RXDATA)
            post_rx_buf(pktid, s_rx_buf[idx]);
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
        if (w >= rg->depth)
            continue;
        u32 n = 0;
        while (rg->r != w && n++ < rg->depth) {
            handle_msg(rg->buf + (u32)rg->r * rg->item + s_rx_dataoffset);
            if (++rg->r == rg->depth)
                rg->r = 0;
        }
        if (n)
            tcm16_w(rg->r_idx_addr, rg->r);
    }
}

/* ---- ioctls (msgbuf.c, fwil.c) ------------------------------------------ */

#define C_UP          2u
#define C_SET_INFRA   20u
#define C_GET_VAR     262u
#define C_SET_VAR     263u

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
    __asm__ volatile("dsb sy" ::: "memory");
    W->done = WIFI_MBOX_DONE;
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
               "the stub did not enumerate the CYW4356");
    if (W->in_magic != WIFI_MBOX_MAGIC || !W->fw_len || !W->nvram_len ||
        W->fw_len > WIFI_FW_MAX || W->nvram_len > WIFI_NVRAM_MAX)
        finish(WIFI_E_INPUT, W->fw_len, "no firmware or NVRAM handed over");

    /* 1. BARs, the bridge window over them, memory and bus master on. */
    set_step(WIFI_ST_BARS);
    u32 bar0 = bar_assign(0x10, PCI_BAR0_BASE);
    if (bar0 < 0x8000u)
        finish(WIFI_E_BAR, bar0, "BAR0 did not size or take its base");
    u32 bar2 = bar_assign(0x18, PCI_BAR2_BASE);
    if (bar2 < 0x200000u || bar2 > PCI_BAR2_MAX)
        finish(WIFI_E_BAR, bar2, "BAR2 (TCM) did not size or take its base");
    W->bar0 = PCI_BAR0_BASE;
    W->bar2 = PCI_BAR2_BASE;
    W->bar2_size = bar2;
    wr32(PCIE_RP1_BASE + 0x20u,
         ((PCI_BAR2_BASE + bar2 - 1u) & 0xFFF00000u) |
         ((PCI_BAR0_BASE >> 16) & 0xFFF0u));
    if (cfg_set(0x04, RP_COMMAND_MEM | RP_COMMAND_BME, 0x0006u))
        finish(WIFI_E_BAR, 0, "command register did not take MEM|BME");

    /* 2. Chip ID and the cores. */
    set_step(WIFI_ST_CHIPID);
    u32 cid = bp_rd(0x18000000u);
    W->chipid = cid;
    if (s_bp_err)
        finish(WIFI_E_WINDOW, 0x18000000u, "backplane window write did not stick");
    u32 chip = cid & 0xFFFFu;
    if (cid == 0xFFFFFFFFu || ((cid >> 28) & 0xF) != 1u ||
        (chip != 0x4356u && chip != 0x4354u && chip != 43567u &&
         chip != 43569u))
        finish(WIFI_E_CHIPID, cid, "ChipCommon chip ID is not a 4356");

    set_step(WIFI_ST_EROM);
    erom_scan(bp_rd(0x18000000u + 0xFCu));
    const wifi_core_t *cr4 = core_get(CORE_CR4, 0);
    const wifi_core_t *p2 = core_get(CORE_PCIE2, 0);
    if (!cr4 || !p2 || !core_get(CORE_CC, 0) || s_bp_err)
        finish(WIFI_E_EROM, W->ncores, "EROM: no CR4, PCIe2 or ChipCommon");
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
        finish(WIFI_E_WINDOW, 0, "backplane window lost after the reset");

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
        finish(WIFI_E_VERIFY, bad, "firmware did not read back from TCM");

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
        finish(WIFI_E_FWUP, written, "firmware did not start (no shared RAM)");
    W->shared_addr = shared;
    if (shared < rambase || shared >= rambase + ramsize)
        finish(WIFI_E_SHARED, shared, "shared RAM address out of range");

    set_step(WIFI_ST_SHARED);
    u32 flags = tcm32(shared);
    W->shared_flags = flags;
    u32 ver = flags & 0xFFu;
    if (ver < 5 || ver > 7)
        finish(WIFI_E_SHARED, flags, "unsupported PCIe shared version");
    s_rx_dataoffset = tcm32(shared + 36);
    if (s_rx_dataoffset > 256)
        finish(WIFI_E_SHARED, s_rx_dataoffset, "rx data offset not plausible");
    s_console_base = tcm32(shared + 20);
    if (s_console_base >= rambase + ramsize)
        s_console_base = 0;
    u32 ri = tcm32(shared + 48);
    if (ri < rambase || ri >= rambase + ramsize)
        finish(WIFI_E_RINGS, ri, "ring info address out of range");

    /* 5. Rings (TCM indices: brcmfmac's fallback, which every firmware
     * of this protocol supports), scratch buffers, host-ready. */
    set_step(WIFI_ST_RINGS);
    u32 ringmem = tcm32(ri + 0);
    u32 h2d_w = tcm32(ri + 4), h2d_r = tcm32(ri + 8);
    u32 d2h_w = tcm32(ri + 12), d2h_r = tcm32(ri + 16);
    if (ringmem < rambase || ringmem >= rambase + ramsize ||
        h2d_w >= rambase + ramsize || d2h_w >= rambase + ramsize)
        finish(WIFI_E_RINGS, ringmem, "ring memory pointers out of range");
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
        finish(WIFI_E_WINDOW, p2->base, "window to the PCIe2 core did not stick");
    if (flags & SHARED_HOSTRDY_DB1)
        reg32_w(s_mb1, 1);

    for (u32 i = 0; i < N_RXDATA; i++) {
        s_rx_buf[i] = dma_alloc(RX_BUF_SIZE, 64);
        if (!s_rx_buf[i])
            finish(WIFI_E_RINGS, i, "out of DMA memory");
        post_rx_buf(PKT_RXDATA + i, s_rx_buf[i]);
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
        finish(WIFI_E_IOCTL, 0, "no reply to the first ioctl (\"ver\")");
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
        mask[E_ESCAN_RESULT / 8] |= 1u << (E_ESCAN_RESULT % 8);
        mask[E_IF / 8] |= 1u << (E_IF % 8);
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

    set_step(WIFI_ST_SCAN);
    {
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
        r = iovar_set("escan", p, sizeof(p));
        if (r)
            finish(r < 0 ? WIFI_E_IOCTL : WIFI_E_FWERR, (u32)r,
                   "the firmware refused escan");
    }
    u32 limit = W->scan_time_ms ? W->scan_time_ms : 12000;
    for (t = 0; t < limit && !s_scan_done; t++) {
        poll_d2h();
        udelay(1000);
        if (!(t & 15))
            hb();
    }
    W->scan_ms = t;
    if (!s_scan_done)
        finish(W->nbss ? WIFI_OK : WIFI_E_SCANTIME, t,
               W->nbss ? "scan stopped at the time limit" :
                         "no final escan event in time");
    if (s_scan_status != E_STATUS_SUCCESS && !W->nbss)
        finish(WIFI_E_SCAN, s_scan_status, "escan ended with an error");
    finish(WIFI_OK, 0, "");
}
