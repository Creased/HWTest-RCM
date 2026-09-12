.section .text.chain_reloc, "ax", %progbits
.arm
.balign 4

.globl chain_reloc_start
.type chain_reloc_start, %function
chain_reloc_start:
    ldmia r0!, {r4-r7}
    stmia r1!, {r4-r7}
    subs  r2, r2, #0x10
    bne   chain_reloc_start
    bx    r3

.globl chain_reloc_end
chain_reloc_end:
.size chain_reloc_start, chain_reloc_end - chain_reloc_start
