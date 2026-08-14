/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: wifi probes
 */
#include "hwtest.h"


/* ======================================================================== */
/* Wi-Fi radio - the CYW4356 WLAN half, reached over PCIe                   */
/*                                                                          */
/* probe_bt_radio above ends by saying a surviving Wi-Fi fault is "the PCIe */
/* link, WLAN core or antenna". This probe takes that next step. The two    */
/* halves of the package are independent from the host's point of view:     */
/* BT is an H4 device on UART-D, WLAN is a PCI Express endpoint. The Switch */
/* device tree wires `pci@2,0` of `pcie@1003000` - root port index 1 - to   */
/* padctl lane `pcie-0` with nvidia,function = "pcie-x1", and the x4 group  */
/* (lanes pcie-1..4) to `pci@1,0`, root port 0.                             */
/*                                                                          */
/* BDK has no PCIe support whatsoever: no AFI or PADS base, no config-space */
/* accessor, no PLLE/PLLREFE bring-up, no UPHY lane init. Everything below  */
/* is a from-scratch T210 root-complex bring-up ported from U-Boot          */
/* (drivers/pci/pci_tegra.c, arch/arm/mach-tegra/tegra210/clock.c and       */
/* .../tegra210/xusb-padctl.c, arch/arm/mach-tegra/powergate.c), cross-     */
/* checked against Linux (drivers/pci/controller/pci-tegra.c                */
/* `tegra210_pcie`, drivers/phy/tegra/xusb-tegra210.c, drivers/soc/tegra/   */
/* pmc.c for the I/O-pad DPD bits), and - for the Mariko-only pieces that   */
/* exist in no mainline tree - against L4T's xusb-tegra210.c.               */
/*                                                                          */
/* THE WORK IS SPLIT ACROSS TWO PROCESSORS, and it has to be. The PCIe      */
/* register apertures answer a CPU-complex master and nobody else: the TRM  */
/* puts the BPMP-Lite crossbar on AHB (ch.19) and PCIe is not an AHB slave, */
/* while the aperture hangs off MSELECT, documented in the CPU Complex      */
/* chapter. With clocks, resets, power and PLLE all correct the BPMP reads  */
/* 0xFFFFFFFF from AFI and one constant from every offset of MSELECT. So    */
/* this function does everything that IS reachable from here - rails,       */
/* WL_REG_ON, I/O pads, the power partition, clocks and resets,             */
/* PLLREFE/PLLE, the pad controller, the lane mux and the UPHY PLL, all     */
/* of it APB and CAR - and then hands the PCIe half to a short AArch64 stub */
/* on CPU0 (cpu_stub/, see cpu_mbox.h) while supervising it.                */
/*                                                                          */
/* Three waits here appear in NEITHER upstream driver, and they are the     */
/* reason a naive port fails on this board. Upstream drives add-in cards,   */
/* whose rails and PERST# are the slot's problem; here the endpoint's own   */
/* power-up is something we start, so CYW4356 Figure 52 / Table 62 (p147)   */
/* applies end to end:                                                      */
/*                                                                          */
/*   WL_REG_ON high -> VDDC          Tregontovdd        1 ms                */
/*                  -> internal POR  Tvddtopor         57 ms                */
/*   REFCLK enabled -> stable        Trefclkstable     10 ms                */
/*                  -> PERST# high   Tref2perst       100 us                */
/*                  -> first config  Tperst2firstconfig 6 ms                */
/*   WL_REG_ON high -> ready         Tvregontofirstconfig  100 ms worst     */
/*                                                                          */
/* The first five are minimums a host must honour: the endpoint is not      */
/* allowed to see a config TLP before ~58 ms, and the refclk has to be      */
/* running for 10 ms before PERST# rises. The last is the other direction - */
/* the part is guaranteed ready within 100 ms of WL_REG_ON, so taking       */
/* longer is free. The Tegra-side bring-up is still interleaved into the    */
/* POR wait rather than run before it, because that is what keeps the       */
/* refclk-stable window from having to be paid twice.                       */

#ifndef WIFI_CFG_COLD_CYCLE
#define WIFI_CFG_COLD_CYCLE 1  /* drop WL_REG_ON for a datasheet cold POR  */
#endif

/* Assert WL_REG_ON at the very start of the probe sweep (see run_all_probes),
 * to keep the WLAN chip powered for the whole sweep before the Wi-Fi probe -
 * matching HOS's boot-on/always-on WL_REG_ON. Off by default. */
#ifndef WIFI_CFG_EARLY_REGON
#define WIFI_CFG_EARLY_REGON 0
#endif

/* Pad control for the root port 1 reset and clock-request lines. Not in
 * bdk's pinmux.h, which has no reason to know about PCIe. */
/* Drive PH0 at all.
 *
 * PH0 is named wifi_en in bdk and in every Switch device tree, and a
 * CYW4356's WL_REG_ON is an enable, which is what "wifi_en" sounds like.
 * On by default, but the drive lives entirely inside the cold cycle: the
 * entry LOG, the drive low and the raise are all nested under #if
 * WIFI_CFG_COLD_CYCLE, so with WIFI_CFG_COLD_CYCLE=0 PH0 is neither
 * driven nor reported. Within the cold cycle it is driven as an enable:
 * low alongside PH1, raised 10 ms ahead of PH1, and left HIGH. U-Boot's
 * pinmux_init and its chain-loader drive PH0 LOW, and that low drive is
 * all that is taken from them; raising the pin again and leaving it high
 * is this probe's own sequence.
 *
 * Recorded caveat, which the default does not repeal: on this board PH0
 * also behaves as a separate RF-disable line that boots output-LOW. It
 * reads 0 in RCM on a console whose radio works fine under Horizon, and
 * driving it HIGH stops Bluetooth answering HCI at all, across power
 * cycles - the behaviour of a kill line and not of an enable. The sweep
 * survives that because the Bluetooth probe runs EARLIER (main.c's probe
 * table lists probe_bt_radio before probe_wifi_pcie), so the HCI verdict
 * is taken before this pin ever moves; a Bluetooth result obtained after
 * this probe means nothing. Build -DWIFI_CFG_DRIVE_PH0=0 to leave the
 * pin exactly as found. */
#ifndef WIFI_CFG_DRIVE_PH0
#define WIFI_CFG_DRIVE_PH0 1   /* drive PH0 as the WLAN core's enable: low
                                * across the cold cycle, as U-Boot's
                                * pinmux_init and chain-loader leave it,
                                * then - this probe's own part - high
                                * again before PH1 and left high */
#endif
#ifndef WIFI_CFG_PH1_LEVEL
#define WIFI_CFG_PH1_LEVEL 1   /* level to leave PH1/wifi_rst at          */
#endif

/* PH0 = WIFI_EN, PH1 = WIFI_RST. bdk names both, and the names are the
 * board's, not this probe's guess. On a CYW4356 the pin that gates the WLAN
 * section's internal regulators is WL_REG_ON - an enable - so it is PH0 that
 * should carry it, and PH1 is the module reset. */
#define PMX_PEX_L1_RST_N    0x44   /* PINMUX_AUX_PEX_L1_RST_N, reset 0x460 */
#define PMX_PEX_L1_CLKREQ_N 0x48   /* PINMUX_AUX_PEX_L1_CLKREQ_N, rst 0x470*/
/* Bring the x4 group's lanes (pcie-1..4) up alongside the x1 link. Nothing
 * is attached to them on this board and only root port 1 is ever trained,
 * but the cross-bar is configured X4_X1, so leaving their mux and IDDQ
 * unset would describe a topology the controller is not in. */
#ifndef WIFI_CFG_X4_LANES
#define WIFI_CFG_X4_LANES   1
#endif

/* Whether this probe runs as part of the boot sweep.
 *
 * The probe is not dangerous to the payload: the PCIe half runs on CPU0
 * (see cpu_mbox.h), so an access that never completes takes out CPU0 alone,
 * and the BPMP notices the mailbox stop, reports the last breadcrumb,
 * powergates the cluster and carries on. That recovery path is exercised on
 * every emulator run, where CPU0 never reports at all.
 *
 * It is INVASIVE in a way the rest of the sweep is not, though. It boots
 * the CPU cluster - CPU rail up via the MAX77621/MAX77812, PLLX, the
 * CRAIL/C0NC/CE0 partitions - and cold-cycles WL_REG_ON, resetting the
 * WLAN section. hwtest is otherwise a read-only pass you can point at an
 * unknown console; booting a CPU complex costs 3-4 seconds on top.
 *
 * Runs in the sweep by default (WIFI_CFG_AUTORUN=1): the wireless verdict
 * is part of the report the techs read, no pager interaction required.
 * Build with -DWIFI_CFG_AUTORUN=0 for the armed-only behaviour. */
