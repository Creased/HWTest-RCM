# Display

Two probes make the Display page: `probe_display` (section
`[DSI panel ID]`) names the panel from the ID it answered over DSI, and
`probe_backlight` (`[Display backlight & PWM]`) checks that the backlight
is being driven. Neither writes a register. Both judge state that hwtest
set up itself at start-up: `main.c` calls bdk's `display_init()`, which
reads the panel's ID, then `display_backlight_pwm_init()` and
`display_backlight_brightness(100, 1000)`, which brings the backlight to
level 100 of 255. A third check, on the Storage page, holds the same ID
against the LCD vendor the factory recorded in CAL0.

Nothing in hwtest tears that state down (`display_end()` is never
called): LDO0 and PV2 on every console, and PI0, PI1, PV1 and PWM0 at
duty 100 on an LCD console, stay as start-up left them until hwtest
reboots or powers off the console, both of which drop every rail at the
PMIC. The GUI's Screen test
and its screenshot shutter change the level and put it back to
`GUI_BACKLIGHT` (100), so a later run reads the same.

Their keys feed the Verdict's **Display** row (`dsi_id`, `backlight`) and
the GUI's **Screen** category; `cal0_panel` belongs to the **Storage** row.
In a custom run the group is "Display" in the text launcher and "Screen"
in the GUI. Terms (DSI, PWM, PRODINFO, CAL0) are in [terms](terms.md); the
[README](../README.md) has the overview.

## Reading the panel ID

The ID is read once, by `display_init()` in hwtest.bin. The steps that
matter here run in this order:

1. `_nx_aula` is set when `fuse_read_hw_type()` is `FUSE_NX_HW_TYPE_AULA`.
   Whether a console is the OLED model is decided by its fuses, not by the
   panel that answers.
