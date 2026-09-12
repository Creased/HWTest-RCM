# hwtest dedicated universal DRAM diagnostic
#
# Produces one self-extracting RCM payload:
#   ../build-ram/hwtest_ram.bin
#
# The payload detects T210 vs T210B01 before hw_init() and dispatches only to
# the matching public Atmosphere Erista/Mariko MTC implementation.

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM, e.g. export DEVKITARM=/opt/devkitpro/devkitARM")
endif

ROOT      := ..
OUT       := $(ROOT)/build-ram
HEKATE    := $(ROOT)/third_party/hekate
BDKDIR    := $(HEKATE)/bdk
ATMOSPHERE := $(ROOT)/third_party/Atmosphere
ATM_PROGRAM := $(ATMOSPHERE)/fusee/program
ATM_GEN     := $(OUT)/atmosphere
ATM_MTC_SRC := $(ATM_GEN)/fusee/program/source/mtc
ATM_TEGRA_INC := $(ATM_GEN)/libraries/libvapours/include/vapours/tegra
ATM_PATCH   := patches/atmosphere-mtc.patch
ATM_STAMP   := $(ATM_GEN)/.patched
GFXDIR    := $(ROOT)/gfx
LDRDIR    := $(ROOT)/loader
TARGET    := hwtest_ram
IPL_LOAD_ADDR := 0x40008000
LDR_LOAD_ADDR := 0x40007000
IPL_MAGIC := 0x4D415254  # "TRAM"
RCM_LIMIT := 130392

CC      := $(DEVKITARM)/bin/arm-none-eabi-gcc
CXX     := $(DEVKITARM)/bin/arm-none-eabi-g++
OBJCOPY := $(DEVKITARM)/bin/arm-none-eabi-objcopy
SIZE    := $(DEVKITARM)/bin/arm-none-eabi-size

GFX_INC   := '"gfx.h"'
FFCFG_INC := '"ffconf.h"'

WARNINGS := -Wall -Wextra -Wsign-compare -Wno-array-bounds -Wno-stringop-overread -Wno-stringop-overflow
ARCH     := -march=armv4t -mtune=arm7tdmi -mthumb -mthumb-interwork $(WARNINGS)
COMMON_FLAGS := $(ARCH) -Os -g -nostdlib -ffunction-sections -fdata-sections -fomit-frame-pointer

CFLAGS := $(COMMON_FLAGS) -std=gnu11 \
    -I$(ROOT) -Isource -I$(BDKDIR) -I$(GFXDIR) \
    -I$(HEKATE)/bootloader/gfx -I$(HEKATE)/bootloader/libs/fatfs \
    -DDEBUG_UART_PORT=1 -DDEBUG_UART_BAUDRATE=115200 -DDEBUG_UART_INVERT=0 \
    -DBL_MAGIC=$(IPL_MAGIC) -DGFX_INC=$(GFX_INC) -DFFCFG_INC=$(FFCFG_INC) \
    -DBDK_MALLOC_NO_DEFRAG

CXXFLAGS := $(COMMON_FLAGS) -ffreestanding -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -std=gnu++23 \
    -Icompat -Isource -I$(ATM_MTC_SRC) -I$(ATM_TEGRA_INC) -I$(BDKDIR)

LDFLAGS := $(ARCH) -nostartfiles -Wl,--nmagic,--gc-sections \
    -Xlinker --defsym=IPL_LOAD_ADDR=$(IPL_LOAD_ADDR) -T $(ROOT)/link.ld

VPATH := $(ROOT) source $(GFXDIR) $(BDKDIR) \
    $(BDKDIR)/display $(BDKDIR)/mem $(BDKDIR)/power $(BDKDIR)/sec $(BDKDIR)/soc \
    $(BDKDIR)/storage $(BDKDIR)/utils $(HEKATE)/bootloader

C_OBJS := \
    start.o exception_handlers.o stubs.o ramtest_main.o \
    gfx.o sprintf.o util.o btn.o \
    bpmp.o ccplex.o clock.o di.o i2c.o irq.o timer.o \
    mc.o sdram.o smmu.o gpio.o pinmux.o pmc.o se.o uart.o fuse.o \
    max7762x.o sdmmc.o hw_init.o

CPP_OBJS := compat_stubs.o ramtest_mtc_universal.o fusee_mtc_mariko.o fusee_mtc_erista.o
OBJS := $(addprefix $(OUT)/,$(C_OBJS) $(CPP_OBJS))

UNC_ELF := $(OUT)/$(TARGET).elf
UNC_BIN := $(OUT)/$(TARGET)_unc.bin
FINAL   := $(OUT)/$(TARGET).bin

.PHONY: all clean
all: $(FINAL)

