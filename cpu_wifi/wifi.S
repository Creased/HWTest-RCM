/*
 * CPU0 entry for the Wi-Fi scan image, AArch64.
 *
 * The same start as cpu_stub/stub.S - park the other cores, stack, vectors,
 * SCR_EL3.NS, I-cache, D-cache invalidate, identity MMU with U-Boot's
 * attributes, M and C in one write - with two differences:
 *
 *  - the page tables live at WIFI_L1_ADDR/WIFI_L2_ADDR, outside the stub's
 *    window, since this image is linked at WIFI_IMG_ADDR;
 *  - the 2-3 GiB range is mapped through an L2 table of 2 MiB blocks
 *    instead of one 1 GiB block, so the one block holding the DMA rings,
 *    the chip's buffers and the Wi-Fi mailbox (WIFI_DMA_ADDR) can be Normal
 *    non-cacheable. The chip writes into that memory behind the CPU's back,
 *    and the BPMP reads the mailbox from DRAM: uncached, neither needs cache
 *    maintenance on this side. Everything else stays write-back, as in the
 *    stub.
 *
 * The exception vectors record into the stub's mailbox (CPU_MBOX_ADDR), as
 * stub.S does, so a fault reads the same way to the supervisor.
 */

#include "../cpu_mbox.h"
#include "../wifi_mbox.h"

.section .text.entry, "ax"
.global _start
.type _start, %function
_start:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, 1f
0:  wfe
    b       0b

1:  ldr     x0, =WIFI_STACK_TOP
    mov     sp, x0

    adr     x0, vectors
    msr     vbar_el3, x0
    isb

    mrs     x0, scr_el3
    orr     x0, x0, #1
    msr     scr_el3, x0
    isb

    ic      iallu
    isb
    mrs     x1, sctlr_el3
    orr     x1, x1, #(1 << 12)
    msr     sctlr_el3, x1
    isb

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

    /* L2 table for 0x80000000..0xBFFFFFFF: 512 blocks of 2 MiB, MT_NORMAL
     * (AttrIndx 4, 0x711) except WIFI_DMA_L2_IDX, Normal non-cacheable
     * (AttrIndx 3 = MAIR byte 0x44, 0x70D). Inner-shareable, AF set, as the
     * stub's DRAM block. */
    ldr     x0, =WIFI_L2_ADDR
    mov     x1, #0                          /* index                     */
    ldr     x2, =0x80000000                 /* block address             */
    ldr     x3, =0x711
    ldr     x4, =0x70D
    mov     x5, #WIFI_DMA_L2_IDX
l2_fill:
    cmp     x1, x5
    csel    x6, x4, x3, eq
    orr     x6, x6, x2
    str     x6, [x0, x1, lsl #3]
    add     x2, x2, #0x200000
    add     x1, x1, #1
    cmp     x1, #512
    b.lo    l2_fill

    ldr     x0, =WIFI_L1_ADDR
    ldr     x1, =0x0060000000000401        /* IO: Device nGnRnE, PXN/UXN */
    str     x1, [x0, #0]
    ldr     x1, =0x0060000040000401
    str     x1, [x0, #8]
    ldr     x1, =WIFI_L2_ADDR
    orr     x1, x1, #3                     /* table descriptor            */
    str     x1, [x0, #16]
    ldr     x1, =0x00600000C0000401
    str     x1, [x0, #24]
    dsb     sy

    tlbi    alle3
    dsb     sy
    isb
    ldr     x1, =0x000000FF440C0400
    msr     mair_el3, x1
    ldr     x1, =0x00000D20
    msr     tcr_el3, x1
    msr     ttbr0_el3, x0
    dsb     ish
    isb
    mrs     x1, sctlr_el3
    mov     x2, #0x5
    orr     x1, x1, x2
    msr     sctlr_el3, x1
    isb
    dsb     sy

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
3:  cmp     x1, x2
    b.hs    5f
    str     xzr, [x1], #8
    b       3b

5:  bl      cpu_stub_main
6:  wfe
    b       6b
.size _start, . - _start

.macro VEC_ENTRY
    b       exc_common
    .balign 128
.endm

    .balign 2048
vectors:
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY
    VEC_ENTRY

/* As stub.S: record into the stub's mailbox and park. */
exc_common:
    mrs     x1, esr_el3
    mrs     x2, far_el3
    mrs     x3, elr_el3
    ldr     x0, =CPU_MBOX_ADDR
    ldr     w4, =CPU_MBOX_EXC
    str     w4, [x0, #MBOX_OFF_EXC_TAKEN]
    str     w1, [x0, #MBOX_OFF_EXC_ESR]
    str     w2, [x0, #MBOX_OFF_EXC_FAR]
    str     w3, [x0, #MBOX_OFF_EXC_ELR]
    ldr     w4, [x0, #MBOX_OFF_STAGE]
    str     w4, [x0, #MBOX_OFF_EXC_STAGE]
    dc      cvac, x0
    add     x4, x0, #MBOX_OFF_EXC_TAKEN
    dc      cvac, x4
    dsb     sy
5:  wfe
    b       5b
.size exc_common, . - exc_common
