# hwtest -- Switch RCM hardware probe
#
# Builds diagnostic payload:
#
#   build/hwtest.bin       UART_B debug/report console + LCD/touch launcher
#
# The dedicated full-memory RAM diagnostic remains a separate payload built by
# `make ram`. The normal launcher chainloads it from SD so the large Erista +
# Mariko memory-training engines do not consume the normal RCM size budget.
#
# Hekate and Atmosphere are git submodules under third_party/. Hekate provides
# the BDK/SoC support; the dedicated RAM payload builds Atmosphere's MTC from
# the pinned submodule plus a small local adaptation patch. Initialize both
# with `git submodule update --init --recursive` after a fresh clone.

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM, e.g. export DEVKITARM=/opt/devkitpro/devkitARM")
endif
include $(DEVKITARM)/base_rules

# Match Hekate's compile-for address. RCM injects at 0x40010000 but start.S
# self-relocates to wherever IPL_LOAD_ADDR points, so the payload still runs
# from where the linker placed it. Keeping 0x40008000 here also avoids the
# bdk's memory_map.h #define fighting our -D.
IPL_LOAD_ADDR := 0x40008000
IPL_MAGIC     := 0x54534854   # "THST", payload tag for hwtest.

TARGET    := hwtest
HEKATE    := third_party/hekate
BDKDIR    := $(HEKATE)/bdk
GFXDIR    := gfx

DEFAULT_DIR := build
DEFAULT_BIN := $(DEFAULT_DIR)/$(TARGET).bin

# Object list for the normal launcher payload.
COMMON_OBJS = \
    start.o exception_handlers.o chain_reloc.o main.o core.o ui_menu.o stubs.o emmcsn.o \
    log.o dx.o report.o verdict.o \
    probe_soc.o probe_power.o probe_memclk.o probe_inputs.o probe_bt.o probe_audio.o probe_audio_beep.o \
    probe_wifi.o probe_display.o probe_storage.o probe_gamecard.o probe_lowlevel.o \
    gfx.o \
    heap.o sprintf.o util.o btn.o dirlist.o \
    bpmp.o ccplex.o clock.o di.o i2c.o irq.o timer.o \
    mc.o sdram.o gpio.o pinmux.o pmc.o se.o tsec.o uart.o \
    fuse.o kfuse.o \
    sdmmc.o sdmmc_driver.o sd.o emmc.o nx_emmc_bis.o \
    bq24193.o max17050.o max7762x.o max77620-rtc.o bm92t36.o regulator_5v.o \
    tmp451.o fan.o \
    touch.o als.o \
    hw_init.o \
    ff.o ffsystem.o ffunicode.o diskio.o

DEFAULT_OBJS := $(addprefix $(DEFAULT_DIR)/,$(COMMON_OBJS))

# Source search path. The bdk subtree isn't a flat directory, so we list each
# leaf so VPATH can find the .c files by name. The bootloader/ leaves are
# pulled in so start.S, diskio.c, ffconf.h and gfx.h are sourced from the
# submodule directly rather than duplicated in-tree.
VPATH = . $(GFXDIR) $(BDKDIR) \
        $(BDKDIR)/display $(BDKDIR)/input $(BDKDIR)/libs/fatfs \
        $(BDKDIR)/mem $(BDKDIR)/power $(BDKDIR)/rtc $(BDKDIR)/sec \
        $(BDKDIR)/soc $(BDKDIR)/storage $(BDKDIR)/thermal $(BDKDIR)/utils \
        $(HEKATE)/bootloader $(HEKATE)/bootloader/gfx \
        $(HEKATE)/bootloader/libs/fatfs

# The BDK's gfx_utils.h does `#include GFX_INC` and bdk/libs/fatfs/fatfs_cfg.h
# does `#include FFCFG_INC`. Both resolve via -I, which points at the
# submodule's bootloader/gfx and bootloader/libs/fatfs.
GFX_INC   := '"gfx.h"'
FFCFG_INC := '"ffconf.h"'

BASE_DEFINES := \
    -DBL_MAGIC=$(IPL_MAGIC) \
    -DGFX_INC=$(GFX_INC) -DFFCFG_INC=$(FFCFG_INC) \
    -DBDK_MALLOC_NO_DEFRAG

DEFAULT_DEFINES := $(BASE_DEFINES) \
    -DDEBUG_UART_PORT=1 -DDEBUG_UART_BAUDRATE=115200 -DDEBUG_UART_INVERT=0


