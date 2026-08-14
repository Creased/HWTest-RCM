/*
 * Shared BPMP <-> CCPLEX mailbox.
 *
 * Included by BOTH sides: by main.c, compiled for the ARM7 BPMP, and by
 * cpu_stub/stub.c, compiled for an AArch64 Cortex-A57. Keep it free of
 * anything either toolchain cannot see - no bdk headers, no libc, plain
 * fixed-width types and offsets that are identical in 32-bit and 64-bit.
 *
 * WHY THERE IS A CPU STUB AT ALL
 *
 * The PCIe register apertures are not reachable from the BPMP. That is
 * architectural, not a bug: the Tegra X1 TRM's AHB chapter puts the
 * BPMP-Lite crossbar on AHB and lists the AHB slaves as XBAR, DRAM,
 * USB-OTG/USB2 and TZRAM - PCIe is not one of them - while the PCIe aperture
 * hangs off MSELECT, whose registers are documented in the CPU COMPLEX
 * chapter. Measured to match: with clocks, resets, power and PLLE all
 * correct, the BPMP reads 0xFFFFFFFF from the AFI window and the same
 * constant from every offset of MSELECT, on both an Erista and a Mariko.
 *
 * So the accesses that must come from a CPU-complex master are done by a
 * short stub on CPU0, and the BPMP supervises.
 *
 * THIS IS ALSO THE SAFER ARRANGEMENT, which is the real reason to do it.
 * A block held in reset, or an aperture that does not decode, does not error
 * a transaction on this SoC - it never completes one. When the BPMP makes
 * such an access the whole console goes silent and needs a manual power
 * cycle, because the BPMP is also what runs the UART, the pager and the
 * reset path. Move those accesses to CPU0 and a stall takes out CPU0
 * alone: the BPMP keeps running, notices the mailbox stopped advancing,
 * prints the last breadcrumb the stub wrote, powergates the cluster and
 * reboots itself. The failure mode goes from "fetch the console" to "a line
 * of output".
 */

#ifndef CPU_MBOX_H
#define CPU_MBOX_H

/* DRAM, well clear of everything else in play: hwtest's heap starts at
 * 0x90000000, hekate's own users sit lower still, and the framebuffer is up
 * at 0xF5A00000. Both addresses are 32-bit so the BPMP can name them. */
#define CPU_STUB_ADDR   0xA0000000u   /* stub image is copied here        */
#define CPU_STUB_MAX    0x00010000u   /* 64 KiB ceiling, asserted at build */
#define CPU_STACK_TOP   0xA0020000u   /* stub's stack, grows down          */
#define CPU_MBOX_ADDR   0xA0030000u   /* mailbox, its own page             */

#define CPU_MBOX_MAGIC  0x50434945u   /* 'PCIE', written once on entry     */
#define CPU_MBOX_DONE   0x444F4E45u   /* 'DONE', written last              */
#define CPU_MBOX_EXC    0x45584321u   /* 'EXC!', an exception was taken    */
/* diag_mode values: a second handoff after a hung read, to observe AFI state
 * the wedged core could not read itself. */
#define CPU_DIAG_NONE       0u
#define CPU_DIAG_READ_INTR  0x52494e54u   /* 'RINT' */

/* Breadcrumbs. The stub stores one of these to `stage` BEFORE the access it
 * names, so if that access never returns the BPMP still knows which one it
 * was. */
#define CPU_STAGE_ENTRY        1u
#define CPU_STAGE_MSELECT_RD   2u
#define CPU_STAGE_MSELECT_WR   3u
#define CPU_STAGE_AFI_RD       4u
#define CPU_STAGE_AFI_WR       5u
#define CPU_STAGE_XLATE        6u   /* AFI address translations           */
#define CPU_STAGE_CTRL_EN      7u   /* xbar, fuse, EN_FPCI, interrupts    */
#define CPU_STAGE_PORT_EN      8u   /* refclk + PERST# on root port 1     */
#define CPU_STAGE_LINK_WAIT    9u   /* DL_UP / DL_LINK_ACTIVE poll        */
#define CPU_STAGE_CFG_RD      10u   /* endpoint configuration space       */
#define CPU_STAGE_BAR_RD      11u   /* endpoint BAR0/window reads + cmd wr  */
#define CPU_STAGE_DONE        12u

/* Byte offsets of the exception-capture fields, for the assembly handler,
 * which cannot see the struct. The C side _Static_asserts these against
 * offsetof, so the two cannot drift apart silently - hand-counted offsets
 * are exactly the sort of thing that is wrong by two fields and writes the
 * fault code over a result. */
#define MBOX_OFF_STAGE      0x04
/* raw_cfg holds 20 dwords (0x50 bytes) for the read-verification pairs:
 * every second endpoint config read on the Erista test unit comes back
 * with a stale per-boot constant, so single readbacks prove nothing and
 * every value of interest is sampled twice. */
#define MBOX_OFF_EXC_TAKEN  0xA4
#define MBOX_OFF_EXC_ESR    0xA8
#define MBOX_OFF_EXC_FAR    0xAC
#define MBOX_OFF_EXC_ELR    0xB0
#define MBOX_OFF_EXC_STAGE  0xB4

#ifndef __ASSEMBLER__

/* Laid out so both compilers agree byte for byte: 32-bit fields only, no
 * pointers, no enums, no packing attributes needed. */
