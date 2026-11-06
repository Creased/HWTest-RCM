# Memory and clocks

Two probes share the "Memory & clocks" name. `probe_dram` asks the DRAM
chips what they are; `probe_clocks` dumps part of CLK_RST (the clock and
reset controller, `0x60006000`), checks that nine PLLs are locked
whenever they are enabled (PLLM exempt) and measures six clock rates.
Each probe has its own LCD page; over UART both print the same
`--- Memory & clocks ---` boundary, so the host viewer shows them as one
pane. Together they are the `PROBE_G_MEMORY` group ("Memory / clocks" in
the text payload's custom menu, "Memory and clocks" in the GUI), and they
run third in a sweep, after Identity and Power and before Storage.

Both front ends run the group at the payload's clocks. The text payload
selects `BPMP_CLK_NORMAL` (408 MHz in bdk's comment) before its sweep. The
GUI calls `gui_clock_fast(false)` first, which puts the BPMP back to
408 MHz and the DRAM back to its 204 MHz boot clock, so that its report
reads as the text payload's does; how the GUI runs outside the sweep is in
[internals](internals.md#the-guis-clocks).

Almost everything here is printed for a person to read or to compare
between consoles. Only two things are judged: whether the two halves of
each DRAM chip's reply agree (`dram_sym`, which as built cannot fail, see
[below](#the-channel-symmetry-check-dram_sym)) and whether any enabled PLL
is unlocked (`plls`).

## DRAM identity

`probe_dram` prints `[DRAM identity]` and goes through these steps in
order:

1. **Type**: `LPDDR4X (Mariko)` when the major nibble of
   `APB_MISC_GP_HIDREV` (bits 7:4) is 2 or more, otherwise
   `LPDDR4 (Erista)`, the same test bdk's `hw_get_chip_id` makes. Nothing
   is read from the DRAM for this line: it names the SoC generation and
   the memory standard that goes with it, and the code's comment notes
   that naming is the only practical difference between the two here.
2. **Fuse DRAM ID**: `fuse_read_dramid(true)`, the raw id burnt into fuse
   ODM4 (bits 7:3, extended with bits 14:12 on Mariko). `true` skips the
   clean-up bdk applies when it uses the id to choose its DRAM parameter
   set (out-of-range ids replaced by a default, the 8 GB override), so the
   number is what the fuses hold. It is printed only: not decoded to a
   part, and not compared with what the chips report. bdk's id-to-part
   tables are the `sdram_ids_erista` and `sdram_ids_mariko` enums in
   `bdk/mem/sdram.h`.
3. **Then four mode-register reads**, which print nothing: calls to
   bdk's `sdram_read_mrx` for `MR5_MAN_ID`, `MR6_REV_ID1`, `MR7_REV_ID2`
   and `MR8_DENSITY`, the same four reads Nyx's console info makes. Each
   call drains `EMC_MRR` (16 reads, 1 us apart), fills its result with
   `0xFF`, writes a rank 0 MRR command to `EMC_MRR` and waits for
   `EMC_STATUS_MRR_DIVLD`. Chip 0 is then taken from `EMC_CH0(EMC_MRR)`
   and, only when `EMC_FBIO_CFG7` bit 2 says the board is dual-channel,
   chip 1 from `EMC_CH1(EMC_MRR)` (bdk: "Each EMC channel is a RAM chip
   module"). If `EMC_ADR_CFG` bit 0 reports a second rank, the read is
   repeated for rank 1. Each wait gives up after 1000 polls 1 us apart and
   bdk drops the error, so a read that never completes returns whatever
   the register holds rather than failing.

Each chip (0, then 1) then gets three lines, built from the `rank0_ch0`
field of each register only. The maker and density lines each have a
decoded and an unknown form; the revision is MR6 and MR7 in hex, MR7 as
two digits. The `Channel sym` line, described
[below](#the-channel-symmetry-check-dram_sym), closes the page.

    Chip 0 vend  : <maker>
    Chip 0 vend  : Unknown (id <MR5>)
    Chip 0 rev   : <MR6>.<MR7>
    Chip 0 dens  : <density>
    Chip 0 dens  : Unknown (MR8 0x<MR8>)

| MR5 | Maker |
|---|---|
| 1 | Samsung |
| 5 | Nanya |
| 6 | Hynix |
| 8 | Winbond |
| 19 | CXMT |
| 255 | Micron |

| MR8 bits 5:2 | Density string |
|---|---|
| 2 | 512MB |
| 3 | 768MB |
| 4 | 1GB |
| 5 | 1.5GB |
| 6 | 2GB |

hwtest names three makers (5, 8, 19) that Nyx's own table has commented
out. The density strings are Nyx's, but Nyx prints each one after a count
(`<n> x 1GB`): the number of ranks whose replies agree times the number of
channels. hwtest prints the bare entry, the density in one MR8 reply (the
chip's `rank0_ch0` byte), which is the unit Nyx multiplies; so
`Chip 0 dens` is not that chip's capacity and the two lines do not add up
to the installed RAM.

All three lines of a chip are green when both its maker and its density
decode, and yellow otherwise. That is colour only: an unknown maker or
density sets no verdict key.

When `EMC_FBIO_CFG7` reports a single channel, chip 1 is never read and
keeps the fill pattern, which prints as maker `Micron` (id 255), revision
`FF.FF` and `Unknown (MR8 0xFF)`, in yellow. That is `0xFF`, not a part.

MR4 (per-die refresh rate and thermal class) is deliberately not read. The
code's comment gives the reason: in the RCM/BPMP context the MR4 read does
not return a trustworthy live value, so any threshold on it only produces
false signals. Catching marginal DRAM needs a trained controller and a
real read/write stress pass, which is the [RAM test](ramtest.md), not
this single passive read.

## The channel symmetry check (`dram_sym`)

For each chip the code compares the `rank0_ch0` and `rank0_ch1` fields of
MR5 and MR6 (MR7 and MR8 are not compared) and prints

    Channel sym  : chip0 OK|MISMATCH, chip1 OK|MISMATCH

in green when both chips match, yellow otherwise. `dram_sym` is PASS when
both match and FAIL with "channel asymmetry" when either does not; it never
warns. The comment above the check speaks of rank 0 against rank 1 and of
the channels agreeing; the code makes only the channel comparison, on two
fields of the same rank 0 read.

As built, the check cannot fail. bdk fills both fields from one read of
the chip's `EMC_CHn(EMC_MRR)`: `rank0_ch0 = mrr & 0xFF` and
`rank0_ch1 = (mrr & 0xFF00 >> 8)`. `>>` binds tighter than `&`, so the
second is `mrr & 0xFF` too, and both fields always hold the same byte (a
chip that was not read has `0xFF` in both). Every run therefore prints
`chip0 OK, chip1 OK` and sets PASS; the MISMATCH line, the FAIL and the
GUI's text for it are unreachable, and `plls` is the only key that can
turn the Memory row red. The rank 1 fields have the same parse, and
nothing in hwtest uses them.

## Clock registers

`probe_clocks` first prints `[Clocks]`: ten raw CLK_RST words in hex. The
comment explains why they are not decoded: getting exact MHz would mean
replaying the full PLL maths, and the raw values are what matters when
comparing two consoles, or one console before and after a configuration
change.

| Label | Offset | bdk name | Holds |
|---|---|---|---|
| `PLLP_BASE` | `0xA0` | `CLK_RST_CONTROLLER_PLLP_BASE` | PLLP's base: enable, lock, dividers |
| `PLLP_OUTA` | `0xA4` | none | |
| `PLLP_OUTB` | `0x68` | none (see below) | |
| `SCLK_BURST` | `0x28` | `CLK_RST_CONTROLLER_SCLK_BURST_POLICY` | the system clock's (SCLK, the BPMP's) source; `bpmp.c` lists the choices |
| `SUPER_SCLK` | `0x2C` | `CLK_RST_CONTROLLER_SUPER_SCLK_DIVIDER` | SCLK's divider (`hw_init` sets it to 1) |
| `CLK_SYSTEM` | `0x30` | `CLK_RST_CONTROLLER_CLK_SYSTEM_RATE` | the bus dividers under SCLK (`bpmp.c`: "S1:H1:P3") |
| `CLK_OUT_ENB_L` / `_H` / `_U` | `0x10` / `0x14` / `0x18` | `CLK_RST_CONTROLLER_CLK_OUT_ENB_L` / `_H` / `_U` | clock-enable bits, one per device |
| `CLK_OUT_ENB_X` | `0x280` | `CLK_RST_CONTROLLER_CLK_OUT_ENB_X` | clock-enable bits, one per device |

A bit that differs in a `CLK_OUT_ENB` word between two consoles is a
device clocked on one and not on the other.

The line labelled `PLLP_OUTB` reads offset `0x68`. bdk defines
`CLK_RST_CONTROLLER_PLLP_OUTB` at `0xA8` (and writes it in `hw_init`) and
has no name for `0x68`, which follows `PTO_CLK_CNT_STATUS` (`0x64`). Treat
that value as an unnamed word, not as PLLP's B outputs.

## PLL lock status (`plls`)

`[Clocks - PLL lock status]` reads each PLL's base register once. The
comment cites the Tegra X1 TRM for the layout: bit 30 is ENABLE and bit 27
is LOCK, set by the hardware once the loop has locked (bdk's
`PLL_BASE_ENABLE` and `PLL_BASE_LOCK`; `dpout.c` uses the same bits for
PLLD2 and PLLDP). An enabled PLL that has not locked is a loop still
hunting for its frequency, and the comment treats that as always wrong; it
associates it with consoles that boot and then hang after a few seconds.

The last column is what the code (hwtest's or bdk's) runs from each PLL,
so it is where an unlocked one points.

| PLL | Base | Used for |
|---|---|---|
| PLLC | `0x80` | the BPMP's boost clocks (`bpmp_clk_rate_set`, which disables it again at `BPMP_CLK_NORMAL`) |
| PLLM | `0x90` | the DRAM clock (EMC) |
| PLLP | `0xA0` | SCLK, so the BPMP, at 408 MHz; the EMC at 204 MHz (PLLP/2, per the comment) |
| PLLA | `0xB0` | audio: MCLK and the I2S bit clock (beep probe) |
| PLLU | `0xC0` | USB (the GUI's USB serial console) |
| PLLD | `0xD0` | the built-in panel (bdk sources `CLK_SOURCE_DISP1` from `PLLD_OUT0`) |
| PLLX | `0xE0` | the A57 cluster (the Wi-Fi link probe's CPU0 bring-up) |
| PLLD2 | `0x4B8` | the DisplayPort pixel clock (`dpout.c`, Dock Test) |
| PLLDP | `0x590` | the DisplayPort SOR macro's reference (`dpout.c`, Dock Test) |

| State | Printed | Colour | Counted |
|---|---|---|---|
| enabled, locked | `ENABLED, LOCKED` | green | no |
| enabled, unlocked | `ENABLED, NO-LOCK (loop is hunting)` | red | yes |
| PLLM enabled, unlocked | `ENABLED, NO-LOCK (transient - DRAM on PLLP)` | grey | no |
| not enabled | `disabled` | grey | no |

PLLM is exempt because, per the comment, hekate enables it as a candidate
DRAM clock while the EMC may still be running from PLLP/2 when the probe
samples; PLLM hunting while it does not feed the EMC is harmless. The code
does not check which source the EMC is actually on: the suffix is printed
whenever PLLM is enabled and unlocked.

The PCIe reference PLL at `0x4C4` (labelled PLLRE in the comment, PLLREFE
in `probe_wifi.c` and the CPU0 stub, which take its lock from the MISC
register at `0x4C8` rather than from its base) is left out. Only the Wi-Fi
probe enables it, and reports it there; listing it here produced spurious
`ENABLED, NO-LOCK` lines on any run that followed a PCIe bring-up.

`plls` is FAIL with "N enabled but not locked" when any listed PLL other
than PLLM is enabled and unlocked, and PASS otherwise. It never warns.

## Decoded rates

`[Clocks - decoded rates]` is its own section so that the host viewer
shows the rates as a separate table beside the raw dump.

`OSC` comes from bdk's `clock_get_osc_freq`. It starts `OSC_FREQ_DET` over
two periods of the 32.768 kHz clock, waits for the busy bit to clear, reads
the count and writes `OSC_FREQ_DET` back to 0. The count is matched against
fixed windows for 12, 13, 16.8, 19.2, 26, 38.4 and 48 MHz and the nominal
value of the matching window is returned, or 0 when none matches. The line
is therefore a classification, never a measurement: it cannot show crystal
drift, and a miss prints `0.000 MHz`. bdk's `hw_init.c` describes the
console's oscillator as a 38.4 MHz crystal; the probe does not compare.

The other six lines come from bdk's `clock_get_dev_freq`, which uses the
PTO (pulse tag observer) counter:

| Label | PTO source | bdk id |
|---|---|---|
| `PLLP_OBS` | `0x43` | `CLK_PTO_PLLP_OBS` |
| `SCLK / BPMP` | `0x1C` | `CLK_PTO_SCLK` |
| `EMC (DRAM)` | `0x24` | `CLK_PTO_EMC` |
| `SDMMC4(eMMC)` | `0x23` | `CLK_PTO_SDMMC4` |
| `SDMMC1(SD)` | `0x20` | `CLK_PTO_SDMMC1` |
| `CCLK_G(A57)` | `0x12` | `CLK_PTO_CCLK_G` |

It programs `PTO_CLK_CNT_CNTL` with the source, divide-by-1 and a window
of 16 periods of the 32.768 kHz clock (about 488 us), resets and starts
the counter, sleeps for the window, waits for `PTO_CLK_CNT_BUSY` to clear,
reads the 24-bit count and writes the control register back to 0. The rate
is count x 32768 / 16 / 1000 kHz, so one count is 2.048 kHz and that is
the resolution. A result of 0 prints `(idle / not clocked)`.

`CCLK_G` is the A57 cluster's clock and the comment expects it to read 0
unless the cluster has been started; the only code that starts it is the
Wi-Fi link probe, which runs after this group. (The comment mentions a
"(BPMP idle)" caveat on that line; the code prints the generic
`(idle / not clocked)` for any clock that reads 0.)

No rate is compared with an expected value: the sweep's 408 MHz BPMP and
204 MHz DRAM clocks are context for reading `SCLK / BPMP` and
`EMC (DRAM)`, not a check.

## What it changes on the console

`probe_dram` issues mode-register reads only: up to eight MRR commands (4
registers, 1 or 2 ranks each) and 16 drain reads per register. DRAM
contents and the EMC configuration are untouched, so there is nothing to
restore. `probe_clocks` runs one `OSC_FREQ_DET` measurement and six
`PTO_CLK_CNT_CNTL` measurements, each leaving its control register at 0
rather than at its earlier value; the register dump and the PLL check only
read. No PLL, clock or reset is enabled or disabled.

The `OSC_FREQ_DET` and PTO busy-waits have no timeout: a counter that never
finishes stops the probe there. The MRR wait does time out, silently, as
described above.

## Verdict and the GUI

Both keys feed the verdict's `Memory` row, which runs whenever
`PROBE_G_MEMORY` was selected. It prints `pass`, or `FAIL` with the failing
key's detail in brackets, for example `FAIL (1 enabled but not locked)`.
Nothing else in the group is judged, and no cross-check in the verdict
looks at memory or clocks.

In the GUI the two keys are shown as:

| Key | Name | Text when it fails |
|---|---|---|
| `dram_sym` | Memory | "The two halves of the memory do not match. This points to a memory fault. Run the RAM Test." |
| `plls` | System clocks | "Some internal clocks did not start. The console may be unstable." |

The `plls` text says the clocks "did not start", but the condition is
enabled and unlocked; a PLL that never started reads `disabled` and passes.
Under the sentence the GUI prints `Technical: <n> enabled but not locked`;
which PLL it is shows only in `[Clocks - PLL lock status]` on the report
page. The `dram_sym` text cannot appear (see above). "Run the RAM Test"
points at the GUI's RAM Test window, which starts the separate
`hwtest_ram.bin` payload ([RAM test](ramtest.md)).

The `Memory` row is one of the two behind the GUI's "Processor and
memory" category; the other is `CPU complex`, and the category's report
pages are "Memory & clocks" and "Wireless". Its all-clear sentence, "The
processor starts and the memory is consistent.", has nothing that can
fail behind its memory half: the `Memory` row turns red only through
`plls`. `CPU complex` is set by the Wi-Fi link probe, so a custom check
that picks Memory and clocks without Wi-Fi and Bluetooth reads "Partly
checked" even when everything that ran passed (the rule is in
[verdict](verdict.md#in-the-gui)). The category's readings take four
lines from `[DRAM identity]`: "Memory" is the Type line, "Memory maker" is
`Chip 0 vend` (chip 1's maker is not shown), and "Memory chip 1" and
"Memory chip 2" are `Chip 0 dens` and `Chip 1 dens` (the GUI counts from
1, the report from 0). Those two show the one-reply density described
above, not the chip's size. The fuse id, the revisions, the symmetry line
and every clock line are seen only on the report page.

## Reading it, and what it cannot tell

The group samples whatever earlier code left. On a first sweep it runs
before Storage, Wireless and Audio. A re-run sees what they did: the beep
probe reprograms PLLA and leaves it enabled, and the Wi-Fi link probe
enables PLLX, which nothing in hwtest or bdk disables. (The GUI's Dock
Test also enables PLLD2 and PLLDP, but clears their enables again in its
teardown.) A PLL that reads `disabled` on one pass and `ENABLED, LOCKED`
on the next is that history, not a fault.

In the text payload every LCD view of either page is such a re-run:
`render_page` calls the page's probe, so paging (`n`, `p`, space, `b`,
VOL+/VOL-) re-runs it just as `r`, `a` and `G` do (keys in the
[README](../README.md#running)). The LCD therefore always shows a
post-sweep sample, while the sweep's UART dump and the report saved from
it hold the in-sweep one, and each view sets `dram_sym` and `plls` again
for the Diagnostics page, which re-reads them when it is shown. In the
GUI a second health check is a re-run. Two GUI-only things also come
before every GUI sweep: on Erista with the Minerva module found, its DRAM
switches up to 1600 MHz program and enable PLLM or PLLMB (PLLMB is not
listed here), and nothing clears that when the DRAM drops back to
204 MHz; and while the USB serial console is the debug console it keeps
PLLU enabled, so the PLLU line can read differently from the text
payload's.

`SDMMC1(SD)` and `SDMMC4(eMMC)` depend on who set up storage. The text
payload initialises the SD card and eMMC before its sweep only when
Storage is selected, so a Memory-only custom run has not initialised them
for the sweep (with a card in, the boot-time look for `hwtest/gui.bin`
mounts it and ends it again, which gates SDMMC1's clock; the eMMC is
untouched); the GUI initialises both once per boot whatever is selected.
The code states no expected value for either case.

Beyond the limits given above, it cannot tell:

- whether the DRAM works, or how warm it is: it reads identity registers
  and never touches memory cells, which is the RAM test's job;
- how many ranks a chip has or whether they agree: bdk reads rank 1, but
  hwtest never looks at those fields (Nyx compares rank 0 with rank 1);
- a difference between the two chips: chip 0 and chip 1 are never
  compared, so a mismatched maker or density shows only by reading the
  two `vend` and `dens` lines side by side;
- the DRAM's supply: the MAX77620's `SD1` (and on Mariko the MAX77812)
  is read by the Power group, see [power](power.md); in the GUI it is the
  "Memory supply" reading under "Power system";
- a PLL that should be running but is disabled, a PLL in bypass (bit 31,
  bdk's `PLL_BASE_BYPASS`, is not read), the state of the lock-detect
  enable PLLD2 and PLLDP have in their MISC registers
  (`PLLSS_MISC_EN_LCKDET` in `dpout.c`), a PLL not in the list (PLLC4,
  PLLE, PLLMB, PLLREFE among them), or a lock that comes and goes between
  samples (each PLL is read once).

The rest of the report is described in the [README](../README.md).
