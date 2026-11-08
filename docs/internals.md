# Internals

## The RAM test

See [the RAM test](ramtest.md).

## The self-extracting loader

TegraRcmSmash, which TegraRcmGUI drives, prepends a 66216-byte exploit
prologue and silently truncates the buffer to 192 KiB, so a payload past
130392 bytes reaches the console cut short. hwtest.bin is therefore
`loader/`, about 1.8 KB of code at `0x40007000`, with the payload trailing
it as a raw deflate stream (`tools/mkpayload.py --codec deflate`, zlib at
level 9, about 62% of the linked image). The Makefile fails a `.bin` over
`RCM_LIMIT`.

`loader.c` boosts the BPMP, copies the stream back to front so it ends at
`0x40038000`, expands it forward into `0x40008000` with `inflate.c` and
jumps there. `inflate.c` is a small decoder after the shape of zlib's
`puff.c`: canonical Huffman codes decoded a bit at a time, every table on
the stack (`.bss` would sit where the output goes), no window of its own.
It stops the loader on a damaged stream instead of jumping into it.

Output and input share IRAM, so two things are checked at build time.
`tools/check_inflate.py` replays the stream through zlib one input byte at
a time and fails the build if the writer ever comes within 256 bytes of
the first unread input byte; it prints the smallest gap it saw (about
30 KB today). `loader/link.ld` asserts that the loader's code and
constants end at or below `0x40008000`, where the output starts.

The RAM test builds the same `loader.c` with `-DLDR_LZ` and keeps the BCL
LZ77 image, checked by `ramtest/check_overlap.py`
([the RAM test](ramtest.md)). hwtest.bin moved to deflate when the LZ
image ran out of room under the limit: deflate packs it about a fifth
tighter.

## The GUI's clocks

hwtest.bin runs the probes with the BPMP at 408 MHz and the DRAM at its
204 MHz boot clock. Drawing is memory-bound and scrolling crawls there, so
the GUI draws where hekate's menus do: the BPMP at hekate's boost clock
(`BPMP_CLK_LOWER_BOOST`, 576 MHz, on Erista; `BPMP_CLK_DEFAULT_BOOST`,
589 MHz, on Mariko), and, as Nyx does, the DRAM trained and switched by
hekate's Minerva module (1600 MHz on Erista; 408 MHz on Mariko, for which
Minerva has no table to train from). The module is read from
`hwtest/libsys_minerva.bso` on the SD card (`make gui` builds it from the
hekate submodule into `build-gui/libsys_minerva.bso`; `make sdzip` puts it
at that path in the zip for the card), or failing that from hekate's own
`bootloader/sys/libsys_minerva.bso`; a copy whose interface does not match
(hekate changes a magic word with it) is passed over. bdk's
`minerva_init()` only looks in `bootloader/sys`, so `gui_clock.c` has its
own copy of it that tries hwtest's path first. bdk's `minerva.c` is not
linked; `hw_deinit()` still refers to its `minerva_deinit()`, so `stubs.c`
provides an empty one, which is safe because hwtest never calls
`hw_deinit()`. The module is looked for only when the SD card is up, and
the card is unmounted afterwards but left powered for the probes. Without
any, the DRAM stays at its 204 MHz boot clock and only the BPMP is raised.

On Erista the module trains at 204, 800 and 1600 MHz, then switches to 800
and then to 1600 MHz (bdk's FSP workaround). Mariko is handed
`MTC_IRB_MAGIC` instead of `MTC_NEW_MAGIC` and switches straight to
408 MHz, with no training. Erista's LPDDR4 drifts at the high clocks, so
while a module is loaded an LVGL task at `LV_TASK_PRIO_HIGHEST` runs its
periodic retraining every `EMC_PERIODIC_TRAIN_MS` (250 ms), as Nyx does. It
acts only while the DRAM is at 1066 MHz or more: never during a check, and
never on Mariko. A switch to the clock Minerva is already at is skipped,
because Minerva hangs on it.

Both go back to the payload's clocks (BPMP 408 MHz, `BPMP_CLK_NORMAL`, and
DRAM 204 MHz) whenever the health check or a custom check runs, so the
readings match the text launcher's, and come back up once the report has
been saved (or its save skipped, with no SD card or eMMC). They also drop
for the [Dock Test](dock.md) while its screen is open (the PD controller
did not answer at the boost), and before [the RAM test](ramtest.md)
starts, which trains from the boot clock and never returns.
`gui_clock_fast(false)` drops them and `gui_clock_fast(true)` raises them
again. It is a plain switch, not a count: a call that matches the current
state returns at once, so the last call wins. The Dock Test drops them on
entry and raises them in its `gui_on_leave()` handler.

