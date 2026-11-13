# Game card reader

The game card slot is not wired to the SoC. A separate ASIC sits between
them (switchbrew calls it Lotus3; the board marking is `GCBRG`), and the
Tegra sees it as an MMC device on SDMMC2 (`0x700B0200`). Horizon's FS then
drives the cartridge through it with vendor commands CMD60-63 layered on
top of MMC.

`probe_gamecard()` answers the one question a repair needs from that chain:
is the reader controller, the ASIC, alive? It powers the ASIC, brings SDMMC2
up, reads one block and checks that the block could only have come from a
device. It is the last page of the Storage group, after
`probe_storage_errors`; the eMMC and SD error counters printed there do not
include game card traffic ([storage](storage.md)).

## What it never does

The ASIC's bootrom accepts exactly one operation, SendFirmware, and loading
a firmware blob burns OTP fuses to enforce anti-downgrade. That cannot be
undone, so the probe stops as soon as the ASIC has demonstrably driven the
bus and leaves everything past that point alone:

- No vendor command (CMD60-63) and no SendFirmware, ever.
- No cartridge access. No command reaches a cartridge, and the only
  cartridge-facing signal it reads is the card-detect line; nothing later
  depends on it: the ASIC test runs the same with or without a cartridge.
  It does power the cartridge interface: LDO5 is switched on, but only
  after its readback is exactly 1.800 V (step 3), and it is put back in
  the teardown.
