/*
 * GUI stage entry.
 *
 * hwtest.bin reads hwtest/gui.bin off the SD card, copies it to its link
 * address (GUI_LOAD_ADDR in gui/link.ld) and jumps to the first word, so
 * unlike an RCM payload this never has to relocate itself. The header after
 * the branch is what hwtest.bin checks before it jumps: a file that is not
 * a GUI stage built for this layout is left alone and the text launcher
 * runs instead.
 *
 * State on arrival: hw_init done, DRAM trained, display up, BPMP MMU off.
 */

.section .text._start
.arm

.extern memset
.type memset, %function

.extern _irq_setup
.type _irq_setup, %function

.globl _start
.type _start, %function
_start:
	B _gui_entry
	.word 0x55475748              /* "HWGU", checked by hwtest.bin       */
	.word __ipl_end               /* end of the loaded image ...         */
	.word __ipl_start             /* ... and where it has to be loaded   */

_gui_entry:
	/* Stack in IRAM: hwtest.bin's own image is no longer needed. */
	LDR SP, =0x4003FF00
	LDR R0, =__bss_start
	EOR R1, R1, R1
	LDR R2, =__bss_end
	SUB R2, R2, R0
	BL memset
	BL _irq_setup
	B .

.globl pivot_stack
.type pivot_stack, %function
pivot_stack:
	MOV SP, R0
	BX LR
