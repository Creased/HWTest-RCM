# The RAM test

`build-ram/hwtest_ram.bin` is for suspected DRAM and memory-bus faults. It
trains the DRAM to 1600 MHz, then writes and verifies almost all of it,
including RAM above 4 GiB, on Erista and Mariko alike, overwriting
everything it tests. It is a payload of its own because both memory
trainers and their tables "cannot fit inside the normal RCM image"
(`main.c`), and because retraining the DRAM kills anything running from
it, the GUI included (`gui/gui_ram.c`): there is no way back to hwtest
without a restart.

It sets no verdict keys (nothing in `ramtest/` calls `dx_set`), writes
nothing to the SD card, and no row of the GUI's results shows its outcome:
the results exist only on its own screens and on UART-B. The GUI's
"Memory" row, whose FAIL text ends "Run the RAM Test.", is the memory
probe's `dram_sym` ([memory](memory.md#the-channel-symmetry-check-dram_sym)).

## Starting it

`make ram` builds it (it is kept out of `all`) and `make sdzip` packs it
into `build/hwtest-sd.zip` as `bootloader/payloads/hwtest_ram.bin`, for
unzipping at the root of the card. It can be injected over RCM, or started
from the card by the text launcher's **RAM test** row or the GUI's **RAM
Test** window (Home's **RAM Test** button, then **Start**). Both call
`ram_payload_load` (`ui_menu.c`), which tries
`bootloader/payloads/hwtest_ram.bin`, `hwtest_ram.bin` and
`/hwtest_ram.bin` in that order, refuses a file under 0x100 bytes or over
0x2FF00 once rounded up to 16 (the room from `0x40010000` up to the
relocator at `0x4003FF00`), and unmounts the card on every failure:

| Failure | Text launcher | GUI |
|---|---|---|
| Card will not mount | "SD card could not be mounted." | "The SD card could not be read." |
| No file at any path | "RAM payload not found." and "Copy hwtest_ram.bin to SD root or bootloader/payloads/." | "hwtest_ram.bin is not on the SD card. Copy it to the root of the card." |
| Wrong size | "RAM payload has invalid size: N bytes" | "hwtest_ram.bin on the SD card is damaged. Copy it again." |

The text launcher then waits on "Press POWER to return to the hwtest
menu." and returns to its menu. The GUI initialises the card again for
later checks and keeps its raised clocks. The GUI's window text is looser
than the code ([GUI](gui.md#ram-test)); in particular it mentions a
reboot, which the payload does not offer.

On success the text launcher shows "Loaded <path> (<n> bytes)" and, for
350 ms, "Starting RAM test..."; the GUI has shown "Loading the RAM test..."
since Start. A console that stays on those lines never drew the RAM test's
own screens. The GUI first puts the DRAM back to 204 MHz and the BPMP to
`BPMP_CLK_NORMAL` (`gui_clock_fast(false)`), because "it trains from the
boot clock"; it does not stop a USB serial console first
([GUI](gui.md#ram-test)). `ram_payload_run` shuts the card
down, stages the file in DRAM at `0xC0000000`, puts the `chain_reloc` copy
loop at `0x4003FF00`, turns the BPMP MMU off and jumps to it; the loop
copies the image to `0x40010000`, "the same 0x40010000 address used by RCM
injection", so a chainloaded run starts exactly as an injected one.

## The image and its build checks

The payload's code, data and stack live in IRAM, which is what lets it
overwrite DRAM: it is linked at `0x40008000` with hwtest's `link.ld`, no
heap is linked, and after `hw_init` the stack grows down from
`IPL_LOAD_ADDR`. The only DRAM it keeps, the SMMU directory and the
framebuffer, is skipped by the sweeps ([below](#reaching-all-of-dram)).

The `.bin` is self-extracting, with hwtest.bin's loader, because
TegraRcmSmash prepends a 66216-byte prologue and silently truncates the
buffer to 192 KiB; the Makefile fails above `RCM_LIMIT` = 130392 bytes
("universal RAM payload exceeds the RCM injector limit"). The stub moves
the BCL LZ77 stream so it ends at `0x40038000` and expands it forward into
`0x40008000`. `ramtest/check_overlap.py` proves at build time that this
never overwrites input still to be read: it walks the token stream and
checks, for every literal and for the last byte of every back-reference
(where input does not advance), that the output stays below the first
unread input byte, failing with "unsafe self-extraction overlap" or
"decoder size mismatch". `link.ld` asserts that `.text` and `.data` end at
or below `0x40036000`, BDK's `sdram_init` scratch, which caps the expanded
image at 0x2E000 bytes. The build's `built` line gives both sizes: at the
time of writing 74200 bytes injected and 172640 expanded.

The Atmosphere code comes from the `third_party/Atmosphere` submodule
(without it the build stops: "Atmosphere submodule missing"). The Makefile
copies its MTC sources and three `tegra_*.hpp` headers to
`build-ram/atmosphere`, adds `source/fusee_fatal.hpp` and
`source/fusee_uncompress.hpp` and applies
`ramtest/patches/atmosphere-mtc.patch` there. The copy is redone only when
the patch or those two headers change, so updating the submodule needs a
clean build. Besides the bounded waits
([below](#bounded-waits-and-the-failure-screen)), the patch 4-byte-aligns
every Erista and Mariko table blob (Erista's are read in place as
`EmcDvfsTimingTable` structures) and strips the `vapours` includes from the
three headers, which build against `ramtest/compat/exosphere.hpp` instead.

## Platform and DRAM ID

Both public Atmosphere MTC (memory training) engines are linked in, but
only one runs. Before `hw_init`, `ipl_main` reads `APB_MISC_GP_HIDREV`
("readable directly from reset state"): major `GP_HIDREV_MAJOR_T210B01`
selects Mariko and `GP_HIDREV_MAJOR_T210` Erista, and the bridge
(`ramtest_mtc_universal.cpp`) calls only that engine from then on. Unlike
BDK's `hw_get_chip_id`, it does not take a major above 2 as Mariko: any
other revision gets an "Unsupported platform" screen, "Unsupported Tegra
HIDREV %08X (major %X); no RAM training will run.", and a halt that reads
no keys.

The timing table follows the raw fuse DRAM ID, `fuse_read_dramid(1)`.
Erista accepts 0 (Samsung 4 GB), 1 (Hynix 4 GB), 2 (Micron 4 GB) and 4
(Samsung 6 GB); Atmosphere gives the Micron part the Samsung 4 GB table.
Mariko accepts 3 and 5-34 through `kMarikoTrainingTableIndices`,
Atmosphere's mapping, which upstream applies to both SoCs; the bridge
refuses the Erista-only IDs on Mariko. Any other ID sets
`RAM_MTC_ERR_UNSUPPORTED` and nothing is trained. The `DRAM ID` line
prints the ID in hex (`22` is ID 34) and nothing decodes it; the comments
of `kMarikoTrainingTableIndices` and `table_index_for_erista` name the
part for each ID (BDK's tables are in [memory](memory.md#dram-identity)).

Mariko's 204 and 1600 MHz tables are LZ4-unpacked into the bridge's
0x26C0-byte `g_mtc_tables_buffer`; Erista's three (204, 800 and
1600 MHz) are used in place in the payload image, as upstream's copy into
the buffer is commented out.

## Training

The mode menu comes first ([below](#menus-and-keys)), so Power off from it
never trains. The "Training DRAM" screen shows the platform, the size
(`MC_EMEM_CFG & 0x3FFF`, in MiB), the 1600 MHz target, "Training..." then
"Training      : PASS", and the DRAM ID. Both engines check that the 204
and 1600 MHz tables carry those rates (else `TABLE`; Erista's 800 MHz
table is not checked) and that `CLK_SOURCE_EMC` holds the 204 MHz table's
source (else `ENTRY_CLOCK`, where upstream silently skipped training).
Mariko trains 1600 MHz from 204 and switches to it; Erista trains 800 and
1600 MHz, switches to 800 then to 1600, waits 100 ms and runs one periodic
compensation. Each step returns at the first error.

Training happens once per boot and the DRAM stays at 1600 MHz; later runs
print "Training      : kept from the previous run". The comment says why:
"The Erista restore can only bring the EMC back to 800 MHz (upstream
Atmosphere has no 1600->204 path for it), and training refuses to start
from anything but the 204 MHz source", so restoring and retraining failed
every time.

### Bounded waits and the failure screen

The patch (`ramtest/patches/atmosphere-mtc.patch`) states its purpose:
Atmosphere's boot path "can wait forever for a dead PLL. A board diagnostic
must return a result instead." It bounds six loops (PLLM and PLLMB lock in
both engines, two DLL status loops in Erista's) at 1000 polls 1 us apart,
makes the existing timeout in `WaitForUpdate`, whose result the callers
mostly ignore, count as a status timeout, and turns the `ShowFatalError`
calls for corrupt tables and an unknown Erista PLLM rate into `TABLE`. A
step that timed out is not marked trained and ends the step loop.

A failure shows "DRAM training failed": red "1600 MHz training: FAIL",
`DRAM ID`, `error flags`, `training mask`, a line per flag and
"Power-cycle before trusting memory state after a HW timeout.", then halts
without reading a key or powering off.

| Flag | Name | Line | Set when |
|---|---|---|---|
| 0x01 | `RAM_MTC_ERR_TABLE` | " - invalid/corrupt timing table" | a table rate is wrong, or Erista's PLLM is asked for an unknown rate |
| 0x02 | `RAM_MTC_ERR_ENTRY_CLOCK` | " - unexpected EMC entry clock; no training attempted" | `CLK_SOURCE_EMC` is not the 204 MHz table's |
| 0x04 | `RAM_MTC_ERR_PLL_TIMEOUT` | " - memory PLL did not lock" | no PLLM or PLLMB lock in 1000 polls |
| 0x08 | `RAM_MTC_ERR_STATUS_TIMEOUT` | " - EMC/DLL status transition timed out" | an EMC status or DLL wait timed out |
| 0x10 | `RAM_MTC_ERR_UNSUPPORTED` | " - unsupported DRAM ID/table" | by the bridge: no table for this ID on this SoC |

`training mask` ORs the training steps that were active when a status wait
timed out (PLL timeouts add nothing), in Atmosphere's bits from
`fusee_mtc_timing_table_common.hpp`: `CA_TRAINING` 0x1,
`CA_VREF_TRAINING` 0x2, `QUSE_TRAINING` 0x4, `QUSE_VREF_TRAINING` 0x8,
`WRITE_TRAINING` 0x10, `WRITE_VREF_TRAINING` 0x20, `READ_TRAINING` 0x40,
`READ_VREF_TRAINING` 0x80, `TRAIN_SECOND_RANK` 0x100,
`BIT_LEVEL_TRAINING` 0x200. The bit enum in `ramtest_mtc.h` is unused and
lacks the QUSE bits; decode the mask with Atmosphere's bits above. Some
paths still stop without a report (code reading): the remaining
`ShowFatalError` calls and the LZ4 decoder's `AMS_ABORT_UNLESS` checks
trap, and BDK's undefined-instruction handler branches back to
`ipl_main`, so the payload starts over (`hw_init`, then the menu) with no
message. BDK leaves the exception type and LR at `0x4003FF18` and
`0x4003FF14`, which nothing here reads.

### Periodic compensation

`mtc_tick` calls the periodic hook at most every `MTC_PERIODIC_MS`
(100 ms): in the menus, on the report screen and, through `training_tick`,
after each 4 MiB region and every 10 ms of the retention hold. In the
menus and on the report screen it stops while a key is held, as the loops
that wait for its release do not tick. An MTC error during a run sets
`g_run_failed` and stops it. On Mariko the hook compensates against the
1600 MHz table when that table is trained and asks for it.

On Erista the code does not do what the comments describe (code reading,
not seen on a console): `DoPeriodicTrainingErista` and
`RestoreMemoryClockRateErista` take their tables from
`g_mtc_tables_buffer`, which the Erista path never fills, and the third
0x1340-byte table's `periodic_training` would lie 0x3C bytes past the end
of the buffer. What sits there is link layout, not code, so the Erista
tick cannot be relied on to compensate; the only compensation that
certainly happens is the one at the end of training.

If the MTC flags are non-zero when a mode is picked, the clock is restored
so training runs again. Mariko returns to 204 MHz and can retrain.
Erista's restore is meant to stop at 800 MHz, where the retrain would fail
`ENTRY_CLOCK` and be reported "instead of every run failing silently"; it
reads the same unfilled buffer, so what it really does is not known.

## Reaching all of DRAM

The BPMP is a 32-bit core and DRAM starts at `0x80000000`, so it reaches
only 2 GiB directly. `smmu_setup` gives it (`MC_SMMU_AVPC_ASID`, ASID 1) a
page directory at `SMMU_HEAP_ADDR` (`0x92000000`) that identity-maps
`0x80000000-0xFFFFFFFF` in 4 MiB entries, so its normal accesses keep
working. Each region is then reached through one 4 MiB window at IOVA
`0xC0000000`, repointed at each 64-bit physical address in turn.
`map_region` cleans and invalidates the BPMP cache before and after each
move and flushes the SMMU, "otherwise the cache can make two different
DRAM regions appear identical"; each fill is committed before the window
moves on. Turning the SMMU on or off takes a TrustZone write to
`MC_SMMU_CONFIG`, so BDK runs a stub on CCPLEX CPU0 for 100 ms, at setup
and at `smmu_teardown` after every run. "SMMU window   : PASS" only means
setup returned: nothing checks the translation, and the FAIL halt cannot
be reached, as BDK's page allocator never returns NULL.

`protected_region` skips any 4 MiB region overlapping the first 4 MiB
("boot-time low-DRAM/carveout state"), the SMMU directory's 4 MiB, or the
framebuffer (`IPL_FB_ADDRESS` `0xF5A00000`, 0x384000 bytes, two regions):
16 MiB, all below 4 GiB. So `scope/pass` reads 1020 / 1024 regions
(4080 MiB) with 4096 MiB, 1532 / 1536 with 6144 and 2044 / 2048 with 8192.
An older 16 MiB low guard "hid three otherwise testable regions" and was
dropped because the payload runs from IRAM.

## Menus and keys

In the menus, keys are read as levels and in one poll VOL- beats VOL+,
which beats POWER (on the report screen POWER is checked first). VOL+
moves down and VOL- up, both wrapping, one step per press. The **RAM test
menu** opens on its first row every time:

| Row | Help line | Patterns (mask) |
|---|---|---|
| Quick test | "One full Address pattern write/verify pass." | address (0x01) |
| Advanced test | "Run all seven RAM patterns." | all seven (0x7F) |
| Custom test | "Choose individual RAM patterns to run." | the ticked ones |
| Power off | "Shut down the console cleanly." | none |

Training and the data-bus preflight run first in every mode. **Custom RAM
test** has a `[x]`/`[ ]` row per pattern, then "Run selected checks" and
"Back". POWER toggles a row, Run with nothing ticked is ignored, and the
ticks (all seven at start) survive Back and later runs until power-off.
Power off writes `MAX77620_ONOFFCNFG1_PWR_OFF` over `I2C_5` once POWER is
released, restoring nothing first.

**Abort** is VOL-, sampled only after each 4 MiB region of a sweep and
every 10 ms of the retention hold: never during training, SMMU setup or
the preflight, nor in the menus, where VOL- is a menu key. A tap released
before the next region boundary is missed, so hold VOL- until the ABORTED
screen appears; seen at the last region of a verify sweep, it discards a
pass that was in fact fully verified. The report waits for every key to be
released ("An abort is normally entered with VOL- still held"). Then POWER
returns to the menu at its first row, not straight into a run, despite the
footer's "run again"; VOL- powers off and VOL+ does nothing.

With a faulty key: either volume key alone reaches every row, as both
wrap; with POWER alone only Quick test can be run, as the menu opens on
it; without VOL- there is no abort and no power-off from the report (the
menu's Power off row still works); without POWER nothing can be started.

## A run

1. The error count, error log and abort and failure flags are cleared, so
   the report covers one run; the menu opens.
2. The mode sets the pattern mask; UART gets
   `[ram] mode=<mode> patterns=<n> mask=<hex>`.
3. If MTC flags are set, the clock is restored so training runs again
   ([above](#periodic-compensation)).
4. Training, or the kept training, then the SMMU window.
5. The data-bus preflight, on a screen of its own.
6. The selected patterns in the fixed order below, until done, aborted or
   failed.
7. `smmu_teardown`, the report, the post-run keys.

The **preflight** uses one word, 0x1000 into the first testable region
(normally `0x80401000`). For each of the 32 bits it writes the bit alone,
then its complement, cleaning and invalidating the cache before each
read-back, and stops at the first mismatch. "walking data bits: FAIL"
sets `g_run_failed`, so no pattern runs.

Each **pass** writes every testable region, then verifies every one in a
separate sweep, because an address-decoder alias can pass an immediate
write/read-back test ([README](../README.md)). A mismatch never stops the
verify sweep; only an abort or an MTC error does.

| Pattern (as printed) | Word | Salt |
|---|---|---|
| address pattern | `lo32(addr) ^ high_mix ^ salt` | 0x13579BDF |
| inverse address | complement of the same | 0x2468ACE0 |
| all-zero, all-one | 0x00000000, 0xFFFFFFFF | none |
| AA pattern, 55 pattern | 0xAAAAAAAA, 0x55555555 | none |
| PRBS + retention | xorshift32 (<<13, >>17, <<5), reseeded per region | 0xC001D00D |

`addr` is the word's physical address and `high_mix` its upper 32 bits
rotated by 16, so a word above 4 GiB differs from the one 4 GiB lower.
Each region's PRBS seed is `lo ^ (hi * 0x9E3779B9) ^ salt ^
(0x6D2B79F5 * (p + 1))` of its base address, zero becoming 0xA5366B4D. For
PRBS only, `RAMTEST_RETENTION_MS` (2000 ms) is added between the sweeps;
nothing touches refresh, so it is a hold with the controller running
normally.

## What it shows

During a pass the screen shows
`Pass i/N   <write|verify> <pattern>   errors <n>` (errors for the whole
run) over a 48-character bar: write fills 0-50 %, verify 50-100 %, and the
retention hold stays at 50 %. UART gets
`[ram] write address pattern 40% errors=0` at each multiple of 10 %. A
finished pass shows "pass <name>: PASS|FAIL, +N errors, Ns" (PASS means no
new errors in that pass) until the next pass clears the screen; UART keeps
`[ram] pass=<name> result=PASS|FAIL new_errors=N seconds=S`. The report,
titled "Final result" or "ABORTED", goes to both:

```
DRAM result : PASS | FAIL | ABORTED
platform    : Mariko/T210B01 | Erista/T210
mode        : quick | advanced | custom
DRAM size   : <n> MiB
scope/pass  : <testable> / <total> regions (<MiB> MiB)
passes done : <completed> / <selected>
stopped on  : pass <i>/<N>, <pattern>, write | retention | verify
phase prog  : <ms> / 2000 ms  or  <done> / <total> regions (<MiB> MiB)
errors seen : <n>
elapsed     : <s> s
MTC flags   : <hex>
bad @ <16 hex digits> exp <hex> got <hex> (<pattern>)
+ <n> additional failing words
```

ABORTED (`COL_WARN`, yellow) means the user aborted; otherwise FAIL
(`COL_ERR`, red) if any word mismatched or `g_run_failed` is set, else
PASS (`COL_OK`, green). ABORTED wins over FAIL, so read `errors seen`
after an abort. `stopped on` and `phase prog` appear only when an abort or
failure came mid-pass. `elapsed` runs from after SMMU setup to the end of
the passes, so it includes the preflight and excludes training. The
footer, "POWER: run again    VOL-: power off", is on the LCD only.

Each mismatching 32-bit word counts once per pattern it fails (saturating
at 0xFFFFFFFF). Counts are printed with `%d`, which the payload's
formatters treat as signed: from 0x80000000 up a count shows as negative,
and `errors seen : -1` means the counter saturated. The first 16 words
are listed, the rest only counted on the `+` line. `bad @` gives the
word's full physical byte address, `00000001xxxxxxxx` above 4 GiB. The
slots fill in sweep order, low to high address and patterns in the fixed
order, so a later pattern's failures may show only in the count.

## UART

The port is fixed at build time, `-DDEBUG_UART_PORT=1` (`UART_B`, the
right Joy-Con rail at 1.8 V; see the [README](../README.md)), 115200 baud,
not inverted; the main payload's UART output selector does not apply and
nothing is read from it. Every `out()` and `out_aligned()` line goes to
both screen and UART (training, failure, unsupported-platform and report
text, the SMMU and preflight lines, the menu's "Powering off..."); the
`[ram]` lines are UART-only, while menus, progress row and bar, the
per-pass line and footers are LCD-only, and so is the report screen's
"Powering off...": on UART a power-off from the report leaves the log
ending at the report. The training, SMMU and preflight lines are cleared
from the LCD by the next screen, so UART is their only lasting record. The
host viewer (`host_tools/`) starts a "RAM test" page at each `[ram] mode=`
line and greys out its UART commands.

## Reading a failure, and what it cannot tell

- A preflight FAIL is one of the 32 data bits not holding a 1 or a 0 at
  one word, before any pass: the code calls it the data-bus preflight,
  though one word cannot tell a data line from a bad cell there. It reads
  "passes done : 0 / N" with no `stopped on` and one `bad @` at the
  preflight word tagged "(address pattern)", though no address pass ran;
  its `got` is a second read, not the value compared.
- Failures in the passes whose data depends on the address (the two
  address patterns and PRBS) while the fixed passes are clean point at
  aliasing: two addresses reaching the same cells, so the word written
  later in the sweep, at the higher address, overwrites the other and the
  mismatch is reported at the lower one. For a pair below 4 GiB, an
  address-pattern `got` XOR 0x13579BDF is the higher address.
- The fixed patterns are the same at every address, so they find bits
  that read back wrong but no aliasing. XOR `exp` and `got` for the
  failing bits. Nothing decodes byte lane, channel, rank or chip; only
  the physical address is given.
- "PRBS + retention" is the only pass with a hold, but also the only one
  with pseudo-random data, so a failure in it alone cannot be put down to
  the hold rather than to the data.
- FAIL with `errors seen : 0` and non-zero `MTC flags` is an MTC error
  mid-run, not bad data.
- A training failure names flags and steps, never a cell. A failure
  screen whose only flag is 0x10 or 0x02 (see the table above) still says
  "1600 MHz training: FAIL" in red, but no training ran, so neither is a
  verdict on the DRAM; nor is the "Unsupported platform" screen of an
  unknown HIDREV, where nothing runs at all. All of these screens halt:
  the code offers no way out.
- `DRAM size` and every sweep bound come from `MC_EMEM_CFG`, which BDK's
  `sdram_init` (in `hw_init`) programmed from the parameter set it picked
  by the fuse DRAM ID: it is not what the chips report, and memory beyond
  it is never swept. The memory probe's mode-register lines
  ([memory](memory.md#dram-identity)) are what the chips say.
- IRAM and the four skipped regions are not tested, and the preflight
  covers one 32-bit word at one address.
- One master, the BPMP, through its write-through cache, at 1600 MHz only.
- Retention is a 2 s hold with refresh running, not a refresh-off test.
- On 6 and 8 GiB consoles address-pattern words are not unique:
  `0x0_8041_0000` and `0x1_8040_0000` get the same word, so an alias
  joining that pair passes both address patterns; PRBS, seeded per
  region, is not tied to the address that way.
- Salts are constant, so a pattern writes the same data in every run: a
  write lost in a later run still verifies if the previous run left that
  pattern's data there, as after a repeated Quick test.

## What it changes on the console

The testable DRAM is overwritten, whatever was loaded there (the GUI and
the staged copy included), and never restored. The EMC stays at 1600 MHz
for the rest of the boot; only the recovery path above restores it. The
SMMU is on for the BPMP during each run only, and CPU0 is booted and
power-gated twice per run to switch it. Power-off goes through the
MAX77620; there is no reboot and no return to hwtest.