#ifndef WIFI_CFG_AUTORUN
#define WIFI_CFG_AUTORUN 1
#endif

bool g_wifi_armed = false;
bool g_wifi_quiet_pass = false;  /* bring-up-only re-entry: skip the handoff */
/* Set once the SoC-side bring-up has run this boot, so the link page can
 * tell "already done" from "needs doing quietly first". */
bool g_wifi_brought_up = false;

/* Apertures, from tegra210.dtsi's pcie@1003000 `reg` and `ranges`. All of
 * this sits below 0x40000000, which the BPMP can address: bdk's MMU table
 * (soc/bpmp.c:121) defines entries for DRAM and IRAM only and leaves the
 * fallback entry RWX, so PCIe space is reachable and uncached. */



/* PADS: on T210 the SERDES lives in the XUSB pad controller, so
 * tegra_pcie_phy_enable()'s PADS_CTL / PADS_PLL_CTL path (Tegra20..124) is
 * dead code here - pcie->phy is non-NULL and U-Boot takes the UPHY branch.
 * The only PADS register T210 touches is the refclk driver config. */

/* Root-port registers, in the port's own 4 KiB window. Offsets below 0x100
 * are ordinary PCI config for the bridge; the 0xF00+ block is NVIDIA's. */

/* XUSB pad controller - lane mux and UPHY PLL P0 (tegra210_lanes[] in
 * U-Boot xusb-padctl.c: every pcie-N lane is 2 bits at 0x028, shift 12+2N;
 * function order for a pci lane is pcie-x1, usb3-ss, sata, pcie-x4). */
#define PADCTL_ELPG_PROGRAM  0x024
#define  ELPG_VCORE_DOWN     BIT(31)
#define  ELPG_CLAMP_EN_EARLY BIT(30)
#define  ELPG_CLAMP_EN       BIT(29)
#define PADCTL_USB3_PAD_MUX  0x028
/* FORCE_PCIE_PAD_IDDQ_DISABLE_MASK0..4, bits 1..5 (TRM 9/22, step 13 of the
 * PCIe root-port init sequence in 34.3.1). */
#define PADCTL_FORCE_PCIE_IDDQ_DIS_ALL  (0x1Fu << 1)

/* XUSB_PADCTL_UPHY_MISC_PAD_Pn_CTL_2, TRM 22.16.7.54 onward: P0 at 0x464,
 * then every 0x40 (P1 0x4A4, P2 0x4E4, P3 0x524, P4 0x564). */
#define PADCTL_UPHY_MISC_PAD_CTL_2(n)  (0x464u + (n) * 0x40u)
#define  UPHY_MISC_TX_IDDQ        BIT(0)
#define  UPHY_MISC_TX_IDDQ_OVRD   BIT(1)
#define  UPHY_MISC_RX_IDDQ        BIT(8)
#define  UPHY_MISC_RX_IDDQ_OVRD   BIT(9)
#define  UPHY_MISC_TX_PWR_OVRD    BIT(24)
#define  UPHY_MISC_RX_PWR_OVRD    BIT(25)
#define  PADCTL_PCIE_LANE_SHIFT(n)  (12 + (n) * 2)
#define  PADCTL_LANE_FUNC_PCIE_X1 0   /* the x1 mux field the stub's flow trains with */
#define  PADCTL_LANE_FUNC_PCIE_X4 3

/* T210B01 UPHY brick configuration, via the indirect config port.
 *
 * NVIDIA's L4T driver (tegra210_pex_uphy_enable,
 * drivers/phy/tegra/xusb-tegra210.c) does:
 *
 *   if (t210b01_compatible(padctl) == 1)
 *           <write usb3_pll_g1_init_data[] through UPHY_PLL_P0_CTL10>
 *   else
 *           CAL_CTRL = 0x136;  DCO_CTRL = 0x2a;
 *
 * So on B01 the CAL_CTRL/DCO_CTRL magic numbers every public T210 driver
 * uses are NOT written at all - the brick is configured by loading a table
 * through an address/data/write-strobe port instead. Taking the T210 branch
 * on a Mariko leaves the PLL unconfigured, and its calibration state machine
 * then never asserts CAL_DONE: CAL_EN latches, CAL_CTRL reads back 0x136
 * and CAL_DONE stays 0 for the full timeout.
 *
 * Same story per lane: pcie_lane_data goes through each lane's CTL8 port. */
#define PADCTL_UPHY_MISC_PAD_PX_CTL8(x) (0x47C + (x) * 0x40)
#define  UPHY_CFG_ADDR(x)   (((x) & 0xFF) << 16)
#define  UPHY_CFG_WDATA(x)  ((x) & 0xFFFF)
#define  UPHY_CFG_RESET     BIT(27)
#define  UPHY_CFG_WS        BIT(24)

/* CLK_RST bits BDK's clock.h does not name. PLLE_BASE/MISC/AUX it does. */
#define  PLLE_MISC_IDDQ_OVERRIDE    BIT(13)
#define  PLLE_MISC_LOCK             BIT(11)
#define  PLLE_AUX_CML_ENABLE        BIT(0)   /* "CML clock" in pci_tegra.c */
#define  PLLE_AUX_CML_BIT1          BIT(1)
/* PLLE hardware-sequencer handoff, from Linux clk-tegra210.c
 * tegra210_plle_hw_sequence_start(). Linux calls this unconditionally at the
 * end of tegra210_uphy_init(), i.e. every time it brings the PCIe PHY up.
 * With the sequencer off, PLLE stays under software control; the hardware
 * sequencer is what keeps the pad PLL in the state the SerDes needs without
 * software holding it there. Note IDDQ_SW_CTRL is bit 14 - distinct from the
 * bit 13 this file already calls PLLE_MISC_IDDQ_OVERRIDE. */
#define CLK_RST_PLLREFE_MISC     0x4C8
#define  PLLREFE_MISC_LOCK          BIT(27)

/* PMC I/O-pad deep-power-down bits for the PCIe pads
 * (drivers/soc/tegra/pmc.c tegra210_io_pads[]): pex-bias 4, pex-clk1 5,
 * pex-clk2 6 in IO_DPD_REQ/STATUS. tegra210.dtsi hangs `pex_dpd_disable`
 * off the controller node, so a board that boots with them parked would
 * have no refclk driver at all - hence reading the status, not assuming. */
#define PMC_IO_DPD_PEX_MASK  (BIT(4) | BIT(5) | BIT(6))

/* Broadcom side. The PCIe function of the BCM/CYW4356 is 14E4:43EC
 * (brcm_hw_ids.h: BRCM_PCIE_4356_DEVICE_ID). Config offset 0x80 is the
 * BAR0 backplane window brcmfmac uses to reach the chip's SoC bus - the
 * probe reads its default value and reports it, but enumeration of the
 * 14E4:43EC function is the verdict; the backplane itself is
 * firmware-gated in ROM phase. */
#define BRCM_VENDOR_ID       0x14E4
#define BRCM_4356_DEVICE_ID  0x43EC

/* Millisecond delay measured on TIMERUS - the same counter get_tmr_us() and
 * every timestamp in this probe read.
 *
 * bdk's msleep() deliberately runs off the RTC instead (timer.c:48, built
 * with USE_RTC_TIMER), and the two counters are independent. Sleeping on one
 * while measuring on the other makes the Table 62 arithmetic disagree with
 * the delays actually taken: the reported POR window then reads 57 ms no
 * matter how long the probe has waited. Still polls the host's reboot
 * request, like msleep_poll does. */
static void pcie_msleep(u32 ms)
{
    while (ms--) {
        usleep(1000);
        uart_poll_reboot();
    }
}

/* --- CPU0 handoff ------------------------------------------------------- */

#ifdef HAVE_CPU_STUB
extern const u8  cpu_stub_bin[];
extern const u32 cpu_stub_bin_size;
#endif

/* The endpoint read runs on CPU0 via the stub - see pcie_cpu_handoff()
 * below. The stub's own enumeration is the project's verdict path,
 * verified on the Erista. */
/* Hand the PCIe-side accesses to a CPU-complex master and supervise.
 *
 * This exists because the BPMP cannot reach PCIe at all - see cpu_mbox.h for
 * the evidence. It is also the safer arrangement: the accesses that stall
 * are made by CPU0, and a stalled CPU0 is a line of output rather than a
 * dead console, because the BPMP is still here to notice, report and reboot.
 *
 * Everything the BPMP CAN do has already been done by the caller - rails,
 * PLLE/PLLREFE, the pad controller, the lane mux, the UPHY PLL, the PCIE
 * partition and its clocks and resets are all APB/CAR and all reachable from
 * here. CPU0 only picks up from MSELECT onwards. */
