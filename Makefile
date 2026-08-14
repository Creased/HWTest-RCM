# hwtest -- Switch RCM hardware probe
#
# Builds two flat .bin payloads:
#
#   build/hwtest.bin       UART_B debug + IsAttached-only Joy-Con detection
#   build-jc/hwtest_jc.bin No UART debug, full Joy-Con polling on both rails
#
# Joy-Con polling can't share UART_B with debug -- the controller can only
# be in one config at a time (1 Mbaud + HW flow control for Joy-Con vs
# 115200 8N1 for debug), and Hekate's joycon.c body is gated on
# !DEBUG_UART_PORT for that reason. So we ship both variants and let the
# user flash whichever fits the moment.
#
# Hekate source tree is vendored as a git submodule at third_party/hekate.
# We reuse its BDK (the bdk/ subdirectory) instead of vendoring the SoC
# drivers in-tree. Run `git submodule update --init` after a fresh clone.

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

# Per-variant output dirs and suffixes. Both variants share the same source
# tree; the only thing that differs is CUSTOMDEFINES and the joycon.o
# inclusion.
DEFAULT_DIR   := build
DEFAULT_BIN   := $(DEFAULT_DIR)/$(TARGET).bin
JC_DIR        := build-jc
JC_BIN        := $(JC_DIR)/$(TARGET)_jc.bin

# Object list shared by both variants.
COMMON_OBJS = \
    start.o exception_handlers.o main.o stubs.o emmcsn.o \
    log.o dx.o report.o verdict.o \
    probe_soc.o probe_power.o probe_memclk.o probe_inputs.o probe_bt.o \
    probe_display.o probe_storage.o probe_lowlevel.o \
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

# Joy-Con build pulls in Hekate's joycon.c on top of the common set.
JC_EXTRA_OBJS := joycon.o

DEFAULT_OBJS := $(addprefix $(DEFAULT_DIR)/,$(COMMON_OBJS))
JC_OBJS      := $(addprefix $(JC_DIR)/,$(COMMON_OBJS) $(JC_EXTRA_OBJS))

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

# JC build leaves DEBUG_UART_PORT undefined so Hekate's hw_init skips the
# UART setup and joycon.c's body compiles in.
JC_DEFINES      := $(BASE_DEFINES) -DJC_PROBE=1

WARNINGS := -Wall -Wsign-compare -Wno-array-bounds -Wno-stringop-overread -Wno-stringop-overflow

ARCH    := -march=armv4t -mtune=arm7tdmi -mthumb -mthumb-interwork $(WARNINGS)
# Extra -D flags from the command line, e.g. a build that keeps the Wi-Fi
# PCIe probe out of the sweep until it is armed from the pager:
#   make EXTRA_DEFINES=-DWIFI_CFG_AUTORUN=0
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
DEF_STAMP_J := $(JC_DIR)/.extra_defines
$(shell mkdir -p $(DEFAULT_DIR) $(JC_DIR) 2>/dev/null; \
    for f in $(DEF_STAMP_D) $(DEF_STAMP_J); do \
        printf '%s\n' '$(EXTRA_DEFINES)' | cmp -s - $$f || \
            printf '%s\n' '$(EXTRA_DEFINES)' > $$f; \
    done)
# The rule below regenerates the stamps when `make clean all` deletes them
# mid-invocation (the parse-time $(shell) above has already run by then).
$(DEF_STAMP_D) $(DEF_STAMP_J):
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(EXTRA_DEFINES)' > $@

CFLAGS_BASE := $(ARCH) -O2 -g -nostdlib -ffunction-sections -fdata-sections \
               -fomit-frame-pointer -fno-inline -std=gnu11 \
               -I. -I$(BDKDIR) -I$(GFXDIR) \
               -I$(HEKATE)/bootloader/gfx \
               -I$(HEKATE)/bootloader/libs/fatfs \
               $(EXTRA_DEFINES)
LDFLAGS := $(ARCH) -nostartfiles -lgcc -Wl,--nmagic,--gc-sections \
           -Xlinker --defsym=IPL_LOAD_ADDR=$(IPL_LOAD_ADDR) -T link.ld

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy

.PHONY: all default jc clean

# `make` (no args) builds both variants.
all: default jc

default: $(DEFAULT_BIN)
jc:      $(JC_BIN)

$(DEFAULT_DIR) $(JC_DIR):
	@mkdir -p $@

# Per-variant compile rules. The %-pattern keeps the obj name parallel
# across variants (e.g. main.o lives in both build/ and build-jc/) but
# each gets its own CFLAGS via the variant-specific define set.
$(DEFAULT_DIR)/%.o: %.c $(DEF_STAMP_D) | $(DEFAULT_DIR)
	$(CC) $(CFLAGS_BASE) $(DEFAULT_DEFINES) -c -o $@ $<
$(DEFAULT_DIR)/%.o: %.S $(DEF_STAMP_D) | $(DEFAULT_DIR)
	$(CC) $(CFLAGS_BASE) $(DEFAULT_DEFINES) -c -o $@ $<

$(JC_DIR)/%.o: %.c $(DEF_STAMP_J) | $(JC_DIR)
	$(CC) $(CFLAGS_BASE) $(JC_DEFINES) -c -o $@ $<
$(JC_DIR)/%.o: %.S $(DEF_STAMP_J) | $(JC_DIR)
	$(CC) $(CFLAGS_BASE) $(JC_DEFINES) -c -o $@ $<

# Link + objcopy per variant.
$(DEFAULT_DIR)/$(TARGET).elf: $(DEFAULT_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(DEFAULT_OBJS) -lgcc

$(DEFAULT_BIN): $(DEFAULT_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@printf '   built %s (%s bytes)\n' $@ $$(stat -c%s $@)

$(JC_DIR)/$(TARGET)_jc.elf: $(JC_OBJS) link.ld
	$(CC) $(LDFLAGS) -o $@ $(JC_OBJS) -lgcc

$(JC_BIN): $(JC_DIR)/$(TARGET)_jc.elf
	$(OBJCOPY) -O binary $< $@
	@printf '   built %s (%s bytes)\n' $@ $$(stat -c%s $@)

clean:
	rm -rf $(DEFAULT_DIR) $(JC_DIR)
