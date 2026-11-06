# Raw state

The last data page of the sweep: four probes from `probe_lowlevel.c`, run
in this order (`core.c`): `probe_gpio_census`, `probe_uart_b`,
`probe_reset` and `probe_pmc_scratch`, all in the `PROBE_G_RAW` group. They
dump state rather than exercise hardware. Because the page runs after every
other data probe, the census and the UART readings show the pins as the
earlier probes left them, not as they were at boot. In the GUI they also
show its own start-up: `jc_init_hw()` re-muxes both Joy-Con rails, then
`_uart_up` sets UART-B up again for the console when the right rail is
empty, and with a right Joy-Con attached UART-B stays the Joy-Con
driver's. The PMIC latches are the exception: they are captured before
the sweep starts (see [The snapshot](#the-snapshot)).

Only `probe_reset` sets verdict keys, three of them: `pmic_nverc`,
`pmic_intlbt` and `pmic_irqsd`. Everything else on the page is text for a
technician. In the GUI the progress screen calls this step "System
details"; the page text is reached from the Power system category's
Details button (that category shows the "Power & charging" and "Raw state"
pages) and from the Report button on the results screen. A custom run
picks the group as "Raw state" in the text launcher's custom test and as
"System details" in the GUI's Custom Check.

## Pin census

`probe_gpio_census`, under the header `[GPIO pin census]`, prints the
pinmux and GPIO state of 17 pins involved in the 5 V regulator, the
display, the two Joy-Con rails, the debug UART and the front-panel
buttons, in one place, for repair triage. A bent pin or a solder fault
shows up as a function selector or GPIO mode that does not match the role
the pin is meant to play. It only reads registers.

### How a pin is read

The GPIO block keeps four bytes per port: the port's bank starts at
`(port >> 2) << 8` and its slot within the bank at `(port & 3) << 2`, with
`CNF` at +0x00, `OE` at +0x10, `OUT` at +0x20 and `IN` at +0x30. The pin is
in GPIO mode when its `CNF` bit is set and in SPIO mode (owned by its
alternate-function controller) when it is clear.

Where the row names a pinmux register, four fields are decoded from it: the
function selector (bits 1:0), the pull (bits 3:2), tristate (bit 4) and
input enable (bit 6). The raw pinmux word is not printed; the decoded
fields carry the same information. The two row forms:

```
  <pin> : fn=N <pull> tri|drv ie=N GPIO|SPIO OE=N OUT=N IN=N  (<role>)
  <pin> : pmx=(n/a)              GPIO|SPIO OE=N OUT=N IN=N  (<role>)
```

`<pull>` is `pull-up`, `pull-dn` or `no-pull` (the reserved pull value also
prints `no-pull`); `tri` means the tristate bit is set, `drv` that it is
clear. A compile-time assert checks a template row against the 80-column
LCD (`LCD_COLS`).

| Pin | Pinmux read | Role printed | Note in the code |
|---|---|---|---|
| PA5 | `PINMUX_AUX_SATA_LED_ACTIVE` | 5V regulator EN | FAN5333 enable |
| PI0 | none | LCD AVDD CH2 EN | +5.4 V |
| PI1 | none | LCD AVDD CH1 EN | -5.4 V |
| PV0 | `PINMUX_AUX_LCD_BL_PWM` | LCD BL PWM | PWM0 channel |
| PV1 | `PINMUX_AUX_LCD_BL_EN` | LCD BL EN | |
| PV2 | `PINMUX_AUX_LCD_RST` | LCD reset (act-low) | |
| PK3 | `PINMUX_AUX_GPIO_PK3` | JC-R charge EN | gates the buffer between Tegra and the rail |
| PG0 | `PINMUX_AUX_UART2_TX` | UART2_TX (JC-R) | |
| PG1 | `PINMUX_AUX_UARTX_RX(UART_B)` | UART2_RX (JC-R) | |
| PH6 | `PINMUX_AUX_GPIO_PH6` | JC-R attach detect | |
| PCC3 | none | JC-L charge EN | |
| PD1 | `PINMUX_AUX_UART3_TX` | UART3_TX (JC-L) | |
| PD0 | `PINMUX_AUX_UARTX_RX(UART_C)` | UART3_RX (JC-L) | |
| PE6 | `PINMUX_AUX_GPIO_PE6` | JC-L attach detect | |
| PX6 | none | VOL+ btn (act-low) | |
| PX7 | none | VOL- btn (act-low) | |
| PY1 | none | HOME btn (Lite SKU) | |

### Reading it

- **SPIO rows.** On a pad whose job is an alternate function (UART, PWM),
  SPIO is the correct state: the controller drives the pin and the GPIO
  `OE`, `OUT` and `IN` bits are irrelevant. Ignore them on those rows.
- **`IN=0` on a pin driven HIGH.** PA5 and PK3 are GPIO outputs whose
  pinmux leaves input enable off (bdk's 5 V regulator code writes 1 to
  `PINMUX_AUX_SATA_LED_ACTIVE`; `main.c` writes `PINMUX_DRIVE_4X |
  PINMUX_PULL_DOWN | 2` to `PINMUX_AUX_GPIO_PK3`). With the input buffer
  disabled, `IN` reads 0 even while the pin is driven, so `ie=0 ... OUT=1
  IN=0` is normal on these two.
- **`pmx=(n/a)`** means the census reads no pinmux register for that row.
  It is not proof that the pad has none: bdk configures PY1 through
  `PINMUX_AUX_BUTTON_HOME`, for instance.
- **Act-low and attach-detect rows.** An `(act-low)` role is asserted
  when LOW: `IN=0` on VOL+ or VOL- means the button is pressed, and PV2
  at 0 holds the panel in reset. The attach-detect lines (PH6, PE6) read
  low when a Joy-Con is on the rail; a bench UART adapter does not pull
  them low. Each shares its contact with its rail's TX, which bdk's
  Joy-Con driver and the GUI release before reading it
  (`gui/gui_dbg.c`); the census reads it with TX left as it is, so its
  `IN` bit is weaker evidence than a reading taken that way. On a Lite
  UART-C (PD1, PD0) carries the built-in controls, not a left Joy-Con.

There is no verdict. The probe sets no `dx_set` key and prints every pin
row in the default colour, and the host viewer classifies census rows as
information (`host_tools/test_parser.py`). The code holds no expected value
per pin, so judging a row is left to the reader, against its role. The
`IN` bit is the only view of a pad's level, and only where input enable is
on.

## UART debug port

The Joy-Con right rail's chassis-side debug header carries `UART_B` at
115200 8N1 with 1.8 V logic, and hwtest writes its log out of it.
`probe_uart_b`, under `[UART_B debug port]`, reports the receive side, so
a break in the adapter wiring can be localised without a scope or a
meter. The receive path:

```
host USB-UART -> chassis cable -> chassis-side level shifter
    (powered from the JC-R 5 V rail, enabled via PA5)
  -> chassis connector -> Tegra-side buffer (gated by PK3 high)
  -> pad PG1 (UART2_RX) -> UART2 RX FIFO -> UART_LSR.RDR=1
```

Break any link and RX stops while TX keeps working, because the return
path is asymmetric: a console whose log reaches the host can still be deaf
to it. For the adapter itself see the [README](../README.md).

`hw_init()` only sets up TX (PG0). `main.c` does the receive side before
anything else runs:

1. PG1 to SPIO, so the UART2 controller owns the pin rather than the GPIO
   block.
2. PG1's pinmux set to `PINMUX_INPUT_ENABLE | PINMUX_PULL_UP`, replacing
   hekate's `INPUT_ENABLE | TRISTATE`. A Joy-Con drives the line all the
   time, but a passive USB-UART adapter only drives it while transmitting
   and lets it float between bytes; switch-coreboot pulls up for the same
   reason.
3. `regulator_5v_enable(REGULATOR_5V_JC_R)`, which powers the rail's level
   shifter.
4. PK3 driven HIGH: it gates the MOSFET that bridges the Tegra-side
   signals to the rail-side pads.

It then waits 2 ms and drains whatever the RX FIFO caught while the
pinmux changed (`main.c` expects the pull-up's LOW-to-HIGH edge to decode
as a spurious `0x00`). When the GUI takes UART-B back as its console,
`_uart_up` in `gui/gui_dbg.c` repeats the pin, pinmux and controller part.
The probe checks what these steps left behind.

### Sequence

1. **PG1 pinmux.** Prints `UART2_RX pmx : 0x%08X` and a `pad config` line:
   the function (`(UART)` when 0, else `(WRONG)`), `tristate` or
   `drive-allowed`, the pull (`pull-up`, `pull-down`, `no-pull` or `?`),
   and `input-enable` or `INPUT-DISABLED`. Green when the function is 0
   and input enable is set, red otherwise. The idle line needs something
   holding it HIGH, so an `idle anchor` line follows:
   - pull-up: green, `internal PULL_UP (line idles HIGH on its own)`, the
     state `main.c` sets;
   - tristate with no pull: yellow, `tristate only (line floats unless
     driven)` with `OK with Joy-Cons, fails on passive USB-UART`. This is
     hekate's default, which relies on the far end driving the line;
   - pull-down: red, the UART would see a permanent BREAK;
   - any other combination prints no anchor line.
2. **PA5 and PK3.** Judged from `CNF`, `OE` and `OUT` read directly, not
   through `gpio_read()`, because the input buffer is off on both and `IN`
   reads 0 while they are driven (see the census). Each prints green `HIGH
   (driven)` when all three bits are set, red `NOT driven HIGH` otherwise,
   with the four bits alongside. The software-side 5 V regulator state per
   device is on the `[5V regulator]` section of the Power & charging page
   ([power](power.md#5-v-regulator-probe_5v)).
3. **Controller registers** at the `UART_B` base `0x70006040`: `UART_LCR`
   with its DLAB bit and word length (`(LCR & 3) + 5`), and `UART_MCR`
   raw.
4. **Baud rate.** Sets `LCR.DLAB`, reads the `DLL`/`DLM` divisor latches
   and writes the saved `LCR` back. A divisor of 0 means the UART was never
   initialised. The input clock comes from `CLK_SOURCE_UARTB` (CLOCK
   `0x17C`): bits 31:30 select the source (0 = `PLLP_OUT0`, taken as
   408 MHz; 3 = `CLK_M`, the oscillator; 1 and 2 are PLLC paths hekate
   does not use for a UART, printed `PLLC?` with 0 kHz). Bit 24 enables the
   source divider, and then input = source * 2 / (N + 2) with N in bits
   7:0; with bit 24 clear the input is the source itself. The
   `CLK_SOURCE` line prints the raw register with `src=` (the source
   name), `src-div=ON` or `src-div=bypassed` (bit 24) and the input clock
   in `kHz in`. Baud = input / (16 * divisor). The `Baud divisor` line is
   green when the divisor is non-zero and the result is within 95% to
   105% of `DEBUG_UART_BAUDRATE` (115200), that is 109440 to 120960, and
   yellow otherwise. For 115200
   bdk's `uart_init()` leaves bit 24 clear with a divisor of 221, which
   prints 115384 baud.
5. **Line status.** `UART_LSR` is decoded into its flags (`FIFOE TMTY THRE
   BRK FERR PERR OVRF RDR`, bits 7 to 0). If `BRK`, `FERR` or `PERR` is
   latched, a red `framing/parity/break errors latched - check baud` line
   follows; an overrun alone is not flagged. `VENDOR_STAT` shows the RX
   FIFO fill, bits 13:8 of `VENDOR_STATUS`.
6. **Raw PG1 sample.** Under its own `[raw PG1 sample (10 ms via GPIO)]`
   header, PG1 is switched to GPIO mode and read in a busy loop for 10 ms
   of the microsecond timer, counting high samples, low samples and edges;
   then the pin goes back to SPIO. The sample count depends on loop speed,
   not on a fixed rate. The counts are printed as
   `samples : N high, N low, N edges`, then the verdict line:
   - edges seen: green, `line ACTIVE (N transitions) - the host is
     sending`. If the line moves but no bytes land in the FIFO, check baud
     and framing;
   - every sample high: green, `line idle HIGH - alive and quiet, ready to
     receive`;
   - every sample low: uncoloured, `line LOW - nothing is driving it`,
     then a second line naming the likely causes and ending `Not a
     problem.`

   With no edges every sample has the same level, so there is no fourth
   case.

### Why a LOW line is not a fault

UART idles HIGH (mark), so a line that samples HIGH has something holding
it up, and a line with edges has an adapter sending. The code reads an
all-LOW sample as nothing driving the line: either there is no adapter on
the rail, which is the usual case since most consoles have none, or there
is one with no power on its console side. Neither says anything about the
console itself; steps 1 to 5 report the console's own side of the path
(pad, enables, controller). So the line is printed in the default colour,
and since the probe sets no verdict key, it cannot reach the verdict or a
GUI check. To someone who has fitted an adapter, the same line says that
the adapter's console side has no power. Read it with step 2 then: a red
`PA5 (5V_EN)` line means the rail's 5 V supply, which powers the level
shifter, is not enabled, and a red `PK3 (JC-R EN)` line that the
Tegra-side buffer is not gated on. With both green the break lies past
these enables: the 5 V rail itself, the rail contacts, the adapter or its
cable.

Step 1 can show a green pull-up anchor on the same console. The code does
not explain why PG1 still samples LOW with the pull-up set; it records that
most consoles read LOW and treats that reading as the expected one.

### What it changes

- `LCR.DLAB` is set for the divisor read, then the saved `LCR` is written
  back. This is the UART that carries the log, but the code notes that
  the flip lasts microseconds and loses no TX byte in flight: THR keeps
  emptying, the shift register being independent of the latch mux.
- PG1 spends about 10 ms in GPIO mode and is then set to SPIO
  unconditionally, not to whatever mode it had before. While PG1 is a
  GPIO the UART does not own the pin, so bytes sent in that window, by a
  host or by a Joy-Con, are not received.

### What it cannot tell

- With an all-LOW sample, it cannot separate "no adapter" from "an
  adapter whose console side has no power".
- A Joy-Con on the right rail drives PG1 itself (`main.c`: Joy-Cons drive
  the line continuously), so with one attached a HIGH or ACTIVE sample
  says nothing about a bench adapter. Remove the Joy-Con before judging
  the adapter path.
- TX is not tested, the line is seen for 10 ms only, and nothing is known
  of the host-side wiring beyond the line level.
- It reports whatever state `UART_B` is in at that moment. In the GUI a
  Joy-Con on the right rail takes `UART_B` (see
  [gui](gui.md#rails-and-joy-cons)), and bdk's Joy-Con driver runs it at
  1 or 3 Mbaud, so the baud line goes yellow there for a reason that has
  nothing to do with the debug port.
- It has no GUI check row.

## Reset reason, PMIC latches and RTC

`probe_reset`, under `[Reset + PMIC latches + RTC + Hekate intent]`,
gathers evidence of why the previous power cycle ended, from three places:
the Tegra PMC's `RST_STATUS`, the MAX77620's non-volatile `NVERC`, and the
MAX77620's `INTLBT` and `IRQSD` interrupt latches. It also prints the RTC,
so a technician can match the boot against their own log.

### The snapshot

The MAX77620's latches clear on read, and the sweep's active probes (fan,
audio, CPU0, game card) can raise the low-battery and thermal bits
themselves. So `probe_reset_snapshot()` reads `NVERC`, `INTLBT` and
`IRQSD` once over `I2C_5` at `0x3C`, before anything active has run:
`main.c` calls it just before the boot sweep, and `gui/gui_main.c` just
after the display comes up, before the touchscreen and Joy-Con bring-up.
Later calls do nothing. `probe_reset` reports that snapshot on every run,
re-runs included (`a`, `r`, the host's `G`, the GUI's Check Again): a plain
re-read came back clean and silently flipped the PMIC verdict to pass. The
other side of this is that events during the session never show here.

### RST_STATUS

The low three bits of PMC `RST_STATUS` (`0x1B4`), printed as `PMC
RST_STATUS: N (name)`. It sets no verdict key.

| Value | Printed | Colour |
|---|---|---|
| 0 | `POR (cold boot - power-on reset)` | green |
| 1 | `WATCHDOG (BPMP timeout - hung firmware?)` | red |
| 2 | `SENSOR (thermal trip - SoC overheat)` | red |
| 3 | `SW_MAIN (software reboot)` | yellow |
| 4 | `LP0 (wake from deep sleep)` | yellow |
| 5 | `AOTAG (always-on tag-based reset)` | yellow |
| 6, 7 | `(reserved N)` | yellow |

By the time the probe runs, bdk has already written to this register.
`hw_init()`, which runs at the start of `hwtest.bin` (the GUI stage has
none of its own and runs after it), saves the register into
`hw_rst_reason` and `SCRATCH200` into `hw_rst_status`, then writes
`PMC_RST_STATUS_POR` into `RST_STATUS` and 0 into `SCRATCH200`. hwtest
reads the live register and never the saved copies, so the line shows
what is left after that write, not the cause the BootROM recorded. Expect
a green `0 (POR ...)` on every run: it is bdk's write, not evidence of a
cold boot. For why the last session ended, read `NVERC`.

### NVERC

The MAX77620's non-volatile reset cause. The header line is `PMIC NVERC :
0xNN`, with ` (clean)` and green when zero, yellow whenever any bit is set;
each set bit then gets its own line:

| Bit | Name | Printed | Colour |
|---|---|---|---|
| 0 | `SHDN` | `forced shutdown via EN0` | yellow |
| 1 | `WTCHDG` | `PMIC watchdog tripped` | red |
| 2 | `HDRST` | `hard reset (long power-button)` | yellow |
| 3 | `TOVLD` | `thermal overload trip` | red |
| 4 | `MBLSD` | `main-batt low-voltage shutdown` | yellow |
| 5 | `MBO` | `main-batt overload` | red |
| 6 | `MBU` | `main-batt under-voltage` | yellow |
| 7 | `RSTIN` | `RST input asserted` | yellow |

`pmic_nverc` is FAIL when `WTCHDG`, `TOVLD` or `MBO` is set (detail `severe
latch 0xNN`), WARN when only other bits are set (`soft latch 0xNN`) and
PASS when the register is 0. The three severe bits are real fault
evidence: the PMIC watchdog fired, a thermal overload tripped or the
battery rail was overloaded. `MBLSD` and `MBU` happen on every deep
discharge and on a battery pull while powered, so they are power history
rather than a fault; the user-triggered `SHDN`, `HDRST` and `RSTIN` only
warn too.

### INTLBT

`PMIC INTLBT : 0xNN`, green with ` (no thermal/low-batt events)` when zero,
yellow otherwise. Bit lines: `TJALRM2` (bit 1, junction-temperature alarm
2) in red, `TJALRM1` (bit 2) and `LBM` (bit 3, low-battery monitor) in
yellow. `pmic_intlbt` is WARN whenever the register is non-zero (detail
`thermal/low-batt latch 0xNN`) and PASS otherwise. It never fails, so a red
`TJALRM2` line still only makes the key WARN. Other bits are not decoded,
but they still turn the header yellow and the key WARN.

### IRQSD

The four `PFI_SDx` bits latch whenever an SD-rail buck regulator passes
through its fault threshold. The PMIC sets them while each buck ramps from
off to regulation at chip POR or `HDRST`, during a controlled shutdown
through `ONOFFCNFG1.PWR_OFF` (this payload's `q` key uses that path; the
rails go down one at a time, so SD0, SD1 and SD3 are typically set while
SD2 often stays clear, the PLL rail being brought down first), and on a
real power fault during operation. `IRQSD` alone cannot tell these apart;
`NVERC` and `RST_STATUS` carry the cause. A non-zero value on its own is
therefore not a finding.

The header line is green `0x00 (no SD-rail power faults)` or, in the
default colour, `0xNN (SD-rail thresholds latched - cross-check NVERC)`.
The bit lines are in the default colour too: `PFI_SD0` (bit 7, CPU rail),
`PFI_SD1` (bit 6, DDR rail), `PFI_SD2` (bit 5, PLL rail), `PFI_SD3` (bit 4,
1V8 rail). `pmic_irqsd` is FAIL only when `IRQSD` is non-zero and `NVERC`
has a severe bit set (detail `rail-fault latch 0xNN (NVERC severe)`): the
console rebooted because of something, and `IRQSD` then points at the rail
that tripped. It is PASS in every other case and never WARN. Bits 0 to 3
are not decoded, but they still count: they give the `thresholds latched`
header and, with a severe `NVERC`, the FAIL.

### RTC

`RTC raw : YYYY-MM-DD hh:mm:ss`, the MAX77620 calendar read through bdk's
`max77620_rtc_get_time()` (which already numbers months from 1). A year of
2010 or earlier prints in the default colour with `(calendar unset, HOS
keeps its own offset)`; otherwise the line is green. HOS adds its own epoch
offset to the calendar, so after a coin-cell discharge or a reflow that
briefly drops VBAT the calendar resets to 2000-01-01 while HOS still shows
the right time. An unset calendar is information, not a fault.

To show the same wall time hekate's GUI does, the probe reads up to 2047
bytes of `0:/bootloader/nyx.ini` and scans each line, leading spaces and
tabs skipped, for `timeoffset=` (hex, optionally negative; the first match
is used) and `timedst=` (`1` turns it on; the last match is used). It does
not track INI sections. An offset of 0 or 1 is ignored, 1 being hekate's
marker for "no offset". With a usable offset, the probe hands it and the
DST flag to bdk (`max77620_rtc_set_epoch_offset()`,
`max77620_rtc_set_auto_dst()`) and prints a green `RTC adjusted :` line
with the adjusted date and time, the offset in seconds and `, auto-DST`
when set. bdk's DST rule adds 3600 s between 28 March and 28 October.
With no usable offset and an unset calendar it prints `RTC adjusted : not
available (no /bootloader/nyx.ini timeoffset)`; with no usable offset and
a set calendar it prints nothing more. The probe opens the file without
mounting the SD card, so it finds it only when something earlier in the
session left the volume mounted (the Storage probes, or a report already
saved). A run without the Storage group, or with no SD card, treats
`nyx.ini` as absent even when it holds an offset.

### In the verdict and the GUI

The verdict's `PMIC` row ([verdict](verdict.md#rows)) aggregates these
three keys with `pmic_rails`, `max77812`, `max77621_cpu` and
`max77621_gpu`, and is gated on `PROBE_G_POWER | PROBE_G_RAW`, so a
custom run with Raw state alone still prints the latches. In the GUI the
keys appear under the Power system category:

| Key | GUI row | States it can take |
|---|---|---|
| `pmic_nverc` | Last shutdown | PASS, WARN, FAIL |
| `pmic_irqsd` | Power warnings | PASS, FAIL |
| `pmic_intlbt` | Low battery or heat warning | PASS, WARN |

Two of the texts in `gui/gui_text.c` do not quite fit the code.
`pmic_nverc`'s warning text speaks of a battery or power event, but WARN
also covers the user resets `SHDN`, `HDRST` and `RSTIN`. `pmic_irqsd` has
only a warning text, which the code never uses since the key is never
WARN; a FAIL shows the generic "This check failed." with the technical
detail under it. `RST_STATUS` and the RTC lines have no row; they are text
only.

### What it changes

- The clear-on-read PMIC latches are consumed once, by the snapshot. The
  read clears them on the PMIC itself, so a later boot, of hwtest or of
  HOS, no longer sees what this one read: this run's report (the copy
  saved to SD, or a UART capture) is the only record.
- Each RTC read first writes `RTC_UPDATE0` (`READ_UPDATE`) over I2C.
- With a usable `nyx.ini` offset, bdk's epoch-offset and auto-DST
  settings stay set for the session, and later callers of
  `max77620_rtc_get_time_adjusted()` in the GUI (`gui_dock.c`,
  `gui_shot.c`) use them. Nothing is written to the PMIC for this.

### What it cannot tell

- The reset cause the BootROM saw, since bdk has written `RST_STATUS`
  before the probe runs.
- Which of a power ramp, a shutdown or a fault set `IRQSD`.
- A PMIC that did not answer. bdk's `i2c_recv_byte()` returns 0 when the
  transfer fails and the snapshot does not check it, so an unreachable
  MAX77620 reads as three clean latches and all three keys PASS. A clean
  result only counts if the PMIC answers elsewhere in the run, for
  instance with a matching `CID4` on the Power & charging page
  ([power](power.md#main-pmic-probe_pmic)).
- Anything that happened after the snapshot.
- HOS's wall time, unless `nyx.ini` holds an offset and the SD volume is
  already mounted when the probe runs.

## PMC scratch and Hekate intent

The PMC has 256 32-bit scratch registers that survive SoC resets and deep
sleep. Hekate, HOS package1, the fuses ipatch and Atmosphere use them to
pass boot state across a handover; most slots are opaque, but three have
known meanings. `probe_pmc_scratch`, under `[PMC scratch]`, prints the
three registers in the default colour, decodes the known bits and sets no
verdict key.

- **`SCRATCH0`** (`0x50`), the reboot-mode flags that choose the next boot
  path. One line per known bit that is set: `WARMBOOT` (bit 0,
  "resume-from-sleep flag set"), `RCM` (bit 1, "next reboot would
  re-enter BootROM RCM"), `PAYLOAD` (bit 29, "CFW payload chainload
  requested"), `BOOTLOADER` (bit 30, "CFW bootloader chainload
  requested") and `RECOVERY` (bit 31, "force fusee/recovery path"). Other
  bits are not decoded. bdk's `hw_init()` does not touch this register,
  and hwtest's own reboot (a PMIC `SFT_RST` power cycle) and power-off
  (`PWR_OFF`) do not write it, so whatever it holds was there when hwtest
  started.
- **`SCRATCH20`** (`0xA0`), the ODM customer configuration. When bit 18,
  the debug-console flag, is set, an info line says HOS would enable kgdb.
  bdk's `hw_init()` clears bits 18 and 19 before any probe runs, so the
  line only appears if something sets the bit again after that.
- **`SCRATCH200`** (`0x840`) holds the reset cause across the BootROM to
  bootloader handover. Printed raw. bdk's `hw_init()` saves it and writes
  0 before any probe runs (see [RST_STATUS](#rst_status)), so the probe
  always reads it after that clear.

### Hekate boot intent

Hekate keeps a next-boot request ("force the menu next time", "do UMS next
time") in two 6-bit slots of the MAX77620's RTC alarm registers, the only
battery-backed scratch storage on the board; it has nothing to do with
time. bdk's `max77620_rtc_get_reboot_reason()` accepts it only when the
magic `0x77` is in both the `ALARM1` and `ALARM2` weekday registers, then
decodes a 4-bit reason, `autoboot_idx`, `autoboot_list` and `ums_idx` from
the two year registers. `probe_pmc_scratch` prints it, so the line
appears under `[PMC scratch]` even though the reset probe's header names
it.

No valid magic prints a green `Hekate intent: none (normal boot, no
override pending)`. Otherwise `Hekate intent: N (name)`, green for 0 and
in the info colour for anything else:

| Reason | Printed | Meaning in the code | Extra line |
|---|---|---|---|
| 0 | `NOP (normal boot)` | normal boot, no special handling | |
| 1 | `SELF (autoboot)` | `autoboot_idx` into `autoboot_list` | `autoboot_idx=N list=N` |
| 2 | `MENU (force Hekate menu)` | force the Hekate menu | |
| 3 | `UMS (force USB mass storage)` | USB mass storage of partition `ums_idx` | `ums_idx=N` |
| 4 | `REC (recovery)` | set `SCRATCH0` `RECOVERY` and reboot to self | |
| 5 | `PANIC` | bootloader-side panic (T210B01 only) | |
| 6-15 | `?` | | |

A console that lands in the Hekate menu instead of HOS on every boot would
be explained by a reason of 2 stuck here. The line is labelled as boot
intent so it does not read like one of the RTC time lines.

This read is not passive. When the magic is valid, bdk clears it by
writing 0 to both weekday registers and issuing an RTC write update, so a
pending intent is consumed by this probe: the next boot will not see it,
and a re-run prints `none`.

### What it cannot tell

The meaning of the opaque slots and undecoded bits, and the values
`SCRATCH20` and `SCRATCH200` held before `hw_init()` changed them. There
is no GUI check row.