static void pcie_cpu_handoff(void)
{
#ifndef HAVE_CPU_STUB
    log_color(COL_WARN,
        "  CPU0 handoff  : not built (devkitA64 absent at build time)\n");
#else
    volatile cpu_mbox_t *mb = (volatile cpu_mbox_t *)CPU_MBOX_ADDR;

    LOG("  -- CPU0 handoff (PCIe needs a CPU-complex master) --\n");
    if (cpu_stub_bin_size > CPU_STUB_MAX) {
        log_color(COL_ERR, "  stub          : %d bytes exceeds window\n",
            cpu_stub_bin_size);
        return;
    }

    memcpy((void *)CPU_STUB_ADDR, cpu_stub_bin, cpu_stub_bin_size);
    memset((void *)mb, 0, sizeof(cpu_mbox_t));

    /* The BPMP's MMU maps DRAM cached (bdk soc/bpmp.c mmu_entries), and CPU0
     * comes out of reset with its MMU off, so it will read DRAM directly.
     * Clean our cache or it fetches a stale or empty stub. */
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);

    LOG("  stub          : %d bytes -> %08X, mailbox %08X\n",
        cpu_stub_bin_size, CPU_STUB_ADDR, CPU_MBOX_ADDR);

    /* lock = false: leaving the non-secure reset vector writable means a
     * later run, or the reboot path, can retarget CPU0 without a cold boot.
     *
     * The whole boot-and-wait is wrapped in up to 3 attempts. Some consoles'
     * CYW4356s wedge the CPU-complex fabric on an early config read - the
     * endpoint's ROM state varies per boot, so a hard AFI reset and a
     * re-run of the stub has a real chance of landing a good one. The
     * reset drains the AFI's outstanding transactions, which is what
     * wedges MSELECT reads on the next boot otherwise. */
    for (u32 attempt = 0; attempt < 3; attempt++) {
    ccplex_boot_cpu0(CPU_STUB_ADDR, false);

    /* Wait for the stub to say anything at all, then for it to finish,
     * treating a stalled heartbeat rather than a fixed deadline as failure -
     * that way a slow step is not mistaken for a dead one. */
    u32 last_hb = 0, last_move = get_tmr_us();
    bool alive = false, done = false;
    for (u32 i = 0; i < 40000; i++) {          /* hard ceiling ~20 s: the
                         * endpoint ROM-init wait + CRS retry loop can take
                         * seconds, and the heartbeat-stall check below is
                         * the real failure detector. */
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);
        if (mb->magic == CPU_MBOX_MAGIC)
            alive = true;
        if (mb->heartbeat != last_hb) {
            last_hb = mb->heartbeat;
            last_move = get_tmr_us();
        }
        if (mb->done == CPU_MBOX_DONE) { done = true; break; }
        /* The stub only advances its breadcrumb between major blocks.
         * Before the config-read stage the blocks are bounded waits
         * with no heartbeat bumps: the PHY bring-up re-calibrates a
         * re-run's already-locked PLLs (the 250 ms cal waits can run
         * to their timeouts, ~1.8 s worst case) and the link waits
         * poll for up to ~0.8 s. A 300 ms window misreads those as a
         * stall. Once the config-read stage is reached every stall is
         * the real wedge and must be caught fast. */
        u32 stall_us = (mb->stage < CPU_STAGE_CFG_RD) ? 2000000 : 300000;
        if (alive && (get_tmr_us() - last_move) > stall_us)
            break;
        usleep(500);
        uart_poll_reboot();
    }

    if (!alive) {
        log_color(COL_ERR,
            "  CPU0          : never reported in (magic=%08X)\n", mb->magic);
        LOG("  (the cluster did not start, or never reached the stub)\n");
        /* If the stub faulted before the magic store, the EL3 exception
         * handler's mailbox record is still readable: name the fault. */
        if (mb->exc_taken == CPU_MBOX_EXC)
            LOG("  (stub fault  : ESR=%08X FAR=%08X ELR=%08X)\n",
                mb->exc_esr, mb->exc_far, mb->exc_elr);
        dx_set("wifi_pcie", DX_FAIL, "CPU0 did not start");
        break;
    }

    /* Stalled. On the first attempts, clear the wedge and go around
     * again without the full diagnostic dump - the final attempt keeps
     * the detailed report. */
    if (!done && attempt < 2) {
        LOG("  CPU0          : stalled (attempt %d/3, hb=%d) - resetting AFI\n",
            attempt + 1, mb->heartbeat);
        ccplex_powergate_cpu0();
        msleep_poll(50);
        CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(6) | BIT(8) | BIT(10);
        msleep_poll(10);
        CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(6) | BIT(8) | BIT(10);
        msleep_poll(10);
        mb->done = 0;
        mb->diag_mode = CPU_DIAG_NONE;
        continue;
    }
    if (!done) {
        /* CPU0 is wedged on an access it cannot report on itself. If it got
         * as far as the config read, the AFI may have latched a code for what
         * went wrong (FPCI timeout, target abort, master abort) that only a
         * NON-wedged master can read. So powergate the hung core and boot it
         * again in diagnostic mode: the AFI stayed powered, so AFI_INTR_CODE
         * still holds the latch. It says whether the transaction reached the
         * wire and got no answer (code 9), errored (3/4), or never left
         * (code 0). */
        if (mb->stage >= CPU_STAGE_CFG_RD && mb->afi_intr_code == 0) {
            ccplex_powergate_cpu0();
            /* Let the cluster actually power down before the re-boot:
             * a re-boot on top of an in-flight powergate can come up
             * in a state where even MSELECT does not answer, which
             * replaces a useful latch read with a second, earlier stall. */
            msleep_poll(50);
            mb->done = 0;
            mb->diag_mode = CPU_DIAG_READ_INTR;
            bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);
            ccplex_boot_cpu0(CPU_STUB_ADDR, false);
            for (u32 i = 0; i < 2000; i++) {
                bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);
                if (mb->done == CPU_MBOX_DONE) break;
                usleep(500);
            }
            mb->diag_mode = CPU_DIAG_NONE;
        }
        /* The breadcrumb is the whole point: it names the access CPU0 was
         * making when it stopped, and the BPMP is still alive to print it. */
        /* Did CPU0 fault, or is it genuinely wedged on the bus? Without the
         * stub's exception vectors the two are indistinguishable from here:
         * both look like the mailbox going quiet. */
        if (mb->exc_taken == CPU_MBOX_EXC) {
            u32 ec  = (mb->exc_esr >> 26) & 0x3F;   /* exception class     */
            u32 dfsc = mb->exc_esr & 0x3F;          /* data fault status   */
            log_color(COL_ERR,
                "  CPU0          : EXCEPTION at stage %d, not a stall\n",
                mb->exc_stage);
            LOG("  ESR_EL3       : %08X  EC=%02X ISS_DFSC=%02X\n",
                mb->exc_esr, ec, dfsc);
            LOG("  FAR/ELR       : faulting addr %08X, at pc %08X\n",
                mb->exc_far, mb->exc_elr);
            /* EC 0x25 = data abort without level change; DFSC 0b010000 is a
             * synchronous external abort, i.e. the fabric answered "no". */
            if (ec == 0x25 && dfsc == 0x10) {
                LOG("  (synchronous external abort: the access WAS answered,\n");
                LOG("   with an error - not an unclaimed transaction)\n");
            }
            dx_set("wifi_pcie", DX_FAIL, "CPU0 fault ESR %08X at %08X",
                   mb->exc_esr, mb->exc_far);
        } else if (mb->rp_bus || mb->rp_cmd) {
            /* The mailbox proves the link trained: the bridge bus numbers
             * and command register were only programmed after DL_UP. A
             * stall with those set is therefore the endpoint's config
             * space never answering - the read hung downstream of a live
             * link. (The stage field above can mislead here: the diag
             * re-run that reads the AFI latch starts a second pass whose
             * own breadcrumbs overwrite the first.) */
            log_color(COL_ERR,
                "  CPU0          : link trained, endpoint config access hung\n");
            LOG("  (hb=%d, first reads: id0=%08X id1=%08X hdr=%02X class=%08X\n",
                mb->heartbeat, mb->raw_cfg[12], mb->raw_cfg[13],
                mb->raw_cfg[14], mb->raw_cfg[15]);
            LOG("   %08X %08X %08X phy=%02X, last id=%08X)\n",
                mb->raw_cfg[16], mb->raw_cfg[17], mb->raw_cfg[18],
                mb->raw_cfg[19], mb->raw_cfg[0]);
            LOG("  (RP0 probes    : pre=%08X postphy=%08X ctrl=%08X port=%08X)\n",
                mb->raw_cfg[5], mb->raw_cfg[6], mb->raw_cfg[7],
                mb->raw_cfg[8]);
            LOG("  (AFI latch after first read: code=%d sig=%08X)\n",
                mb->afi_intr_code, mb->afi_intr_sig);
            LOG("  (RP after first read: LNKSTA=%04X devctl/sts=%08X SECSTS=%08X)\n",
                mb->link_stat >> 16, mb->raw_cfg[10], mb->raw_cfg[11]);
            /* PHY state the stub saw right before the first read -
             * padctl/UPHY/PLLE/PADS, same set U-Boot's md.l can dump. */
            {
                const volatile u32 *d =
                    (const volatile u32 *)0xA0008800u;
                LOG("  (phy 7009F004 : %08X %08X %08X %08X\n",
                    d[0], d[1], d[2], d[3]);
                LOG("   phy 7009F360 : %08X %08X %08X %08X\n",
                    d[4], d[5], d[6], d[7]);
                LOG("   phy 7009F37C : %08X %08X %08X %08X\n",
                    d[8], d[9], d[10], d[11]);
                LOG("   phy 7009F460 : %08X %08X %08X %08X\n",
                    d[12], d[13], d[14], d[15]);
                LOG("   plle 60006068: %08X %08X %08X %08X\n",
                    d[16], d[17], d[18], d[19]);
                LOG("   xio  6000651C : %08X\n",
                    d[20]);
                LOG("   (REFCLK_CFG0 after PERST#: %08X, want 90B890B8)\n",
                    mb->raw_cfg[16]);
            }
            LOG("  (the bridge was programmed, so the link WAS up - the\n");
            LOG("   stall is the endpoint's config space never completing\n");
            LOG("   a read, which is the ROM-phase CYW4356's known quirk,\n");
            LOG("   not a dead bus)\n");
            dx_set("wifi_pcie", DX_FAIL,
                   "link up, endpoint config read hung");
        } else {
            log_color(COL_ERR,
                "  CPU0          : STALLED at stage %d (hb=%d, no exception)\n",
                mb->stage, mb->heartbeat);
            dx_set("wifi_pcie", DX_FAIL, "CPU0 stalled at stage %d",
                   mb->stage);
        }
        /* Whatever it managed to record before it stopped. CAR is what the
         * root-port windows hang off, so RST_DEVICES_U bit 10 (PCIEXCLK)
         * being clear is the precondition for stage 9 to be legal at all. */
        LOG("  CPU0 state    : RST_DEVICES_U=%08X (PCIEXCLK=%d) AFI_CFG=%08X\n",
            mb->car_rst_u, (mb->car_rst_u >> CLK_U_PCIEXCLK) & 1,
            mb->afi_cfg_en);
        LOG("  CPU0 saw      : MSELECT=%08X -> %08X  AFI_CFG0=%08X\n",
            mb->mselect_cfg, mb->mselect_cfg2, mb->afi_probe0);
        LOG("  CPU0 BAR0     : start=%08X sz=%08X fpci=%08X\n",
            mb->bar0_start, mb->bar0_sz, mb->bar0_fpci);
        if (mb->rp_bus || mb->rp_cmd)
            LOG("  CPU0 bridge   : BUS=%08X (sec=%d sub=%d) CMD/STS=%08X\n",
                mb->rp_bus, (mb->rp_bus >> 8) & 0xFF,
                (mb->rp_bus >> 16) & 0xFF, mb->rp_cmd);
        /* Root port 1 is the WLAN port; its DISABLE bit is bit (1+1) = 2. */
        LOG("  CPU0 port en  : AFI_PCIE_CONFIG=%08X port1 disable=%d\n",
            mb->afi_pcie_cfg, (mb->afi_pcie_cfg & BIT(2)) ? 1 : 0);
        if (mb->afi_fpci_to)
            LOG("  CPU0 FPCI-to  : AFI_FPCI_TIMEOUT readback=%08X\n",
                mb->afi_fpci_to);
        if (mb->stage >= CPU_STAGE_CFG_RD) {
            static const char *ic[] = {
                "nothing latched", "INI_SLVERR", "INI_DECERR",
                "TGT_SLVERR(EP tgt-abort)", "TGT_DECERR(master-abort/UR)",
                "TGT_WRERR", "SM_MSG", "DFPCI_DECERR",
                "AXI_DECERR(aperture miss)", "FPCI_TIMEOUT" };
            u32 c = mb->afi_intr_code & 0x1F;
            LOG("  CPU0 AFI intr : code=%d %s sig=%08X\n", c,
                c < 10 ? ic[c] : "?", mb->afi_intr_sig);
            if (c == 0) {
                LOG("  (no AFI error during the hung read: the config TLP is\n");
                LOG("   outstanding with no completion AND no error - the\n");
                LOG("   endpoint is not answering, not a fabric fault)\n");
            }
        }
        /* Reaching stage 10 at all means stage 9 finished, i.e. the root
         * port window answered and the link-training loop ran to a verdict -
         * so these are worth printing even on a stall. A programmed bridge
         * (rp_bus set) proves the same even when the diag re-run's early
         * breadcrumbs have overwritten the stage field. */
        if (mb->stage >= CPU_STAGE_LINK_WAIT || mb->rp_bus || mb->rp_cmd)
            LOG("  CPU0 link     : port=%d LNKSTA=%04X gen%d x%d RP=%04X:%04X\n",
                (int)mb->port, mb->link_stat >> 16,
                (mb->link_stat >> 16) & 0xF, (mb->link_stat >> 20) & 0x3F,
                mb->rp_id & 0xFFFF, mb->rp_id >> 16);
        if (mb->stage < CPU_STAGE_CFG_RD && !(mb->rp_bus || mb->rp_cmd)) {
            LOG("  (stage 2/3 = MSELECT, 4/5 = AFI window - that access\n");
            LOG("   never returned, which here means it is not decoding)\n");
        }
    } else {
        log_color(COL_OK, "  CPU0          : ran to completion (hb=%d)\n",
            mb->heartbeat);
        /* TRM 16.3.1 gives MSELECT_CONFIG_0 reset = 0x07ff4020. Seeing that
         * from CPU0, where the BPMP reads one constant at every offset, is
         * the measurement this whole handoff exists to take. */
        log_color(mb->mselect_cfg == 0xFFFFFFFF ? COL_ERR : COL_OK,
            "  MSELECT (CPU) : %08X -> %08X (TRM reset 07FF4020)\n",
            mb->mselect_cfg, mb->mselect_cfg2);
        /* probe1 is a write-read-back of AFI_AXI_BAR5_START, not a toggle of
         * AFI_PCIE_CONFIG: the witness has to be a genuinely writable
         * register. The AFI_PCIE_CONFIG disable bit for a third root port
         * this SoC does not have never changes, so a live aperture holding
         * a perfectly good 0x00103025 (xbar X4_X1 in bits 23:20) reads back
         * unchanged and would be called dead. */
        bool afi_ok = (mb->afi_probe0 != 0xFFFFFFFF) &&
                      (mb->afi_probe1 == 0xA5A50000u);
        log_color(afi_ok ? COL_OK : COL_ERR,
            "  AFI (CPU)     : cfg=%08X witness=%08X -> %s\n",
            mb->afi_probe0, mb->afi_probe1,
            afi_ok ? "APERTURE LIVE" : "not decoding");
        dx_set("wifi_pcie", afi_ok ? DX_WARN : DX_FAIL,
            afi_ok ? "PCIe aperture live, stub returned no link verdict"
                   : "PCIe does not decode even from CPU0");
        if (!afi_ok)
            return;

        /* The aperture is live, so the stub went on to train the link. */
        u32 speed = (mb->link_stat >> 16) & 0xF;
        u32 width = (mb->link_stat >> 20) & 0x3F;
        log_color(mb->port == 1 ? COL_OK : COL_ERR,
            "  RP1 link      : %s (LNKSTA=%04X gen%d x%d)\n",
            mb->port == 1 ? "UP, DL active" : "down",
            mb->link_stat >> 16, speed, width);
        LOG("  RP1 id        : %04X:%04X\n",
            mb->rp_id & 0xFFFF, mb->rp_id >> 16);

        if (mb->port != 1) {
            log_color(COL_ERR,
                "  Result        : no PCIe link on root port 1\n");
            LOG("  (SoC side and aperture are proven; the link did not\n");
            LOG("   train. This is intermittent on this console - retry\n");
            LOG("   before suspecting the module, supply or solder)\n");
            /* The PHY bring-up status is the one number that explains a
             * dead link: bit 0 = PLLE failed, bit 1 = UPHY cal failed. */
            LOG("  PHY (CPU0)    : plle=%s uphy=%s\n",
                (mb->raw_cfg[19] & 1u) ? "FAILED" : "locked",
                (mb->raw_cfg[19] & 2u) ? "FAILED" : "locked");
            dx_set("wifi_pcie", DX_FAIL, "no link on root port 1");
            ccplex_powergate_cpu0();
            LOG("  CPU0          : powergated\n");
            return;
        }

        u16 vid = mb->ep_id & 0xFFFF, did = mb->ep_id >> 16;
        bool is_4356 = (vid == BRCM_VENDOR_ID && did == BRCM_4356_DEVICE_ID);
        log_color(is_4356 ? COL_OK : COL_WARN,
            "  EP config     : %04X:%04X %s\n", vid, did,
            is_4356 ? "Broadcom " WLAN_CHIP_NAME " WLAN" : "UNEXPECTED DEVICE");
        LOG("  EP class/rev  : %06X rev %02X  BAR0=%08X (default)\n",
            mb->ep_class >> 8, mb->ep_class & 0xFF, mb->ep_bar0);
        /* raw_cfg slots are PAIRS of consecutive reads of the same
         * address - see cpu_mbox.h. On this Erista every second endpoint
         * config read returns a stale per-boot constant, so the pair is
         * the smallest unit that can be trusted: one of the two is real.
         * Expected good values: id 43EC14E4, class 02800003,
         * bar0 FF9F800x (ROM-phase default), cmd xxxx00x6, win 18000000. */
        LOG("  CFG baseline  : id=%08X/%08X cmd0=%08X class=%08X/%08X\n",
            mb->raw_cfg[0], mb->raw_cfg[1], mb->raw_cfg[2],
            mb->raw_cfg[3], mb->raw_cfg[4]);
        LOG("  CFG write chk : bar0=%08X/%08X cmd=%08X/%08X\n",
            mb->raw_cfg[6], mb->raw_cfg[7], mb->raw_cfg[8],
            mb->raw_cfg[9]);
        LOG("  CFG window    : %08X/%08X\n",
            mb->raw_cfg[10], mb->raw_cfg[11]);
        /* The command register is the one config write the ROM-phase chip
         * accepts; these are its verify rounds and the root port's
         * secondary status across the dance (RMA/RTA bits = the endpoint
         * errored a downstream request). */
        LOG("  CFG retries   : cmd=%d SECSTS %08X->%08X\n",
            mb->raw_cfg[19] & 0xFF, mb->rp_secsts0, mb->rp_secsts1);
        if (mb->raw_cfg[17] || mb->raw_cfg[18])
            LOG("  CFG recheck   : id=%08X/%08X (path alive at the end)\n",
                mb->raw_cfg[17], mb->raw_cfg[18]);
        LOG("  AFI INTR      : code=%04X sig=%04X\n",
            mb->afi_intr_code & 0xFFFF, mb->afi_intr_sig & 0xFFFF);
        if (mb->afi_fpci_to)
            LOG("  FPCI timeout  : TO=%04X\n",
                mb->afi_fpci_to & 0xFFFF);

        dx_set("wifi_pcie", is_4356 ? DX_PASS : DX_WARN,
               is_4356 ? "" : "endpoint is %04X:%04X", vid, did);

        if (is_4356) {
            /* Enumeration is the verdict: the chip answered config reads
             * with the right ID, took the command write and kept the path
             * alive - the WLAN core is present and on the bus. The die-
             * level ChipID would need a firmware download (the backplane is
             * firmware-gated in ROM phase), which is the OS's job, not the
             * probe's. */
            log_color(COL_OK,
                "  Result        : WLAN core enumerated on the PCIe bus\n");
        }
        break;
    }
    }

    /* Put the cluster back regardless of outcome. The payload self-resets
     * shortly after, but leaving a CPU running and its rail up while the
     * pager waits for input is not a state to hand back to anyone. */
    ccplex_powergate_cpu0();
    LOG("  CPU0          : powergated\n");
