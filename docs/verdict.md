# The verdict

The verdict is the `Diagnostics` page, `g_pages[0]` in core.c (the
[README](../README.md)'s page table calls it Verdict). The sweep runs it
last, because it reads what the other probes recorded, and the pager shows
it first. It has two sections. `[Cross-checks]` runs three checks that
read the hardware again and compare readings that each look healthy on
their own. `[Verdict]` reads no hardware: it rolls the recorded findings
up into one line per part of the console and a total. The GUI's results
are built from the same findings and the same table of rows. Over UART
the page starts at `--- Diagnostics ---`. Comments in verdict.c that call it
`--- Verdict ---` are out of date.

## The finding registry

dx.c keeps one table, `_dx_tbl[DX_CAP]` with `DX_CAP` 64. Each entry is a
`dx_finding_t`: the key (a string literal, kept by pointer), a severity
(`DX_PASS`, `DX_WARN` or `DX_FAIL`) and `detail[48]`, a short reason that
is empty on a clean pass (a PASS can carry one: `als_id` "N/A on Lite").

- `dx_set` finds the key with `strcmp` and overwrites its severity and
  detail, or appends a new entry. The last write wins, not the worst.
- Once the table is full, new keys are dropped silently. hwtest.h asks
  whoever adds a key to count them; 49 are in use.
- The detail is rendered into a 256-byte scratch buffer and cut to 47
  characters, because bdk's `s_vprintf` has no length bound. A reason one
  byte too long used to overwrite the next entry's key pointer.
- `dx_get` returns NULL for a key nobody set in this sweep: the
  registry's own "did not run". Some probes also record a check they
  could not make as a WARN whose detail starts with a soft prefix (see
  [In the GUI](#in-the-gui)).
- `dx_reset` empties the table. Only `run_probe_mask` calls it, at the
  start of a sweep.

Three keys are written from more than one place. `emmc_bus` comes from
the eMMC bus width in `probe_storage`. Every BOOT0 excursion in
`probe_boot0_pkg1` and `probe_autorcm` ends in `emmc_back_to_user`, which
overwrites the key with FAIL "USER switch-back failed" if switching back
to USER fails.
`probe_wifi_pcie` writes `wifi_pcie` only for its two FAILs, which stop
the bring-up with `g_wifi_brought_up` false; the CPU0 handoff in
`probe_wifi_link` writes it only when the bring-up completed (see
[wireless](wireless.md)). `cpu_complex` is written by the handoff's up to
three boot attempts: PASS by the attempt loop after each boot that
succeeds, FAIL by `_cpu0_boot_checked` when one fails, which ends the
handoff. The diagnostic re-boot writes nothing when it succeeds, so a
failure there replaces an earlier PASS. `health_color` in dx.c only picks
a line colour and plays no part in the verdict.

## Where it runs

`run_probe_mask(mask)` in core.c is the sweep both front ends use:

1. `g_run_mask = mask & PROBE_G_ALL`, then `dx_reset()`. (Built with
   `WIFI_CFG_EARLY_REGON=1`, off by default, it then drives PH1 high so
   the WLAN chip is powered for the whole sweep; see
   [wireless](wireless.md).)
2. Pages 1 to 41, in table order. Each page whose group is in
   `g_run_mask` (`page_enabled`) gets `status_set(name)`, the
   `--- name ---` boundary in `COL_HEADER`, then its probe.
3. Page 0, `probe_verdict`, the same way. `page_enabled(0)` is always
   true.
4. `status_set("done - rendering pager")`.

`g_page_groups` gives each page one group. The groups follow the page
names, except that SoC and Fuses share `PROBE_G_IDENTITY`, and Audio is
split: `probe_audio` and `probe_audio_clocks` are `PROBE_G_AUDIO`,
`probe_audio_beep` is `PROBE_G_SOUND`. Three orderings matter to the
findings. `probe_storage_errors` follows the Storage reads it counts (its
comment says "last in the group", but `probe_gamecard` comes after it).
`probe_bt_radio` runs before `probe_wifi_pcie` because by default
(`WIFI_CFG_DRIVE_PH0=1`) the Wi-Fi probe drives PH0 high and leaves it
there, after which Bluetooth stops answering HCI; the code says a
Bluetooth result obtained after the Wi-Fi probe means nothing.
`probe_audio_beep` records nothing unless the codec answers and APE is out
of reset, which the two Audio pages before it arrange. The text launcher's
Custom test therefore adds Audio whenever Sound is picked. The GUI's Custom
Check does not, so Sound picked alone skips, and leaves `audio_beep` unset,
unless an earlier run left that state (see [audio](audio.md)).

| Group | Text launcher (Custom test) | GUI (Custom Check) |
|---|---|---|
| `PROBE_G_IDENTITY` | Identity / fuses | Identifying the console |
| `PROBE_G_POWER` | Power / thermal | Battery, charging and power |
| `PROBE_G_MEMORY` | Memory / clocks | Memory and clocks |
| `PROBE_G_STORAGE` | Storage / gamecard | Storage and game card reader |
| `PROBE_G_WIRELESS` | Wireless | Wi-Fi and Bluetooth |
| `PROBE_G_AUDIO` | Audio hardware | Sound chip |
| `PROBE_G_SOUND` | Sound output | Sound output (plays a short tone) |
| `PROBE_G_DISPLAY` | Display | Screen |
| `PROBE_G_INPUTS` | Touch / inputs | Touch, sensors and buttons |
| `PROBE_G_RAW` | Raw state | System details |

The group names in this note (Power, Storage, Raw and so on) are these
entries; Boot integrity, for example, needs both the Power and the Storage
entry ticked.

**PMIC latches.** `probe_reset` reports `NVERC`, `INTLBT` and `IRQSD` from
a snapshot taken once per boot before the sweep, so every later sweep
reports the same latch values ([raw-state](raw-state.md#the-snapshot)).

**Text payload.** main.c arms the report capture (`REPORT_CAP`, 128 KiB),
sets `g_show_status` and `_log_uart_only` (UART and capture only; the LCD
shows a spinner row), takes the snapshot and runs the sweep. Then it
detaches the capture, calls `save_report()` if both `g_sd_ok` and
`g_emmc_ok` are set, and calls `render_page(0)`. main.c brings the SD card
and eMMC up only when Storage is selected, so a text custom run without
Storage saves no report. `render_page(0)` runs `probe_verdict` a second
time, for the LCD and UART. So the SD report holds the sweep's verdict,
the LCD a second one, and UART both. The second run repeats the
cross-checks against live hardware, so plugging in a charger between the
two can change it. The saved file opens on the verdict. Because the
verdict is written last, a capture that fills before the
`--- Diagnostics ---` boundary holds none, and its `[capture truncated]`
marker is there to say so (see [storage](storage.md#the-sd-report)).

**Pager.** Every render runs that page's probe again, without
`dx_reset`. Navigating (`n`, `p`) or refreshing (`r`) overwrites that
page's keys, and showing page 0 re-aggregates whatever the registry
holds. `a` runs `run_probe_mask(g_run_mask)`: a fresh registry, the same
mask, the capture re-armed. `G<name>` re-runs every page of that name
without checking `page_enabled`. It can record keys outside the mask, but
the verdict still skips their rows; `GDiagnostics` re-runs the verdict.
`w` re-runs `probe_wifi_pcie` and `probe_wifi_link`. `s` writes the
capture buffer to the SD card again. Only the boot sweep and `a` are
captured, and `a` does not save, so after `a` the file still holds the
previous sweep until `s`. After navigation, `r`, `G` or `w` the LCD
verdict can differ from the saved one, and `s` still saves the last
captured sweep.

Later runs also see the state earlier ones left. Any run of the Bluetooth
page after the Wi-Fi probe (paging onto it, `r`, `a`, `G Wireless`)
overwrites `bt_hci` with a result that means nothing, so in a given boot
only a `bt_hci` taken before the first Wi-Fi run counts, in practice the
first sweep's ([wireless](wireless.md)); page 0 then aggregates the new
value into the Wireless row. The rails and clocks that later sweeps read
differently are in [power](power.md#what-hwtest-has-already-changed) and
[memory](memory.md#reading-it-and-what-it-cannot-tell).

**GUI.** `gui_run` sets `g_run_mask` from its own mask. Home always passes
`PROBE_G_ALL`, Custom Check its selection, and an empty mask becomes
`PROBE_G_ALL`. It then calls the same `run_probe_mask` and `save_report`.
So the verdict and the `xc_*` keys exist in the GUI too; its progress step
is "Putting the results together". The GUI runs the verdict once, inside
the sweep: its results and its Diagnostics capture are that run's, and
nothing re-reads the cross-checks afterwards. It brings the SD card and
eMMC up at start-up whatever is picked, so its custom runs save a report
too. A Check Again with Wireless picked runs the Bluetooth page after the
earlier check's Wi-Fi probe, so its `bt_hci` means nothing (see above).

## Step breadcrumbs

`status_set` names only the page, and Bluetooth, the Wi-Fi bring-up and
the Wi-Fi link all show as "Wireless". `status_step(msg)` names the step
inside the probe. It sends `[step] <msg>` (a 96-byte line, so keep it
short) through `DBG_SEND`, then calls `status_set(msg)`. During the text
boot sweep it also cleans the BPMP cache: the framebuffer sits in
write-back-cached DRAM, and without the clean the LCD can show the
previous step and blame the wrong one. In the text payload `DBG_SEND` is
`uart_send_crlf` on UART-B, which waits for TX idle, so the line is out
before the next access, which may hang. In the GUI it is `dbg_send`, to
UART-A, UART-B, UART-C or USB serial; only the UART ports go through
`uart_send_crlf` and its TX-idle wait.

Breadcrumbs bypass `_log_emit`. They reach only the debug console, never
the SD report or the GUI's page capture, and muting does not stop them.
The host viewer keeps the latest as **Last step** on its Runtime page. In
the GUI the step shows under the progress bar, without its "Wireless: "
prefix, and `dbg_send` sends nothing on a rail UART a Joy-Con has taken
(see [gui](gui.md#rails-and-joy-cons)). All 15 call sites are Wireless
(one in probe_bt.c, fourteen in probe_wifi.c); every other page shows only
its name.

`status_set` in both front ends first polls the debug console for `R`
(`uart_poll_reboot`), so a host can reboot the console between pages and
steps, and `msleep_poll` does the same in the long waits. A hang inside
one bus access never reaches a poll (see
[gui](gui.md#the-r-reboot)).

## Cross-checks

The chips A and B read are in [power](power.md) (charger, fuel gauge and
USB-PD controller), and the eMMC modes C reads in
[storage](storage.md#sd-and-emmc-bring-up).

A probe's own finding catches a chip that reports a fault. A cross-check
catches two chips that disagree about the same physical state, which
neither can see alone. The comment's canonical case is a broken trace
between the BQ24193 charger output and the MAX77620 `ACOK` pin: each chip
reports healthy, but they disagree about VBUS.

- The section runs when `g_run_mask` has `PROBE_G_POWER` or
  `PROBE_G_STORAGE`. Otherwise it prints the `[Cross-checks]` header and
  then only `custom run: cross-checks need Power or Storage selected` in
  `COL_WARN`.
- Each check needs only its own group: A and B need Power, C needs
  Storage. Gating all three on both groups once threw the charger checks
  away on a Power-only run.
- If `charger_id` is FAIL (no ACK from the BQ24193), the section prints
  `(charger dead - VBUS checks skipped)` and skips A and B. A dead charger
  reads as "no VBUS" and would make a false disagreement, and its own row
  already says what is wrong. A `charger_id` WARN does not skip them.

The checks read the chips fresh, not the registry, and write nothing to
the hardware. They read BQ STATUS `0x08`, not the double-read `FAULT`
register.

### A. VBUS, ACOK and USB-PD

| Observer | Reads | Present when |
|---|---|---|
| BM92T36 | `STATUS1` insert bit, via `bm92t36_get_source_info` | bit set |
| BQ24193 | `VBUS_STAT`, bits 7:6 of `0x08` (0 none, 1 USB-SDP, 2 adapter, 3 OTG) | non-zero, so OTG counts |
| MAX77620 | `ACOK` bit of `ONOFFSTAT` | bit set |

All three present, or all three absent, sets `xc_vbus_acok` to PASS.
Anything else is FAIL "VBUS observers disagree". The line is
`VBUS<->ACOK   : PD=ins|no BQ=present|absent ACOK=0|1` plus
`(consistent)` or `(all say no source)` in `COL_OK`, or `(MISMATCH -
localise the break)` in `COL_ERR`. The pair that disagrees localises the
broken trace. For example `PD=ins BQ=present ACOK=0` is the comment's
canonical case: a break between the BQ24193 output and the MAX77620
`ACOK` pin. All three absent with a known-good charger plugged in points
upstream of all of them: the port, the cable or the PD controller.

The check cannot tell whether a charger is plugged in, so "all absent"
passes and the tester has to know. bdk's `_bm92t36_read_reg` zeroes its
buffer before reading, so a BM92T36 that does not answer reads "not
inserted". With a charger in, that gives `usb_pd` FAIL and `xc_vbus_acok`
FAIL together, both on the Charger row.

### B. Charge state against current direction

The check uses `CHRG_STAT` (bits 5:4 of the same `0x08` read: 0 not
charging, 1 pre, 2 fast, 3 done), the MAX17050 `Current` (the instant
reading, not `AvgCurrent`, in uA, divided by 1000) and the BQ24193
`InputCurrentLimit`.

- Charging means `CHRG_STAT` 1 or 2; "done" does not count.
- A +/-30 mA dead band: positive means above +30 mA, negative below
  -30 mA.
- The input is limited when `VBUS_STAT` is 1 (USB-SDP), the limit is
  unreadable, or it is 500 mA or less.
- Charging, negative and not limited sets `xc_charge_dir` to WARN "BQ
  charging but gauge sees %d mA". Anything else is PASS.

A charger that pumps while the gauge sees current leaving the pack points
at the sense resistor or its trace. Such a console looks as if it does not
charge in HOS, though the charger works. A limited source is exempt
because the console really does run partly off the pack there while the
BQ still says fast charging. The comment cites a real Erista on USB-SDP
with a 500 mA limit that measured -224 mA. The result stops at WARN
because the dead band and the headroom test are empirical heuristics.

The line is `CHRG<->Curr   : BQ chrg=<n>, gauge=<n> mA` plus the first
note that fits: `(NEGATIVE - sense resistor?)` for the WARN, in
`COL_WARN`, then `(input-limited, pack helps)`, `(charging, current +)`,
`(idle/discharging)` for `CHRG_STAT` 0, or `(consistent)`, all in
`COL_OK`.

In bdk, `max17050_get_property` and `bq24193_get_property` return 0 for
these two properties whatever the bus does, and a NAK reads as 0. So the
"limit unreadable" branch never fires, and a gauge that does not answer
reads 0 mA and passes. `fuel_devname` is what catches a dead gauge.

### C. eMMC HS400 needs an 8-bit bus

This check runs only when `g_emmc_ok` and `emmc_storage.sdmmc` are set;
otherwise it prints no line and sets no key. The mode is `emmc_get_mode()`,
what bdk negotiated for this session. EXT_CSD `CARD_TYPE` would only say
what the part supports. The bdk values are 0 `EMMC_INIT_FAIL`, 1
`EMMC_1BIT_HS52`, 2 `EMMC_8BIT_HS52`, 3 `EMMC_MMC_HS200` and 4
`EMMC_MMC_HS400`. HS400 on a bus narrower than 8 bits sets `xc_emmc_mode`
to WARN "HS400 active but bus %d-bit" and prints `(HS400 active but bus
narrow!)` in `COL_ERR`. Anything else is PASS, `(consistent)`. The line is
`eMMC HS400    : mode=<n>, bus=<n>-bit`, and any width but 8 or 4 prints
as 1. A session that fell back to HS200 or HS52 reads as consistent: a
narrow bus is `emmc_bus`'s FAIL (any width but 8), and failed transfers
are `emmc_errors`'.

## Rows

`g_dx_macros` in verdict.c is the table of rows. Each `struct dx_macro`
has a padded name, a NULL-terminated key list, a `group_mask` and
`require_all_groups`. Key order matters: it decides which reason a row
shows.

| Row | Runs when | Keys | GUI category |
|---|---|---|---|
| Boot integrity | Power and Storage | `soc_pmic_otp`, `fuses_pkg1` | System and security |
| Identity | Identity | `kfuse` | System and security |
| PMIC | Power or Raw | `pmic_rails`, `pmic_nverc`, `pmic_irqsd`, `pmic_intlbt`, `max77812`, `max77621_cpu`, `max77621_gpu` | Power system |
| Charger | Power | `charger_id`, `charger_pg`, `charger_fault`, `usb_pd`, `xc_vbus_acok` | Charging |
| Battery | Power | `batt_health`, `batt_ntc`, `fuel_devname`, `xc_charge_dir` | Battery |
| Thermal | Power | `tmp451_id`, `soc_die_temp`, `pcb_temp`, `fan_stalled` | Temperature and fan |
| Storage | Storage | `emmc_health`, `emmc_bus`, `sd_bus`, `wlan_cal`, `cal0_hash`, `cal0_panel`, `cal0_model`, `gpt`, `prodinfo`, `xc_emmc_mode`, `emmc_errors`, `sd_errors`, `gc_asic` | Storage |
| CPU complex | Wireless | `cpu_complex` | Processor and memory |
| Wireless | Wireless | `bt_hci`, `wifi_pcie` | Wi-Fi and Bluetooth |
| Audio | Audio or Sound | `audio_codec`, `audio_clocks`, `audio_beep` | Sound |
| Display | Display | `dsi_id`, `backlight` | Screen |
| Inputs | Inputs | `touch_id`, `touch_ito`, `als_id` | Touch and sensors |
| Memory | Memory | `dram_sym`, `plls` | Processor and memory |

Boot integrity needs both groups because the PMIC OTP check is a Power
probe and the burnt-fuse/pkg1 check a Storage one. `kfuse` has its own row
because `probe_kfuse` is an Identity page. Under the Storage row, an
Identity-only run would lose a CRC failure, and a Storage run would claim
a check it never ran. PMIC takes either group: the rails and the MAX77812
come from Power, the latches from `probe_reset` in Raw state. CPU complex
stands alone so that a dead CPU rail reads as that, not as a Wi-Fi fault;
HOS needs the same cluster from its first stage.

| Key | Set by | Can be | GUI name |
|---|---|---|---|
| `soc_pmic_otp` | `probe_pmic` | pass, fail | Power chip model |
| `fuses_pkg1` | `probe_boot0_pkg1` | pass, warn, fail | System version fuses |
| `kfuse` | `probe_kfuse` | pass, warn, fail | Security fuses |
| `pmic_rails` | `probe_regulators` | pass, warn, fail | Internal voltages |
| `pmic_nverc` | `probe_reset` | pass, warn, fail | Last shutdown |
| `pmic_irqsd` | `probe_reset` | pass, fail | Power warnings |
| `pmic_intlbt` | `probe_reset` | pass, warn | Low battery or heat warning |
| `max77812` | `probe_max77812`, Mariko only | pass, fail | Memory and graphics power chip |
| `max77621_cpu` | `probe_pmic`, Erista only | pass, fail | Processor power chip |
| `max77621_gpu` | `probe_pmic`, Erista only | pass, fail | Graphics power chip |
| `charger_id` | `probe_charger` | pass, warn, fail | Charging chip |
| `charger_pg` | `probe_charger` | pass, fail | Charger power input |
| `charger_fault` | `probe_charger` | pass, fail | Charging faults |
| `usb_pd` | `probe_usbpd` | pass, fail | USB-C power chip |
| `xc_vbus_acok` | cross-check A | pass, fail | USB-C power detection |
| `batt_health` | `probe_battery` | pass, warn | Battery wear |
| `batt_ntc` | `probe_battery` | pass, warn, fail | Battery temperature |
| `fuel_devname` | `probe_battery` | pass, fail | Battery level chip |
| `xc_charge_dir` | cross-check B | pass, warn | Charge reaching the battery |
| `tmp451_id` | `probe_thermal` | pass, fail | Temperature sensor |
| `soc_die_temp` | `probe_thermal` | pass, warn, fail | Processor temperature |
| `pcb_temp` | `probe_thermal` | pass, warn | Board temperature |
| `fan_stalled` | `probe_fan` | pass, warn, fail | Fan |
| `emmc_health` | `probe_emmc_health` | pass, warn, fail | Internal storage wear |
| `emmc_bus` | `probe_storage`, `emmc_back_to_user` | pass, fail | Internal storage connection |
| `sd_bus` | `probe_storage` | pass, warn | SD card connection |
| `wlan_cal` | `probe_serial` | pass, fail | Wi-Fi factory settings |
| `cal0_hash` | `probe_serial` | pass, fail | Factory data |
| `cal0_panel` | `probe_serial` | pass, warn | Original screen |
| `cal0_model` | `probe_serial` | pass, warn | Original storage |
| `gpt` | `probe_gpt` | pass, fail | Storage layout |
| `prodinfo` | `probe_serial` | pass, warn | Factory data |
| `xc_emmc_mode` | cross-check C | pass, warn | Storage speed |
| `emmc_errors` | `probe_storage_errors` | pass, fail | Internal storage reads |
| `sd_errors` | `probe_storage_errors` | pass, warn, fail | SD card reads |
| `gc_asic` | `probe_gamecard` | pass, warn, fail | Game card reader |
| `cpu_complex` | `probe_wifi_link` | pass, fail | Main processor |
| `bt_hci` | `probe_bt_radio` | pass, warn, fail | Bluetooth |
| `wifi_pcie` | `probe_wifi_pcie`, `probe_wifi_link` | pass, warn, fail | Wi-Fi |
| `audio_codec` | `probe_audio` | pass, warn | Audio chip |
| `audio_clocks` | `probe_audio_clocks` | pass, fail | Audio system |
| `audio_beep` | `probe_audio_beep` | pass, fail | Sound playback |
| `dsi_id` | `probe_display` | pass, fail | Screen connection |
| `backlight` | `probe_backlight` | pass, warn | Backlight |
| `touch_id` | `probe_touch` | pass, fail | Touchscreen chip |
| `touch_ito` | `probe_touch` | pass, warn, fail | Touch panel self-test |
| `als_id` | `probe_als` | pass, fail | Light sensor |
| `dram_sym` | `probe_dram` | pass, fail | Memory |
| `plls` | `probe_clocks` | pass, fail | System clocks |

The conditions behind each key are in its probe's note:
[soc](soc.md), [power](power.md), [memory](memory.md),
[storage](storage.md), [gamecard](gamecard.md), [wireless](wireless.md),
[audio](audio.md), [display](display.md), [inputs](inputs.md) and
[raw-state](raw-state.md).

## Aggregation

`probe_verdict` runs the cross-checks first, since they write the `xc_*`
keys, then prints `[Verdict]` and walks the rows in table order:

1. A row with `require_all_groups` runs only if every group in its
   `group_mask` was selected; any other row needs one. A row that does not
   run is neither printed nor counted.
2. Each key goes through `dx_get`. Unset keys are skipped, and the rest
   are counted as pass, warn or fail.
3. The reason is the first WARN in key order, unless there is a FAIL: the
   first FAIL in key order replaces it for good. An empty detail falls
   back to the key's name. A row names one reason only: its other WARN
   and FAIL keys show only in the totals, so check the probe pages (or the
   GUI category, which lists every check) before calling a row a single
   fault.
4. The row reads `FAIL` in `COL_ERR` if anything failed, else `warn` in
   `COL_WARN`, else `pass` in `COL_OK`. With nothing recorded it reads
   `not tested` in `COL_WARN`. The line is `<name> : <state> (<reason>)`,
   with no brackets when there is no reason.
5. After a blank line, `Verdict      : <p> pass, <w> warn, <f> fail`, in
   `COL_ERR`, `COL_WARN` or `COL_OK` by the same rule. A run that recorded
   nothing comes out green. `probe_backlight` (Display) and `probe_uart_b`
   (Raw state) print `Verdict      :` lines of their own; those are the
   conclusions of those sections, and the overall verdict is the one under
   `[Verdict]` on Diagnostics.

The totals count keys, not rows, and only keys in rows that ran. That
matters for Boot integrity, the only all-groups row. In a Power-only run
`soc_pmic_otp` is recorded but its row is skipped, so a PMIC OTP failure
is missing from the verdict and the totals, while the Power page still
prints its `CID4` line in red. The same goes for `fuses_pkg1` in a
Storage-only run. PMIC is the opposite case. It runs on Power alone or Raw
alone, the keys that did not run are simply not counted, and nothing marks
the row as partial.

A probe that returns early without `dx_set` is invisible here; only its
own page says what happened:

- A TMP451 that does not answer leaves out `soc_die_temp` and `pcb_temp`.
  A BQ24193 that does not answer leaves out `charger_pg` and
  `charger_fault`.
- `fuses_pkg1` needs a pkg1 the probe recognises. The `cal0_*` keys need
  the CAL0 body to have been read, and `cal0_model` and `cal0_panel` a
  model or panel the probe can judge.
- `sd_bus` needs the SD card to initialise. A failed eMMC init sets no
  `emmc_bus`; it shows up in `emmc_errors`, because bdk's
  `emmc_initialize` counts each failed attempt. `gpt` stays unset when
  its read fails.
- `dsi_id` is skipped when the panel id is unknown, which happens in a GUI
  started by an older hwtest.bin.
- `cpu_complex` is missing when the CPU stub was not built or the
  bring-up did not complete. `audio_beep` is missing after either of its
  "Skipped" exits.
- `max77812` is never set on Erista, and the `max77621_*` keys exist only
  there.

Some probes never record a key: `probe_soc`, `probe_fuses`,
`probe_pmic_gpios`, `probe_5v`, `probe_partitions`, `probe_sd_content`,
`probe_joycon`, `probe_inputs`, `probe_gpio_census`, `probe_uart_b` and
`probe_pmc_scratch`. `probe_autorcm` records only through
`emmc_back_to_user`. A red line reaches the verdict only if its probe
records a key for it. A `PMC RST_STATUS` of WATCHDOG or SENSOR on Raw
state, for example, prints in red and sets nothing, so read the pages even
when the verdict is clean.

## In the GUI

Each result category in gui_text.c (`g_gui_cats`) covers one or two
verdict rows, as in the Rows table. `_row` finds a row by its name
without the padding (a prefix match followed by a space or the end).
`_row_ran` applies exactly the rule `probe_verdict` uses to decide that a
row ran.

A finding is untested (unset), ok (PASS), fail (FAIL) or, for a WARN,
soft or warn. It is soft when the detail starts with the key's soft
prefix: `prodinfo` "no keys", `bt_hci` "inconclusive", `touch_ito` "not
run". A soft WARN means only "not checked". `_cat_walk` takes the worst
state over the category's rows, ranking untested, ok, soft, partial, warn,
fail. On screen ([gui](gui.md#results)) these read:

| State | Status | Mark colour |
|---|---|---|
| untested | "Not checked" | grey |
| ok | "Working normally" | green |
| soft | "Working normally" | grey |
| partial | "Partly checked" | grey |
| warn | "Worth a look" | yellow |
| fail | "Problem found" | red |

A row that did not run lists all its keys as untested and marks the
category skipped. A row that ran leaves unset keys out silently, so
without the CPU stub `cpu_complex` simply vanishes from "Processor and
memory". A skipped row turns an otherwise ok or soft category into
partial. That works only per row, so a Power-only run shows "Power
system" as fine, with the latch keys neither checked nor marked.

The first screen counts categories in fail or warn; partial and untested
count for neither. The headline is "Problems found", "Mostly healthy" or
"All good". When `g_run_mask` is not `PROBE_G_ALL` it adds "A custom
check: what was not picked shows as not checked.", so "All good" can come
with categories never checked. Text totals count keys; this headline
counts categories. Categories are listed worst first, a soft one as
"Working normally". A category's screen lists warn and fail checks first,
with the gui_text.c sentences, or "This check failed." / "This check gave
a warning." when there is none. A soft check reads "Not checked on this
console." and an untested one "Not picked for this check.". The detail
follows as "Technical: <detail>", except on ok and soft checks.

Text and GUI part ways in four places:

- `pmic_irqsd` can FAIL, but its gui_text.c entry has no fail sentence,
  so the GUI shows the generic one.
- `cal0_hash` and `prodinfo` are both called "Factory data".
- A soft WARN still counts as warn in the text verdict and turns its row
  yellow. Without prod.keys the Storage row reads `warn (no keys)`, unless
  an earlier key supplies the reason. The GUI shows the same finding as
  working normally, with a grey mark.
- In a Power-only run, `soc_pmic_otp` shows as "Not picked for this
  check." because its row did not run. The finding is still recorded,
  though, so a failure still carries its "Technical:" line.

No category lists the Diagnostics page. The cross-check lines and the text
verdict are reachable only through the Results "Report" button, which
lists every captured page in capture order. Diagnostics is therefore last
there, although the text pager opens on it. That view stops past 28000 px
with "The rest is in the report saved on the SD card.", and Diagnostics,
being last, is the first page it loses; the saved file, which starts with
it, is then the only place to read it.

## Colours

`ui_theme.h` sets `COL_OK` green `0xFF96FF00`, `COL_WARN` yellow
`0xFFFFDD00`, `COL_ERR` red `0xFFFF5050`, `COL_INFO` cyan `0xFF00DDFF`
for `HEADER()` section titles and `COL_HEADER` orange `0xFFFF8000` for page
boundaries. Colour reaches only the LCD and the GUI's page capture, where
red, cyan and orange become `FF3C28`, `00FFC7` and `FF9F43`. UART and the
SD report are plain text, so there the state words carry the severity. On
the LCD a rendered line wider than 80 columns folds onto an indented
continuation; UART and the report keep it whole.

A line's colour does not always match its key's severity, so do not read
red as fail. `xc_emmc_mode` prints in `COL_ERR` but is a WARN. `dram_sym`,
`emmc_bus` and `fuel_devname` print in `COL_WARN` but are FAILs. A `not
tested` row is yellow.

## Left out on purpose

There is no agreement check between the SoC, PCB and battery
temperatures. The code's comment gives the reason: the sensors measure
different points with different thermal masses, and in a cold RCM boot
they really do disagree. What each probe chose not to judge (heat and
battery wear that never fail, the `charger_fault` mask, `pmic_irqsd`, the
MAX77812 status bytes, the PLLs `plls` leaves out) is in its own note:
[power](power.md), [raw-state](raw-state.md) and [memory](memory.md).
