/*
 * CPU0 entry, AArch64.
 *
 * ccplex_boot_cpu0() points the secure-boot reset vector here and releases
 * CPU0, so this is the very first instruction the Cortex-A57 executes. State
 * on arrival: EL3, AArch64, MMU off, caches off, interrupts off, SP
 * undefined. With the MMU off every access is Device-nGnRnE, which is
 * exactly what we want - the stub's stores land in DRAM immediately and the
 * BPMP sees them without either side doing cache maintenance on the fly.
 *
 * The only jobs here are: give C a stack, zero the BSS the linker gave us,
 * and never return.
 */

#include "../cpu_mbox.h"

.section .text.entry, "ax"
.global _start
.type _start, %function
_start:
    /* Park every core except CPU0. ccplex_boot_cpu0() only releases CPU0, so
     * this should be unreachable - but a stray core arriving here would race
     * us on the mailbox for no benefit, and MPIDR is one instruction. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, 1f
0:  wfe
    b       0b

1:  ldr     x0, =CPU_STACK_TOP
    mov     sp, x0

    /* Exception vectors, BEFORE the first PCIe access.
     *
     * Without these, an access that is error-terminated rather than ignored
     * takes a synchronous external abort straight into whatever VBAR_EL3
     * holds at reset - CPU0 disappears, the mailbox stops advancing, and the
     * supervisor on the BPMP reports a stall. A bus that never answers and a
     * bus that answers "no" then look exactly the same. With a handler
     * installed, the fault records ESR/FAR/ELR and says so. */
    adr     x0, vectors
    msr     vbar_el3, x0
    isb

    /* Mark every bus access non-secure, exactly like U-Boot on this
     * port: its start.S stays at EL3 but sets SCR_EL3.NS. */
    mrs     x0, scr_el3
    orr     x0, x0, #1                      /* NS: non-secure accesses   */
    msr     scr_el3, x0
    isb

    /* Instruction cache on before the MMU, in U-Boot's exact order
     * (initr_caches: icache_enable() precedes dcache_enable()). */
    ic      iallu
    isb
    mrs     x1, sctlr_el3
    orr     x1, x1, #(1 << 12)              /* I: enable icache         */
    msr     sctlr_el3, x1
    isb

    /* Invalidate the D-cache by way (U-Boot's invalidate_dcache_all()
     * before dcache_enable): C is about to come on and the bootrom's
     * stale lines must not alias. A57 L1D: 32 KiB, 2 ways, 64 B lines. */
    mov     w6, #2
    mov     w7, #256
dc_iw:  sub     w7, w7, #1
    mov     w6, #2
