ENTRY(_start)

/* BDK hands out a few FIXED IRAM addresses that are not part of any section
 * and that the link knows nothing about. The one that matters here is
 * SDRAM_PARAMS_ADDR (memory_map.h, the hwtest-owned shadow of the BDK
 * header): sdram_init(), reached from hw_init() on the first line of
 * ipl_main, decompresses the DRAM parameter blob to the scratch and writes
 * SDRAM_PARAMS_SZ = 0x838 bytes there, unconditionally, on every boot.
 *
 * That write happens after start.S has zeroed .bss and before any probe
 * runs, so a static inside the window is destroyed between the two. The
 * damage is silent: nearly every static is written before it is read
 * (dx_reset() clears _dx_n, g_show_status is assigned during init, the
 * storage flags are set by their own probes), so only a static READ before
 * it is written can expose it. The Wi-Fi PCIe probe's arming flag was
 * exactly that - it came up already set, so the probe ran during the boot
 * sweep before anything had asked for it.
 *
 * That happened with the scratch at upstream BDK's 0x40030000, a window
 * hwtest's image has outgrown - the Joy-Con variant's .text/.data alone now
 * ends at 0x40030910. Hence the shadowed header, which moves the scratch up
 * to 0x40036000, clear of the image; the ASSERT below keeps it that way.
 *
 * The MAX() below is what makes the .bss collision impossible: it starts
 * .bss at the end of the scratch window rather than wherever the image
 * happens to end. Do not drop it on the grounds that .bss currently stops
 * short of 0x40036000 - in the Joy-Con build it does so by about 300 bytes.
 * IRAM is 256 KiB and .bss occupies no image bytes, so the gap is free. */
SDRAM_PARAMS_ADDR = 0x40036000;
SDRAM_PARAMS_END  = 0x40036838;
IRAM_STACK_TOP    = 0x4003FF00;   /* SYS_STACK_TOP_INIT / IPL_RELOC_ADDR */

SECTIONS {
	PROVIDE(__ipl_start = IPL_LOAD_ADDR);
	. = __ipl_start;
	.text : {
		*(.text._start);
		KEEP(*(._boot_cfg));
		KEEP(*(._ipl_version));
		*(.text._irq_setup);
		*(.text*);
	}
	.data : {
		*(.data*);
		*(.rodata*);
	}
	. = ALIGN(0x10);
	__ipl_end = .;

	/* Only the loaded image (.text + .data) lives below here, so moving
	 * the cursor past BDK's scratch buffer does not grow build/hwtest.bin
	 * by a single byte. MAX rather than a plain assignment so this can only
	 * ever move the cursor forward. */
	. = MAX(., SDRAM_PARAMS_END);
	.bss : {
		__bss_start = .;
		*(COMMON)
		*(.bss*)
		__bss_end = .;
	}

	ASSERT(__ipl_end <= SDRAM_PARAMS_ADDR,
		"link.ld: .text/.data now reach BDK's SDRAM param scratch at 0x40036000 - sdram_init() will corrupt code or constants")
	ASSERT(__bss_end <= IRAM_STACK_TOP - 0x2000,
		"link.ld: .bss has grown into the pre-pivot stack below 0x4003FF00")
}