WARNINGS := -Wall -Wsign-compare -Wno-array-bounds -Wno-stringop-overread -Wno-stringop-overflow

ARCH    := -march=armv4t -mtune=arm7tdmi -mthumb -mthumb-interwork $(WARNINGS)
# Extra -D flags from the command line, e.g. a build that keeps the Wi-Fi
# PCIe probe out of the sweep until it is armed from the pager:
#   make EXTRA_DEFINES=-DWIFI_CFG_AUTORUN=0
# Size matters: TegraRcmGUI shells out to TegraRcmSmash, which builds a fixed
# 66216-byte exploit prologue and then SILENTLY truncates the whole buffer to
# 192 KiB - so anything past 130392 bytes of payload is quietly dropped and the
# console jumps into a partial image with no error anywhere. Inlining is left
# enabled: at -Os it costs nothing and -fno-inline actually made the image
# ~4 KB larger.
#
# LTO is deliberately NOT here. Under devkitARM's gcc 16.1.0 it produces an
# image whose .bss objects lose their values sometime after start.S zeroes
# them: gfx_con.fntsz came back 8 instead of 16 (every glyph rendered at half
# height), g_report_body came back NULL (the SD report silently never saved),
# and the crc32 table BDK allocates on first use came back unusable, which
# made a healthy GPT read as two CRC mismatches. gcc 15.2.0 builds the same
# tree correctly with LTO, and 16.1.0 builds it correctly without - so this is
# the toolchain, not the source, and -fno-strict-aliasing does not help.
# The release CI runs whatever devkitARM the container ships, so pinning a
# compiler would only move the trap. Costs ~4.7 KB against the ceiling above,
# which self-extraction leaves room for. Re-test the payload in the emulator
# before putting it back.
OPT    ?= -Os
INLINE ?=
EXTRA_DEFINES ?=

# Rebuild when the flags change.
#
# Make compares timestamps, and EXTRA_DEFINES has none - so `make
# EXTRA_DEFINES=-DFOO=1` straight after an ordinary build would silently reuse
# objects compiled without -DFOO, producing a payload that does not contain the
# change. A stamp file holding the current flags gives make the timestamp it
# needs.
#
# The stamp rule below is the first rule in this file, so pin the default goal
# before it or `make` with no arguments builds a stamp instead of the payload.
.DEFAULT_GOAL := all

DEF_STAMP_D := $(DEFAULT_DIR)/.extra_defines
$(shell mkdir -p $(DEFAULT_DIR) 2>/dev/null; \
    for f in $(DEF_STAMP_D); do \
        printf '%s\n' '$(EXTRA_DEFINES) $(OPT)' | cmp -s - $$f || \
            printf '%s\n' '$(EXTRA_DEFINES) $(OPT)' > $$f; \
    done)
# The rule below regenerates the stamps when `make clean all` deletes them
# mid-invocation (the parse-time $(shell) above has already run by then).
$(DEF_STAMP_D):
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(EXTRA_DEFINES) $(OPT)' > $@

# -MMD -MP: each object leaves a .d listing the headers it pulled in, so a
# change to hwtest.h or cpu_mbox.h rebuilds what includes it. Without this
# a mailbox layout change rebuilt the CPU0 stub but not probe_wifi.o, and
# the two sides disagreed on the struct.
CFLAGS_BASE := $(ARCH) $(OPT) -g -nostdlib -ffunction-sections -fdata-sections \
               -fomit-frame-pointer $(INLINE) -std=gnu11 -MMD -MP \
               -I. -I$(BDKDIR) -I$(GFXDIR) \
               -I$(HEKATE)/bootloader/gfx \
               -I$(HEKATE)/bootloader/libs/fatfs \
               $(EXTRA_DEFINES)
LDFLAGS := $(ARCH) -nostartfiles -lgcc -Wl,--nmagic,--gc-sections \
           -Xlinker --defsym=IPL_LOAD_ADDR=$(IPL_LOAD_ADDR) -T link.ld

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy

.PHONY: all default ram gui sdzip clean

# `make` (no args) builds the normal launcher payload.
all: default

default: $(DEFAULT_BIN)

# Everything that goes on the SD card, in one zip laid out as it goes there
# (tools/mksdzip.py lists the files): unzip it at the root of the card.
SD_ZIP := $(DEFAULT_DIR)/hwtest-sd.zip