#endif
}

/* Bounded register poll. Everything that waits in this probe goes through
 * here, so the host's UART 'R' can still reclaim the console while a link
 * is training. */
static bool pcie_wait(volatile u32 *reg, u32 mask, u32 want, u32 ms)
{
    u32 dl = get_tmr_us() + ms * 1000;
    for (;;) {
        if ((*reg & mask) == want)
            return true;
        if (get_tmr_us() > dl)
            return false;
        usleep(50);
        uart_poll_reboot();
    }
}

/* T210 lane mux: 2 bits per pcie-N lane in XUSB_PADCTL_USB3_PAD_MUX. */
static void pcie_lane_func(u32 lane, u32 func)
{
    u32 v = XUSB_PADCTL(PADCTL_USB3_PAD_MUX);
    v &= ~(3u << PADCTL_PCIE_LANE_SHIFT(lane));
    v |=  (func & 3u) << PADCTL_PCIE_LANE_SHIFT(lane);
    XUSB_PADCTL(PADCTL_USB3_PAD_MUX) = v;
}

/* Take the three PCIe I/O pads out of deep power down. Mirrors
 * tegra_io_pad_prepare/power_enable/unprepare with the DPD_SAMPLE window
 * the PMC needs around the request. */
static bool pcie_io_pads_wake(u32 *before)
{
    *before = PMC(APBDEV_PMC_IO_DPD_STATUS) & PMC_IO_DPD_PEX_MASK;
    if (!*before)
        return true;                 /* already awake, nothing to request */

    PMC(APBDEV_PMC_DPD_SAMPLE) = 1;
    PMC(APBDEV_PMC_SEL_DPD_TIM) = 0x1F;   /* >= 200 ns in PCLK cycles     */
    PMC(APBDEV_PMC_IO_DPD_REQ)  = PMC_IO_DPD_REQ_DPD_OFF | PMC_IO_DPD_PEX_MASK;
    bool ok = pcie_wait(&PMC(APBDEV_PMC_IO_DPD_STATUS),
                        PMC_IO_DPD_PEX_MASK, 0, 250);
    PMC(APBDEV_PMC_DPD_SAMPLE) = 0;
    return ok;
}