- `PowGc` (PE5, pad 0x07 in switchbrew's GPIO pad table) is never driven.
  Horizon never raises it: boot configures it as an output at 0 and no
  sysmodule, FS included, touches it again, so the pad stays as the
  console's boot code set it.
- No CMD0 and no 400 kHz identification phase (step 6).

## How it runs

The bring-up is FS's own, read out of the extracted sysmodule rather than
guessed, because guessing produced a silent bus for a long time: earlier
versions of this probe brought the rails up in the wrong order, drove
`PowGc` high, and let a 1-bit identification-speed init claim the
controller first. FS's gamecard path touches exactly four devices, the two
GPIOs and the two rails used here, and writes no pinmux and no clock
registers; the pinmux writes below are the probe's own.

1. **Card detect, before any rail.** Slot pin 2 (CD#) is strapped to
   ground inside the cartridge shell and wired straight to the connector,
   so it reads low with a cartridge seated even with the gamecard rails
   off. That separates "empty slot" from "dead reader" before anything is
   switched on. The source comment records this as confirmed on hardware
   both ways: 0 with a cartridge in, 1 with the slot empty. The pad is PS3
   (`PINMUX_AUX_CAM_I2C_SDA`), which boots muxed to I2CVI and has to be
   reclaimed. The probe saves its pinmux, sets
   `PINMUX_INPUT_ENABLE | PINMUX_PULL_UP | PINMUX_TRISTATE`, makes PS3 a
   GPIO input, waits 200 us, reads the pin and puts the pinmux straight
   back. The GPIO writes are masked (`GP_MWR`, one store of
   `(pins << 8) | value`, so only PS3 moves) because port S also carries
   the fan tach (PS7) and the charger status lines (PS1, PS6).

2. **Hold the ASIC in reset.** GameCardReset is PBB3 (switchbrew pad 0x05,
   `PINMUX_AUX_GPIO_X1_AUD`). FS drives it to 1 to run the ASIC and to 0
   to hold it in reset; boot leaves it at 0 from cold. The probe saves the
   pinmux, sets `PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN`, then writes
   OUT = 0, CNF = 1, OE = 1 in that order, masked, because PBB4 next to it
   is the codec's alert line. Reset is now held by a driven output.

3. **Rails, LDO3 then LDO5.** Horizon's `pcv` service names both MAX77620
   LDOs "GcCard". LDO3 is the board-side 3.1 V gamecard source and LDO5
   the 1.8 V interface supply. LDO3 is not the Lotus3 silicon core rail:
   the bridge has lower internal domains. FS's `SetGcPower` enables LDO3,
   then LDO5, then settles 1.5 ms, and the order matters. The probe saves
   the raw `LDO3_CFG` (`0x29`) and `LDO5_CFG` (`0x2D`) bytes and whether
   each rail already reports power-OK, then:
   - sets LDO3 to 3.100 V and enables it;
   - sets LDO5 to 1.800 V, reads `LDO5_CFG` back, decodes it as
     `(cfg & 0x3F) * 50000 + 800000` uV and enables LDO5 only if that is
     exactly 1.800 V. LDO5 boots at the PMIC's 3.1 V OTP default against a
     1.8 V interface, and enabling it as found would put 3.1 V on that
     interface with a game card possibly seated, so it is set down and
     checked rather than trusted;
   - sleeps 2 ms (`GC_POWER_SETTLE_MS`; FS uses 1.5 ms);
   - reads LDO3's power-OK bit, and LDO5's if it was enabled (POK in
     `LDOx_CFG2`, which is what `max77620_regulator_get_status()` returns;
     it is not the enable bit).

   If LDO3 does not report POK, or LDO5 was not enabled or does not report
   POK, the probe records the FAIL below and goes straight to the teardown:
   reset is never released and SDMMC2 is never started.

4. **Release reset and read it back.** PBB3 goes to 1; after 10 us the
   probe prints what the pin actually reads, then waits 50 ms. "Drove it
   and nothing answered" and "the pad never moved" are different faults,
   and only the readback separates them. The readback is printed, not
   judged: the probe carries on whatever it reads.

5. **SDMMC2 pads, Mariko only** (`hw_get_chip_id()` is T210B01). Here the
   SDMMC2 pads are ordinary muxed pads and nothing in RCM has configured
   them. BDK's bring-up only ORs the Schmitt bit into them
   (`_sdmmc_config_sdmmc2_schmitt()`), which cannot clear `TRISTATE` or
   `PARKED` nor set `INPUT_ENABLE`, while the Erista leg of the same switch
   un-parks its pad brick outright (`APB_MISC_GP_EMMC2_PAD_CFGPADCTRL` ANDed
   with `0xF8003FFF`). hekate never drives SDMMC2, so the gap had no user to
   notice it. The source comment records a measurement on a Mariko in RCM:
   CLK reads `0xA064` before this step, which has `PARKED` (bit 5) set, so
   the bus could never have come up.

   The probe writes HOS's own initial pad table for this board, decoded:
   CLK and CMD input-enabled with no pull, DAT0-7 input-enabled with a
   pull-up, and the DDR-only strobes CLKB, DQS and DQSB tristated with a
   pull-down. On CLK, CMD and DAT0-7 a plain write clears the function,
   tristate and park bits as a side effect; on the strobes it clears
   function and park and sets tristate. The three strobe offsets (`0x2B8`,
   `0x2C0`, `0x2C4`) are defined in the probe because the BDK header lacks
   them. It then reads `PINMUX_AUX_SDMMC2_CLK` back to commit the writes
   before printing `SDMMC2 pads : assigned (T210B01)`. On Erista the step
   is skipped; the pads get only what `sdmmc_init()` does to them (the
   un-park, then pad auto-calibration or its fallback drive values).

6. **Controller.** FS's Activate never runs this port at an identification
   clock: it brings it straight up 8-bit at 1.8 V at its own "GcAsicSpeed"
   (200 MHz from CAR divided by two), and the first thing on the bus is
   CMD21 tuning. BDK's `sdmmc_storage_init_gc()` does exactly that and is
   dead code in hekate with no callers; this probe is its only user. It
   runs `sdmmc_init(sdmmc, SDMMC_2, SDMMC_POWER_1_8, SDMMC_BUS_WIDTH_8,
   SDHCI_TIMING_MMC_HS100)`, waits 1 ms plus 10 card clocks, runs
   `sdmmc_tuning_execute()` with `MMC_SEND_TUNING_BLOCK_HS200` (CMD21, up
   to 256 iterations at HS100) and turns on card-clock power save. BDK sets
   the HS100 source clock to 199680 kHz and divides it by two, so the card
   clock it records, and the probe prints, is 99840 kHz.

7. **One real block.** Tuning completing proves nothing.
   `sdmmc_tuning_execute()` judges only `SDHCI_CTRL_TUNED_CLK`, and the
   Tegra's tuning state machine sets that after sweeping its taps whether
   or not anything answered; it throws away the per-iteration result that
   tracks data arriving. The source comment records it as measured: with
   the reader module (the code's name for the part of the reader that can
   be unplugged) unplugged, tuning "passes". So the probe asks for data:
   one CMD21 with an R1 response and a single 128-byte block (BDK's own
   tuning path uses 128 bytes on an 8-bit bus and 64 only on 4-bit), into
   a zeroed buffer. In BDK's command path this fails if the response
   does not arrive within 2 s, if the data does not complete within
   1.5 s, or if an error interrupt fires during either.

8. **Check the content.** CMD21 returns the JEDEC tuning block, whose
   8-bit form opens `FF FF 00 FF FF FF 00 00` and carries a mix of `0xCC`,
   `0x33`, `0xEE`, `0xDD`, `0xBB` and `0x77` further in. A host controller
   cannot invent that, so a match is device-sourced proof; all-00 or all-FF
   means the transfer completed locally and says nothing about anything
   out there. The probe requires the first eight bytes to match exactly
   and at least 16 of the 128 bytes to be neither `0x00` nor `0xFF`. Those
   bytes are counted, not compared one by one against the pattern.

9. **Teardown.** On every path that reached the controller init, pass or
   fail, `sdmmc_end()` stops the card clock, switches SDMMC2 I/O power off
   and disables the SDMMC2 clock. Then, on every path and in FS's order:
   reset low, 2 ms, LDO5 disabled if it was enabled here, reported POK,
   and was not reporting POK before (an LDO5 enabled without POK is put
   back by the `LDO5_CFG` write-back), LDO3 disabled if it was not
   reporting POK before, the saved `LDO5_CFG` and `LDO3_CFG` bytes written
   back, the PBB3 pinmux restored, and a final 100 ms. FS enforces 100 ms
   off before the next power-up; nothing in the probe re-enters, but the
   delay keeps that contract if the pager re-runs the page.

## What it changes and what it puts back

Put back:

- The PS3 pinmux (straight after the card-detect read) and the PBB3
  pinmux.
- `LDO3_CFG` and `LDO5_CFG`. Each byte holds both the voltage field and
  the power-mode bits 7:6 (BDK's `max7762x_regulator_enable()` writes the
  mode into `LDOx_CFG`), so the write-back, the last rail operation, is
  what leaves each rail at the voltage and power mode it was found in. The
  `CFG2` registers are only read.

Left as the probe set them:

- The GPIO configuration. PS3 stays a GPIO input. PBB3 stays a GPIO
  output driving 0, reset asserted, which is the level boot leaves on it
  from cold.
- On Mariko, the thirteen SDMMC2 pinmux writes of step 5.
- What `sdmmc_init()` does to the SDMMC2 pads on the way: the Erista
  un-park of `APB_MISC_GP_EMMC2_PAD_CFGPADCTRL`, the Mariko Schmitt bits,
  and BDK's fallback pad drive values if auto-calibration times out. Any
  SDMMC2 register state beyond what `sdmmc_end()` switches off also stays.

This is one of the active probes the [README](../README.md) warns about:
it changes gamecard state, and its teardown is not exact. It is also one
of the probes that can raise the MAX77620's low-battery and thermal
interrupt latches, which is why `probe_reset_snapshot()` reads those
latches once before the sweep starts ([raw-state](raw-state.md)).

## What it reports

A passing run prints these lines (format strings from the code, `N` for
the counted values; the pads line appears on Mariko only):

```
[Gamecard reader - Lotus3 ASIC on SDMMC2]
  Card detect  : cartridge seated
  Rails        : LDO3 on 3.100 V, LDO5 on 1.800 V
  GC reset     : PBB3 reads 1 (1 = running)
  SDMMC2 pads  : assigned (T210B01)
  ASIC         : returned data at 99840 kHz, 8-bit
  Data[0..7]   : FF FF 00 FF FF FF 00 00
  Tuning block : JEDEC pattern, N varied bytes of 128
  Result       : reader present and answering
```

- `Card detect` reads `slot empty` (pin 1) or `cartridge seated` (pin 0).
  It carries no verdict key.
- `Rails` is green when LDO3 reports POK and LDO5 both reads back
  1.800 V and reports POK; red otherwise. The voltages on it are fixed
  text, not readbacks. `LDO3 OFF` means LDO3 did not report POK after
  being enabled. `LDO5 OFF` has two causes the line does not separate: the
  readback was not 1.800 V, so the probe never enabled LDO5, or it was
  enabled and did not report POK.
- `GC reset` has no colour and no verdict. It is the only place a reset
  pad that never moved shows up.
- The failure lines, in yellow, are `ASIC : bus would not come up`,
  `ASIC : bus up, but no data returned` (followed by an uncoloured
  `(reader module absent, unseated or dead)`), and
  `Tuning block : not the JEDEC pattern (N varied bytes)`.
  `Data[0..7]` is printed before the content check, so it is there to read
  when the check fails.

The output stops at the failing step: a rails FAIL ends at `Rails`; the
two `ASIC` WARNs end at the `ASIC` line (the no-data one with its module
hint) with no `Data[0..7]`; no WARN prints `Result`.

The probe sets one key, `gc_asic`. Only the rails case is a FAIL; every
bus or data outcome is a WARN.

| Severity | Detail | Condition |
|---|---|---|
| FAIL | `gamecard rails would not come up` | LDO3 not POK after enable and the 2 ms settle; or the LDO5 readback was not 1.800 V; or LDO5 not POK after enable |
| WARN | `SDMMC2 bus did not come up` | `sdmmc_storage_init_gc()` returned non-zero: `sdmmc_init()` failed, or tuning did not set `SDHCI_CTRL_TUNED_CLK` |
| WARN | `no data from the reader on SDMMC2` | the CMD21 block read failed (response or data error or timeout) |
| WARN | `SDMMC2 data is not the tuning pattern` | first eight bytes not `FF FF 00 FF FF FF 00 00`, or fewer than 16 of the 128 bytes outside `0x00`/`0xFF` |
| PASS | (empty) | all of the above passed |

The key is absent when the Storage group did not run.

## Where it lands

On the Verdict page `gc_asic` is the last key of the `Storage` row, which
runs when the Storage group is selected (`PROBE_G_STORAGE`). The row takes
the worst severity of its keys, and its reason is the first FAIL's detail
or, failing that, the first WARN's, in key order. The row therefore
shows the game card's own text only when no earlier storage key has a
finding of the same or a worse severity ([verdict](verdict.md)).

In the GUI ([gui](gui.md)), `gc_asic` is the check "Game card reader" in
the "Storage" category. All three WARNs read "The game card reader did
not answer properly. Game cards may not be read."; the FAIL reads "The
game card reader could not be powered." There is no soft prefix, so no
WARN from this probe is downgraded to "not checked". The category's
all-good text, "Internal storage and SD card are healthy.", does not
mention the reader. Its readings include "Game card slot", taken from the
`Card detect` line (`slot empty` or `cartridge seated`); none of the ASIC
lines is shown as a reading. The GUI's Game Card screen reads the same
line live (see [The Game Card screen](#the-game-card-screen)).
In the GUI the group is "Storage and game card reader"; the text menu's
Custom list calls it "Storage / gamecard".

## What a failure points at

- **FAIL (rails):** the supply side. LDO3 or LDO5 did not report
  power-OK, or LDO5 would not take 1.8 V. The ASIC was never released
  from reset and the bus never started, so nothing is known about it.
- **Bus would not come up:** SDMMC2 itself did not initialise or lock
  (for example the controller's internal clock did not report stable
  within BDK's 2 s, or `SDHCI_CTRL_TUNED_CLK` stayed clear). Since tuning
  is recorded as completing even with the reader module unplugged, this
  WARN does not by itself point at the reader module.
- **No data returned:** the probe's own words cover it: the reader module
  is absent, unseated or dead. Read `GC reset` alongside it: 1 means the
  ASIC was released and did not answer, 0 means the reset pad never
  moved and the ASIC was never let out of reset.
- **Not the tuning pattern:** a transfer completed but the block is not
  the device's tuning pattern. `Data[0..7]` and the varied-byte count show
  what came back; all-00 or all-FF means the transfer completed locally
  and proves nothing about the ASIC.
- **`Card detect` reads `slot empty` with a cartridge in:** the slot's CD#
  contact, its line to PS3 (an open line reads 1 through the pull-up), or
  the strap to ground inside that cartridge's shell. It sets no verdict
  and does not affect the ASIC test.

## The Game Card screen

The GUI's Game Card screen (a tile on Home) reads the same
card-detect line live, every 100 ms, with the same pad and GPIO setup and
nothing powered, and says whether a cartridge is in the slot and how many
times one went in and came out. It never runs the ASIC test or anything
past the line; see [Game Card](gui.md#game-card).

## What it cannot tell

- Anything about a cartridge: whether a seated one can be read, or what
  it is.
- The ASIC's firmware state or version, or its anti-downgrade fuses.
- LDO3's actual voltage. Only LDO5 is read back; LDO3 is judged on POK
  alone.
- Absent, unseated and dead reader modules apart: one message covers all
  three.
- Whether the card-detect line itself is intact. The pad is read with
  its pull-up on, so an open line reads 1, the same as an empty slot.
- Bus quality beyond one 128-byte block at 99.84 MHz: there is no
  sustained transfer and no error-rate count.
- Anything that depends on `PowGc`.
