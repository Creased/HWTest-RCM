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
		/* The loader's code and constants end here. The payload expands
		 * from IPL_LOAD_ADDR upwards, so everything the loader runs or
		 * reads while it does must sit below that. */
		__ldr_code_end = .;
		/* The compressed payload trails the code; loader.c relocates it
		 * before expanding it, so nothing here depends on its address. */
		*(._payload_lz);
		. = ALIGN(4);
	}
	__ldr_end = .;
	ASSERT(__ldr_code_end <= 0x40008000,
	       "loader code runs past IPL_LOAD_ADDR: the payload would overwrite it")
	. = ALIGN(0x10);
	__ipl_end = .;
}