sdzip: default ram gui
	@python3 tools/mksdzip.py $(SD_ZIP)

# Dedicated full-memory DRAM diagnostic. Kept out of `all` because the build
# embeds both public Atmosphere MTC engines/tables and is intentionally a
# separate service payload, not part of the normal hwtest sweep.
ram:
	$(MAKE) -C ramtest all

$(DEFAULT_DIR):
	@mkdir -p $@

# Normal payload compile rules.
$(DEFAULT_DIR)/%.o: %.c $(DEF_STAMP_D) | $(DEFAULT_DIR)
	$(CC) $(CFLAGS_BASE) $(DEFAULT_DEFINES) -c -o $@ $<
$(DEFAULT_DIR)/%.o: %.S $(DEF_STAMP_D) | $(DEFAULT_DIR)
	$(CC) $(CFLAGS_BASE) $(DEFAULT_DEFINES) -c -o $@ $<


# ---- CPU0 stub (AArch64) --------------------------------------------------
#
# The PCIe register apertures are not reachable from the BPMP - they hang off
# MSELECT, whose only master is the CPU complex (TRM ch.16/19, and the T210
# block diagram: MSelect is fed by CCPLEX/CPUCIF, and its 32-bit "AXI>ARM7
# (APC)" port is a bridge INTO the ARM7 world, not out of it). So the handful
# of accesses that must come from a CPU-complex master are built separately
# for AArch64 and embedded in the payload as a byte array, which the BPMP
# copies to DRAM before releasing CPU0 at it.
#
# Separate toolchain for the stub. Absent, the stub is skipped and the probe
# reports that the CPU path was not built rather than failing the build, so
# the BPMP-side diagnostics keep working on a machine without it.
#
# devkitA64 is the default because it sits next to devkitARM in any devkitPro
# install, but nothing here needs it specifically: the stub is freestanding
# (-ffreestanding -nostdlib -mgeneral-regs-only, no libc, no syscalls), so any
# bare-metal aarch64 gcc will do. Override A64_CC/A64_OC to use one - which is
# what CI does, because devkitPro's package server answers 403 to cloud runners
# and Debian's gcc-aarch64-linux-gnu is always installable.
DEVKITA64 ?= $(dir $(DEVKITARM))devkitA64
A64_CC    ?= $(DEVKITA64)/bin/aarch64-none-elf-gcc
A64_OC    ?= $(DEVKITA64)/bin/aarch64-none-elf-objcopy
# Accept either an absolute path (devkitA64) or a bare name found on PATH (a
# distro cross-compiler); $(wildcard) alone only ever matches the former.
HAVE_A64  := $(or $(wildcard $(A64_CC)),$(shell command -v $(A64_CC) 2>/dev/null))

# Skipping the stub is right for a developer who only has devkitARM, but it is
# wrong for a release: the binary still builds, still boots, and simply reports
# the CPU path as unbuilt, so a release pipeline missing devkitA64 ships a
# quietly degraded payload and nothing fails to say so. REQUIRE_A64=1 turns
# that silence into a hard error; CI sets it.
ifeq ($(HAVE_A64),)
ifneq ($(REQUIRE_A64),)
$(error No aarch64 compiler at '$(A64_CC)'. The CPU0 stub cannot be built \nand REQUIRE_A64 is set. Install devkitA64, or point A64_CC/A64_OC at any \nbare-metal aarch64 gcc, e.g. A64_CC=aarch64-linux-gnu-gcc)
endif
endif

A64_CFLAGS := -march=armv8-a -mgeneral-regs-only -ffreestanding -nostdlib \
              -fno-builtin -fno-stack-protector -Os -Wall -Wextra \
              -fno-asynchronous-unwind-tables -fno-unwind-tables

$(DEFAULT_DIR)/cpu_stub.elf: cpu_stub/stub.S cpu_stub/stub.c cpu_stub/stub.ld \
                             cpu_mbox.h $(DEF_STAMP_D) | $(DEFAULT_DIR)
	$(A64_CC) $(A64_CFLAGS) $(EXTRA_DEFINES) -T cpu_stub/stub.ld -o $@ \
	    cpu_stub/stub.S cpu_stub/stub.c -lgcc

$(DEFAULT_DIR)/cpu_stub.bin: $(DEFAULT_DIR)/cpu_stub.elf
	$(A64_OC) -O binary $< $@
	@printf '   built %s (%s bytes)\n' $@ $$(stat -c%s $@)

