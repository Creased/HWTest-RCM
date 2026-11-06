# Inputs

Four probes share the `Inputs` page, in this order: the touch panel, the
ambient light sensor, the Joy-Con rail detect lines, and a census of the
buttons and the AC adapter. In `hwtest.bin` each probe gets its own LCD
page; the GUI merges pages that share a name, so all four land in one
"Inputs" report page. The text launcher's custom selection calls the group
"Touch / inputs", the GUI "Touch, sensors and buttons".

Only the touch controller and the light sensor are judged. The rail lines
and the census are printed for the technician to read and set no verdict
key.

## Touch panel

Section `[Touch panel (FTS4, I2C3 @ 0x49)]`. The controller is an
STMicroelectronics FingerTip S part (bdk's driver calls it FTS4) on I2C3 at
`0x49`. It is held in reset by PJ7 and powered from `LDO6`, a rail it
shares with the light sensor.

### Powering it without the calibration shortcut

bdk's `touch_power_on()` is deliberately not used. Besides powering the
controller, it carries hekate's maintenance shortcut: if VOL+ and VOL- are
both held at that moment (`btn_read_vol()`), it runs the ITO self-test
(below) and, when that comes back clean, `touch_execute_autotune()`.
Autotune resets the controller, trims its low-power oscillator, runs the
mutual-sense and self-sense compensation tuning, then sends
`FTS4_CMD_SAVE_CX_TUNING` (`0xFC`), which saves the new compensation data
to the controller's EEPROM, before re-enabling sensing.

In plain words: holding both volume keys at the wrong moment would
recalibrate the touchscreen and store that calibration permanently in the
controller, over whatever was stored before. A repair sweep must not write
to the part it is diagnosing because of which keys happen to be held, and
the GUI powers the panel the same way: once at start-up and again after
every run. Nyx offers the same calibration as a deliberate tool, behind a
warning, a POWER confirmation and a five-second countdown during which the
screen must not be touched. hwtest has no such tool, and nothing in it
calls `touch_power_on()` or `touch_execute_autotune()`.

`touch_power_on_safe()` keeps only the power, reset and sense sequence:

1. The touch reset pin is set up: its pad, `DAP4_SCLK`, pinmuxed with a
   pull-down (function 1), and PJ7 made an output and driven low; 20 us.
2. `LDO6` set to 2.9 V and enabled.
3. I2C3 pinmuxed, clocked and initialised; 1 ms.
4. PJ7 driven high to release the reset; 10 ms.
5. `touch_sys_reset()`: up to three attempts, each writing `0x80` to HW
   register `0x0028`, waiting 10 ms, then up to 20 ms for the
   `FTS4_EV_CONTROLLER_READY` event.
6. If the reset succeeded, `touch_sense_enable()`: finger sense mode,
   `FTS4_CMD_MS_MT_SENSE_ON`, `FTS4_CMD_CLEAR_EVENT_STACK`. Any failed I2C
   write fails the power-on.

`touch_sys_reset()` is global in bdk's `touch.c` but missing from
`touch.h`, so hwtest declares it locally. Two other parts of bdk's sequence
are left out: the wait for the ready event straight after the reset is
released, and `touch_init()` with its chip-ID precheck and three retries.
Without the precheck a controller that is not an FTS4 can still power on
"OK"; its chip ID is judged on the next line instead.

If power-on fails, the probe prints
`power-on : FAILED (panel cable / IC fault?)`, sets `touch_id` FAIL
("safe touch init failed") and `touch_ito` WARN ("not run because touch
init failed"), powers the controller off and stops there. A failed
power-on means the controller did not take the reset, report ready or
accept a sense command over I2C3: the panel cable, the controller or
`LDO6`. The light sensor shares `LDO6` but not the bus, so an `als_id`
FAIL on the same run (not on a Lite) points at the rail rather than the
panel.

### Identity and firmware

`touch_get_chip_info()` reads HW register `0x0004` and prints `chip_id`,
`fw_ver`, `config_id` and `config_ver`. If that read fails, all of them
read 0. The `clone` line comes from a second read of the same block
starting two bytes earlier (`0x0002`): bdk expects the chip ID to reappear
two bytes further in, and flags the part as a clone when it does not.
`touch_id` passes when `chip_id` is `0x3670` (`FTS4_I2C_CHIP_ID`) and fails
otherwise ("chip_id 0x%04X != FTS4"). A clone that reports the genuine chip
ID still passes; the flag only colours `clone : yes` and skips the ITO
test. hekate reads a clone flag on a genuine chip ID as the mark of the
7-inch LCD OEM clone screen fitted to a Switch OLED, and names its touch
panel "OEM Clone TSP".

`touch_get_panel_vendor()` sends `FTS4_CMD_VENDOR` with
`FTS4_VENDOR_GPIO_STATE`, waits up to 2 s for the answer and matches the
three GPIO state bytes against bdk's table:

| idx | Panel |
|---|---|
| 0 | NISSHA NFT-K12D |
| 1 | GiS GGM6 B2X |
| 2 | NISSHA NBF-K9A |
| 3 | GiS 5.5" |
| 4 | Samsung TSP |
| -1 | GiS VA 6.2" |

It prints `vendor : <name> (idx n)`. `vendor : <error>` means the command
or its answer failed. For a GPIO combination missing from the table bdk
returns idx -2 and leaves the name pointer unset (NULL), which hwtest
passes to `%s` unchecked: the line shows `(idx -2)` after an undefined
name, and without the three GPIO bytes hekate prints, so the report
cannot identify such a panel.

`touch_get_fw_info()` takes the address of the firmware information from
FB register `0x0060`, reads nine bytes there and prints `fw_id`, `ftb_ver`
and `fw_rev`; `fw_rev` is shown raw and byte-swapped ("display"), the order
hekate prints it in. If either read fails the probe prints `fw read FAILED`
and no pairing line.

`pairing` looks `fw_id` up in a table based on hekate's touch information
screen, which names each firmware family (`4CD60D/0` to `4CD60D/5`) and the
panel it belongs with:

| `fw_id` | Panel |
|---|---|
| `00.10.01.00` | -1, GiS VA 6.2" |
| `00.10.02.00`, `00.12.01.00`, `32.00.00.01` | 0, NISSHA NFT-K12D |
| `00.1A.03.00`, `32.00.01.02` | 1, GiS GGM6 B2X |
| `00.29.01.00`, `32.00.03.02` | 2, NISSHA NBF-K9A |
| `31.05.18.20`, `32.00.04.02` | 3, GiS 5.5" |
| `32.00.05.01`, `33.00.05.02`, `33.00.05.03`, `33.00.05.10` | 4, Samsung TSP |
| `FF.FF.FF.FF` | -1, GiS VA 6.2" |

`pairing : OK` when the panel the firmware belongs with is the panel the
vendor query found. `MISMATCH` covers four cases: the firmware belongs
with a different panel, the `fw_id` is not in the table, the vendor query
failed, or the panel's GPIO combination is not in bdk's table (idx -2).
hekate shows a similar comparison as "Paired" or "Error", but it replaces
the `fw_id` of a clone (genuine chip ID, clone flag set) with
`FF.FF.FF.FF` and always reports it as paired, which hwtest does not. It
is not judged: no verdict key depends on it.

### The ITO self-test

The FTS4's ITO production test (`FTS4_CMD_ITO_CHECK`, `0xA7`) is the
controller's own electrical check of the panel's sensor lines: it looks
for force and sense lines that are open or shorted and reports an error
class plus the affected line. That is exactly the test a repair tool
wants, and unlike autotune it saves nothing: the controller is reset
before and after it, and no compensation data is written.

As general background (the code does not spell this out): ITO is indium
tin oxide, the transparent conductor the sensor grid is made of. The panel
senses by mutual capacitance, the "mutual sense" of bdk's tuning commands:
force lines are the ones the controller drives, sense lines the ones it
listens on, and a finger shows up as a change in coupling where the two
cross. An open is a line broken somewhere along its length; a short
connects a line to something it must not touch.

The test runs only on a genuine FTS4. If `chip_id` is not `0x3670` or the
clone flag is set, it prints `ITO test : skipped (unsupported/clone
controller)` and sets `touch_ito` WARN ("unsupported or clone controller").
Otherwise `touch_panel_ito_test()`:

1. checks the chip ID and clone flag again;
2. resets the controller (`touch_sys_reset()`);
3. sends `FTS4_CMD_ITO_CHECK`;
4. waits up to 2 s for event `FTS4_EV_ERROR` (`0x0F`) with status
   `FTS4_EV_ERROR_ITO_TEST` (`0x05`), whose first two payload bytes are the
   error class and the line number;
5. resets the controller again. Sensing is not re-enabled; the probe powers
   the controller off next.

| Printed | `touch_ito` | When |
|---|---|---|
| `PASS, no opens/shorts` | PASS | error class and line both 0 |
| `FAIL err EE pin N (label)` | FAIL, "err EE pin N label" | any other answer, even class 00 with a nonzero line |
| `no result within timeout` | WARN, "ITO command timed out" | any failure in steps 1 to 4, not only the 2 s wait running out |

Class 00 with a nonzero line prints `FAIL err 00 pin N (no error)`: the
controller named a line but no class, and hwtest treats that as a
failure.

`pin` is the line number the controller reports; hekate's tool calls it
the channel. The labels are hwtest's names for bdk's `touch_ito_error`
values. The code gives only the enum names, so the last column reads each
name and goes no further.

| err | Label | Reading of the name |
|---|---|---|
| `00` | no error | |
| `01` | force open | a force line is open |
| `02` | sense open | a sense line is open |
| `03` | force-GND short | a force line is shorted to ground |
| `04` | sense-GND short | a sense line is shorted to ground |
| `05` | force-VCM short | a force line is shorted to VCM; hekate's tool prints "Force Short to VDD" |
| `06` | sense-VCM short | the same for a sense line |
| `07` | force-force short | a force line is shorted to another force line |
| `08` | sense-sense short | a sense line is shorted to another sense line |
| `09` | force-sense short | a force line is shorted to a sense line (`ITO_F2E_SENSE`) |
| `0A` | FPC force open | a force line is open at the FPC (`ITO_FPC_FORCE_OPEN`) |
| `0B` | FPC sense open | the same for a sense line |
| `0C` | key force open | a key force line is open (`ITO_KEY_FORCE_OPEN`) |
| `0D` | key sense open | a key sense line is open |
| `0E`-`10` | reserved | |
| `FF` | max errors | `ITO_MAX_ERR_REACHED` |
| other | unknown | not one of bdk's values |

A FAIL points at the panel's own lines or, for the FPC classes, at the
FPC; a dead supply or bus would have failed power-on or the chip ID before
the test ran. The test names the line, not the area; the GUI's
[Touchscreen test](gui.md#touchscreen-test), a grid that turns green under
a finger, shows which part of the screen is dead. No touch is ever read,
so passing `touch_id` and `touch_ito` does not show that touches register,
and the stored compensation data that autotune would rewrite is not
checked.

### What it leaves behind

The probe ends with `touch_power_off()`: PJ7 low (controller held in
reset), `LDO6` off, I2C3 clock disabled. The `DAP4_SCLK` pinmux, PJ7 as an
output and the I2C3 pinmux stay as set, and `LDO6` is left off and
programmed to 2.9 V whatever its state was before (the light-sensor probe
leaves it the same way). Nothing is written to the controller's EEPROM at
any point: the probe sends resets, sense-mode commands, register reads,
the vendor query and the ITO check, nothing else.

In `hwtest.bin` nothing powers touch again; its launcher works with the
physical keys. In the GUI, touch polling is held during a run and
`gui_input_resume()` calls `touch_power_on_safe()` again afterwards. If
that fails the GUI's pointer reads as released, and the Touchscreen Test
knows it (`gui_touch_ok()`).

## Ambient light sensor

Section `[BH1730 ambient light sensor, I2C2 @ 0x29]`. The Rohm BH1730 sits
on the front bezel near the speaker grille and drives HOS's
auto-brightness. It is worth checking because a failed sensor leaves the
screen stuck at minimum or maximum brightness whatever the light, which is
easily mistaken for a backlight or panel fault.

The Switch Lite (`FUSE_NX_HW_TYPE_HOAG`) has no sensor. There the probe
prints `N/A : Switch Lite has no ALS (auto-brightness omitted)` and sets
`als_id` PASS ("N/A on Lite") without touching the hardware; reading the
bus would get a NACK, read as `0x00`, and fail the verdict for a part that
was never fitted. The comment calls this a neutral verdict, but it is a
PASS: it counts in the pass total, and its detail is never shown, because
the verdict and the GUI show a reason only for a WARN or FAIL.

On every other model `als_power_on()`:

1. sets `LDO6` to 2.9 V and enables it (the touch probe has just turned it
   off);
2. pinmuxes, clocks and initialises I2C2;
3. reads the ID register `0x12`;
4. soft-resets the sensor (`BH1730_SPECCMD_RESET`);
5. sets gain 64x and an integration of 38 cycles (the timing register is
   written as 256 - 38);
6. writes `BH1730_CTL_POWER_ON | BH1730_CTL_ADC_EN` to the control
   register, starting continuous conversion.

`als_id` passes when the ID's high nibble is 7 (a BH1730 returns `0x71`,
part 7, revision 1) and fails otherwise ("ID 0x%02X != BH1730"). The
printed line is `ID : 0xNN (BH1730 verified)` when it passes and
`ID : 0xNN (UNEXPECTED, no chip / I2C fault?)` when it fails. A failed
I2C read returns 0, so a missing sensor, a dead one and a broken bus all
read `ID : 0x00`.

With a good ID the probe prints `Gain / cycle` (expect `64x / 38`), waits
110 ms with `msleep_poll()` so that one integration window (about 103 ms,
38 x 2.7 ms) has completed, and the UART `R` reboot still works meanwhile.
Then it prints the raw `Visible` and `IR` counts and the `Lux` figure from
bdk's `get_als_lux()`:

* `(saturated)` is appended to the `Visible` line when either channel
  reads 65535, even if only the IR channel saturated.
* `Lux` is 0 when the visible count is 0, and also when the IR to visible
  ratio is 1.879 or more, because no calibration row matches. It is capped
  at 100000.
* bdk always uses the Icosa/Iowa optical-window table and multiplier. The
  Aula ones are defined but never selected, so on an OLED console the lux
  figure is computed with the other models' coefficients.

None of these readings is judged: only the ID sets a key. One sample with
no dark and bright comparison cannot show that the sensor responds to
light, so a sensor that answers its ID but is blind still passes.

Afterwards the probe writes 0 to the control register, stopping
conversion, and turns `LDO6` off. bdk has no `als_power_off()`, and this
probe used to leave the sensor converting on a live rail. The I2C2 clock
and pinmux stay enabled.

## Joy-Con rails

Section `[Joy-Con rail attachment]`. In `hwtest.bin` UART_B is the 115200
diagnostic console, so this probe does not speak the right Joy-Con's
serial protocol (in the GUI, bdk's Joy-Con driver does: see
[Rails and Joy-Cons](gui.md#rails-and-joy-cons)). What it does read are
the two IsAttached lines, PH6 (right) and PE6 (left). Both are active low:
a Joy-Con, or a chassis adapter that shorts the detect line, pulls the
line to ground. `hw_init()` sets both pins up as inputs
(`PINMUX_INPUT_ENABLE | PINMUX_TRISTATE`). hekate's
`_jc_rail_detect()` first turns the shared UART TX pins into inputs and
does a dummy read of both lines (bdk's comment: "HW BUG: Unlatch gpio
buffer"); this probe skips that and samples each line once. bdk's comment
says no more about the bug, and nothing in hwtest records whether skipping
the dummy read changes the result.

It prints `JC-R rail` and `JC-L rail` as `ATTACHED` or `empty`, then
`serial poll : not used; UART_B stays on hwtest debug`, which only says
that this probe leaves the serial link alone: in a GUI run the console may
be on another port and UART_B may belong to a Joy-Con. The probe's comment
says a chassis adapter on the right rail reads `JC-R rail : ATTACHED`,
confirming that the rail's spring contacts are seating; the GUI's rail
check (`gui/gui_dbg.c`) records that a bench UART adapter does not pull
the line low ([gui](gui.md#rails-and-joy-cons)). The probe changes
nothing and sets no key.

Only the detect line is read, so the probe cannot tell a Joy-Con from a
chassis adapter or from a detect line shorted to ground, and it sees
nothing of a Joy-Con's buttons or sticks; the GUI's
[Joy-Con tests](joycon.md) cover those.

## Input census

Section `[Input census]`. The buttons and the AC adapter on one page, so
each switch and contact can be checked in one place. All of them are
plain GPIO or I2C reads, sampled once with no debounce and no wait: a key
reads `PRESSED` only if it is held at that instant, and a switch that
never closes looks the same as one that is not being pressed. On a healthy
console with nothing held every key reads `released`. A `PRESSED` line
(drawn in the warning colour, though it sets no key) while nobody holds
that key points at a stuck switch or a line held at its active level.

In `hwtest.bin`'s pager the probe runs again each time its page is shown,
and the keys stay bound: VOL+ and VOL- turn pages (and win over a UART `r`
sent while one is held), and POWER powers the console off once released.
So a key shows `PRESSED` there only if it is held as the page is drawn,
typically the VOL key used to reach it, and POWER cannot usefully be
tested on the pager. In the GUI the keys are ignored while a check runs
(`gui_input_hold()`), so a key held as the Inputs step runs is sampled
without the GUI acting on it.

| Line | Read from | Notes |
|---|---|---|
| `POWER btn` | MAX77620 `ONOFFSTAT` (`0x15`) bit `EN0`, over I2C5, active high | Erista also routes POWER to a GPIO, but the resistor is missing on retail boards |
| `AC adapter` | `ONOFFSTAT` bit 1 (`ACOK`) | `present` or `absent` |
| `ONOFFSTAT` | the raw byte | bit 0 (`LID`) is not decoded |
| `VOL+ btn` | PX6, active low | read on its own: `btn_read()` also folds in POWER |
| `VOL- btn` | PX7, active low | |
| `HOME btn` | PY1, active low | physical only on the Lite; elsewhere `N/A (no physical switch on this SKU)` with the raw level, which idles high or floats |

The AC line is information only here. The same `ACOK` bit is judged by
the Verdict's `xc_vbus_acok`
[cross-check](verdict.md#a-vbus-acok-and-usb-pd), which
compares it with the BM92T36 and BQ24193 views of VBUS and counts on the
`Charger` row (the GUI's "USB-C power detection"), not on `Inputs`.

A failed I2C read returns 0, so if the PMIC does not answer the POWER key
reads `released`, the adapter `absent` and the registers `0x00`, with no
error printed.

### Power-key configuration

`ONOFFCNFG1` (`0x41`) is printed with its MRT (manual reset time) code and
a decode of bit 7, `SFT_RST`. The code is printed raw, not in seconds: the
code-to-seconds table is only in the datasheet, and bdk's one data point
(`hw_init()` writes code 3 "for forced shutdown off to 6s") contradicts the
linear 2 s per step decode an earlier version printed. HOS and hekate both
program code 3, so code 3 is marked `= stock`. The `long-press = SFT
shutdown` / `long-press = hard reset` wording for bit 7 is the probe's own
reading; hwtest's reboot (`do_reboot()`) writes that same bit to start its
PMIC power cycle. Note that `hwtest.bin`'s `hw_init()` writes this register
(reserved bit plus MRT code 3) at every boot, before the GUI too, so the
line shows bdk's own setting (`MRT code 3 = stock`,
`long-press = hard reset`), not what HOS or the board had before.

`ONOFFCNFG2` (`0x42`) lists the wake sources that are enabled: `POWER`
(`WK_EN0`), `ACOK`, `MBATT`, `ALARM1`, `ALARM2`, `WDOG` (`WD_RST_WK`) and
`SFT-RST` (`SFT_RST_WK`); bit 5 (`SLP_LPM_MSK`) is not shown. `hw_init()`
does not write this register. If the POWER button does nothing, check
that `POWER` is in the list. A console that wakes by itself usually has an
unwanted wake source set, `WK_MBATT` on a deteriorating battery being the
classic. `SFT_RST_WK` is a bit software sets on purpose: hwtest's `R`
reboot sets it before power-cycling the PMIC, and bdk's reboot and
power-off paths set or clear it.

## Verdict and GUI rows

The Verdict's `Inputs` row aggregates three keys and is printed whenever
the Inputs group is selected. All three are set every time the group
runs. The worst severity wins; the reason shown is the first FAIL's detail
in key order, else the first WARN's, for example
`Inputs : FAIL (safe touch init failed)` (see [verdict](verdict.md)).

| Key | PASS | WARN | FAIL | GUI row |
|---|---|---|---|---|
| `touch_id` | chip ID `0x3670` | never | power-on failed, or another chip ID | Touchscreen chip |
| `touch_ito` | no open or short | not run (power-on failed), skipped (wrong or clone chip), no result | an open, a short or another error class | Touch panel self-test |
| `als_id` | ID `0x7x`, or a Lite | never | any other ID | Light sensor |

In the GUI these rows make up the "Touch and sensors" category. Its
all-clear text, "The touchscreen and light sensor answer.", also appears
on a Lite, where `als_id` passes without a sensor. `touch_ito` has the
soft prefix "not run": the WARN set when power-on failed is shown as "Not
checked on this console." with no technical line and is not counted as a
warning in the GUI, although the text verdict counts it as a warn. The
other two `touch_ito` WARNs show the GUI's warning text with the detail
under it. A `touch_ito` FAIL reads "The touch panel has a broken line."
whatever the class, shorts included; the detail under it
(`err EE pin N label`) gives the class.

The category screen also shows readings taken from the report by section
header and label, so the `[Touch panel`, `[BH1730` and `[Joy-Con rail`
headers and these labels must not change:

| Reading | Report line |
|---|---|
| Touch panel | `vendor`, without the ` (idx n)`; `<error>` when the query failed, absent when power-on failed |
| Touch firmware | `fw_id`; absent when power-on or the firmware read failed |
| Touch self-test | `ITO test`; absent when power-on failed |
| Light (lux) | `Lux`; absent on a Lite or with a bad ID |
| Left rail, Right rail | `JC-L rail`, `JC-R rail`: `ATTACHED` or `empty` |

The census lines feed no reading; they appear only on the technical
`Inputs` page. The "Power system" category's "Touch supply" reading is
`LDO6` from the regulators page, which runs before this one, so it shows
the rail as it was before the touch and light-sensor probes ran.