/* T210B01 ONLY: the UPHY management clock.
 *
 * This is the difference between a Jetson TX1, where U-Boot's UPHY sequence
 * works, and a Mariko, where it does not. On Erista the padctl PCIe pad takes
 * exactly one clock - tegra210.dtsi gives it `clocks = <PLL_E>; clock-names =
 * "pll";`. On T210B01 it takes two: NVIDIA's own tegra210b01 device tree adds
 * `<PLL_P_UPHY_OUT>` as "uphy_mgmt", and L4T's driver enables it inside
 * tegra210_pcie_pad_probe() behind `if (t210b01_compatible(padctl) == 1)`,
 * before a single UPHY register is touched.
 *
 * That clock is what runs the UPHY calibration state machine and its indirect
 * config port. With it gated, CAL_EN latches and the FSM simply never
 * advances, so CAL_DONE stays 0 for ever - while ELPG, the lane mux and
 * CTL1/2/4 keep reading and writing perfectly, because those are ordinary
 * padctl APB registers.
 *
 * The CAR side is two registers, neither of which BDK touches anywhere:
 * PLLP_MISC1 carries the pll_p_out_hsio and pll_p_out_xusb branch gates, and
 * PEX_SATA_USB_RX_BYP - a register that means something else entirely on
 * Erista - is repurposed on B01 as this clock's divider and gate. So the
 * whole block is gated on Mariko: writing 0x6D0 on an Erista would be poking
 * a live RX-bypass control for no reason. */
#define CLK_RST_PLLP_MISC1           0x680
#define  PLLP_MISC1_HSIO_EN          BIT(29)
#define  PLLP_MISC1_XUSB_EN          BIT(28)
#define CLK_RST_PEX_SATA_USB_RX_BYP  0x6D0
#define  UPHY_MGMT_DIV_MASK          0xFFu
#define  UPHY_MGMT_CLK_EN            BIT(8)
/* Tegra's 7.1 fractional divider is rate = parent * 2 / (reg + 2), so PLLP's
 * 408 MHz over reg=6 gives the 102 MHz NVIDIA's own init table asks for.
 * Leaving the field at 0 would run it at 408 MHz, out of spec. */
#define  UPHY_MGMT_DIV_102MHZ        6

/* Is the pad controller readable at all? A module held in reset does not
 * error a transaction, it never completes one - the same rule that made
 * MSELECT kill the console, and XUSB_PADCTL obeys it too. Reading
 * PADCTL_ELPG_PROGRAM before clearing RST_DEV_W bit 14 hangs this Mariko
 * dead. So nothing in this file may touch XUSB_PADCTL without asking this
 * first. */
static bool pcie_padctl_readable(void)
{
    return ((CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_W) >> CLK_W_XUSB_PADCTL) & 1) == 0;
}