`gui_clock_init()` runs during the GUI's start-up
([the GUI](gui.md#start-up)) and logs each step on the debug console as it
completes. These lines always go out on UART-B, on the right rail: they
are sent before the Joy-Con driver can take the rail and before Custom
Check can move the console. The console is the only place the clocks are
reported:

```
gui: DRAM <n> MHz (<module path, or "no libsys_minerva.bso found">)
gui: BPMP boosted
gui: measured BPMP <n> MHz, DRAM <n> MHz
```

The first line gives the DRAM clock the GUI draws at and the copy of the
module it came from. The third is counted by the PTO (`CLK_PTO_SCLK`,
`CLK_PTO_EMC`), so it shows the clocks actually reached rather than the
ones asked for. Minerva's whole part (the module load, the trainings and
the switches) runs before the first line, and only the BPMP boost runs
between the first and the second (its DRAM switch is skipped, the DRAM
being there already). So a console that stops after `gui: DRAM` hung in
the boost. One whose last line is the `=== hwtest GUI ===` banner stopped
somewhere before `gui: DRAM`: in Minerva or in an earlier start-up step,
the display and storage bring-up among them; a run with no module at
either path tells the two apart.

Then, every couple of seconds while anything is drawn, the console gets
what a frame costs, including the time spent waiting for the VIC to finish
rotating the previous frame. This line follows the console wherever Custom
Check has moved it, and is dropped while that console is a rail a Joy-Con
has taken:

```
gui: <n> frames, <n> ms avg, <n> ms worst, VIC wait <n> ms/frame
```

## The GUI's fonts

The GUI's fonts are Inter at Nyx's 30 px (most text) and 20 px (hints) plus
a 25 px of its own for the home buttons' captions (six buttons in the row
make the tile 180 px instead of Nyx's 256, and it is scaled as a whole),
Source Code Pro at 18 px for report text, and Font Awesome 4.7 symbols at
the two text sizes, at 100 px (the home buttons) and at 120 px (the
results' verdict). In C they are `gui_font_30`, `gui_font_20`,
`gui_font_25` and `gui_font_mono`, then `gui_icons_30`, `gui_icons_20`,
`gui_icons_100` and `gui_icons_120`. Nyx's own glyph bitmaps are not in
the source tree: they are read at run time from hekate's `res.pak`, at
offsets that move between releases, so the GUI bundles its own.
`tools/mkfont.py` rasterises them into:

- `gui/res/fonts.bin`: every glyph bitmap back to back, at 4 bpp, linked
  into gui.bin as `gui_font_blob` by `gui/fonts_blob.S` (its
  `.rodata.gui_font_blob` section lands in gui.bin's `.data`, where
  `gui/link.ld` puts all read-only data);
- `gui/gui_fonts.c`: one `lv_font_t` per face and size, pointing into the
  blob;
- `gui/gui_fonts.h`: their declarations and the `ICON_*` strings.

Only that output is in the tree, with the faces' licence in
`gui/res/LICENSE-fonts.txt`. `gui/lv_conf.h` turns bdk's own fonts (Inter
UI and the hekate symbols) off and makes `&gui_font_30` LVGL's
`LV_FONT_DEFAULT`.

The text fonts cover printable ASCII. A text glyph is as wide as the
face's own advance, so the space between letters is already in it; Nyx's
glyphs are cut to their ink, and bdk's LVGL spaces letters 3 px apart for
them. `gui_theme_init()` therefore sets `text.letter_space` to 0 in
LVGL's built-in styles before anything copies them (the hint style puts 1
back).

