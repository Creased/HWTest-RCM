ENTRY(_start)

/* The GUI stage runs from DRAM, where hwtest.bin copies it after its own
 * hw_init has trained the DRAM. Same slot hekate gives Nyx (NYX_LOAD_ADDR,
 * 16 MiB below the heap at 0x82000000); nothing in the probes uses it. */
GUI_LOAD_ADDR = 0x81000000;
GUI_SZ_MAX    = 0x01000000;

SECTIONS {
	PROVIDE(__ipl_start = GUI_LOAD_ADDR);
	. = __ipl_start;
	.text : {
		*(.text._start);
		KEEP(*(._gui_header));
		*(.text._irq_setup);
		*(.text*);
	}
	.data : {
		*(.data*);
		*(.rodata*);
		/* Inside the section, so the padding is in gui.bin too: hwtest.bin
		 * checks the file is at least __ipl_end - __ipl_start long. */
		. = ALIGN(0x10);
	}
	__ipl_end = .;
	.bss : {
		__bss_start = .;
		*(COMMON)
		*(.bss*)
		__bss_end = .;
	}

	ASSERT(__bss_end <= GUI_LOAD_ADDR + GUI_SZ_MAX,
		"gui/link.ld: the GUI stage has outgrown its 16 MiB slot under the heap")
}