/* Take the pad controller itself out of reset and out of ELPG clamp, so its
 * registers latch what is written to them.
 *
 * This is separate from the lane-mux programming, and the order is
 * load-bearing. A module held in reset does not remember writes, and when
 * it is later released every register reverts to its reset value - so the
 * lane mux MUST be programmed after this runs, not before. Doing it the
 * other way round is close to undetectable on lane pcie-0, because the
 * encoding for pcie-x1 is 0 and 0 is also the reset value: the dropped
 * write and the successful write are the same register contents. It is only
 * visible on the x4 lanes, whose encoding is 3, which is exactly the path a
 * root-port-0 fallback would need. */
static void pcie_padctl_ungate(void)
{
    u32 v;

    /* Mariko's UPHY management clock, before anything else: L4T enables it at
     * pad-probe time, ahead of every UPHY access, and the calibration FSM
     * below is what it clocks. Erista has no such clock and 0x6D0 means
     * something else there, so this is strictly T210B01. */
    if (((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2) {
        CLOCK(CLK_RST_PLLP_MISC1) |= PLLP_MISC1_HSIO_EN | PLLP_MISC1_XUSB_EN;
        usleep(2);
        u32 mgmt = (CLOCK(CLK_RST_PEX_SATA_USB_RX_BYP) & ~UPHY_MGMT_DIV_MASK) |
                   UPHY_MGMT_DIV_102MHZ;
        CLOCK(CLK_RST_PEX_SATA_USB_RX_BYP) = mgmt;                    /* divider */
        CLOCK(CLK_RST_PEX_SATA_USB_RX_BYP) = mgmt | UPHY_MGMT_CLK_EN; /* gate on */
        usleep(2);
        LOG("  uphy mgmt clk : PLLP_MISC1=%08X RX_BYP=%08X (T210B01, 102 MHz)\n",
            CLOCK(CLK_RST_PLLP_MISC1), CLOCK(CLK_RST_PEX_SATA_USB_RX_BYP));
    }

    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_PADCTL);
    (void)CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_W);
    usleep(10);

    /* Confirm before the first read, exactly as for MSELECT: if the reset
     * did not clear, the ELPG read below would never return. */
    if (!pcie_padctl_readable())
        return;

    /* Release the UPHY power clamps in order - CLAMP_EN, then
     * CLAMP_EN_EARLY, then VCORE_DOWN - 100 us apart. */
    v = XUSB_PADCTL(PADCTL_ELPG_PROGRAM);
    v &= ~ELPG_CLAMP_EN;
    XUSB_PADCTL(PADCTL_ELPG_PROGRAM) = v;
    usleep(100);
    v &= ~ELPG_CLAMP_EN_EARLY;
    XUSB_PADCTL(PADCTL_ELPG_PROGRAM) = v;
    usleep(100);
    v &= ~ELPG_VCORE_DOWN;
    XUSB_PADCTL(PADCTL_ELPG_PROGRAM) = v;

    CLOCK(CLK_RST_CONTROLLER_RST_DEV_Y_CLR) = BIT(CLK_Y_PEX_USB_UPHY);
}


void probe_wifi_pcie(void)
{
    log_color(COL_INFO, "[Wi-Fi radio (" WLAN_CHIP_NAME " WLAN core over PCIe)]\n");

    if (!WIFI_CFG_AUTORUN && !g_wifi_armed) {
        log_color(COL_WARN,
            "  Not armed     : boots the CPU cluster, so not in the sweep\n");
        LOG("  Send 'W' over UART, or press 'w' on the pager, to run it.\n");
        LOG("  (build -DWIFI_CFG_AUTORUN=1 to run it automatically)\n");
        return;
    }
    g_wifi_armed = false;    /* one shot: re-arm deliberately each time */

    /* ---- entry state -------------------------------------------------- */
    u32 enb_u = CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_U);
    u32 rst_u = CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_U);
    LOG("  entry clocks  : PCIE=%d AFI=%d  rst PCIE=%d AFI=%d PCIEXCLK=%d\n",
        (enb_u >> CLK_U_PCIE) & 1, (enb_u >> CLK_U_AFI) & 1,
        (rst_u >> CLK_U_PCIE) & 1, (rst_u >> CLK_U_AFI) & 1,
        (rst_u >> CLK_U_PCIEXCLK) & 1);
    LOG("  entry power   : PWRGATE PCIE=%d  PLLE lock=%d PLLREFE lock=%d\n",
        (PMC(APBDEV_PMC_PWRGATE_STATUS) >> POWER_RAIL_PCIE) & 1,
        (CLOCK(CLK_RST_CONTROLLER_PLLE_MISC) & PLLE_MISC_LOCK) ? 1 : 0,
        (CLOCK(CLK_RST_PLLREFE_MISC) & PLLREFE_MISC_LOCK) ? 1 : 0);

    /* Which of the three PCIe apertures the CPU complex's own decoder treats
     * as MMIO. TRM 12.6.172: "configures some of the address apertures (in
     * CCPLEX-AXD) to be MMIO or DRAM", 0 = MMIO, 1 = DRAM, and it is
     * write-disabled once PMC_SEC_DISABLE has been set. That decoder is the
     * one CPU0 uses to reach PCIe, so an aperture flipped to DRAM here sends
     * the stub's config reads at DRAM instead of at the controller -- which
     * looks exactly like a bus that never answers. Boot-time state, and
     * nothing in this payload writes it, so it is pure information. */
    {
        u32 amap = PMC(APBDEV_PMC_GLB_AMAP_CFG);
        LOG("  CCPLEX amap   : %08X  PCIe apertures A1=%s A2=%s A3=%s\n", amap,
            (amap & BIT(1)) ? "DRAM" : "MMIO",
            (amap & BIT(2)) ? "DRAM" : "MMIO",
            (amap & BIT(3)) ? "DRAM" : "MMIO");
    }

    /* The two 1.05 V rails the PCIe analog blocks sit on, named in the
     * Switch DT's pcie@1003000 supply list: LDO1 = VDD_PEX_1V05 feeds
     * dvddio-pex and dvdd-pex-pll, LDO7 = AVDD_1V05_PLL feeds
     * avdd-pll-uerefe (the UPHY reference PLL). Both are
     * regulator-always-on under Horizon; a bare payload inherits whatever
     * the boot ROM left, so read, then force, then read back. */
    /* Enabling these is not enough - they have to be at the voltage the
     * board specifies, and on a bare payload they are not.
     *
     * LDO7 is AVDD_1V05_PLL, which the Switch DT hangs `avdd-pll-uerefe-
     * supply` off for pcie@1003000: it is the analog supply of the UPHY
     * REFERENCE PLL, the one whose calibration this probe waits on. The DT
     * pins it at exactly 1050000 uV. A console in RCM is still sitting on
     * its cold OTP default, which on Mariko is 1.000 V (Erista 1.05 V), and
     * pcv - the HOS service that would program it - never runs here.
     * On a Mariko in RCM LDO7 reads off at 1.000 V, and UPHY PLL P0
     * frequency calibration does not complete. Enabling the rail without
     * correcting the voltage calibrates the SERDES on an under-volted
     * reference.
     *
     * LDO1 is VDD_PEX_1V05, feeding dvddio-pex and dvdd-pex-pll; it already
     * reads 1.050 V on both generations, but it is set explicitly anyway so
     * the probe does not depend on that staying true.
     *
     * Names are padded in the table, not with a printf width: bdk's
     * s_printf only understands `%<fill><width>s` (sprintf.c:138-163), so a
     * `%-18s` would print "%-" then "18s" and consume the wrong varargs. */
    /* step_mv is per rail, NOT a constant: probe_regulators' own table has
     * LDO1 at 25 mV/step and LDO7 at 50 mV/step from a 800 mV base. Decoding
     * both at 25 mV misprints LDO7's correct 1.05 V as "925 mV". */
    static const struct { u32 id; u8 cfg; u32 uv; u32 step_mv; const char *name; }
    pex_rails[] = {
        { REGULATOR_LDO1, 0x25, 1050000, 25, "LDO1 VDD_PEX_1V05 " },
        { REGULATOR_LDO7, 0x31, 1050000, 50, "LDO7 AVDD_1V05_PLL" },
    };
    for (u32 i = 0; i < ARRAY_SIZE(pex_rails); i++) {
        u8 was = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, pex_rails[i].cfg);
        int vr = max7762x_regulator_set_voltage(pex_rails[i].id, pex_rails[i].uv);
        max7762x_regulator_enable(pex_rails[i].id, true);
        msleep(5);
        u8 now = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, pex_rails[i].cfg);
        u32 mv_was = 800 + (was & 0x3F) * pex_rails[i].step_mv;
        u32 mv_now = 800 + (now & 0x3F) * pex_rails[i].step_mv;
        log_color((now >> 6) && mv_now == pex_rails[i].uv / 1000 ? COL_OK : COL_ERR,
            "  %s: en %d->%d  %d->%d mV\n", pex_rails[i].name,
            was >> 6, now >> 6, mv_was, mv_now);
        /* max7762x_regulator_set_voltage returns 0 on success (max7762x.c:183). */
        if (vr)
            LOG("  (set_voltage refused %d uV on this rail)\n", pex_rails[i].uv);
    }

    /* ---- let PERST# and CLKREQ# actually leave the SoC ----------------
     * PINMUX_AUX_PEX_L1_RST_N (0x70003044) resets to 0x460, and bit 5 of
     * that is PARK. A parked pad does not drive, so the controller can
     * toggle AFI_PEX1_CTRL's reset bit all it likes and the endpoint's
     * PCIE_PERST_L never moves - it just sits deasserted on its own
     * pull-up, and the endpoint never gets the reset that releases its
     * configuration block. The mux field is already PE1 (PM = 0) out of
     * reset, so PARK, and TRISTATE on the reset line, are the whole job.
     * CLKREQ# is an input to the SoC, so it keeps its tristate. */
    {
        u32 rst_pmx = PINMUX_AUX(PMX_PEX_L1_RST_N);
        u32 clk_pmx = PINMUX_AUX(PMX_PEX_L1_CLKREQ_N);
        PINMUX_AUX(PMX_PEX_L1_RST_N) =
            (rst_pmx & ~(PINMUX_PARKED | PINMUX_TRISTATE | 3u));
        /* CLKREQ# is left tristated on purpose - TRM 34.3.1 step 19a sets
         * PINMUX_AUX_PEX_L1_CLKREQ_N_0[TRISTATE] to 1 when L1 PM substates
         * are disabled, which they are here. Only unpark it. */
        PINMUX_AUX(PMX_PEX_L1_CLKREQ_N) =
            (clk_pmx & ~(PINMUX_PARKED | 3u)) | PINMUX_TRISTATE;
        LOG("  PEX_L1 pads   : RST %08X->%08X  CLKREQ %08X->%08X\n",
            rst_pmx, PINMUX_AUX(PMX_PEX_L1_RST_N),
            clk_pmx, PINMUX_AUX(PMX_PEX_L1_CLKREQ_N));
    }

    /* ---- WL_REG_ON: start the endpoint's own power-up ----------------- */
    u32 t_regon;