typedef struct {
    volatile unsigned int magic;      /* CPU_MBOX_MAGIC once CPU0 is alive */
    volatile unsigned int stage;      /* last CPU_STAGE_* entered          */
    volatile unsigned int heartbeat;  /* ++ per step, proves forward motion*/
    volatile unsigned int done;       /* CPU_MBOX_DONE when finished       */

    volatile unsigned int mselect_cfg;   /* MSELECT_CONFIG_0 as read       */
    volatile unsigned int mselect_cfg2;  /* re-read after the write        */
    volatile unsigned int afi_probe0;    /* AFI_PCIE_CONFIG as read        */
    volatile unsigned int afi_probe1;    /* witness write read back        */

    volatile unsigned int port;          /* root port that came up, or ~0  */
    volatile unsigned int link_stat;     /* RP_LINK_CONTROL_STATUS         */
    volatile unsigned int rp_id;         /* root port vendor:device        */
    volatile unsigned int ep_id;         /* endpoint vendor:device         */
    volatile unsigned int ep_class;      /* class code + revision          */
    volatile unsigned int ep_bar0;       /* BAR0 after assignment          */
    /* Config-space diagnostics, as pairs of consecutive reads of the same
     * address. On the Erista test unit every second endpoint config read
     * returns a stale per-boot constant (strict good/bad alternation in
     * access order, independent of register and of delays), so nothing
     * downstream is trusted from a single read.
     *  [0]/[1]  cfg 0x00 VID:DID dword, twice   [2]  cfg 0x04 baseline
     *  [3]/[4]  cfg 0x08 class/rev, twice      [5]  RP0 id, pre-PHY probe
     *  [6]/[7]  RP0 post-PHY/ctrl probes, then default BAR0 (0x10) x2
     *  [8]/[9]  cmd (0x04) write-verify x2     [10]/[11] RP devctl/SECSTS,
     *                                               then window (0x80) x2
     *  [12]/[13] cfg 0x00 VID:DID dword, twice [14]  cfg 0x0E header type
     *  [15]     cfg 0x08 class/rev, first      [16]  PADS_REFCLK_CFG0 rb
     *  [17]/[18] cfg 0x00 re-read at the end (path still alive?)
     *  [19]     PHY bring-up status (bit0 = PLLE failed, bit1 = UPHY
     *           failed) until the cmd write-verify rounds overwrite it
     *           on the full-success path                               */
    volatile unsigned int raw_cfg[20];
    volatile unsigned int car_rst_u;     /* RST_DEVICES_U as CPU0 reads it */
    volatile unsigned int afi_cfg_en;    /* AFI_CONFIGURATION readback     */
    volatile unsigned int bar0_start;    /* AFI_AXI_BAR0_START readback    */
    volatile unsigned int bar0_sz;       /* AFI_AXI_BAR0_SZ readback       */
    volatile unsigned int bar0_fpci;     /* AFI_FPCI_BAR0 readback         */
    volatile unsigned int rp_bus;        /* RP_BUS_NUMBERS readback        */
    volatile unsigned int rp_cmd;        /* RP command/status readback     */

    /* Exception capture. Without these an aborting access and a stalled one
     * are indistinguishable: CPU0 runs at EL3, and with no vectors installed
     * a synchronous external abort branches into whatever VBAR_EL3 happens
     * to hold, the mailbox stops advancing, and the supervisor calls it a
     * stall. The stub installs vectors and records the fault here, so
     * "the bus never answered" and "the bus said no" can be told apart. */
    volatile unsigned int exc_taken;     /* CPU_MBOX_EXC once one fires    */
    volatile unsigned int exc_esr;       /* ESR_EL3  - fault class + code  */
    volatile unsigned int exc_far;       /* FAR_EL3  - faulting address    */
    volatile unsigned int exc_elr;       /* ELR_EL3  - faulting instruction*/
    volatile unsigned int exc_stage;     /* stage at the time of the fault */
    /* Root port Secondary Status (config 0x1C): bit 29 = Received Master
     * Abort, bit 28 = Received Target Abort, bit 30 = Signalled System
     * Error. Sampled before and after the downstream config/memory writes,
     * so a set bit is attributable to those accesses and not to history. */
    volatile unsigned int rp_secsts0;    /* RP 0x1C before the writes       */
    volatile unsigned int rp_secsts1;    /* RP 0x1C after  the writes       */
    /* AFI_INTR_CODE + SIGNATURE captured right after the endpoint config
     * reads, to classify why they did (or did not) complete. Only
     * meaningful once something (the AFI FPCI timeout) actually terminates
     * a hung read. */
    volatile unsigned int afi_intr_code;
    volatile unsigned int afi_intr_sig;
    volatile unsigned int afi_fpci_to;   /* AFI_FPCI_TIMEOUT readback */
    volatile unsigned int diag_mode;     /* CPU_DIAG_* for a re-entry pass  */
    /* AFI_PCIE_CONFIG read BACK after programming, to confirm the clear
     * stuck. Bit (port+1) is that port's DISABLE bit; the entry value has
     * port 1 disabled and the stub clears it. A port left disabled would
     * train a link and refuse to forward configuration. */
    volatile unsigned int afi_pcie_cfg;
} cpu_mbox_t;

#endif /* __ASSEMBLER__ */
#endif /* CPU_MBOX_H */
