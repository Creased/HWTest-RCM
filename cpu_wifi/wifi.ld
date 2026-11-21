/*
 * The Wi-Fi scan image is copied to WIFI_IMG_ADDR and entered there with the
 * MMU off, like the stub, so it is linked at that exact address. Keep this in
 * step with WIFI_IMG_ADDR / WIFI_IMG_MAX / WIFI_L1_ADDR in ../wifi_mbox.h.
 */

ENTRY(_start)

SECTIONS {
	. = 0xA0100000;             /* WIFI_IMG_ADDR */

	.text : {
		KEEP(*(.text.entry))
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

	__wifi_end = .;

	ASSERT(__wifi_end <= 0xA0130000,
		"wifi image (with .bss) reaches its page tables at WIFI_L1_ADDR")

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.attributes) }
}