2. `REGULATOR_LDO0` is set to 1.2 V and enabled (bdk's "DSI AVDD"), on
   every console.
3. On every console but an Aula, the LCD and WLED (backlight LED) driver
   pins are set up: `LCD_BL_PWM` and `LCD_BL_EN` muxed as
   `PINMUX_PULL_DOWN`, the LCD AVDD channels switched on (PI0 and PI1 high,
   then 10 ms to settle), PV0 and PV1 made outputs and driven low, then PV1
   driven high to enable the WLED driver. An Aula gets none of this.
4. The panel reset is released (PV2 high), then 60 ms.
5. `DSI_BTA_TIMING`, the timeout for the panel to take the bus and reply,
   is set to `0x40103` on an Aula and `0x50204` otherwise.
6. `_panel_id_raw` is set to the sentinel `0xCCCCCC`, then up to three
   `display_dsi_read(MIPI_DCS_GET_DISPLAY_ID, 3, ...)` attempts are made,
   5 ms apart. A reply that is an `ACK_ERROR_RES`, of an unknown type, or
   without the escape byte counts as no answer and copies nothing, so the
   sentinel survives three of them. A long-read reply copies up to three
   bytes; a 1- or 2-byte short-read reply copies only that many.
7. bdk decodes the ID for its own use (below) and picks the panel's init
   sequence by it. IDs it has no case for, clones included, take the
   default case, commented "Allow spare part displays to work", which
   exits sleep mode with the 5.5-inch panels' 120 ms wait. A failed read
   lands there too on an LCD console, and an Aula always takes the
   Samsung's case (below), so start-up carries on either way.

## Decoding it

The three bytes, as bdk's `di.h` documents them:

| Byte | Holds | Values |
|---|---|---|
| 0 | Vendor | `10` JDI, `20` InnoLux, `30` AUO, `40` Sharp, `50` Samsung |
| 1 | Model / revision | e.g. `93`, `94`, `95` for AUO A062TAN00, 01, 02 |
| 2 | Board (panel size) | `0F` Icosa/Iowa 6.2", `10` Hoag 5.5", `20` Aula 7.0" |

`panel_id_decoded()` keeps bytes 2 and 0 as `0xBBVV`, board high and vendor
low: an AUO on an Icosa/Iowa board is `0x0F30`. It is the formula bdk
uses, without the two things bdk then does to its own copy: it folds every
JDI (vendor byte `0x10`) into the generic `0x0010`, and it makes every Aula
`0x2050`, the Samsung AMS699VC01. That copy is right for driving the panel
and wrong for reporting it: held against CAL0, a JDI console would read as
having a replaced screen and an OLED as having its own, whatever is fitted.
So the report and the CAL0 comparison use the ID as it answered; only the
backlight verdict and the "Driven as" line look at bdk's copy, through
`display_get_decoded_panel_id()`.

Two JDI panels break the board convention: the LPM062M326A answers
`[10] 81 [26]` and the LAM062M109A `[10] 96 [09]`, so they decode to
`0x2610` and `0x0910`.

The model byte is dropped, so hwtest names a panel family, not a revision:
every InnoLux P062CCA-AZx reads as "InnoLux P062CCA" and every AUO
A062TAN0x as "AUO A062TAN". The byte is still on the Raw ID line.

### Panel names

`panel_model_name()`, which the CAL0 check shares, knows bdk's `PANEL_*`
list:

| Decoded | Printed name | Notes |
|---|---|---|
| `0x0010` | JDI 062M (generic) | bdk's folded JDI ID; from the panel only with a board byte of `00` |
| `0x0910` | JDI LAM062M109A | 6.2" |
| `0x2610` | JDI LPM062M326A | 6.2" |
| `0x0F20` | InnoLux P062CCA | 6.2" |
| `0x0F30` | AUO A062TAN | 6.2" |
| `0x1020` | InnoLux 2J055IA | 5.5" (Hoag, the Lite) |
| `0x1030` | AUO A055TAN | 5.5" |
| `0x1040` | Sharp LQ055T1SW10 | 5.5" |
| `0x2050` | Samsung AMS699VC01 | 7.0" OLED (Aula) |
| `0x0F83` | OEM clone 6.2" | a clone of the AUO A062TAN01, answers `[83] 94 [0F]` |
| `0x00B3` | OEM clone 5.5" | found with fake AUO A055TAN02 markings |
| `0x0000` | OEM clone | found with fake AUO A055TAN02 markings |

bdk's notes on the clones add two cases the table cannot catch. The 6.2"
clone sometimes answers `[30] 94 [0F]`, a genuine AUO A062TAN01's ID, and
is then named as one. A 5.5" clone answering `[40] 94 [0F]` (`0x0F40`)
appears only in a comment, so it prints as "Sharp, model unknown".

An ID not in the table is named by its vendor byte alone ("AUO, model
unknown"), or "Unknown" when that byte is none of the five vendors.

## The Aula (OLED)

The OLED panel has no PWM backlight: it is self-emissive, and brightness
is a MIPI DCS command (`MIPI_DCS_SET_BRIGHTNESS`) carried over DSI. bdk
picks the path by its decoded ID being `0x2050`, which every Aula is:

- `display_backlight_pwm_init()` returns at once, so on an Aula nothing in
  hwtest or bdk writes `PWM_CSR_0` or muxes `LCD_BL_PWM` to PWM0.
- `display_backlight_brightness()` scales the 0..255 level to the panel's
  range (`level * 2047 / 255`), sends it, then stores the 0..255 level in
  `DC_DCS_BACKLIGHT_LEVEL` (bdk's name for `DC_COM_SCRATCH_REGISTER_B`,
  `0x54200C98`). It skips the send when that register already holds the
  level asked for. After start-up it holds `0x00000064` (100). It is
  bdk's bookkeeping, written after the command is sent, not a value read
  back from the panel.

An Aula panel that answered something else is still driven as the
Samsung, and the panel ID probe says so. Nyx tells the 7-inch LCD clone
made for the OLED model apart by its touch controller (an FTS4 that reads
as a clone); hwtest does not, so on an Aula it goes by the DSI answer
alone.

## Handing the ID to the GUI

The GUI stage never runs `display_init()`, yet bdk's display code inside
it needs the decoded ID before anything is drawn: it decides whether a
brightness change goes over DSI (an OLED) or to the PWM. So
`ui_boot_gui()` in hwtest.bin passes the raw ID across the jump: R0 is
`GUI_ARGS_MAGIC`
(`0x44494E50`, "PNID") and R1 is `display_get_verbose_panel_id()`.
`gui/start.S` keeps both in `gui_boot_args` past the BSS clear, and the
first thing `ipl_main()` does is act on them:

- With the magic, `panel_id_set(raw)` stores the raw ID and sets bdk's
  decoded copy with `display_set_decoded_panel_id()`. On an Aula it passes
  the sentinel `0xCCCCCC` instead of the real ID, on purpose: bdk's setter
  (Nyx's) forces the Samsung only when the ID decodes to `0xCCCC`, a failed
  read, whereas `display_init()` forces it on every Aula. The sentinel gets
  the GUI the same driving hwtest.bin used.
- Without it (a GUI started by an older hwtest.bin), `panel_id_lost()`
  marks the ID unknown and sets bdk's copy as a failed read leaves it: an
  Aula is still driven as the OLED, by its fuses, and any other console
  gets `0xCCCC` and the PWM path.

bdk's raw copy stays zero in the GUI, which is why the probes go through
`panel_id_raw()`: the value the GUI was given when there is one, bdk's own
otherwise. `panel_id_known()` is false only in a GUI started by an older
hwtest.bin; the panel ID report and the CAL0 comparison then do not run.

## Panel ID probe: what it reports

```
  Raw ID bytes : 30 94 0F
  Decoded ID   : 0x0F30
  Model        : AUO A062TAN
```

The raw bytes are printed byte 0 first (vendor, model, board), uncoloured.
What follows depends on the answer:

- **Read failed** (`Raw ID bytes : CC CC CC`, the sentinel survived): red
  `Decoded ID   : none (read FAILED)` and `Model        : Unknown`, and
  `dsi_id` is **FAIL** with "DSI ID read failed (cable?)".
- **Any other value**: `dsi_id` is **PASS**, set before the name is looked
  up. The decoded ID is printed in green. The Model line is green for a
  named panel and yellow for a clone, a vendor-only name or "Unknown";
  that colour is the only notice a clone or an unknown panel gets, and the
  verdict flags neither.
- **An Aula whose panel answered something other than `0x2050`**: a yellow
  `Driven as    : Samsung AMS699VC01, as on every Aula` follows.

When the ID is not known, the section is the single line
`Raw ID bytes : not known (this GUI was started by an older hwtest.bin)`
and no `dsi_id` is set. `dsi_id` never warns.

## Backlight probe

On an LCD console start-up leaves the state the verdict is read against:
`display_backlight_pwm_init()` enables the PWM clock, writes
`PWM_CSR_0 = PWM_CSR_EN` (25 kHz, per bdk's comment), sets `LCD_BL_PWM`'s
function to 1 (PWM0) and hands PV0 from the GPIO controller to it
(`gpio_config(..., GPIO_MODE_SPIO)`); the brightness call then ramps the
duty to 100, one step a millisecond. PV0 is the `LCD_BL_PWM` pad and PV1
the `LCD_BL_EN` pad, the WLED driver enable raised before the ID read.

The probe only reads. The lines it prints:

- `PWM_CSR_0` (`0x7000A000`), raw, then `PWM enabled  : yes` (green) or
  `no` (yellow), from bit 31, `PWM_CSR_EN`.
- `PWM duty     : N / 255 (P%)`, bits 23:16, only when the PWM is enabled;
  yellow at 0. The percentage is rounded down. Start-up leaves `PWM_CSR_0`
  at `0x80640000` on an LCD console, which reads `100 / 255 (39%)`.
- `DCS BL level : 0x...`, `DC_DCS_BACKLIGHT_LEVEL`, uncoloured. It means
  something only on an OLED; the code's comment says that on an LCD it
  stays at its boot value.
- `BL_PWM mux`: the raw word, then the function (1 `PWM0`, 0 `RSVD0`,
  anything else "other"), tristate or driven, and the pull.
  `BL_EN  mux` is the raw word only.
- `GPIO PV0 EN  : HIGH` (green) or `LOW (off)` (yellow), only when
  `LCD_BL_PWM` is not muxed to PWM0, so the pad holds a static level.
  `gpio_read()` returns `GPIO_IN`, the pad's actual level, so in PWM mode
  it would sample the ~25 kHz waveform at an arbitrary instant; the row is
  left out then, on purpose.
- `GPIO PV1 SIG : HIGH|LOW`, always, never judged. The code's comment says
  some Mariko variants leave it low because their WLED driver needs no
  separate enable.
- `Verdict      : ...`, the section's own conclusion, which sets
  `backlight`. The first matching row wins:

| Condition | `Verdict` line | `backlight` |
|---|---|---|
| bdk's ID is `0x2050`, DCS level not 0 | `OLED (AULA) DCS backlight 0x...` | PASS |
| bdk's ID is `0x2050`, DCS level 0 | `... (level 0 - screen dark)` | WARN "OLED DCS level 0" |
| PWM enabled, pad muxed to PWM0, duty > 0 | `PWM0 driving BL_PWM @ duty N - backlight lit` | PASS |
| PWM enabled, duty 0 | `PWM enabled but duty = 0 (screen dark)` | WARN "duty 0" |
| pad not muxed to PWM0 | `BL_PWM not muxed to PWM0 - DSI-internal panel?` | WARN "DSI-internal" |
| anything else (PWM0, PWM disabled) | `PWM disabled - backlight is off` | WARN "PWM disabled" |

`backlight` never fails. The OLED row comes first and keys on bdk's ID,
not the panel's answer, because that ID is what decides which path bdk
drives; every Aula takes it. Judged against PWM0 instead, every OLED unit
would be flagged, since its PWM path sits idle by design. The yellow
`PWM enabled  : no` an Aula prints is not a fault for the same reason.

## CAL0 LCD vendor against the fitted panel

`probe_serial()` on the Storage page (section
`[Switch serial number (PRODINFO)]`) reads CAL0's `lcd_vendor` once the
body has been decrypted ([storage](storage.md#prodinfo-and-cal0) covers
how it gets there). CAL0 keeps the bytes the other way round from the
DSI read, board in byte 0 and vendor in byte 2, and the probe recombines
them into the same `0xBBVV` with Nyx's decode,
`((v & 0xFF) << 8) | (v >> 16)`. The code's example, an AUO on a bench
Erista, is `0x30940F` in CAL0 and `30 94 0F` on the DSI line: printed the
way hwtest prints each, the two read alike left to right although the
byte order underneath is opposite.

```
  LCD vendor   : 30940F -> 0x0F30 AUO A062TAN
  Panel fitted : 0x0F30 (matches CAL0)
```

- A zero field prints `LCD vendor   : not recorded in CAL0` and compares
  nothing. The factory never wrote one, which the code calls common on
  Mariko, and comparing would report every such console as having a
  replaced screen.
- Otherwise the CAL0 value is printed with its name, or "(unknown)". When
  the panel ID is known and its read did not fail, the fitted panel's
  decoded ID follows (hex only): green `(matches CAL0)` and `cal0_panel`
  **PASS**, or yellow `(differs from CAL0 - replaced?)` and **WARN** with
  "fitted panel 0x...., CAL0 says 0x....".

A mismatch is a WARN, never a FAIL: a replaced panel is ordinary repair
work, it just means the factory calibration no longer matches what is
fitted, and HOS keeps driving the replacement with the original's
calibration.

`cal0_panel` is not set when the CAL0 body is not to hand (eMMC not up, no
PRODINFO in the GPT, an unusable GPT entry, a BIS read failure, a missing
or wrong `sd:/switch/prod.keys`, a body size out of range), when the field
is zero, when the ID is not known, or when the DSI read failed. It does
run after a `cal0_hash` mismatch. It needs only the Storage group in a
custom run, since the ID was read at start-up.

The comparison is the 16-bit board and vendor value, the model byte
dropped on both sides. A replacement of the same family and size (an
A062TAN02 for an A062TAN01) reads as matching, and so does the 6.2" clone
when it answers `[30] 94 [0F]`; answering `0x0F83` against an AUO's CAL0,
it warns.

## In the verdict and the GUI

The Verdict's Display row takes the worse of `dsi_id` and `backlight`,
with the first FAIL's reason, else the first WARN's, for example
`Display        : warn (duty 0)` ([verdict](verdict.md#aggregation)).
`cal0_panel` shows on the Storage row, which gives one reason by the same
rule: any FAIL in that row, or a WARN from one of the five keys listed
before it (`emmc_health`, `emmc_bus`, `sd_bus`, `wlan_cal`, `cal0_hash`),
takes the reason, so the `Panel fitted` line on the Storage page is where
a mismatch always shows.

In the GUI's results:

| Key | Row | When it warns | When it fails |
|---|---|---|---|
| `dsi_id` | Screen connection | never | "The screen does not answer. Its cable may be loose, or the screen may be damaged." |
| `backlight` | Backlight | "The screen's backlight looks off or very dim." | never |
| `cal0_panel` | Original screen (Storage category) | "The screen fitted is not the one this console left the factory with: it has been replaced." | never |

Every `backlight` WARN, "DSI-internal" and "PWM disabled" included, gets
that one sentence, with the probe's reason under it (`Technical: duty 0`,
`Technical: DSI-internal` and so on); `cal0_panel`'s gives both IDs. The
Screen category reads "The screen and its backlight answer." when both
pass, and shows three readings: **Panel** (the Model line), **Panel ID**
(the Decoded ID line) and **Backlight** (only what the PWM duty line has
in parentheses, such as `39%`). After a failed read Panel reads "Unknown"
and Panel ID "none (read FAILED)". A reading the run did not print is left
out: with the ID unknown there is no Panel or Panel ID, on an Aula there
is no Backlight (nothing in hwtest enables its PWM, and the `PWM duty`
line needs it enabled), and the OLED's DCS level is never one of them.

## What a failure points at

- **`dsi_id` FAIL**: the panel did not answer three ID reads at start-up.
  The probe's reason suggests the cable, the GUI the cable or the panel.
  On every console check `LDO0 (Display)` on the Power page
  ([power](power.md#regulators-probe_regulators)) and PV2 (panel reset)
  in the pin census on the Raw state page
  ([raw state](raw-state.md#pin-census)); on an LCD console also PI0 and
  PI1 (LCD AVDD, +5.4 V and -5.4 V per bdk's comment), with PV0 and PV1.
- **`backlight` WARN on an LCD console**: the Tegra side is not driving the
  backlight as start-up left it; the reason says how (duty at 0, the PWM
  controller disabled, or the pad no longer muxed to PWM0).
- **`backlight` WARN on an Aula**: the last level bdk stored was 0.
- **`cal0_panel` WARN**: the panel answers with another ID than the one
  the factory recorded: a replacement, or a clone answering its own ID.
  Read it together with `cal0_model`: CAL0 lives on the eMMC, and the
  code's comment says PRODINFO travels with it, so when that also warns
  the factory data may come from another console, rather than the panel
  having been replaced.

## What it cannot tell

- **The picture.** Nothing here looks at pixels. The GUI's Screen test
  ([gui](gui.md#screen-test)) does it by eye: the whole panel in white,
  red, green, blue and black, then white while the backlight level runs
  down and up between 255 and 8, in steps of 6, until the page is turned.
  It sets no key.
- **The link after start-up.** Neither probe reads the panel again:
  `dsi_id` PASS says the panel answered the ID read when hwtest started,
  and a reseated cable shows nothing new until the console is rebooted.
- **The backlight itself.** PASS on an LCD means the Tegra PWM is enabled
  at a non-zero duty with the pad muxed to PWM0; the code calls that the
  strongest signal there is without talking to the WLED driver IC over its
  own bus, which the Switch does not expose. The driver, the LED string
  and the connector are not seen. On an Aula, PASS means bdk stored a
  non-zero level; the panel is not asked.
- **The GPIO side of the routing.** The probe does not read the PV0
  `GPIO_MODE_SPIO` setting start-up made, and takes the pinmux function
  alone as proof of routing. The PV0 level (coloured, but it never changes
  `backlight`), the tristate bit, `LCD_BL_EN`'s mux and PV1 are printed,
  not judged.
- **Short replies.** A 1- or 2-byte reply leaves `0xCC` in the bytes it did
  not fill (raw `0xCCCC30`, say). That is not the sentinel, so `dsi_id`
  passes and the panel reads as `0xCC30`, "AUO, model unknown". A panel
  answering all zeros cannot be told from the `0x0000` OEM clone.