# Atmosphere is a pinned git submodule. Build from an isolated generated copy
# so hwtest's bounded-wait/error-reporting adaptations remain a small patch and
# the upstream MTC sources, timing headers and table blobs are not duplicated.
ATM_UPSTREAM_TEGRA := \
    $(ATMOSPHERE)/libraries/libvapours/include/vapours/tegra/tegra_clkrst.hpp \
    $(ATMOSPHERE)/libraries/libvapours/include/vapours/tegra/tegra_emc.hpp \
    $(ATMOSPHERE)/libraries/libvapours/include/vapours/tegra/tegra_mc.hpp
ATM_LOCAL_SHIMS := source/fusee_fatal.hpp source/fusee_uncompress.hpp

$(ATM_STAMP): $(ATM_PATCH) $(ATM_LOCAL_SHIMS) | $(OUT)
	@test -f $(ATM_PROGRAM)/source/mtc/fusee_mtc_erista.cpp || { \
	  echo 'ERROR: Atmosphere submodule missing; run git submodule update --init --recursive'; exit 1; }
	@rm -rf $(ATM_GEN)
	@mkdir -p $(ATM_GEN)/fusee/program/source $(ATM_TEGRA_INC)
	@cp -a $(ATM_PROGRAM)/source/mtc $(ATM_GEN)/fusee/program/source/
	@cp $(ATM_LOCAL_SHIMS) $(ATM_GEN)/fusee/program/source/
	@ln -s ../../../../third_party/Atmosphere/fusee/program/mtc_tables $(ATM_GEN)/fusee/program/mtc_tables
	@cp $(ATM_UPSTREAM_TEGRA) $(ATM_TEGRA_INC)/
	@cd $(ATM_GEN) && patch -s -p1 < $(abspath $(ATM_PATCH))
	@touch $@

$(OUT):
	@mkdir -p $@

$(OUT)/%.o: %.c | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.S | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/compat_stubs.o $(OUT)/ramtest_mtc_universal.o: $(ATM_STAMP)

$(OUT)/fusee_mtc_mariko.o: $(ATM_STAMP)
	$(CXX) $(CXXFLAGS) -c -o $@ $(ATM_MTC_SRC)/fusee_mtc_mariko.cpp

$(OUT)/fusee_mtc_erista.o: $(ATM_STAMP)
	$(CXX) $(CXXFLAGS) -c -o $@ $(ATM_MTC_SRC)/fusee_mtc_erista.cpp

$(UNC_ELF): $(OBJS) $(ROOT)/link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	@$(SIZE) $@

$(UNC_BIN): $(UNC_ELF)
	$(OBJCOPY) -O binary $< $@

$(OUT)/payload_lz.h: $(UNC_BIN) $(ROOT)/tools/mkpayload.py $(ROOT)/tools/lzpack.py
	@python3 $(ROOT)/tools/mkpayload.py $< $(OUT)/payload.lz $@
	@python3 check_overlap.py $(OUT)/payload.lz $(UNC_BIN)

LDR_CFLAGS := $(ARCH) -Os -g -nostdlib -ffunction-sections -fdata-sections \
    -fomit-frame-pointer -std=gnu11 -I$(BDKDIR) -I$(OUT) -I$(LDRDIR) \
    -DBL_MAGIC=$(IPL_MAGIC)
LDR_LDFLAGS := $(ARCH) -nostartfiles -Wl,--nmagic,--gc-sections \
    -Xlinker --defsym=LDR_LOAD_ADDR=$(LDR_LOAD_ADDR) -T $(LDRDIR)/link.ld

$(OUT)/ldr_loader.o: $(LDRDIR)/loader.c $(OUT)/payload_lz.h
	$(CC) $(LDR_CFLAGS) -c -o $@ $<
$(OUT)/ldr_start.o: $(HEKATE)/loader/start.S | $(OUT)
	$(CC) $(LDR_CFLAGS) -c -o $@ $<
$(OUT)/ldr_lz.o: $(BDKDIR)/libs/compr/lz.c | $(OUT)
	$(CC) $(LDR_CFLAGS) -c -o $@ $<

LDR_OBJS := $(OUT)/ldr_start.o $(OUT)/ldr_loader.o $(OUT)/ldr_lz.o

$(OUT)/loader.elf: $(LDR_OBJS) $(LDRDIR)/link.ld
	$(CC) $(LDR_LDFLAGS) -o $@ $(LDR_OBJS) -lgcc

$(FINAL): $(OUT)/loader.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -c%s $@); unc=$$(stat -c%s $(UNC_BIN)); \
	 printf '   built %s (%s bytes, %s uncompressed)\n' $@ $$size $$unc; \
	 if [ $$size -gt $(RCM_LIMIT) ]; then \
	   echo '   ERROR: universal RAM payload exceeds the RCM injector limit'; exit 1; \
	 fi

clean:
	rm -rf $(OUT)