dc_iw2: sub     w6, w6, #1
    lsl     x9, x6, #30
    lsl     x10, x7, #6
    orr     x9, x9, x10
    dc      ivac, x9
    cbnz    w6, dc_iw2
    cbnz    w7, dc_iw
    dsb     sy
    isb

    /* Identity MMU, attributes replicated bit-for-bit from U-Boot's
     * tegra_mem_map: IO = Device-nGnRnE non-shareable + PXN/UXN, DRAM =
     * MT_NORMAL (WBWA) inner-shareable. The MMU-off default is Device
     * INNER-SHAREABLE, and a shareable read to a non-coherent slave
     * (the PCIe AFI) goes down the coherency path, which returns fabric
     * zeros and wedges after a few - while U-Boot's non-shareable
     * reads enumerate the endpoint cleanly. */
    ldr     x0, =0xA0008000                /* L1 table (NS-proven image area) */
    ldr     x1, =0x0060000000000401        /* IO: Device nGnRnE, PXN/UXN */
    str     x1, [x0, #0]                   /* 0x00000000..0x3FFFFFFF      */
    ldr     x1, =0x0060000040000401
    str     x1, [x0, #8]                   /* 0x40000000..0x7FFFFFFF      */
    ldr     x1, =0x0000000080000711        /* DRAM: MT_NORMAL WBWA, IS, executable */
    str     x1, [x0, #16]                  /* 0x80000000..0xBFFFFFFF      */
    ldr     x1, =0x00600000C0000401
    str     x1, [x0, #24]                  /* 0xC0000000..0xFFFFFFFF      */

    /* MMU enable, ATF's EL3 recipe (enable_mmu_internal_el3) in the
     * exact order: the two proven T210 bootchains (ATF, U-Boot) both
     * bring the D-cache on together with or right after M. M=1 with
     * C=0 wedges this console's fetch path, so C is set in the same
     * write as M. */
    tlbi    alle3
    dsb     sy
    isb
    /* MAIR_EL3: U-Boot's MEMORY_ATTRIBUTES */
    ldr     x1, =0x000000FF440C0400
    msr     mair_el3, x1
    /* TCR_EL3: TG0 4 KB, T0SZ = 32, walks IS/WBWA/WBWA (ips = 0) */
    ldr     x1, =0x00000D20
    msr     tcr_el3, x1
    msr     ttbr0_el3, x0
    dsb     ish
    isb
    mrs     x1, sctlr_el3
    mov     x2, #0x5                        /* M | C: MMU + D-cache      */
    orr     x1, x1, x2
    msr     sctlr_el3, x1
    isb
    dsb     sy

    /* Zero .bss. */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
3:  cmp     x1, x2
    b.hs    5f
    str     xzr, [x1], #8
    b       3b

5:  bl      cpu_stub_main

    /* cpu_stub_main is not supposed to come back, but if it does, stop here
     * rather than run off into whatever follows. The BPMP will powergate the
     * cluster shortly regardless. */
6:  wfe
    b       6b
.size _start, . - _start

/*
 * AArch64 vector table: 16 entries of 128 bytes, 2 KiB aligned. Every one of
 * them lands on the same handler - this stub has no interrupts, no lower EL
 * and nothing to recover to, so the only interesting question is "what
 * faulted and where", and that is identical for all sixteen.
 */
.macro VEC_ENTRY
    b       exc_common
    .balign 128
.endm

    .balign 2048
vectors:
    /* Current EL, SP_EL0 */
    VEC_ENTRY               /* sync  */
    VEC_ENTRY               /* irq   */
    VEC_ENTRY               /* fiq   */
    VEC_ENTRY               /* serror*/
    /* Current EL, SP_ELx - this is where we actually run */
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    /* Lower EL, AArch64 */
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    /* Lower EL, AArch32 */
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY

/*
 * Record the fault into the mailbox and park. Deliberately does not try to
 * resume: the BPMP is watching, and a precise post-mortem beats a fragile
 * recovery. Uses only x0-x3 and touches no stack, so it works even if the
 * fault happened with SP in a bad state.
 */
exc_common:
    mrs     x4, CurrentEL
    lsr     x4, x4, #2
    cmp     x4, #2                      /* at EL2 use the EL2 registers */
    b.eq    1f
    mrs     x1, esr_el3
    mrs     x2, far_el3
    mrs     x3, elr_el3
    b       2f
1:  mrs     x1, esr_el2
    mrs     x2, far_el2
    mrs     x3, elr_el2
2:  ldr     x0, =CPU_MBOX_ADDR
    /* Offsets come from cpu_mbox.h and are _Static_asserted against
     * offsetof() on the C side, so they cannot drift from the struct.
     * With the D-cache on, push the record out for the supervisor. */
    ldr     w4, =CPU_MBOX_EXC
    str     w4, [x0, #MBOX_OFF_EXC_TAKEN]
    str     w1, [x0, #MBOX_OFF_EXC_ESR]
    str     w2, [x0, #MBOX_OFF_EXC_FAR]
    str     w3, [x0, #MBOX_OFF_EXC_ELR]
    ldr     w4, [x0, #MBOX_OFF_STAGE]
    str     w4, [x0, #MBOX_OFF_EXC_STAGE]
    dc      cvac, x0
    dsb     sy
5:  wfe
    b       5b
.size exc_common, . - exc_common
