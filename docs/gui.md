# The GUI

The GUI is hwtest's second front end: the same probes as the text payload,
worked from the touchscreen, a Joy-Con or the console's own buttons, with
the results in plain words. It is built on hekate's bdk and the LVGL 5 it
carries, as Nyx is, and it is far past the RCM size limit, so it is not a
payload: `make gui` builds `build-gui/gui.bin`, which goes on the SD card
as `hwtest/gui.bin`, and hwtest.bin loads it into DRAM and jumps to it, the
way hekate hands over to Nyx. The screens are listed in the
[README](../README.md#gui). The GUI records no finding of its own (nothing
in `gui/` calls `dx_set`): every result it shows comes from the probes,
whose conditions are in their own notes. The Joy-Con screens and the
attach glow are in [Joy-Con tests](joycon.md), the Bluetooth test in
[The Bluetooth test](bluetooth.md), the Dock Test in [Dock Test](dock.md),
and the GUI's clocks and fonts in [Internals](internals.md).

## Hand-over from hwtest.bin

hwtest.bin itself reaches the console through `loader/`, a self-extracting
stub ([Internals](internals.md#the-self-extracting-loader)), because
TegraRcmSmash silently drops everything past 130392 bytes of payload. gui.bin never goes through it: hwtest.bin reads it whole off
the card. `main.c` calls `ui_boot_gui()` (`ui_menu.c`) once the display is
up, the "hwtest - initialising" splash drawn and the backlight started,
before the text launcher. It returns only when the GUI does not start, and
the launcher then runs as it does with no card. The GUI never returns to
hwtest.bin: reaching the text launcher from it takes a power-off or a
reboot, then hwtest.bin started again with VOL- held.

1. One `btn_read()`. With VOL- held it waits for the key to be let go and
   returns: the launcher would otherwise take the held key as a step up and
   open on "Power off". It is a single read, taken once the display and the
   backlight are up, so VOL- has to be held from injection until the
   splash shows.
2. Card detect on PZ1, set up as bdk's SDMMC1 bring-up sets it, with
   100 us to settle. No card returns at once, because `sd_mount()` would
   print its errors over the splash. A failed mount returns too.
3. `sd_file_read("hwtest/gui.bin")`, then `sd_end()` whatever came back.
4. The header `gui/start.S` puts at the front: word 0 the branch to the
   entry, word 1 `0x55475748` ("HWGU"), word 2 `__ipl_end`, word 3
   `__ipl_start`. The file is taken when it is at least 16 bytes, the magic
   matches, the load address is `NYX_LOAD_ADDR` (0x81000000) and
   `__ipl_end - __ipl_start` is no more than the file's length (unsigned,
   so an end below the start is refused too). `gui/link.ld` keeps the final
   alignment padding inside `.data` so that the file is that long. Anything
   else is freed and the launcher runs.
5. `display_get_verbose_panel_id()`, the whole file copied to 0x81000000,
   `bpmp_mmu_disable()` (bdk cleans and invalidates the cache, then
   switches it off), and a call into the image with R0 = `GUI_ARGS_MAGIC`
   (`0x44494E50`, "PNID") and R1 = the panel id. It does not come back.

Nothing else is checked: no version or build match, no checksum, and the
copy is as long as the file, with no upper bound. A gui.bin from another
build with the same header layout starts, and so does one damaged past its
header. Nothing says why the GUI was skipped: the launcher just appears.
UART-B does show whether it was: hwtest.bin's `[boot] backlight ok` is
followed by `=== hwtest GUI ===` when the GUI starts, or by
`[boot] launcher enter` when it was skipped. With neither, it stopped in
between: in the card read, the copy or the GUI's first steps. The only
size guard is at build time: `gui/link.ld` asserts that the image and its
BSS end by 0x82000000, inside the 16 MiB slot hekate gives Nyx under the
heap.

The panel id is passed because bdk's display code branches on it (an
OLED's brightness goes over DSI) and the GUI never runs `display_init()`;
see [Display](display.md#handing-the-id-to-the-gui).

## Start-up

`_start` (`gui/start.S`) arrives with `hw_init` done, the DRAM trained,
the display up and the MMU off. The image is linked where it is loaded, so
it never relocates. It puts the stack at 0x4003FF00 in IRAM ("hwtest.bin's
own image is no longer needed"), clears BSS, keeps R0 and R1 in
`gui_boot_args` past that clear and calls bdk's `_irq_setup`, which sets
the exception stacks and vectors and enters `ipl_main()` (`gui_main.c`):

1. The panel id from `gui_boot_args`, or `panel_id_lost()` without the
   magic ("An older hwtest.bin passes none").
2. The heap, the BPMP MMU on, the BPMP at `BPMP_CLK_NORMAL`.
3. `_log_uart_only = true`: what the probes print goes to the debug
   console, the report buffer and the GUI's page capture, never to the
   screen, which belongs to LVGL.
4. "=== hwtest GUI ===" on the debug console. Nothing has moved the
   console yet, so it goes out on UART-B as hwtest.bin left it.
5. LVGL and the display (below).
6. `probe_reset_snapshot()`: the MAX77620's clear-on-read latches, read
   once per boot before anything active runs, the touchscreen and Joy-Con
   bring-up included, so every check in a session reports that snapshot
   ([Raw state](raw-state.md#the-snapshot)).
7. `gui_hw_prepare()`: `sd_initialize`, `emmc_initialize` and
   `tmp451_init`, once per boot whatever is picked later, because the
   status bar reads the serial from the eMMC and the clocks load Minerva
   from the card. The text payload brings the card and the eMMC up only
   for Storage, and the TMP451 only for Power. Nothing at start-up retries
   them, but a check with Storage picked runs `sd_initialize` or
   `emmc_initialize` again for one that is not up and sets `g_sd_ok` and
   `g_emmc_ok` afresh; the RAM Test's failure path (below) brings the card
   up again too.
8. `gui_clock_init()` ([Internals](internals.md#the-guis-clocks)), the
   theme (which reads the console's serial), the status bar, the input
   devices (which power the touchscreen and both Joy-Con rails, see
   [Rails and Joy-Cons](#rails-and-joy-cons)), then Home.

LVGL draws a 1280x720 landscape picture straight into `NYX_FB2_ADDRESS`,
the VIC's input buffer, and the VIC rotates it into the panel's portrait
framebuffer. Nyx draws into a buffer of its own and copies each area
across; on the BPMP's bus that copy is a large part of what a frame costs,
so here it is not made. LVGL uses an area's width as its row stride, so
every invalidated area is widened to the full screen width
(`_round_area`); and it addresses its buffer from the area's top row, so
an invisible full-screen "sentinel", kept the newest child of every screen,
takes LVGL's cover query before each area, waits for the VIC and moves the
buffer address to that row. The panel is pointed at the VIC output only
once a full first frame exists, so it never shows a half-drawn screen.
While the Dock Test has a picture on a TV, `_present()` also copies the
rows each frame drew into the TV's own buffer, so the second head never
scans a half-drawn screen either ([Dock Test](dock.md#the-frame-on-the-tv)).

The loop then runs a pending screen switch, `lv_task_handler()`, the VIC
pass and the frame statistics when anything was drawn (the line itself at
most every 2 s), `gui_cmd_poll()` (`R` and the PC's commands, see
[Commands from the PC](#commands-from-the-pc)), `gui_dbg_poll()` (the USB
serial port), a VOL sample and a 500 us sleep.

The status bar along the bottom shows on the left the console's serial
from PRODINFO, which decrypts only with `sd:/switch/prod.keys`. Without
it that side says why, in yellow (`console_serial()` tells them apart):
"No prod.keys on the SD card", "prod.keys could not be read" or
"prod.keys is not this console's" (no usable `bis_key_00`, or CAL0 does
not decrypt with it). It stays empty when the eMMC or the SD card did not
come up. On the right, refreshed every 5 s, the
battery as Nyx shows it: the MAX17050's `RepSOC` to a tenth of a percent,
the symbol red at 15% and below, a bolt while the BQ24193 reports a charge
status, the current (green into the battery, red out of it) and the cell
voltage, orange with a warning below 3200 mV. The bar is hidden on the
Touchscreen and Screen tests and comes back with the next screen.

An undefined instruction, an abort or an SWI does not hang the GUI: bdk's
handlers (`exception_handlers.S`, linked in) clear BSS and run
`_irq_setup` again, so the GUI starts over at Home with the last results
gone (only zero-initialised state resets: the Custom Check selection, in
`.data`, stays). `gui_boot_args` is in BSS, so the panel id is lost
(`panel_id_lost()`); the debug console is back on UART-B, where a second
`=== hwtest GUI ===` goes out; and the PMIC snapshot is read again, after
the first read already cleared its latches. bdk stores the fault type at
0x4003FF18; nothing in hwtest reads it.

## Input

Every screen works with any one of three inputs, on one LVGL focus group.

**Touch.** bdk's `touch_power_on()` is not used: with VOL+ and VOL- both
held it runs the ITO test, autotune and `FTS4_CMD_SAVE_CX_TUNING`, which
writes the touch controller's EEPROM. `touch_power_on_safe()`
(`probe_inputs.c`) does only the power, reset and sense part; see
[Inputs](inputs.md#powering-it-without-the-calibration-shortcut). If it
fails, touch reads as released until it is powered again after a health
check. Each read (every 33 ms, `LV_INDEV_READ_PERIOD`) is, in order:
nothing when touch did not start or input is held; `touch_poll()`; a
screenshot when the sample comes from a second contact; the screen's touch
sink, when it has one: a sample it takes reaches LVGL as a release, and
one it declines goes on to LVGL; otherwise the point and press go to
LVGL. The screenshot comes before the sink, so it works on every screen,
the touch test included.

**Buttons and Joy-Con.** The keypad read (`_keys_read`):

- VOL+ is next, VOL- previous, POWER enter. `btn_read()` takes the volume
  keys from GPIO and POWER from the MAX77620's `ONOFFSTAT` over I2C. A
  console button wins over the Joy-Con.
- A Joy-Con counts when no console button is down and no VOL press is
  waiting to be replayed, Joy-Con navigation is on and one is connected.
  A is enter; D-pad down or right, or the stick pushed down or right, is
  next; up or left on either is previous. The stick is the left
  Joy-Con's when that one is connected, otherwise the right's. Sticks
  rest up to a tenth of their travel off the nominal centre, each its own
  way, so the centre is taken where the stick sat when its Joy-Con was
  first seen, and only with both axes inside 0x400-0xC00 ("Not while it
  is being held over"). A direction counts beyond 0x280 from it. A Lite's
  left stick reports Y upside down and is inverted. With no Joy-Con
  connected the centre is forgotten. The other buttons do not navigate.
- Joy-Con B is not passed to LVGL: its press calls `gui_back()`, "back" on
  every screen.
- A held next or previous repeats after 400 ms, then every 120 ms, by
  reporting a release for one read; enter never repeats. LVGL's own
  repeat is not used, since a message box's buttons would fire on it.
  LVGL takes an enter held past `LV_INDEV_LONG_PRESS_TIME` (bdk's 5 s,
  unchanged) as a long press and acts on nothing at its release, so
  POWER held for more than 5 s does nothing.
- An open message box holds the focus, frozen to it, and next and
  previous become right and left between its buttons; enter presses the
  one selected.

LVGL reads the keys only between frames and acts on the release after a
press it saw, so a press shorter than a frame would be missed (as in the
Dock Test, with its I2C and redraws at the payload's clocks).
`gui_input_sample()`, two GPIO reads, runs as each frame is flushed, in
long tasks and on every loop pass, and latches a VOL press that came and
went between two keypad reads; the next read replays it once. A press
counts only after its button has been up 40 ms (`VOL_DEBOUNCE_MS`), since
a release bounces. POWER is not latched.

The Joy-Con driver has to be polled at the input rate
([Joy-Con tests](joycon.md#talking-to-a-joy-con)): from the keypad read
while navigation is on, and by screens that read the Joy-Con themselves
(`gui_jc_state()`). Each poll first runs `gui_dbg_rails()` (see
[Rails and Joy-Cons](#rails-and-joy-cons)). On a Lite
(`FUSE_NX_HW_TYPE_HOAG`) the built-in controls come the same way.

Some screens take an input for themselves:

| Screen | Joy-Con | Touch | Way out |
|---|---|---|---|
| Joy-Con Test, Motion, IR Camera | no navigation, B does nothing | LVGL's | POWER on the focused Close, or a tap |
| Screen test | no navigation; any button turns the page | the screen's | its closing box only |
| Touchscreen test | navigation on: B leaves for Home | the screen's | Finish and its box, or Joy-Con B |
| Bluetooth | navigation on until Pair from rails, then the rails are not polled | LVGL's | Close |
| Health check, while it runs | not read | not read | none until it ends, bar `R` |

The next screen switch gives everything back.

## Screens and navigation

Home is laid out as Nyx's, with its tiles in two rows: Health Check,
Joy-Con and Touchscreen, then Screen, Dock and Game Card, three across, and
past a divider the two that leave for payloads of their own, RAM Test over
Lockpick_RCM. The four columns share the width between the margins
(276 px each, 13 px apart, 34 px about the divider), the tiles are 210 px
high, and the two rows sit midway between the tab bar and the bottom row;
a tile's icon and caption sit about its middle in Nyx's proportions.
Lockpick_RCM is greyed out (the theme's inactive button, its text grey)
when it is not on the SD card, and then neither a touch nor VOL+ / VOL-
reaches it. Along the bottom: Custom Check, Last Results (only once a
check has run this boot), Reboot and Power Off. Focus starts on Health
Check; VOL+ / VOL- go through the six tests row by row, then RAM Test and
Lockpick_RCM, then the bottom row. Power Off asks first, then writes
`MAX77620_ONOFFCNFG1_PWR_OFF`, the same power-off as the text launcher's,
which does not ask. Reboot asks
first ("The console powers off and on again."), then calls `do_reboot()`
(see [The R reboot](#the-r-reboot)). The tab left of Home, named
"hwtest <version>" after `GUI_VERSION` (`git describe --tags --always` at
build time), is an About page; only a tap on its tab opens it, since the
tab bar is not in the focus group.

`gui_show()` only records the next screen and the main loop builds it,
because a screen is usually changed from inside an action of the screen
being deleted. `_switch_screen` runs the leaving screen's clean-up hook
(its tasks, the Screen test's backlight), then sets back to Home, Joy-Con
navigation on, no touch sink and the status bar shown, and builds the new
screen. Close and Joy-Con B both call `gui_back()`, which leads Home unless
the screen named another target: a category goes back to Results, a report
to its category or to Results, Motion and IR Camera to the Joy-Con Test.

Two LVGL 5 habits are worked around in `gui_theme.c`. Its focus group
steps onto hidden objects, so the focus is moved on past them. And a
window's header buttons are laid out from the right, so their focus order
is rebuilt for VOL+ to walk them left to right, with the head pointer
fixed by hand because `lv_ll_move_before` orphans a node moved before the
head.

## The health check

Home's Health Check always runs everything ("the whole check, whatever
was picked before"); Custom Check runs a selection and Check Again, on
Results, repeats the last run's without asking (`gui_run_mask()` turns an
empty mask into the whole check). `gui_run()`:

1. Sets `g_run_mask`, which `page_enabled()` reads.
2. Builds the progress window: no Close, the current step, a bar and its
   percentage, a line for the step inside a page, "Keep the console on.
   Some checks make a short sound or run the fan.", and the step list.
3. Counts the enabled pages (Diagnostics, page 0, always is).
4. Holds input and drops to the payload's clocks (`gui_clock_fast(false)`),
   so the readings match the text launcher's.
5. Allocates the 128 KiB report buffer (`REPORT_CAP`) once, arms it and
   empties the page capture.
6. Prints "=== hwtest full UART dump ===", runs `run_probe_mask()`
   (`core.c`: `dx_reset()`, every enabled page in table order, Diagnostics
   last), and prints "=== end of dump ===".
7. Stops the capture, ticks the last step, shows "Done" and "Saving the
   report...".
8. Saves the report, but only when `g_sd_ok` and `g_emmc_ok` are both set
   (by start-up, or by this run's Storage pages): the test short-circuits,
   so with either missing `save_report()` never runs and no `[save]` line
   appears. Then it raises the clocks again.
9. Powers touch up again: the touch and light-sensor probes power the
   panel's LDO6 down when they finish.
10. Releases input and shows Results.

| Step | Report pages |
|---|---|
| Identifying the console | SoC, Fuses |
| Battery, charging and power | Power & charging |
| Memory and clocks | Memory & clocks |
| Storage and game card reader | Storage |
| Wi-Fi and Bluetooth | Wireless |
| Sound | Audio (both the Sound chip and Sound output groups) |
| Screen | Display |
| Touch, sensors and buttons | Inputs |
| System details | Raw state |
| Putting the results together | Diagnostics |

A step none of whose pages was picked is grey, "(not picked)", so the list
still reads as the whole check.

The probes block, and nothing else runs while they do: no input is read,
which is why Close is hidden. `status_set()` is the progress hook. `core.c`
calls it before every page, and the long probes call it through
`status_step()`, which also sends "[step] <msg>" to the debug console. A
page name (matched by pointer against `g_pages`) advances the count and
the step list; any other message, until every enabled page has started,
shows as the sub-line, only its part after ": " when it has one
("Wireless: PCIe lanes" shows "PCIe lanes"). A new page redraws at once, a
step at most every 120 ms. The percentage is pages started out of pages
enabled, so it reaches 100% as Diagnostics starts. `status_set()` also
polls for `R`, the only way out of a running check. The hold stops the
keypad and touch reads but not the VOL sampling as frames are flushed, so
a VOL press that spans a redraw of the progress screen is latched anyway
and moves the focus once when Results opens.

The Joy-Cons are not polled while the check runs, so the driver brings
them up again afterwards. What each probe does to the console is in its
own doc.

### The page capture

Every line the probes print also reaches `gui_log_capture()`, which splits
the text at the "--- Name ---" boundaries `run_probe_mask` prints. Pages
with the same name merge, so the 42 entries of `g_pages` give 11 pages:
SoC, Fuses, Power & charging, Memory & clocks, Storage, Wireless, Audio,
Display, Inputs, Raw state, Diagnostics. There is room for 16, 64 KiB
each, allocated on first use and kept; a page that outgrows its 64 KiB
loses the rest without a mark. A line's colour survives as LVGL recolour
markup; a `#` in the text is doubled outside a coloured span and dropped
inside one, where LVGL would read it as the end of the colour.

### The report file

`save_report()` (`report.c`) writes the report buffer to
`backup/<eMMC serial>/hwtest.txt` ([Storage](storage.md#the-sd-report)).
Once the card and the eMMC are up (at start-up, or through a run's
Storage pages), every run saves, custom ones included, and a partial
report overwrites a full one.

## Results

Three levels, each a tap deeper, all read back from the finding registry
the sweep filled (`dx_get`). How a finding becomes a state (not checked,
ok, soft, partly checked, warning, failure), the soft prefixes, how a row
that was not picked makes a category "Partly checked", and where the GUI
and the text verdict part ways are in [The verdict](verdict.md#in-the-gui).

| Category | Verdict rows | Pages behind Details |
|---|---|---|
| Battery | Battery | Power & charging |
| Charging | Charger | Power & charging |
| Power system | PMIC | Power & charging, Raw state |
| Temperature and fan | Thermal | Power & charging |
| Storage | Storage | Storage |
| Wi-Fi and Bluetooth | Wireless | Wireless |
| Processor and memory | CPU complex, Memory | Memory & clocks, Wireless |
| Sound | Audio | Audio |
| Screen | Display | Display |
| Touch and sensors | Inputs | Inputs |
| System and security | Boot integrity, Identity | SoC, Fuses, Storage |

The CPU complex row runs with the Wireless group and Boot integrity needs
Power and Storage both, so "Processor and memory" and "System and
security" read "Partly checked" on many custom runs. The CPU stub is
linked only when the build finds an aarch64 compiler (`HAVE_A64`); without
it `cpu_complex` is never set, so no CPU complex check is listed. The
"CPU start-up" reading does not appear either way, as the code stands
([wireless](wireless.md#the-bring-up-sequence)).

**Results.** The header has Report and Check Again. On the left a large
mark and a headline: "Problems found" when a category failed, "Mostly
healthy" when one warned, otherwise "All good", with the number of
categories (not checks) behind it. Partly checked and not checked
categories count for neither, so a one-group custom run that passed reads
"All good"; the line "A custom check: what was not picked shows as not
checked." is the cue. Then whether the report was saved: "The full report
is saved on the SD card, in the backup folder." or "The report could not
be saved to the SD card.", which also appears when the card is fine but
the eMMC, which names the folder, did not come up. On the right the eleven
categories, worst first: "Problem found", "Worth a look", "Partly
checked", "Working normally" (a soft one too), "Not checked". Eight rows
fill the list, the rest scroll, and the focus starts on the first.

**A category.** The header has Details, focused. On the left the mark and
the status, a sentence (the category's own when it passed), then its
readings. On the right one focusable row per check, warnings and failures
first: the check's plain name, its `gui_text.c` sentence and, under a
warning or failure, "Technical: " and the probe's detail (at most 47
characters). The fallbacks and the soft and not-picked wordings are in the
verdict note. A check of a row that ran but whose key the probe never set
is not listed; with nothing listed the column says "Nothing was checked
here on this run."

A reading is looked up in the page text the check captured: inside the
block that starts with its `[section` header, the first line that starts
with its label, the value after the first ": ", either whole, only what is
in parentheses, or up to the first " (". A reading the run did not print
is left out, and so is one that would run past the bottom of the column.
As the code stands, the two readings given no section, "Processor" and
"Revision" (`Chip ID` and `Major.Minor` on the SoC page), never appear: a
reading without a section is matched only above the page's first `[...]`
line, and the SoC page opens with `[SoC]`.

**The technical report.** Report on Results shows every captured page in
capture order (Diagnostics last) as "Technical Report"; Details shows only
the category's pages. The text keeps the probes' colours, one label per 40
lines (or 4 KiB), because LVGL lays out a label's whole text on every
redraw and one label of the full report would crawl. Down and Up scroll
360 px. LVGL's coordinates are 16-bit, so past 28000 px it stops with "The
rest is in the report saved on the SD card." No category's Details
includes Diagnostics, the text verdict and its cross-checks: only Report
shows it, last, so it is the first page that limit cuts, and the saved
file is then the only place to read it
([The verdict](verdict.md#in-the-gui)). A page the run did not produce
reads "(this page did not run)".

**Last Results.** It rebuilds Results from the registry, the run mask and
the capture as they are, which only a check changes. Nothing is kept
across a reboot but the files on the card.

## Custom Check

One checkbox per group: "Identifying the console", "Battery, charging and
power", "Memory and clocks", "Storage and game card reader", "Wi-Fi and
Bluetooth", "Sound chip", "Sound output (plays a short tone)", "Screen",
"Touch, sensors and buttons", "System details". All are ticked to begin
with. Run, in the header and focused first, does nothing with nothing
ticked. The selection is remembered for the session, but only when Run is
pressed: boxes changed and then left with Close come back as they were.

The text launcher's Custom test unticks Sound output with Sound chip and
ticks Sound chip with Sound output; the GUI does not. Sound output picked
alone skips, and Sound reads "Not checked", unless an earlier run left the
codec answering and APE out of reset ([audio](audio.md#verdict-keys)).

The right half, "UART output", picks the debug console's port.

## The debug console

The console starts on the port `hwtest/hwtest.ini` picks
(`dbgcfg.c`, shared with hwtest.bin; see
[the README](../README.md#the-debug-console-hwtesthwtestini)), UART-B when
the file is not there or does not say, and Custom Check can move it, one
port at a time, for the rest of the session. The USB serial port also runs
beside whichever port is picked, from the first lines of `ipl_main`
(`gui_dbg_usb_start()`), so a PC on the USB-C port gets everything,
start-up included, and can send the same commands, with no choice made on
the console. `gui_usb=0` in the file turns it off; it is read only once
the GUI is up, so the start-up lines have gone over USB by then:

| Port | Where |
|---|---|
| UART-B, right rail | the default |
| UART-C, left rail | not offered on a Lite, whose controls are on it |
| UART-A, test pads | the screen gives pad 3 of cluster C as TX, pad 2 RX |
| USB serial | the USB-C port, see [USB serial](#usb-serial) |

The UARTs run at 115200 8N1 with 1.8 V levels. Everything the console
carries follows the port: the check's report as it runs, the GUI's own
lines, and `R`. That is because gui.bin builds `core.c`, `log.c` and the
probes again with `-DHWTEST_GUI`, which turns `hwtest.h`'s `DBG_SEND` and
`DBG_RECV` from UART-B into `gui_dbg.c`'s `dbg_send` and `dbg_recv`; the
same define makes `log.c`'s `_log_emit` also call `gui_log_capture()`
([The page capture](#the-page-capture)).

A UART is brought up (`_uart_up`) as main.c brings UART-B up: 115200 with
no inversion (the Joy-Con driver inverts a rail's TX), RX pulled up because
a bench adapter leaves the line floating between bytes, 2 ms to settle,
then what the line caught drained. UART-A also gets its pinmux and clock.
UART-C gets the left rail's 5 V (`REGULATOR_5V_JC_L`) and its
charge-enable PCC3 driven high, which bridge the rail's pads to the SoC,
then its pinmux and clock; for UART-B main.c did the same. Nothing of it
is undone on moving away: the old UART stays configured, and UART-C's 5 V
and charge-enable stay on. USB stays up beside the new port.

`gui_dbg_init()` reads the file once the rails are set up, after
`jc_init_hw()`. A `console=` value it does not know is reported as "gui:
hwtest/hwtest.ini: console=<value> unknown" and the console stays where it
is; `gui_usb=0` sends "gui: gui_usb=0, USB serial off" and stops USB; a
port that cannot be had (UART-C on a Lite, or a rail a Joy-Con has taken)
gives "gui: <port> is not there, the console stays on <port>". The text
launcher reads the same file in `main.c` (`_console_from_config()`), once
`ui_boot_gui()` has returned, and moves its own console with the same
`dbg_uart_up()`; the GUI's `gui_usb` means nothing to it.

Moving sends "gui: debug console moves to <port>" on the old port and
"=== hwtest GUI === debug console on <port>" on the new one. Either is
dropped when its port is a rail a Joy-Con has taken; both also go out over
USB. A USB start that fails keeps the old port, and the checkboxes follow
the port in use. The note
under them, refreshed every 300 ms, says where the port is and, for USB,
its state: "Waiting for a PC.", "The PC has it: open the port there (COMx
on Windows, /dev/ttyACM0 on Linux).", "Open on the PC." or "The USB
controller did not come up." After a USB start has failed, the note under
any other port ends "USB serial: the USB controller did not come up." A
rail port no Joy-Con has taken carries "A Joy-Con attached there takes
the port over."

### Rails and Joy-Cons

bdk's Joy-Con driver, built without `DEBUG_UART_PORT` (with it the driver
compiles to nothing), takes both rails' UARTs at start-up (`jc_init_hw()`)
and switches on both rails' 5 V and charge-enable
([Joy-Con tests](joycon.md#what-the-tests-change)); on a Lite it brings up
the built-in controls on UART-C instead. With nothing on the right rail
UART-B goes back to being the console. A rail's IsAttached line shares its
contact with that UART's TX, so `_rail_attached()` reads it as bdk's
driver does: TX released to an input, a dummy read that unlatches the
input buffer, then the detect pin (PH6 right, PE6 left), low when a
Joy-Con is there; a bench UART adapter does not pull it low. TX is handed
back after. It runs, never on a Lite:

- on UART-B at start-up;
- on UART-C when Custom Check picks it;
- before every Joy-Con poll, on UART-B unless a Joy-Con has already taken
  it and on UART-C while it is the console, each once that UART's last
  bytes have gone out.

bdk's own detect inside `joycon_poll()` then releases both TX pins again.

Once a Joy-Con has been seen on a rail, that UART is the Joy-Con's for the
rest of the session: nothing is sent on it or read from it, even after the
Joy-Con comes off. The Custom Check note "A Joy-Con is attached there, so
the port is its own: nothing goes out on it." stays after it has gone.
With the default port and a right Joy-Con on, the bench hears nothing more
and `R` over UART-B stops working; UART-A or USB carry everything. On a
Lite UART-C is the controls' from the start, and UART-B keeps main.c's
set-up untouched.

The detection runs only when the Joy-Con is polled, and nothing polls it
during a health check. Going by the code, a Joy-Con slid onto the
console's rail during a run is not noticed until the first poll after it,
and the run's output keeps going out on that rail until then.

### The R reboot

An uppercase `R` reboots. `dbg_recv()` reads the USB serial port, the
chosen port and, whichever that is, UART-B as long as no Joy-Con has taken
it, "so a bench that reboots the console over it keeps working". On every
pass of the main loop `gui_cmd_poll()` (`gui/gui_push.c`) reads it: an `R`
outside a command line reboots, a `!` starts one
([Commands from the PC](#commands-from-the-pc)), anything else is ignored.
During the health check `uart_poll_reboot()` (`core.c`) reads it instead,
before every page and step and in the probes' long waits
(`msleep_poll()`), and takes only `R`, though not inside a `!` command
line, so a PC's `!RUN` sent during a check does not reboot the console
(the command itself is lost); nothing reads it while anything else blocks
the loop, a screenshot included.

`do_reboot()` prints "Rebooting (PMIC power cycle)...", gives the line
50 ms, sets `MAX77620_ONOFFCNFG2_SFT_RST_WK` and writes
`MAX77620_ONOFFCNFG1_SFT_RST`: a full PMIC power cycle through POR, not a
PMC `MAIN_RST`. On a modchipped Mariko `MAIN_RST` is a warm reset into a
patched RCM and leaves the console silent; a POR is what the modchip
catches, so the boot payload is injected again. It is bdk's
`power_set_state(POWER_OFF_REBOOT)` without the `hw_deinit()`, which
stalls on this payload. Home's Reboot is the same call. Neither saves
anything first.

### USB serial

A CDC ACM device (`gui/usbser.c`). Windows 10 and later bind usbser.sys to
it ("USB Serial Device (COMx)"), Linux cdc_acm and macOS its CDC driver, so
nothing is installed. It is 1209:0001, pid.codes' test pair "for a device
that is not released under an ID of its own", with the strings "hwtest"
and "hwtest console" and no serial number: an interface association over
the communications interface, whose interrupt endpoint 0x82 is never sent
on, and the data interface, bulk 0x01 out and 0x81 in (512-byte packets at
high speed, 64 at full speed). `SET_LINE_CODING` is kept and read back
(115200 8N1 until the PC sets another; "the rate only matters to the PC"),
the DTR bit of `SET_CONTROL_LINE_STATE` says a program holds the port,
`SEND_BREAK` is acknowledged and ignored, and anything else stalls.

Output is queued from the moment the port is up and sent once DTR is set:
before a program opens the port, and after it closes it, the queue keeps
the newest 16 KiB, so a PC that opens the port late still reads the
start-up. While a program holds the port and the queue is full, a byte
waits up to 20 ms for the PC. If the PC took nothing in that time, the
waiting stops until it reads again, and the oldest bytes make room as
when no program holds the port: a PC that cannot read while the port
still looks open (the console is the USB host for a dock) cost 20 ms a
byte, and the Dock Test's picture log held the GUI for 21 s. A newline
goes out as CR LF.
Each transfer is kept one byte short of a full packet, so it ends on a
short one and the PC hands it over at once. The queue is cleared when the
port starts and when a PC goes away.

Input is read whenever the port is configured, DTR or not, so an `R` from
a program that does not raise DTR still reboots. It goes through a 4 KiB
queue, and a packet is only taken from the controller when it fits whole:
otherwise it stays there, no read is queued, and the controller holds the
PC off. Nothing is dropped, which a file sent over the port depends on.

Nothing runs the port in the background: it moves on each loop pass and
on every send and read, at most one packet a poll. A send is only queued,
so unlike a UART's a `[step]` line may still be queued when the console
stops, and the last line received need not name the step that hung.

Under the class sits Mariko's XUSB controller (`gui/usbser_xusb.c`) or
Erista's USB2 controller (`gui/usbser_usb2.c`), forks of bdk's `xusbd.c`
and `usbd.c` reduced to this device and changed never to wait for a PC:
`init()` brings the controller up and arms the port, `poll()` handles what
came in. Their controller, PHY and clock sequences are bdk's. Both enable
PLLU when the port comes up and disable it when it stops, so once USB
serial has been picked the PLLU line of the Memory & clocks page reads as
the port left it, not as the console booted. XUSB reports a PC that went
away, after which the port starts over with DTR and the queues cleared.
USB2 never reports one: a PC plugged in again resets the bus and
enumeration starts over from there. Until then, by the code, the port
reads as configured and DTR stays as the last PC left it, so the note can
keep saying "Open on the PC." with the cable out. USB2 also runs its
control stages synchronously, as bdk does ("a PC completes them within
microseconds"), each wait bounded at `USB_XFER_SYNCED_ENUM` tries, about
2 s. XUSB's `init()` cannot fail, so "The USB controller did not come up."
can only appear on Erista.

Before the GUI hands over to another payload (the RAM test, or `!RUN`
below), `gui_dbg_end()` gives the queue up to 300 ms to drain and stops the
controller: it keeps a read queued, which it would go on filling by DMA in
memory the next payload owns.

### Commands from the PC

A line that starts with `!` on the debug console is a command
(`gui/gui_push.c`), so a PC on the USB-C port, or a bench UART, can put a
new build on the SD card and start it with nobody at the console. The
answer is one line, `+...` or `-ERR <reason>`:

| Command | Answer, and what it does |
|---|---|
| `!PING` | `+PONG hwtest GUI <version>, console on <port>` |
| `!PUT <path> <size> <crc32 hex>` | `+READY <size>`, then exactly `<size>` raw bytes; `+OK <path> <size> <crc>` once the file is on the card |
| `!LS <folder>` | `+OK <folder>: <name> <size> ... <sub>/ ...` on one line; only `backup` and `hwtest` and what is under them |
| `!GET <path>` | `+DATA <size> <crc32 hex>`, the file as `=<hex>` lines of 48 bytes, then `+END`; only under `backup/` and `hwtest/`, up to 4 MiB. The lines go over USB alone when it is up, paced by the PC |
| `!RMLOGS` | `+OK removed <n>`: every Dock Test log, `hwtest/tvlink_*.txt`, deleted and nothing else (`-ERR removed <n>, the rest would not go` if some stay) |
| `!RUN` | `+OK starting hwtest.bin, <n> bytes`, then `bootloader/payloads/hwtest.bin` is started from the card, which brings up the `hwtest/gui.bin` now there |
| `!BT OPEN`, `SCAN`, `LIST`, `CONNECT <n>`, `DISCONNECT`, `STATE`, `CLOSE`, `RAILPAIR` | the [Bluetooth test](bluetooth.md#from-a-pc), without the confirmation box |

`!PUT` writes only the four files a release puts on the card
(`hwtest/gui.bin`, `hwtest/libsys_minerva.bso`,
`bootloader/payloads/hwtest.bin`, `bootloader/payloads/hwtest_ram.bin`)
and `hwtest/hwtest.ini`, up to 8 MiB. Nothing is drawn while the bytes come, since at 115200 a
frame's worth would overrun a UART's FIFO; a transfer that stops for 5 s
fails. The CRC-32 (bdk's `crc32_calc`, the same as zlib's) is checked, the
file is written as `<path>.new`, read back and checked again, and only then
renamed over the old one, so a transfer cut short leaves the old file in
place. `!RUN` stages the payload as the RAM test is staged
(`ram_payload_run()`): USB stopped, the boot clocks back, the payload
copied to 0x40010000 from IRAM. Over USB a whole release (about 1.3 MB)
goes over in about a second.

`host_tools/hwtest_push.py` speaks it from the PC, finding the port by its
USB ID: `release` puts the four built files on the card (hwtest.bin last),
runs it, waits for the port to come back and pings the new build; `log`
streams the console and reconnects across restarts; `cmd` sends any
command and can follow the log after it; `console uart-b|uart-c|uart-a
[--gui-usb 0|1]` writes `hwtest/hwtest.ini`; `ls`, `get` and `report`
(the last saved `backup/<eMMC serial>/hwtest.txt`) read the card back.
Only `+...` and `-ERR` lines count as answers, since a report's page
headers start with `---`. Every wait has a deadline as
well as a quiet spell, since the GUI logs its frame times every couple of
seconds while it draws.

## Touchscreen test

A grid of 16 by 9 squares of 80 px over the whole panel, because dead
spots are often along an edge, and in the middle a panel with the
instructions, Finish (focused), a count of the 144 areas, "POWER finishes
too." and, when touch did not start, "The touchscreen did not start." The
screen takes the touch through its sink:

- A stroke that begins on Finish, while the panel shows, is LVGL's tap
  and marks nothing for its whole length.
- Any other stroke hides the panel while the finger is down, so the
  squares under it can be seen turning; it comes back 1 s after the lift.
- Samples arrive at the input rate, so a quick stroke leaves gaps: the
  squares on the straight line from the last sample are marked too, a
  point every 20 px.

The test ends when all 144 are green, or on Finish (a tap, POWER or
Joy-Con A). The squares never touched turn red and a box says "The whole
screen responds.", "No touch was detected." (pointing at the health
check's Touch and sensors results) or "<n> area(s) did not respond.",
with Again and Done. Joy-Con navigation stays on, so Joy-Con B leaves for
Home at once, with no result. Nothing is recorded or saved, and no results
row is fed.

The coordinates are bdk's, already compensated: a 15 px edge offset is
clamped off and the rest scaled to 1280x720, so the outermost squares are
reached a little inside the glass edge. A contact bdk takes for a palm
rest (a `z` reading of 500 or more) arrives as a lift and marks nothing.
With touch not started no sample ever arrives, and only POWER or Joy-Con
A end the test, with "No touch was detected."

It cannot tell a square that was missed from a dead one, and the box says
as much. A quick curved stroke can mark squares on the straight line
between two samples that the finger did not cross. Accuracy and pressure
are not checked, and only the first contact draws: a sample from a second
one takes a screenshot.

## Game Card

Whether a cartridge is in the slot, live: the slot's card-detect line,
read every 100 ms, as the health check reads it (see
[Game card reader](gamecard.md)). The line is wired straight to the slot,
so it reads with the reader unpowered: no rail goes on and nothing is sent
to the reader or to a cartridge. While the screen is open the pad
(`CAM_I2C_SDA`, booted as I2CVI) is a pulled-up input and PS3 a GPIO
input; leaving puts the pad back. A change counts once two readings in a
row agree, as a cartridge going in can make the contact chatter. The
screen shows "A cartridge is in the slot" or "The slot is empty" and how
many times one went in and came out since it opened, and the debug
console gets `gui: gamecard screen, ...` at the start and
`gui: gamecard inserted` or `gui: gamecard taken out` at each change.
The title is not shown: reading anything from a cartridge needs the
reader's firmware, and loading that burns OTP fuses in the reader.

## Screen test

Six pages: White, Red, Green, Blue, Black, then Backlight, white while the
backlight sweeps between 8 and 255 by 6 levels every 30 ms (about 1.2 s
end to end). A caption gives the page and its number; the first page
explains what to look for. A tap, VOL+, VOL- or any Joy-Con button turns
the page, no sooner than 400 ms after the last turn, and a key already
held as the test opens does not count. POWER is left to the box at the
end, "so the press that reaches it is not also the one that opened it".

There is no Close, the touch is the screen's and the Joy-Con does not
navigate, so the test is left only through that box: Again or Done,
picked with VOL and POWER or a tap. The Joy-Con cannot answer it, since
the screen stops reading the Joy-Con once the box is up. The backlight
goes back to 100 (`GUI_BACKLIGHT`, the level main.c set) at the end, on
leaving and on every page but the last: a fixed level, not whatever it
was before. It is set with bdk's `display_backlight_brightness()`, over
DSI on the Samsung OLED panel and by PWM otherwise. The closing box says
what a fault points at: dots that stayed dark or bright, lines or patches
of another shade are the panel; brightness that was uneven or flickered on
the Backlight page is the backlight. Nothing is measured and nothing
recorded: the eyes judge.

## Screenshots

A second finger on the panel takes one, on any screen, at most once every
2 s. Strictly it is a sample from the controller's second contact (bdk's
`finger` is the event's touch id plus one), not a count of fingers. The
frame is read (after a cache invalidate) from `NYX_FB2_ADDRESS`, LVGL's
landscape buffer, so the file is 1280x720 as the screen reads, a 32 bpp
BMP with its rows bottom-up. It is what the GUI drew, not what the panel
shows, so a panel fault never appears in it. The backlight then goes to
255 for 120 ms as a shutter and back to 100. With the card up the file is
saved as `hwtest/screenshots/hwtest_YYYYMMDD_HHMMSS.bmp`, with the RTC's
time, the folders made as needed (Nyx wants three fingers and writes
under `bootloader/screenshots/`).

A notice at the top for 2.5 s says "Screenshot saved as <path>",
"Screenshot not taken: out of memory" or "Screenshot not saved: no SD
card". The last covers a card that is not up (`g_sd_ok`, set at start-up
and again by a check with Storage or the RAM Test's failure path, is
clear), one that would not mount and a file that could not be created.
bdk's `sd_save_to_file()` ignores the write's own result, so a write that
fails part way still says "saved". The notice outlives the 2 s gap, so a
shot taken 2 to 2.5 s after the last one carries its notice. The whole
capture runs inside the touch read, the main loop stalled meanwhile. No
screenshot can be taken while the health check runs, or at all when touch
did not start. Nothing goes to the debug console.

## RAM Test

The RAM test is its own payload, `hwtest_ram.bin`, because it retrains the
DRAM, which nothing running from DRAM, this GUI included, can survive. The
window says that it takes several minutes and that coming back takes a
power-off or a reboot. Start shows "Loading the RAM test..." at once and
calls the text launcher's `ram_payload_load()`; on success it drops to the
payload's clocks, since the RAM test trains from the boot clock, and calls
`ram_payload_run()`, which does not return. Where the file is looked for,
the size limits and the hand-off are in
[The RAM test](ramtest.md#starting-it).

On a failure the loader has powered the card down, so it is initialised
again for later checks, and the window says "The SD card could not be
read.", "hwtest_ram.bin is not on the SD card. Copy it to the root of the
card." or "hwtest_ram.bin on the SD card is damaged. Copy it again."; the
clocks stay up. The USB serial port is stopped before the hand-off
(`gui_dbg_end()`, see [USB serial](#usb-serial)).

## Lockpick_RCM

A shortcut to Lockpick_RCM, which saves the console's keys to the SD card
as `switch/prod.keys`, the file the health check takes the eMMC's BIS key
from (see [Storage](storage.md)). It is looked for in
`bootloader/payloads/`, then at the card's root, under the names the packs
give it: `Lockpick_RCM.bin` (deejay87's Universal Pack and Lockpick_RCM's
releases) and `Lockpick_RCM_Pro.bin` (HATS); FAT matches the names in any
case. Home checks when it is built, so a card changed while hwtest runs
shows on the next return to Home. The tile asks first ("Start
Lockpick_RCM?": hwtest closes, the keys go to `switch/prod.keys`, coming
back takes a restart). Start hands over as the RAM Test does:
`payload_load()` with those paths (the file at least 256 bytes and, padded
to 16, no larger than the 196,352 bytes from `0x40010000` to the
relocator), `gui_dbg_end()`, the clocks back to the boot speed, then
`ram_payload_run()`, which stages it and runs it at `0x40010000` as RCM
runs a payload. `gui: starting <path>, <n> bytes` goes to the debug console
first. On a failure the card is initialised again and a message box says
"The SD card could not be read.", "Lockpick_RCM is not on the SD card any
more." or that it is too large or too small to run.

## Debug console lines

```
=== hwtest GUI === <version>           start-up, on UART-B and USB serial
gui: DRAM <n> MHz (...)                the clocks, see Internals
gui: BPMP boosted
gui: measured BPMP <n> MHz, DRAM <n> MHz
gui: <n> frames, <n> ms avg, <n> ms worst, VIC wait <n> ms/frame
[step] <msg>                           a step inside a probe
=== hwtest full UART dump ===          the check, page by page
=== end of dump ===
[save] ...                             the report file
gui: report: <n> pages, <n> labels, <n> px, built in <n> ms
gui: debug console moves to <port>
=== hwtest GUI === debug console on <port>
gui: push wrote <path>, <n> bytes      a !PUT, see Commands from the PC
gui: gamecard screen, ...              the Game Card screen opening, see Game Card
gui: gamecard inserted / taken out     a change on that screen
gui: starting <path>, <n> bytes        Lockpick_RCM handed over, see Lockpick_RCM
gui: bt <step>                         the Bluetooth test, see bluetooth.md
gui: bt> <what> <code> len <n>: ...    its trace
Rebooting (PMIC power cycle)...
```

The report line comes each time a technical report is built, so a console
that stops there shows how far it got. The Joy-Con and Dock Test lines are
in their own docs.