#if WIFI_CFG_COLD_CYCLE
    /* PH1 low holds the WLAN section in reset (p143 s21.1.1). BT_REG_ON is
     * deliberately not touched: with PH4 high the shared CBUCK stays up, so
     * this is a WLAN-section reset rather than a whole-package cold start,
     * and the Bluetooth probe's result upstream is not invalidated. The
     * 10 ms discharge rule is qualified "where both signals have been
     * driven low", so it does not bind here - 20 ms is taken anyway. */
    /* Report PH0 as found, before the lines below move it. The default
     * (WIFI_CFG_DRIVE_PH0=1) drives it: low here for the cold cycle, back
     * high as the enable 10 ms ahead of PH1, and left high on exit. See
     * WIFI_CFG_DRIVE_PH0. */
    LOG("  PH0 wifi_en   : %d on entry (%s)\n",
        gpio_read(GPIO_PORT_H, GPIO_PIN_0),
        WIFI_CFG_DRIVE_PH0 ? "driven high by this probe"
                           : "left as found");
#if WIFI_CFG_DRIVE_PH0
    PINMUX_AUX(PMX_PH0_WIFI_EN) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_0, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_0, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_0, 1);
#endif

    PINMUX_AUX(PMX_PH1_WL_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_1, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_1, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_1, 1);
    pcie_msleep(20);
    /* Enable, then release the reset. WL_REG_ON starts the section's
     * regulators and the internal power-on sequence; the reset should not
     * come off until that has had its Tvddtopor to run. */
#if WIFI_CFG_DRIVE_PH0
    GP_MWR(GPH_MOUT, GPIO_PIN_0, 1);
    (void)GPIO(GPH_OUT);
#endif
    t_regon = get_tmr_us();
    pcie_msleep(10);
    /* PH1 is named wifi_rst, and nothing so far establishes its polarity.
     * Left high it is either "reset released" (active low) or "held in
     * reset" (active high) - and the second would look exactly like a link
     * that trains with a dead core, because the PCIe PHY trains off the
     * shared rail whether or not the WLAN core behind it is running. */
    GP_MWR(GPH_MOUT, GPIO_PIN_1, WIFI_CFG_PH1_LEVEL);
    (void)GPIO(GPH_OUT);
    log_color(gpio_read(GPIO_PORT_H, GPIO_PIN_1) ? COL_OK : COL_ERR,
        "  PH0/PH1       : %d,%d (cold cycle, t=0 for Table 62)\n",
        gpio_read(GPIO_PORT_H, GPIO_PIN_0),
        gpio_read(GPIO_PORT_H, GPIO_PIN_1));
#else
    t_regon = get_tmr_us();
    LOG("  PH1 WL_REG_ON : %d (left as found)\n",
        gpio_read(GPIO_PORT_H, GPIO_PIN_1));