# Emit the blob as a C array. Written by hand rather than with xxd so the
# build does not depend on it being installed.
# Plain types, no bdk header: this file is generated into build/ and does not
# get the -I flags the in-tree sources do. unsigned char / unsigned int match
# bdk's u8 / u32 exactly on this target.
$(DEFAULT_DIR)/cpu_stub_bin.c: $(DEFAULT_DIR)/cpu_stub.bin
	@printf '/* generated from cpu_stub.bin - do not edit */\n' > $@
	@od -An -v -tx1 $< | awk 'BEGIN{printf "const unsigned char cpu_stub_bin[] = {\n"} \
	    {for(i=1;i<=NF;i++) printf "0x%s,", $$i; printf "\n"} \
	    END{printf "};\nconst unsigned int cpu_stub_bin_size = sizeof(cpu_stub_bin);\n"}' >> $@

$(DEFAULT_DIR)/cpu_stub_bin.o: $(DEFAULT_DIR)/cpu_stub_bin.c | $(DEFAULT_DIR)
	$(CC) $(CFLAGS_BASE) $(DEFAULT_DEFINES) -c -o $@ $<


# Release builds carry the CPU0 stub so the PCIe/WLAN probe is not degraded.
ifneq ($(HAVE_A64),)
DEFAULT_OBJS    += $(DEFAULT_DIR)/cpu_stub_bin.o
DEFAULT_DEFINES += -DHAVE_CPU_STUB=1
endif

