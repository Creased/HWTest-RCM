/*
 * The stub is copied to CPU_STUB_ADDR and entered there with the MMU off, so
 * it is linked at that exact address - no relocation, no PIC, no surprises.
 * Keep this in step with CPU_STUB_ADDR in ../cpu_mbox.h; the ASSERT below
 * catches the image outgrowing the window it is copied into.
 */

ENTRY(_start)

SECTIONS {
	. = 0xA0000000;             /* CPU_STUB_ADDR */

	.text : {
		KEEP(*(.text.entry))    /* _start must be the first byte */
		*(.text*)
	}
	.rodata : { *(.rodata*) }
	.data   : { *(.data*) }

	. = ALIGN(8);
	.bss : {
		__bss_start = .;
		*(COMMON)
		*(.bss*)
		. = ALIGN(8);
		__bss_end = .;
	}

	__stub_end = .;

	ASSERT(__stub_end - 0xA0000000 <= 0x10000,
		"cpu stub exceeds CPU_STUB_MAX (64 KiB)")

	/* stub.S builds its L1 page table at 0xA0008000, inside this same
	 * window, so the image has to stop short of it: the four descriptor
	 * stores would otherwise land on the stub's own code or data, with the
	 * MMU about to be switched on over the result. The 64 KiB ceiling above
	 * is the copy window and does not catch that. */
	ASSERT(__stub_end <= 0xA0008000,
		"cpu stub image reaches its own L1 page table at 0xA0008000")

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.attributes) }
}
