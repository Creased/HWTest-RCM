#!/bin/sh
# The GUI's screens, built for the host: bdk's LVGL 5 and the screen code
# exactly as the console build compiles them, against stubs for the hardware
# (host_tools/sim/sim_stubs.c) and a draw buffer that ends up in a PNG.
#
#   host_tools/sim/build.sh && build-sim/sim out-dir
#
# Needs a host gcc and zlib's headers (zlib1g-dev), which is what the PNGs
# are deflated with. O= puts the build somewhere other than build-sim.
#
set -e
cd "$(dirname "$0")/../.."
O=${O:-build-sim}
L=third_party/hekate/bdk/libs/lvgl
mkdir -p "$O"
# -Wno-unused-*: the Wi-Fi screen's scan is built without HAVE_CPU_WIFI here
# (there is no CPU0 image to run), which leaves its run path unreferenced.
# -Wno-*-pointer-cast: an address on the console is a u32, and this is a
# 64-bit host; the code that keeps addresses in words is not run here.
CF="-std=gnu11 -O1 -g -Wall -include stdint.h -DLV_CONF_INCLUDE_SIMPLE
    -include gui/lv_conf.h -Igui -I. -Ithird_party/hekate/bdk
    -Igfx -Ithird_party/hekate/bootloader/gfx -Ithird_party/hekate/bootloader/libs/fatfs
    -DHWTEST_GUI -DHWTEST_HOSTSIM -DDEBUG_UART_BAUDRATE=115200
    -DGUI_VERSION=\"hostsim\" -Wno-builtin-declaration-mismatch
    -include host_tools/sim/sim_host.h -DWIFI_MBOX_ADDR=sim_mbox_base
    -Wno-unused-function -Wno-unused-variable
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast"
SRC="$L/lv_core/*.c $L/lv_draw/*.c $L/lv_hal/*.c $L/lv_misc/*.c $L/lv_objx/*.c
     $L/lv_themes/lv_theme.c $L/lv_themes/lv_theme_hekate.c $L/lv_fonts/*.c
     gui/gui_theme.c gui/gui_fonts.c dbgcfg.c third_party/hekate/bdk/utils/sprintf.c
     host_tools/sim/sim.c host_tools/sim/sim_stubs.c"
# Everything, every time: four seconds, and no stale object to explain a
# test that passes against code that is no longer there. sim.c carries two
# of the screens' sources inside it, which no object-file timestamp sees.
pids=
for f in $SRC; do
    gcc $CF -c -o "$O/$(basename "$f" .c).o" "$f" &
    pids="$pids $!"
done
rc=0
for p in $pids; do wait "$p" || rc=1; done   # set -e does not see a background job
[ "$rc" = 0 ] || exit 1
# The host's own side: plain C against the host's headers, which contradict
# bdk's (malloc, usleep), so none of the console's flags go near it.
gcc -std=gnu11 -O1 -g -Wall -c -o "$O/sim_io.o" host_tools/sim/sim_io.c
gcc -c -o "$O/fonts_blob.o" gui/fonts_blob.S   # the glyph blob: directives only, no cpp flags
gcc -o "$O/sim" "$O"/*.o -lz -lm
printf '   built %s\n' "$O/sim"