# Link + objcopy.
$(DEFAULT_DIR)/$(TARGET).elf: $(DEFAULT_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(DEFAULT_OBJS) -lgcc

# ---- self-extracting wrapper --------------------------------------------
#
# What ships is a small stub with the LZ-compressed image trailing it, not the
# linked image itself. TegraRcmSmash - which TegraRcmGUI drives - prepends a
# fixed 66216-byte exploit prologue and then truncates the whole buffer to
# 192 KiB without reporting anything, so a payload past 130392 bytes is
# silently cut off and the console jumps into a partial image. Compressing
# gets the shipped file back under that ceiling, and speeds up chainloading.
#
# The codec is the BCL LZ77 that BDK already carries as LZ_Uncompress(), so
# the on-device half is hekate's own decompressor and only the packer is ours.
LDR_LOAD_ADDR := 0x40007000
LDRDIR        := loader

$(DEFAULT_DIR)/$(TARGET)_unc.bin: $(DEFAULT_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(DEFAULT_DIR)/payload_lz.h: $(DEFAULT_DIR)/$(TARGET)_unc.bin
	@python3 tools/mkpayload.py $< $(DEFAULT_DIR)/payload.lz $@
	@python3 ramtest/check_overlap.py $(DEFAULT_DIR)/payload.lz $<

LDR_CFLAGS := $(ARCH) -Os -g -nostdlib -ffunction-sections -fdata-sections               -fomit-frame-pointer -std=gnu11 -I$(BDKDIR) -I$(DEFAULT_DIR)               -I$(LDRDIR) -DBL_MAGIC=$(IPL_MAGIC)
LDR_LDFLAGS := $(ARCH) -nostartfiles -Wl,--nmagic,--gc-sections                -Xlinker --defsym=LDR_LOAD_ADDR=$(LDR_LOAD_ADDR) -T $(LDRDIR)/link.ld

$(DEFAULT_DIR)/ldr_loader.o: $(LDRDIR)/loader.c $(DEFAULT_DIR)/payload_lz.h
	$(CC) $(LDR_CFLAGS) -c -o $@ $<
$(DEFAULT_DIR)/ldr_start.o: $(HEKATE)/loader/start.S
	$(CC) $(LDR_CFLAGS) -c -o $@ $<
$(DEFAULT_DIR)/ldr_lz.o: $(BDKDIR)/libs/compr/lz.c
	$(CC) $(LDR_CFLAGS) -c -o $@ $<

LDR_OBJS := $(DEFAULT_DIR)/ldr_start.o $(DEFAULT_DIR)/ldr_loader.o $(DEFAULT_DIR)/ldr_lz.o

$(DEFAULT_DIR)/loader.elf: $(LDR_OBJS) $(LDRDIR)/link.ld
	$(CC) $(LDR_LDFLAGS) -o $@ $(LDR_OBJS) -lgcc

$(DEFAULT_BIN): $(DEFAULT_DIR)/loader.elf
	$(OBJCOPY) -O binary $< $@
	@printf '   built %s (%s bytes, %s uncompressed)\n' $@ $$(stat -c%s $@) $$(stat -c%s $(DEFAULT_DIR)/$(TARGET)_unc.bin)
	@if [ $$(stat -c%s $@) -gt 130392 ]; then \
	    printf '   ERROR: %s bytes is over the 130392-byte RCM injector limit;\n' $$(stat -c%s $@); \
	    printf '   the injector truncates silently, so this payload must not ship\n'; \
	    rm -f $@; exit 1; fi

# ---- GUI stage ------------------------------------------------------------
#
# The touch / Joy-Con front end, built with bdk's LVGL as Nyx is. It is far
# past the RCM ceiling, so it does not ship as a payload: it goes on the SD
# card as hwtest/gui.bin, and hwtest.bin loads it into DRAM and jumps to it
# (ui_boot_gui). Same probes as the payload; no hw_init of its own, since
# hwtest.bin has already done it by then.
#
# No DEBUG_UART_PORT here: with it set, bdk's Joy-Con driver compiles to
# nothing. UART-B debug output stays, under the GUI's own control
# (UART_DEBUG_ON), until a Joy-Con takes the right rail.
#
# Not size-limited, so -O2 for drawing speed. LVGL's sources pick up the
# GUI's lv_conf.h ahead of bdk's (same include guard).
GUI_DIR := build-gui
GUI_BIN := $(GUI_DIR)/gui.bin
GUI_OPT ?= -O2
# Shown in the main screen's first tab, as Nyx shows hekate's version.
GUI_VERSION ?= $(shell git describe --tags --always 2>/dev/null)
LVGLDIR := $(BDKDIR)/libs/lvgl

VPATH += gui $(LVGLDIR)/lv_core $(LVGLDIR)/lv_draw $(LVGLDIR)/lv_fonts \
         $(LVGLDIR)/lv_hal $(LVGLDIR)/lv_misc $(LVGLDIR)/lv_objx \
         $(LVGLDIR)/lv_themes $(BDKDIR)/ianos $(BDKDIR)/ianos/elfload

GUI_OBJS = \
    gui_start.o exception_handlers.o chain_reloc.o core.o ui_menu.o stubs.o emmcsn.o \
    log.o dx.o report.o verdict.o \
    probe_soc.o probe_power.o probe_memclk.o probe_inputs.o probe_bt.o probe_audio.o probe_audio_beep.o \
    probe_wifi.o probe_display.o probe_storage.o probe_gamecard.o probe_lowlevel.o \
    gfx.o \
    heap.o sprintf.o util.o btn.o dirlist.o \
    bpmp.o ccplex.o clock.o di.o vic.o i2c.o irq.o timer.o \
    mc.o sdram.o gpio.o pinmux.o pmc.o se.o tsec.o uart.o \
    fuse.o kfuse.o \
    sdmmc.o sdmmc_driver.o sd.o emmc.o nx_emmc_bis.o \
    bq24193.o max17050.o max7762x.o max77620-rtc.o bm92t36.o regulator_5v.o \
    tmp451.o fan.o \
    touch.o als.o joycon.o \
    hw_init.o \
    ff.o ffsystem.o ffunicode.o diskio.o \
    ianos.o elfload.o elfreloc_arm.o \
    lv_group.o lv_indev.o lv_obj.o lv_refr.o lv_style.o lv_vdb.o \
    lv_draw.o lv_draw_rbasic.o lv_draw_vbasic.o lv_draw_arc.o lv_draw_img.o \
    lv_draw_label.o lv_draw_line.o lv_draw_rect.o lv_draw_triangle.o \
    lv_hal_disp.o lv_hal_indev.o lv_hal_tick.o lv_font_builtin.o \
    lv_anim.o lv_area.o lv_circ.o lv_color.o lv_font.o lv_ll.o lv_math.o \
    lv_mem.o lv_task.o lv_txt.o lv_gc.o lv_log.o \
    lv_arc.o lv_bar.o lv_btn.o lv_btnm.o lv_cb.o lv_cont.o lv_ddlist.o lv_img.o \
    lv_label.o lv_line.o lv_list.o lv_mbox.o lv_page.o lv_roller.o \
    lv_slider.o lv_sw.o lv_tabview.o lv_ta.o lv_win.o lv_imgbtn.o \
    lv_theme.o lv_theme_hekate.o \
    gui_main.o gui_theme.o gui_home.o gui_custom.o gui_tests.o gui_stick.o \
    gui_jcwire.o gui_jcattach.o gui_jcmotion.o gui_jcir.o gui_screen.o gui_shot.o gui_dock.o gui_dbg.o usbser.o usbser_xusb.o usbser_usb2.o usbpd.o dpout.o gui_ram.o \
    gui_run.o gui_results.o gui_text.o gui_clock.o \
    gui_fonts.o gui_fonts_blob.o

GUI_OBJS := $(addprefix $(GUI_DIR)/,$(GUI_OBJS))

GUI_ASFLAGS := $(ARCH) $(GUI_OPT) -g -nostdlib -ffunction-sections -fdata-sections \
               -fomit-frame-pointer -std=gnu11 -MMD -MP \
               -Igui -I. -I$(BDKDIR) -I$(GFXDIR) \
               -I$(HEKATE)/bootloader/gfx \
               -I$(HEKATE)/bootloader/libs/fatfs \
               $(BASE_DEFINES) -DHWTEST_GUI -DDEBUG_UART_BAUDRATE=115200 \
               $(EXTRA_DEFINES)
GUI_CFLAGS  := $(GUI_ASFLAGS) -DLV_CONF_INCLUDE_SIMPLE -include gui/lv_conf.h \
               -DGUI_VERSION='"$(GUI_VERSION)"'
GUI_LDFLAGS := $(ARCH) -nostartfiles -lgcc -Wl,--nmagic,--gc-sections -T gui/link.ld

# hekate's DRAM training module, which the GUI loads from
# hwtest/libsys_minerva.bso - or, when that is not on the card, from hekate's
# own bootloader/sys/libsys_minerva.bso - to run the DRAM at Nyx's clocks
# (gui_clock.c). Built from the submodule as hekate builds it, with the
# objects and the output kept here.
GUI_MINERVA := $(GUI_DIR)/libsys_minerva.bso

gui: $(GUI_BIN) $(GUI_MINERVA)

$(GUI_MINERVA): | $(GUI_DIR)
	$(MAKE) --no-print-directory -C $(HEKATE)/modules/hekate_libsys_minerva \
	    BUILDDIR=$(CURDIR)/$(GUI_DIR)/minerva OUTPUTDIR=$(CURDIR)/$(GUI_DIR)
	@printf '   built %s - copy to SD as hwtest/libsys_minerva.bso (or leave it to hekate'"'"'s bootloader/sys copy)\n' $@

$(GUI_DIR):
	@mkdir -p $@

$(GUI_DIR)/%.o: %.c gui/lv_conf.h $(DEF_STAMP_D) | $(GUI_DIR)
	$(CC) $(GUI_CFLAGS) -c -o $@ $<
$(GUI_DIR)/%.o: %.S $(DEF_STAMP_D) | $(GUI_DIR)
	$(CC) $(GUI_ASFLAGS) -c -o $@ $<
$(GUI_DIR)/gui_start.o: gui/start.S | $(GUI_DIR)
	$(CC) $(GUI_ASFLAGS) -c -o $@ $<
$(GUI_DIR)/gui_fonts_blob.o: gui/fonts_blob.S gui/res/fonts.bin | $(GUI_DIR)
	$(CC) $(GUI_ASFLAGS) -c -o $@ $<

ifneq ($(HAVE_A64),)
GUI_CFLAGS += -DHAVE_CPU_STUB=1
GUI_OBJS += $(GUI_DIR)/cpu_stub_bin.o
$(GUI_DIR)/cpu_stub_bin.o: $(DEFAULT_DIR)/cpu_stub_bin.c | $(GUI_DIR)
	$(CC) $(GUI_CFLAGS) -c -o $@ $<
endif

$(GUI_DIR)/gui.elf: $(GUI_OBJS) gui/link.ld
	$(CC) $(GUI_LDFLAGS) -o $@ $(GUI_OBJS) -lgcc

$(GUI_BIN): $(GUI_DIR)/gui.elf
	$(OBJCOPY) -O binary $< $@
	@printf '   built %s (%s bytes) - copy to SD as hwtest/gui.bin\n' $@ $$(stat -c%s $@)

-include $(wildcard $(DEFAULT_DIR)/*.d)
-include $(wildcard $(GUI_DIR)/*.d)

clean:
	rm -rf $(DEFAULT_DIR) $(GUI_DIR) build-jc build-ram