#endif

    /* ---- Tegra root complex, interleaved into the 57 ms POR wait ------ */
    u32 dpd_before = 0;
    bool dpd_ok = pcie_io_pads_wake(&dpd_before);
    log_color(dpd_ok ? COL_OK : COL_ERR,
        "  PEX I/O pads  : DPD %s%s\n",
        dpd_before ? "was parked, released" : "already awake",
        dpd_ok ? "" : "  (STILL PARKED)");

    /* Reset everything, drop the partition, then bring it back up in the
     * order tegra_powergate_sequence_power_up() uses. Note the clamping
     * quirk: PCIE and VDEC are swapped in REMOVE_CLAMPING_CMD, a Tegra20
     * bug carried forward, so the PCIe partition is unclamped by writing
     * the VDEC bit (powergate.c tegra_powergate_remove_clamping). */
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) =
        BIT(CLK_U_PCIE) | BIT(CLK_U_AFI) | BIT(CLK_U_PCIEXCLK);

    /* U-Boot powers the partition OFF before bringing it up, because it has
     * to assume an unknown state. This console is measurably NOT in an
     * unknown state: PWRGATE_STATUS bit 3 already reads 1 at payload entry,
     * on both a Mariko and an Erista. Cycling an already-live partition is
     * a real state change here rather than a no-op, and powering a block
     * down is the kind of thing that leaves it wedged - so only do it when
     * the partition is actually gated. */
    int pg = 0;
    bool pg_was_up = (PMC(APBDEV_PMC_PWRGATE_STATUS) >> POWER_RAIL_PCIE) & 1;
    if (!pg_was_up)
        pg = pmc_domain_pwrgate_set(POWER_RAIL_PCIE, 1);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_SET) = BIT(CLK_U_PCIE);
    usleep(10);
    PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) = BIT(4);   /* VDEC bit = PCIE */
    usleep(10);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(CLK_U_PCIE);
    u32 pg_state = (PMC(APBDEV_PMC_PWRGATE_STATUS) >> POWER_RAIL_PCIE) & 1;
    log_color((pg == 0 && pg_state) ? COL_OK : COL_ERR,
        "  PCIE partition: %s (was %s, bit %d = %d)\n",
        (pg == 0 && pg_state) ? "ungated" : "UNGATE FAILED",
        pg_was_up ? "already up, left alone" : "gated, ungated now",
        POWER_RAIL_PCIE, pg_state);
    /* An AFI read into a gated partition is another access that never
     * completes rather than faulting, so this is a hard stop, not a
     * warning to walk past. */
    if (pg != 0 || !pg_state) {
        log_color(COL_ERR,
            "  Result        : PCIE partition down, refusing to read AFI\n");
        dx_set("wifi_pcie", DX_FAIL, "PCIE power partition would not ungate");
        return;
    }

    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(CLK_U_AFI);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_SET) = BIT(CLK_U_AFI);

    /* CML clock (pci_tegra.c has_cml_clk): PLLE_AUX bit 0 set, bit 1 clear. */
    u32 aux = CLOCK(CLK_RST_CONTROLLER_PLLE_AUX);
    aux |= PLLE_AUX_CML_ENABLE;
    aux &= ~PLLE_AUX_CML_BIT1;
    CLOCK(CLK_RST_CONTROLLER_PLLE_AUX) = aux;

    /* U-Boot's tegra_pcie_power_on cycles the PCIe partition: the
     * powergate OFF, then the sequence power-up (powergate ON, clock,
     * 10 us, clamp removal, 10 us, reset release). The endpoint sees a
     * full host-side power episode - PERST# drop and a lane electrical
     * idle - which the stub's config reads depend on. The AFI stays in
     * reset through the cycle, exactly as U-Boot leaves it. */
    LOG("  PCIe partition: powergate cycle (U-Boot's power_on)\n");
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) =
        BIT(CLK_U_PCIE) | BIT(CLK_U_AFI) | BIT(CLK_U_PCIEXCLK);
    pmc_domain_pwrgate_set(POWER_RAIL_PCIE, 0);
    usleep(10);
    pmc_domain_pwrgate_set(POWER_RAIL_PCIE, 1);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_SET) = BIT(CLK_U_PCIE);
    usleep(10);
    PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) = BIT(4);   /* VDEC bit = PCIE */
    usleep(10);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(CLK_U_PCIE);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(CLK_U_AFI);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_SET) = BIT(CLK_U_AFI);

    /* The PLLE/PLLREFE enable itself runs on CPU0 - the stub's ported
     * driver owns the whole PHY bring-up. */
    LOG("  PLLREFE/PLLE  : left to the CPU0 side\n");

    /* ---- SERDES, before anything asks the PCIe side a question ----
     *
     * All of this is XUSB pad controller and CAR, so the BPMP can do it
     * even though it cannot reach one PCIe register. It has to happen
     * BEFORE the AFI aperture test and the CPU0 handoff below, because
     * the root-port register windows do not answer until the port has
     * lanes and the UPHY PLL is locked. */
    /* Pad controller out of reset FIRST, then the lane mux, then the PLL.
     * The ordering is load-bearing: a padctl held in reset does not latch
     * writes and reverts every register when released, so a lane mux
     * programmed before this line is silently thrown away. */
    pcie_padctl_ungate();
    if (!pcie_padctl_readable()) {
        log_color(COL_ERR,
            "  Result        : XUSB_PADCTL stuck in reset, refusing to read\n");
        dx_set("wifi_pcie", DX_FAIL, "XUSB_PADCTL would not leave reset");
        return;
    }

    /* pcie-0 carries the x1 link to the radio, pcie-1..4 the x4 group.
     * pcie-5/6 are usb3-ss on this board and are left strictly alone. */
    /* TRM 34.3.1 step 13: "Disable the IDDQ for all lanes for PCIe tests" -
     * XUSB_PADCTL_USB3_PAD_MUX_0[FORCE_PCIE_PAD_IDDQ_DISABLE_MASK0..4],
     * which are bits 1 through 5 and reset to 0. Left at that default
     * (USB3_PAD_MUX reads 0x817FC000, the whole low byte clear) every lane
     * runs with its IDDQ override at the reset value while the mux changes
     * underneath it. */
    XUSB_PADCTL(PADCTL_USB3_PAD_MUX) |= PADCTL_FORCE_PCIE_IDDQ_DIS_ALL;

    /* TRM 34.3.1 steps 16-18: park each lane before its ownership changes,
     * then release it.
     *
     * Step 16 forces TX/RX power and IDDQ under software override and drives
     * IDDQ on, step 17 changes the mux, step 18 drops the overrides so the
     * hardware owns the lane again. The point of the bracketing is that the
     * lane is quiescent while its owner changes underneath it - a lane
     * re-pointed live can come up in a state where the link still trains,
     * because training ordered sets are enormously redundant, while
     * ordinary traffic does not survive.
     *
     * P0..P4 are the five PCIe lanes, which is the set the TRM lists; the
     * USB 3.0 super-speed lanes are separate pads and are not touched. */
    for (u32 l = 0; l <= 4; l++) {
        u32 r = PADCTL_UPHY_MISC_PAD_CTL_2(l);
        XUSB_PADCTL(r) |= UPHY_MISC_TX_PWR_OVRD | UPHY_MISC_RX_PWR_OVRD;
        XUSB_PADCTL(r) |= UPHY_MISC_TX_IDDQ | UPHY_MISC_RX_IDDQ;
        XUSB_PADCTL(r) |= UPHY_MISC_TX_IDDQ_OVRD | UPHY_MISC_RX_IDDQ_OVRD;
    }

    pcie_lane_func(0, PADCTL_LANE_FUNC_PCIE_X1);
#if WIFI_CFG_X4_LANES
    for (u32 l = 1; l <= 4; l++)
        pcie_lane_func(l, PADCTL_LANE_FUNC_PCIE_X4);
#endif

    /* U-Boot's exact USB3_PAD_MUX state, byte for byte (the padctl
     * reference dump): pcie-0 owned by PCIe (field code 1), every
     * lane's IDDQ released (bits 1..8), the unconnected lanes per the
     * DT pinmux. The code-1 field alone breaks link training; it needs
     * the IDDQ bits alongside it, which is exactly what U-Boot's pinmux
     * programming writes. */
    XUSB_PADCTL(PADCTL_USB3_PAD_MUX) = 0x817FC1FEu;

    for (u32 l = 0; l <= 4; l++) {
        u32 r = PADCTL_UPHY_MISC_PAD_CTL_2(l);
        XUSB_PADCTL(r) &= ~(UPHY_MISC_TX_PWR_OVRD | UPHY_MISC_RX_PWR_OVRD);
        XUSB_PADCTL(r) &= ~(UPHY_MISC_TX_IDDQ_OVRD | UPHY_MISC_RX_IDDQ_OVRD);
    }
    LOG("  lane power    : P0_CTL_2=%08X (IDDQ/PWR overrides released)\n",
        XUSB_PADCTL(PADCTL_UPHY_MISC_PAD_CTL_2(0)));
    LOG("  lane mux      : USB3_PAD_MUX=%08X (pcie-0 -> pcie-x1)\n",
        XUSB_PADCTL(PADCTL_USB3_PAD_MUX));

    /* Per-lane setup L4T does for every PCIe lane, which no public T210
     * driver has: on B01, load the lane's own defaults through its indirect
     * port. Lane 0 is the one carrying the radio; the x4 group gets the same
     * treatment when it is in play. U-Boot never touches PX_CTL1, and this
     * path leaves it alone too. */
    {
        bool b01 = (((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2);
        u32 last = WIFI_CFG_X4_LANES ? 4 : 0;
        for (u32 l = 0; l <= last; l++) {
            if (b01)
                XUSB_PADCTL(PADCTL_UPHY_MISC_PAD_PX_CTL8(l)) =
                    UPHY_CFG_ADDR(0x97) | UPHY_CFG_WDATA(0x0080) |
                    UPHY_CFG_RESET | UPHY_CFG_WS;
        }
    }

    /* The UPHY cal, the PLLE sequencer handoff and the lane IDDQ all
     * run on CPU0, exactly where U-Boot's driver runs them: the stub
     * ports that code (stub_plle_enable/stub_uphy_enable). Pre-calibrating
     * on the BPMP leaves the UPHY sequencer-owned, which broke U-Boot's
     * re-calibration. The padctl ungate and the lane mux above are its
     * preconditions. */
    LOG("  UPHY PLL P0   : left to the CPU0 side\n");

    /* ---- honour Tvddtopor before the endpoint is asked anything ------- */
    u32 elapsed = (get_tmr_us() - t_regon) / 1000;
    if (elapsed < 58) {
        pcie_msleep(58 - elapsed + 1);   /* +1 covers the truncating divide */
        elapsed = (get_tmr_us() - t_regon) / 1000;
    }
    LOG("  POR window    : %d ms since WL_REG_ON (Tvddtopor 57 ms)\n", elapsed);
    g_wifi_brought_up = true;
}

/* Second half, on its own page: the CPU0 handoff and everything it reports.
 *
 * The two are split because together they overrun the 22 rows this display
 * has, not because they are independent - this half needs the bring-up to
 * have happened. In the boot sweep it always has, since the pages run in
 * order. Re-running this page on its own from the pager would not, so it
 * brings the SoC side up first when it finds it has not been done, with the
 * output suppressed so the page still fits. */
void probe_wifi_link(void)
{
    HEADER("[Wi-Fi radio - PCIe link and endpoint]");

    /* The endpoint's config space only answers when the full bring-up
     * precedes the handoff: a bare handoff re-run reads FFFF:FFFF,
     * because the endpoint does not survive the PERST#/link re-train
     * without the WL_REG_ON/PLLE/padctl sequence ahead of it. So the
     * bring-up runs (quietly) before EVERY handoff. */
    bool armed = g_wifi_armed;
    g_wifi_armed = true;
    g_wifi_quiet_pass = true;
    log_mute(true);
    probe_wifi_pcie();
    log_mute(false);
    g_wifi_quiet_pass = false;
    g_wifi_armed = armed;
    if (!g_wifi_brought_up) {
        log_color(COL_WARN, "  Not armed     : nothing to report\n");
        return;
    }

    /* Everything from MSELECT onwards has to be issued by a CPU-complex
     * master, so CPU0 takes it from here and the BPMP supervises. */
    pcie_cpu_handoff();
}