bdk's LVGL is built without UTF-8 (`LV_TXT_UTF8 0`) and treats every byte
as one letter, so the 44 icons (`ICON_*`) are mapped onto single bytes
from 0xC0 to 0xEB: below that, a leading byte in 0x80-0xBF is a
button-matrix control code (tab names, message box buttons) and is never
drawn as a letter. The set is the `ICONS` table in `tools/mkfont.py`: its
order assigns the bytes from `ICON_FIRST` (0xC0) and its names become
`ICON_<NAME>`, so an icon is added by appending to it and regenerating,
with room for 20 more up to 0xFF. (`SAVE` and `SDCARD` are the same
floppy-o glyph, FA 4 having no SD card.) The 30 and 20 px icon faces are
generated at the cell height of the text font of the same size and
chained onto it with `lv_font_add()` by `gui_fonts_init()`, which is
hand-written in `gui_main.c` and called from `gui_theme_init()`, so one
label can mix text and icons on a line. An `ICON_*` letter therefore works
inline only in `gui_font_30` and `gui_font_20`; `gui_font_25` and
`gui_font_mono` have no icon face chained.

To regenerate, install freetype-py and give the script the same three
faces the committed output was made from: Inter 4.001 (variable, opsz and
wght axes), Source Code Pro (variable, wght axis) and FontAwesome 4.7
(`.otf`).

```
python3 tools/mkfont.py --text Inter[opsz,wght].ttf \
    --mono SourceCodePro[wght].ttf --icons FontAwesome.otf
```

## BDK's SDRAM parameter scratch

BDK hands out a few fixed IRAM addresses no section knows about, and
`SDRAM_PARAMS_ADDR` (`memory_map.h`) is one of them: `sdram_init()`, reached
from `hw_init()` on the first line of `ipl_main`, copies the chip's DRAM
parameter table there and patches it for the fused DRAM ID. That writes
the size of the parameter struct (0x768 bytes on Erista, 0x838 on Mariko;
`SDRAM_PARAMS_SZ` reserves `0x838`), unconditionally, on every boot. Any
static that lands in that window is destroyed during `hw_init`, after
`start.S` zeroes `.bss`, before any probe runs.

Nothing in the payload can defend against that, so the link script stays out
of the way: hwtest.bin's `link.ld` (the root one) starts `.bss` above the
scratch window and asserts that `.text`/`.data` never reach it (a build
that does fails with "link.ld: .text/.data now reach BDK's SDRAM param
scratch at 0x40036000"). The gap costs no image bytes, since `.bss`
occupies none. Such a collision is harmless for any variable written
before it is first read, which is most of them, so it surfaces only as a
static read before assignment coming up with a garbage value. That is how
it was found: with the scratch at upstream BDK's 0x40030000, the Wi-Fi
PCIe probe's arming flag came up already set and the probe ran in the
boot sweep before anything had asked for it.

hwtest's image has outgrown that window, so the repository root carries
its own `memory_map.h`, which every source including `<memory_map.h>`
picks up ahead of BDK's, `sdram.c` and `gui/lv_conf.h` among them (the
builds put `-I.` before `-I$(BDKDIR)`). It is byte-identical to BDK's
except for one line (and the comment saying so), which moves the scratch
to 0x40036000-0x40036838. Everything else reads the copy too, so after
updating the hekate submodule, copy BDK's new `memory_map.h` over it and
reapply that line. `link.ld` repeats those two addresses by hand
(`SDRAM_PARAMS_ADDR`, `SDRAM_PARAMS_END`) and nothing ties them to the
header, so move both together. `.bss` starts at `MAX()` of the image's end
and the window's end, so it starts above the scratch even while the image
ends well short of it. Do not drop the `MAX()` because `.bss` happens to
end below the scratch today: the first assertion checks only
`.text`/`.data`, so nothing would catch `.bss` growing into the window. A
second assertion keeps `__bss_end` at or below 0x4003DF00 ("link.ld: .bss
has grown into the pre-pivot stack"), 8 KiB (0x2000) under the stack
`hw_init()` runs on before `ipl_main` pivots it (its top is
`SYS_STACK_TOP_INIT`, 0x4003FF00); with `.bss` starting above the scratch,
that leaves `.bss` at most 0x76C8 bytes, about 30 KiB.

The RAM test links its expanded image with the same script and puts the
repository root first on its include path (`ramtest/Makefile`), so the
moved scratch and both assertions apply to it too
([the RAM test](ramtest.md#the-image-and-its-build-checks)).

The GUI stage needs no such guard, and `gui/link.ld` has none: it runs from
DRAM at 0x81000000 and arrives with `hw_init` already done by hwtest.bin.
