ENTRY(_start)

SECTIONS {
	PROVIDE(__ipl_start = LDR_LOAD_ADDR);
	. = __ipl_start;
	.text : {
		*(.text._start);
		*(.text*);
	}
	.data : {
		*(.data*);
		*(.rodata*);
		/* The compressed payload trails the code; loader.c relocates it
		 * before expanding it, so nothing here depends on its address. */
		*(._payload_lz);
		. = ALIGN(4);
	}
	__ldr_end = .;
	. = ALIGN(0x10);
	__ipl_end = .;
}
