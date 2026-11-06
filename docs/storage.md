# Storage

The Storage group is ten pages, all named "Storage": nine probes in
`probe_storage.c`, then the game card reader, which has its own note
([gamecard](gamecard.md)). Between them they cover the SD card on SDMMC1
and the eMMC on SDMMC4: how each link came up, the eMMC's partitions, wear
and GPT, the factory data in PRODINFO, the BOOT0 boot partition, what is on
the SD card, and the SDMMC error counters. Terms (GPT, BOOT0/pkg1, AutoRCM,
PRODINFO, CAL0) are in [terms](terms.md); the overview is in the
[README](../README.md).

| Section on the page | Function | Verdict keys |
|---|---|---|
| `[SD card (SDMMC1)]`, `[eMMC (SDMMC4)]` | `probe_storage` | `sd_bus`, `emmc_bus` |
| `[Switch serial number (PRODINFO)]` | `probe_serial` | `prodinfo`, `wlan_cal`, `cal0_hash`, `cal0_model`, `cal0_panel` |
| `[eMMC partitions]` | `probe_partitions` | none |
| `[eMMC health & EXT_CSD]` | `probe_emmc_health` | `emmc_health` |
| `[eMMC GPT]` | `probe_gpt` | `gpt` |
| `[BOOT0 / pkg1 fingerprint]` | `probe_boot0_pkg1` | `fuses_pkg1` (and `emmc_bus`) |
| `[AutoRCM detection]` | `probe_autorcm` | none (and `emmc_bus`) |
| `[SD card content]` | `probe_sd_content` | none |
| `[SDMMC error counters]` | `probe_storage_errors` | `emmc_errors`, `sd_errors` |

The Diagnostics page adds one cross-check on the eMMC, `xc_emmc_mode`.
Nothing in the group writes data to the eMMC; the only thing written to
the SD card is the report file.

## Running it

The table is the sweep order (`g_pages` in `core.c`), and the order is
deliberate. `probe_serial` and `probe_gpt` read the USER partition before
either BOOT0 probe switches away from it, and `probe_storage_errors` reads
the counters after every other storage probe has done its reads. Only
`probe_gamecard` runs after it, so game card activity is not in its counts.

`probe_storage` is not where the SD card and eMMC first come up. The text
payload initialises both before the sweep, and only when Storage is among
the selected groups (`main.c`). The GUI does it once at start-up, whatever
is picked (`gui_hw_prepare()`), because its status bar reads the serial
from the eMMC and its clock set-up loads hekate's Minerva DRAM clock module
from the SD ([internals](internals.md#the-guis-clocks)). `probe_storage`
re-initialises a device only when its `initialized` flag is clear: a
second `sd_initialize()` would power-cycle the card and negotiate UHS
again, which can fail the second time and lose the good link from the
first.

Each page is one probe call, and the text payload's pager makes that call
again whenever the page is shown (`render_page()`). Showing a Storage page
therefore repeats its reads (the PRODINFO decrypt, the switch to BOOT0 and
back) and overwrites that probe's verdict keys. `a` re-runs the sweep for
the groups selected at launch (`g_run_mask`) from a cleared verdict
registry; `G` with a group name re-runs that group's pages without
clearing it. Any failure or retry during these runs also adds to the
[error counters](#error-counters), which are never reset.

## SD and eMMC bring-up

`probe_storage` prints each card's identity from its CID (manufacturer,
OEM, product name, revisions, serial, date) and, for the SD card, its
size. Three lines describe the link: `bus_width`, `card_clock` (the rate
configured after the handshake, in MHz with the raw kHz value) and
`init mode`. A clock just under the nominal rate, such as 199.68 MHz for
200 MHz, is divider rounding.

The init mode is where the bdk's fallback ladder stopped. Each device
starts at its fastest mode; when initialisation fails at a level, the bdk
power-cycles the device, steps one level down and tries again, until a
level sticks or the ladder runs out. `init mode` names the level that
stuck (`sd_get_mode()`, `emmc_get_mode()`):

| Level | SD | eMMC | Colour |
|---|---|---|---|
| 4 | `UHS SDR104 (4-bit, max)` | `HS400 (8-bit, DDR, max for Switch eMMC)` | green |
| 3 | `UHS SDR82 (4-bit)` | `HS200 (8-bit)` | green |
| 2 | `HS25 (4-bit)` | `HS52 (8-bit)` | yellow |
| 1 | `HS25 (1-bit, fallback - signal-integrity issue?)` | `HS52 (1-bit, fallback)` | red |
| 0 | `INIT FAILED` | `INIT FAILED` | red |

The SD ladder only has a DDR208 level above SDR104 when the bdk is built
with `BDK_SDMMC_UHS_DDR200_SUPPORT`, which hwtest does not define. With no
SD card inserted the bdk resets the SD mode and stops without counting a
failure; `probe_storage` then prints `sd_initialize FAILED (ejected?
unsupported?)` in yellow and sets no SD key. A failed eMMC prints
`emmc_initialize FAILED` in red and sets no eMMC key.

Every failed level adds one to that device's init-failure counter,
including the attempts made before the sweep, and a device that fails at
every level counts five: four levels plus the step into `INIT FAILED`.
A device that came up below its top level therefore FAILs its error key
(`1 init fail` after one failed attempt; see
[Error counters](#error-counters)) even when its `init mode` is green and
its bus key passes. That counter, not the mode line, is how a speed
fallback reaches the verdict.

After a complete failure the two devices differ. `emmc_initialize()` resets
the eMMC mode to HS400 before it starts, so `probe_storage`'s retry of a
failed pre-init climbs the whole ladder again. The SD mode stays at
`INIT FAILED` (only `sd_end()`, or finding the slot empty, resets it), so
the retry fails at once, without touching the card, and adds one more
count.

| Key | PASS | WARN | FAIL |
|---|---|---|---|
| `sd_bus` | bus 4-bit or wider | narrower: `N-bit (dirty slot?)` | never |
| `emmc_bus` | 8-bit | never | narrower: `N-bit (trace damage?)` |

`emmc_bus` has a second writer: a failed switch back from BOOT0 overwrites
it with FAIL `USER switch-back failed` (see [BOOT0](#boot0-pkg1-and-autorcm)).
None of the clock, mode or SD class lines sets a key; the mode and class
lines are coloured, the clock line is not.

The SD card also reports its classes from `SD_STATUS` (ACMD13), which the
bdk cached at init: speed class `C<n>` (green from C10, yellow below,
including the `C0` of a card that states none), UHS grade (U3 green, U1
yellow), video class (green from V30) and application class (A2 green),
then one `AU size`: the UHS allocation unit when the card reports one
(marked `(UHS)`), else the legacy one, and no line when that code is 0 or
above 14. Speed class and UHS grade are sustained sequential write floors
in MB/s (U1 10, U3 30), the video class a video-recording write floor,
and the application class a random IOPS floor (A1 1500 read/500 write,
A2 4000/2000). The code's comment names U3/V30/A2 as the modern target
for the Switch; anything lower works but explains "loads slowly"
complaints. These are the card's own claims, not a measurement.

The GUI's Storage category takes three readings from this page:
**Internal chip** (the eMMC's `prod_name`), **SD card** (the SD `size`)
and **SD card speed** (the SD `init mode` up to its first " (", such as
`UHS SDR104`).

## PRODINFO and CAL0

`probe_serial` decrypts the start of the PRODINFO partition, where the
factory calibration (CAL0) lives, and reads the console serial, the radio
calibration and the factory inventory out of it. PRODINFO is AES-XTS
encrypted with the console's `bis_key_00`, so a plain eMMC read returns
ciphertext. The probe reads through the bdk's BIS layer, which decrypts
with the Security Engine, and that needs the key in SE keyslots 0 (crypt
half) and 1 (tweak half).

### Sequence

1. Needs the eMMC; otherwise `eMMC not initialised` in red, and no key.
2. Load the key from the SD card. Mount `0:` afresh (the GUI reads the
   serial again on later sweeps, and the volume may have been unmounted in
   between), open exactly `0:/switch/prod.keys` (Lockpick_RCM's file) and
   read at most 32 KiB. Find the line that starts, after any spaces or
   tabs, with `bis_key_00` followed by a space, a tab or `=`, so a longer
   name such as `bis_key_00_alt` cannot match as a prefix; after the `=`,
   64 hex digits give the 32-byte key. Slot 0 gets the first 16 bytes and
   slot 1 the last 16, the pair `nx_emmc_bis_init()` selects for PRODINFO.
   The file body is wiped on every exit, the parsed key once it has been
   loaded. The AES key registers are write-only, so the load cannot be
   read back; a good CAL0 magic is the only proof it worked.
3. Parse the GPT (`emmc_gpt_parse()`) and find `PRODINFO`. The range comes
   from the table, not a constant (the comment gives the stock range as
   LBA 34..8191).
4. Check the range before using it: not starting at LBA 0, not ending
   before it starts, not running past the end of the eMMC. bdk's
   `emmc_gpt_parse()` ignores the result of the read that feeds it, so an
   eMMC that cannot read yields entries built from whatever was in the
   buffer. One failing console gave a PRODINFO entry at
   `0xFFFFFFFF..0xFFFFFFFF`, and handing that to the BIS layer ended the
   run, with every later page lost. For the same reason the parsed list is
   released by `gpt_free_bounded()`, not bdk's `emmc_gpt_free()`, which
   walks until it meets the head again and never ends on a list built
   from garbage (two dumps from a failing console stopped there with no
   output). hwtest's walk stops after 128 entries or at the first pointer
   outside the heap and leaks the rest, a few hundred bytes on a console
   that is already failing.
5. Start BIS on the real eMMC (not emuMMC) with its cluster cache off and
   read sectors 0 and 1. If the header's `body_size`, still unverified,
   is at least `sizeof(nx_emmc_cal0_t) - 0x40` and at most 64 KiB, read
   the whole of CAL0 as well, rounded up to whole sectors.
6. End BIS and clear keyslots 0 and 1 if hwtest loaded them; the BIS layer
   does not clear them itself.
7. CAL0 is good when both sector reads succeeded and sector 0 starts with
   `CAL0`.

When no key loaded, the reason is one of `SD not ready`,
`SD f_mount failed`, `no sd:/switch/prod.keys (run Lockpick first)`,
`out of memory`, `f_read failed`, `bis_key_00 not in keyfile`,
`malformed line (no =)` or `bad hex in bis_key_00`.

### Keys, magic and serial

The `BIS keys` line is printed after the CAL0 check so it can be coloured
by the outcome: green `loaded from sd:/switch/prod.keys` when the key
loaded and CAL0 decoded; yellow `loaded but invalid (wrong unit's
prod.keys?)` when it loaded and CAL0 did not decode; yellow
`skipped (<reason>)` when no key was loaded. Neither yellow case is red: a
missing keyfile, or one from another console, is a configuration problem,
not a hardware fault. The words "invalid" and "skipped" let the host
viewer's text classifier grade the line without colour codes, since the
serial console carries plain text.

Then `Partition : LBA a..b (N KiB)`, `CAL0 magic` (`CAL0` in green, or
`???? (invalid - decrypt produced gibberish)` in yellow), and `Serial`,
read from CAL0 offset 0x250: up to 24 characters, stopping at the first
unprintable byte, with `(empty / wiped)` when there are none. Its format
is not checked, and an empty serial sets no key: `prodinfo` still passes,
and only this line and the GUI's **Serial number** reading show it.
Without a decoded CAL0 the `Serial` line is a yellow `skipped (...)` that
says whether the keys were wrong or absent, and the probe stops there.

| Severity | Detail | Condition |
|---|---|---|
| PASS | (empty) | hwtest loaded the key and CAL0 decoded |
| WARN | `decode failed (wrong keys?)` | the key loaded, CAL0 did not decode |
| WARN | `no keys` | hwtest loaded no key |
| WARN | `GPT entry unusable` | the PRODINFO range failed step 4 |

That is the `prodinfo` key. The last case prints
`PRODINFO GPT : LBA <start>..<end> is not a usable range` in red, then
`(GPT unreadable or corrupt - suspect the eMMC, not the keys)`. There is
no key at all when the eMMC is down or PRODINFO is not in the GPT
(`PRODINFO partition not in GPT`, red). On these two GPT exits the
`BIS keys` line is not coloured by the outcome: when PRODINFO is not in
the GPT it comes first, in yellow, with the bare reason or
`loaded from sd:/switch/prod.keys`; with an unusable range it is not
printed at all.

Two outcomes are easy to misread. A BIS read error with the key loaded
reports exactly like a wrong key (`loaded but invalid`, `decode failed`),
because the CAL0 check includes the read results; only the red
`BIS read failed (a / b)` after it tells them apart. And `no keys` reflects
only whether hwtest loaded a key, not what the keyslots hold: if slots 0
and 1 already carry the right key (the comment notes that the
RCM-Emulator pre-loads them with `--prod-keys`), CAL0 decodes and
everything below prints while the line says `skipped` and `prodinfo`
stays `no keys`.

### Radio calibration

From the two decrypted sectors: `WLAN MAC` (CAL0 0x210), `BD MAC` (0x220)
and `WLAN cc num` (`WlanCountryCodeNum`, 0x80). HOS refuses to bring Wi-Fi
up when the WLAN calibration is missing, and "can't connect" then looks
just like a dead radio. An all-00 or all-FF MAC or a zero count means the
data itself is gone, which no chip-level work will fix. All-FF is what
erased flash reads as; all-00 is a zero-filled or re-created PRODINFO.

The MAC lines turn red with `(all zero ...)` or `(all FF ...)`.
`WLAN cc num` turns red at 0 (`no country codes - Wi-Fi cannot init`) and
above 128, the size of CAL0's table (`out of range - corrupt`).

`wlan_cal` FAILs with, in this order of precedence, `WLAN MAC all-zero`,
`WLAN MAC all-FF`, `no WLAN country codes` or `WLAN cc num N corrupt`, and
PASSes otherwise, so any count from 1 to 128 passes. The BD MAC only
colours its own line; no key looks at it.

### The rest of CAL0

This part needs the full CAL0 from step 5. Without it the probe prints
`CAL0 body : not read (body_size 0x... out of range)` in yellow (the same
text when the size was in range but the allocation or the read failed),
returns, and sets none of `cal0_hash`, `cal0_model` and `cal0_panel`.

- `CAL0 body : SHA-256 OK`, or `MISMATCH - body corrupt` in red: SHA-256
  over `body_size` bytes from offset 0x40 (`cfg_id1`) against the stored
  `body_sha256`. It catches a PRODINFO that decrypts cleanly but has been
  corrupted or hand-edited since, the state in which HOS refuses to boot
  without saying why. `cal0_hash` FAILs `CAL0 body SHA-256 mismatch` and
  PASSes otherwise. It is a hash, not a signature: an edit that also
  rewrites the hash passes.
- `CAL0 header` (version, body size, update count) and `Config ID`.
- `Product model : N - <name>`: 0 invalid, 1 Nx (Erista), 2 Copper
  (Erista dev), 3 Iowa (Mariko), 4 Hoag (Lite), 5 Calcio (Mariko dev),
  6 Aula (OLED). For 1 to 6 the model's family is held against the SoC
  (Mariko when `HIDREV` bits 7:4 read 2): `Model vs SoC : agree`, or
  `DISAGREE - PRODINFO is from another console family` in yellow.
  PRODINFO travels with the eMMC, so a disagreement is what a donor-board
  repair looks like from software, and it explains a unit that boots but
  is refused online. `cal0_model` WARNs
  `CAL0 says model N, SoC is Mariko|Erista`. It only sees the family: a
  donor eMMC from the same family agrees. Model 0 or a value past 6 is
  not judged: no `Model vs SoC` line and no `cal0_model` key.
- `LCD vendor` and `Panel fitted`: CAL0's `lcd_vendor` against the panel
  ID read at boot. `cal0_panel` WARNs
  `fitted panel 0x..., CAL0 says 0x...` on a mismatch. A zero field,
  common on Mariko, prints `not recorded in CAL0` and is not compared. The
  decode, and when the comparison is skipped, are in
  [display](display.md#cal0-lcd-vendor-against-the-fitted-panel).
- Printed, never compared: the first `WLAN cc tbl` entry with the table's
  last index (entries are regulatory-domain tags, not ISO country codes,
  and a blank entry under a non-zero count means the table is damaged),
  `Battery lot` with its revision, `USB-C PD rev`, `Touch IC` vendor,
  `6-axis IMU` type and mount, `Stick L/R` types and `Region code`.
- `Speaker cal : blank (never calibrated)` in yellow when the `spk_cal`
  block is all zero, `present` otherwise. HOS reads a blank block as no
  calibration: quiet, flat-sounding audio from speakers that are otherwise
  fine. No key.

The GUI shows the `Serial` line as the **Serial number** reading under
System and security: the serial, or without keys the whole `skipped (...)`
text. Its status bar shows the serial as well, read once at start-up by
`console_serial()` (the same key load and range checks, a two-sector read,
nothing logged or recorded); it stays blank without keys and is not
refreshed if they appear later.

## Partitions

`probe_partitions` makes no read and no partition switch: every size comes
from the EXT_CSD the bdk cached at init. `USER (GPP)` is `sec_cnt` sectors,
with the size in MiB. `BOOT0` and `BOOT1` are both `BOOT_SIZE_MULT`
(EXT_CSD[226]) x 128 KiB, one field for both, so BOOT1 is not measured on
its own. `RPMB` is `RPMB_SIZE_MULT` (EXT_CSD[168]) x 128 KiB. It sets no
key; the GUI's **Internal storage** reading is the USER size in MiB.

## eMMC health and EXT_CSD

Also from the cached EXT_CSD, with no new commands.

`PRE_EOL_INFO` is the JEDEC reserved-block state: 1 normal (green),
2 warning (yellow), 3 urgent (red), 0 unknown; anything above 3 prints `?`.
`Life used A` and `Life used B` are `DEVICE_LIFE_TIME_EST_TYP_A` and
`_B` (EXT_CSD[268] and [269]), estimated wear in 10% steps: 0x01 prints
`(0-10%)`, 0x0A `(90-100%)`, and 0x0B, life exceeded, `(100-100%)`. 0x0B is
red, 0x09 and 0x0A yellow, lower values green. 0x00 is JEDEC "not
defined", a part that does not report that type (some Switch eMMCs leave B
at 0), and prints `not reported`. Values above 0x0B print
`reserved / unknown` in yellow.

| Severity | Detail | Condition |
|---|---|---|
| FAIL | `urgent/exceeded (eol E, A=a B=b)` | `PRE_EOL_INFO` 3, or A or B at 0x0B |
| WARN | `aging/unknown (eol E, A=a B=b)` | `PRE_EOL_INFO` 2; A or B at 0x09 or 0x0A; A or B above 0x0B; or A and B both 0 |
| PASS | (empty) | otherwise |

That is `emmc_health`; the detail gives the values in decimal where the
lines print hex. A life estimate is a wear figure, not a JEDEC failure:
80-100% used is late-life service, so a WARN, and FAIL is kept for an
exceeded estimate or an urgent reserved-block state. A `PRE_EOL_INFO` of 0
or above 3, or B alone at 0, does not move the key.

The other lines are information. `EXT_CSD rev`, the raw `Card type`,
`bkops_en`, and `Cache size`, which prints the `CACHE_SIZE` field divided
by 1024 and labels it KiB; Nyx treats the same field as KiB and shows the
quotient as MiB, so the two disagree about the unit. From the raw EXT_CSD:
`SEC_FEATURE` [231] (bits 0, 4 and 6 named `secure-erase`, `auto-erase`,
`sanitize`; other bits are not named), `TRIM_MULT` [232] (yellow
`TRIM NOT supported` at 0), `SEC_TRIM_MULT` [229], the native sector size
from `DATA_SECTOR_SIZE` [61], `HPI_FEATURES` [503] (yellow `NOT supported`
without bit 0) and `RST_N_FUNC` [162]. They are there for two repair
complaints: a very slow HOS shutdown, often a replacement chip without HPI,
TRIM or sanitize; and saves that get slow after a few months, where TRIM is
off and wear levelling cannot reclaim space. None of them sets a key.

The GUI's **Wear** reading is the parenthesised part of `Life used A` (a
range such as `0-10%`, or `not reported` or `reserved / unknown`; B is
not shown) and **End of life** is the `PRE_EOL_INFO` word.

## GPT

`probe_gpt` reads USER sectors 1 to 5, the primary GPT header and the four
sectors after it. It assumes USER is the selected partition, which holds
in the sweep because it runs before the BOOT0 probes.

1. A failed read prints `GPT read failed` and stops with no `gpt` key; the
   failure lands in the [error counters](#error-counters) instead.
2. Without `EFI PART` at the start it prints `signature missing - not a
   GPT-partitioned eMMC` and the first eight bytes, and sets `gpt` FAIL
   `signature missing`.
3. `partitions : N entries, S bytes each (LBA x)` from the header.
4. Header CRC32. The header size (+0x0C) must be 0x5C to 0x100; otherwise
   `hdr_size : N (suspicious - skipping CRC)` in yellow, both CRCs are
   skipped (no `hdr CRC32` or `ent CRC32` line) and there is no `gpt` key
   at all. The CRC is computed over the header with its CRC field (+0x10)
   zeroed and printed against the stored value.
5. With a valid header size, the entry-array CRC32 (+0x58), only when the
   entry LBA is 1 to 4, the count at most 128, the entry size at most
   512, and the array fits in what is left of the 5-sector window. Those
   fields come off the disk, so they are bounded first: a corrupt entry
   LBA used to underflow the window arithmetic and send the CRC into a
   wild read the BPMP may not come back from. Otherwise `ent CRC32 :
   skipped (entries span N bytes, only M in read window)`, yellow when a
   field was out of bounds, and the entry CRC counts as passed.
6. `gpt` PASSes when both CRCs match (or the entry CRC was skipped) and
   FAILs `header CRC mismatch`, or else `entry CRC mismatch`. It never
   WARNs.
7. The listing, one row per entry with a non-zero type GUID:
   `NN. <name> : <size> MiB|KiB [LBA first..last]`, numbered by slot, the
   name folded to ASCII (`.` for anything else) and padded or cut to 22
   characters. It is abandoned, with a yellow
   `unusual entry size / count - aborting decode` at an entry size other
   than 128 or more than 32 entries, or
   `part_ent_lba N outside read window - aborting decode` at an entry LBA
   outside 1 to 4; neither line changes `gpt`. It lists at most what fits
   in the window: 16 entries when the array starts at LBA 2.

The CRC32 is hwtest's own, computed bitwise, rather than bdk's
`crc32_calc()`. That one builds its table lazily in a heap allocation;
if the allocation fails, every call returns the same value, which reads as
both CRCs mismatching on a good console.

## BOOT0: pkg1 and AutoRCM

Both probes select the BOOT0 hardware partition with
`sdmmc_storage_set_mmc_partition()`, a CMD6 switch that writes the
partition number into `PART_CONFIG` (EXT_CSD[179]), the same call Hekate's
`emmc_set_partition()` makes. They read, then select USER (0) again
through `emmc_back_to_user()`. If that fails it prints
`eMMC : switch back to USER FAILED` in red and overwrites `emmc_bus` with
FAIL `USER switch-back failed`, since anything reading the eMMC afterwards
would get BOOT0 in place of USER. In the sweep the USER readers (the GPT
and PRODINFO) have already run, so this matters for a later page or
sweep; there is no retry, and the next sweep's `probe_storage` sets
`emmc_bus` from the bus width again whichever partition is selected. On
that sweep `probe_gpt` and `probe_serial`'s GPT lookup read BOOT0 sector
1, so `gpt` FAILs `signature missing` and `PRODINFO partition not in GPT`
is printed while `emmc_bus` passes: the switch is at fault, not the
table. When the switch to BOOT0 itself fails (`partition switch failed`,
`partition switch to BOOT0 failed`) no switch-back is attempted, so a
CMD6 that was accepted but failed the status check after it can leave
BOOT0 selected. A failed read after the switch (`BOOT0 read failed`,
`sector 1 read failed`, both red) still switches back and sets no key;
the verdict sees it only through the [error counters](#error-counters).

### pkg1 fingerprint

`probe_boot0_pkg1` reads one sector at BOOT0 sector 0x800 (Hekate's
`PKG1_BOOTLOADER_MAIN_OFFSET`, byte 0x100000), where the main package1
starts. On Erista its `pk1_hdr_t` opens the sector; on Mariko a 0x170-byte
`bl_hdr_t210b01_t` comes first. The layout is chosen from the data rather
than the SoC: whichever 14-byte timestamp window (0x10 for Erista, 0x180
for Mariko) holds more printable characters wins, Erista on a tie. The SoC
is not reliable for this when the emulator's `--oem` flag does not match
the dumped BOOT0, or a BOOT0 image has been copied between families. When
layout and SoC disagree, a yellow `Note : SoC=..., BOOT0=..., using BOOT0
layout` says so. The Mariko layout also prints four `bl_hdr` fields
(version, size, load address, entry point).

If none of the 14 timestamp bytes is printable, it prints
`pkg1 build : (BOOT0 sector 0x800 read all-zero - pkg1 absent)` in yellow
and stops without a key; the test is "no printable byte", not literally
zeros. The code's comment gives two causes: the read succeeded against an
unprovisioned eMMC (the emulator default, or a brand-new board), or it
was silently muxed away. Either way there is no pkg1 to check. Otherwise
it prints `pkg1 build` (unprintable bytes as `.`), `pkg1 keygen` and
`pkg1 version`, then `HOS version`, looked up by the
timestamp's first eight characters (YYYYMMDD) in a 30-entry table that
mirrors Hekate's `_pkg1_ids[]`, from 1.0.0 to 22.0.0+. An exact match
prints the HOS range in green. Anything else prints
`newer than HOS <latest entry not after it>` in yellow, which a timestamp
older than the first entry also gets, as `newer than HOS 1.0.0`.

On an exact match, and only then, the anti-downgrade cross-check sets
`fuses_pkg1`: the burnt fuse count (bits set in `fuse_read_odm(7)`)
against the count that pkg1 expects, taken from Hekate's
`_pkg1_ids[].fuses`. This page does not print the burnt count; it is in
the WARN and FAIL details, and the Fuses page prints it as `Burnt fuses`.

| Severity | Detail | Meaning |
|---|---|---|
| FAIL | `N burnt > M: OFW won't boot, CFW only` | the console once ran a newer HOS that burnt a fuse, then pkg1 was downgraded; OFW halts at boot, CFW such as Atmosphere patches the check |
| WARN | `N burnt < M: OFW burns on next boot` | pkg1 is newer than the fuses: updated under CFW with fuse burning blocked, or restored from a newer dump; OFW burns the missing fuses on its next boot and carries on |
| PASS | (empty) | the counts are equal |

`fuses_pkg1` sits on the verdict's Boot integrity row with
`soc_pmic_otp`, not on Storage. That row needs both Power and Storage
selected: on a run without Power the key is recorded but the row is
skipped, and the GUI lists its checks as not picked.

### AutoRCM

`probe_autorcm` reads BOOT0 sector 1, where the first BCT starts (byte
0x200), and switches back to USER straight after the read. Hekate's
AutoRCM toggle zeroes byte 0x10 of the BCT's RSA modulus. The probe
compares bytes 0x10 and 0x11 with what a clean BCT holds
(`nx_emmc_get_autorcm_masks()`: 0xF7 and 0x86 on a production unit, 0x37
and 0x84 otherwise), prints both, then gives one result:

- `N/A (BootROM RCM patched)` in green when `fuse_check_patched_rcm()`
  says the BootROM's RCM is patched, which includes every Mariko. AutoRCM
  does nothing there and Mariko's BCT is not comparable, so a mismatch
  would not mean corruption.
- `invalid (BCT[0] signature byte mismatch)` in red when byte 0x11 is
  wrong.
- `DISABLED (clean BCT)` in green when byte 0x10 is right.
- `ENABLED (BCT modulus byte zeroed)` in yellow otherwise: any value other
  than the clean one, not only zero.

This is the test Hekate's `tools_autorcm_enabled()` makes, on the first
BCT only. It sets no key, so AutoRCM never reaches the verdict; the GUI
shows the result as the **AutoRCM** reading under System and security.

## SD card content

`probe_sd_content` reports what is on the SD card and sets no key. It
needs the SD (`SD not ready` in red otherwise) and mounts `0:` once per
boot (`f_mount failed (N)` in red).

- `DIR`, `FILE` or `absent` for `0:/atmosphere`, `0:/atmosphere/contents`,
  `0:/bootloader`, `0:/bootloader/payloads`, `0:/switch`, `0:/emuMMC` and
  `0:/Nintendo`.
- `atmosphere`: the first source that yields a version, named in
  parentheses. `/atmosphere/release.txt`, then `/atmosphere/version`
  (the first line of either, up to 63 bytes); then a HATS pack's
  `/manifest.json` (the first `"version"` string after the first
  `"atmosphere"` key, within the first 4 KiB); then `/HATS_VERSION.txt`
  (the parenthesised text after `**Atmosphere**` on the same line, within
  2 KiB; the bold markers keep it off the later
  `Atmosphere-NX/Atmosphere`). With none of them,
  `version file absent, contents/ exists` in yellow when
  `atmosphere/contents` exists, else `absent`.
- `hekate`: `/bootloader/update.bin` carries hekate's version block at
  0x118, the magic `ICTC` (`0x43544349`) followed by one ASCII digit per
  byte. It prints `vX.Y.Z (update.bin)`, `update.bin magic mismatch
  (0x...)` in yellow, or `update.bin absent`. That is the version of the
  file, not of whatever actually boots.
- `emummc.ini`: `present` or `absent` (`0:/emuMMC/emummc.ini`), not
  parsed.

The GUI shows **hekate** and **Atmosphere** under System and security,
each up to its first " (".

## Error counters

The bdk keeps three counters per device and bumps them inside its transfer
path. They matter because a card can initialise perfectly and still be
retrying on every transfer, the "it works but everything loads slowly"
complaint no identity or health probe can see. `probe_storage_errors`
prints them as the last storage probe before the game card, so the counts
include every read above it; run first, it would miss every retry and
failure caused by the other probes' reads.

How the bdk counts (`_sdmmc_storage_readwrite()`):

- A failed transfer is tried up to five times, 50 ms apart, and each
  failed try adds one to the **SD** card's retry counter, whichever device
  it was on. Nothing ever increments the eMMC's retry counter.
- When the tries run out, the device's R/W failure counter goes up and the
  device is re-initialised: the first time by a full `sd_initialize()` or
  `emmc_initialize()` (power cycle, the current level, then down the
  ladder as at bring-up), after that one level down per attempt, counting
  an init failure when that fails. A successful re-init gives the
  transfer three more tries. An I/O error mid-sweep can therefore lower
  the speed an earlier `init mode` line reported.
- The init-failure counter also takes every failed level of the bring-up
  ladder ([above](#sd-and-emmc-bring-up)).

hwtest never resets them, so they cover everything since the payload
started: the bring-up before the sweep, the GUI's start-up reads, earlier
sweeps, page re-renders, and `a` and `G` runs.

The lines are `eMMC init failures` and `eMMC R/W failures` (red when
non-zero), `eMMC R/W retries : n/a (BDK counts these against SD)`,
`SD init failures` and `SD R/W failures` (red when non-zero), and
`R/W retries : N (SD + eMMC combined)` (yellow when non-zero). When the SD
card did not initialise (missing or failed),
`(SD never initialised - counts reflect that)` follows.

| Key | Severity | Detail | Condition |
|---|---|---|---|
| `emmc_errors` | FAIL | `N init fail` | eMMC init failures above 0 |
| | FAIL | `N R/W fail` | otherwise, eMMC R/W failures above 0 |
| | PASS | (empty) | both 0 |
| `sd_errors` | FAIL | `N init fail` | SD init failures above 0 |
| | FAIL | `N R/W fail` | otherwise, SD R/W failures above 0 |
| | WARN | `N retries` | otherwise, combined retries above 0 |
| | PASS | (empty) | all 0 |

What that means in practice:

- An eMMC transfer that needed a retry raises the SD card's WARN, and the
  GUI then blames the SD card ("The SD card needed several tries to be
  read. Try another card."), even with no card inserted.
- A missing SD card counts nothing, so it gives no finding here.
- A dead eMMC FAILs `emmc_errors` with its init count; `probe_storage`
  has printed `emmc_initialize FAILED`, and every eMMC probe after it a
  line starting `eMMC not initialised`, all setting nothing.

## eMMC mode cross-check

`xc_emmc_mode` holds the `init mode` level above against the bus width;
it is described in [verdict](verdict.md#c-emmc-hs400-needs-an-8-bit-bus).
The bdk always asks for 8 bits at HS400, so it is a guard: a narrow bus
already FAILs `emmc_bus`.

## In the verdict and the GUI

The verdict's `Storage` row runs when the Storage group is selected
(`PROBE_G_STORAGE`) and aggregates, in this order, `emmc_health`,
`emmc_bus`, `sd_bus`, `wlan_cal`, `cal0_hash`, `cal0_panel`, `cal0_model`,
`gpt`, `prodinfo`, `xc_emmc_mode`, `emmc_errors`, `sd_errors` and
`gc_asic` (the game card). A key that was never set is left out, not
counted as a pass. The row is FAIL if any key FAILs, else warn if any
WARNs, for example `Storage : warn (no keys)`; its reason is the first
FAIL's detail in that order, else the first WARN's. The registry and the
aggregation in general are in [verdict](verdict.md).

That example is the common case: a console with no `prod.keys` on its SD
card and nothing else wrong reads `warn (no keys)` in the text verdict,
while the GUI shows its Storage category as working normally, because
`prodinfo`'s soft prefix `no keys` turns that WARN into "Not checked on
this console." `decode failed (wrong keys?)` and `GPT entry unusable` are
real warnings in both.

In the GUI each key is a check in the Storage category, whose all-good
text is "Internal storage and SD card are healthy."; `fuses_pkg1` is in
System and security.

| Key | GUI check | WARN | FAIL |
|---|---|---|---|
| `emmc_health` | Internal storage wear | yes | yes |
| `emmc_bus` | Internal storage connection | never | yes |
| `sd_bus` | SD card connection | yes | never |
| `wlan_cal` | Wi-Fi factory settings | never | yes |
| `cal0_hash` | Factory data | never | yes |
| `cal0_panel` | Original screen | yes | never |
| `cal0_model` | Original storage | yes | never |
| `prodinfo` | Factory data | yes, `no keys` soft | never |
| `gpt` | Storage layout | never | yes |
| `xc_emmc_mode` | Storage speed | yes | never |
| `emmc_errors` | Internal storage reads | never | yes |
| `sd_errors` | SD card reads | yes | yes |
| `fuses_pkg1` | System version fuses | yes | yes |

`emmc_bus` has one FAIL text for both of its causes, so a
`USER switch-back failed` also reads "not fully connected"; the
`Technical:` line under it, which carries the detail, tells the two
apart.

The texts in `gui/gui_text.c` leave out exactly the states the code never
produces. `prodinfo` and `cal0_hash` share the name "Factory data". The
`emmc_health` warning covers both of its causes, a worn chip and one that
"does not report its wear".

## The SD report

While capture is armed, every log line also goes into a 128 KiB buffer: in
the text payload around the boot sweep and each `a` run, in the GUI around
each Health Check or Custom Check. The file has LF line endings and no
colour. If the buffer fills, `[capture truncated]` is added once and later
lines are dropped. The `[step]` breadcrumbs go to the debug console
(UART-B in the text payload; in the GUI the console picked, a UART or USB
serial) and are not in it.

`save_report()` needs the SD card, the eMMC (for the folder name) and the
buffer; without them it logs `[save] SD not ready - cannot write report`,
`[save] eMMC not ready - no serial for path` or
`[save] report buffer not allocated`. A console with a dead eMMC gets no
report file even with a working SD card. The file is
`backup/<eMMC serial>/hwtest.txt`, built by `emmcsn_path_impl()` (vendored
from Nyx) the way Nyx and Hekate name their backup folders, so the report
lands next to any eMMC backup of the same console. The serial is the eMMC
chip's CID serial in hex, not the console serial. Each save replaces the
file. A failed mount or open logs `[save] f_mount failed` or
`[save] f_open failed (N): <path>`, a failed or short write
`[save] write failed (fr a/b)` rather than success, and a good save
`[save] <path> (<N> bytes)` in green. The `[save]` lines are logged
after capture is detached, so they are never in the file.

The verdict is computed last but read first, so the file starts at the
`--- Diagnostics ---` page (cross-checks, verdict, end-of-dump line) and
the rest of the buffer follows. Without that marker, as in a truncated
capture, the buffer is written in order.

- Text payload: written automatically after the boot sweep when the SD
  and eMMC are up; `s` writes the same buffer again without probing. `a`
  re-arms the capture, so a later `s` saves that sweep; `r`, `G`, `w` and
  paging are not captured. A Custom test without Storage never brings the
  SD card up, so it writes nothing.
- GUI: written after every Health Check or Custom Check ("Saving the
  report..."), whatever was picked, since the SD and eMMC are up from
  start-up; a custom run therefore replaces the full report with a partial
  one. The results screen says whether the save worked.

The file holds what the probes printed, including the SD and eMMC CID
serials, the console serial and the WLAN and Bluetooth MAC addresses.

## What it changes on the console

- eMMC: no data is written. The BOOT0 probes change the selected
  partition and select USER again. BIS runs with its cache off, so ending
  it has nothing to flush.
- SE keyslots 0 and 1: loaded with `bis_key_00` and cleared after the
  reads, by `probe_serial` and by the GUI's `console_serial()`. When
  hwtest loaded no key it leaves them as they were.
- Memory: the full CAL0 carries the console's device, SSL and eTicket
  keys and certificates, so, like the keyfile body, it is wiped with
  volatile stores before it is freed; the parsed key is wiped after use.
  Two exits leave something behind. When the full CAL0 was not read, the
  probe returns with the two decrypted sectors (serial, MACs, country
  table) still in its static buffers, although the code's comment says
  every exit wipes them. On `bad hex in bis_key_00` the key bytes parsed
  before the bad digit stay in a stack array. The serial and the CAL0
  text fields copied out for printing are not wiped either, but they are
  in the log anyway.
- SD card: read-only apart from the report file and its `backup` folders.
  The probes bind `0:` with their own mounts.
- Bring-up: the SD card and eMMC are initialised before the sweep, and
  `probe_storage` leaves a device that is already up alone.

## What a failure points at

- **`emmc_bus` narrower than 8-bit:** the eMMC's data lines or the chip,
  board damage or a failing part. **`USER switch-back failed`:** the eMMC
  stopped taking a partition switch in the middle of the sweep.
- **`emmc_errors`:** with `init fail`, some speed level did not come up,
  even if the mode line looks fine; with `R/W fail`, transfers failed five
  times in a row. Read it with the mode and bus lines.
- **`emmc_health` FAIL:** the chip reports its life exceeded or its
  reserved blocks nearly gone. Data loss is likely.
- **`emmc_health` WARN:** late-life wear, or a chip that does not report
  its wear or reports a reserved value.
- **`gpt` FAIL:** the primary GPT header or its entry array is damaged.
  After a `USER switch-back failed`, `signature missing` is BOOT0 still
  selected, not a damaged table.
- **`prodinfo` `GPT entry unusable`:** the eMMC, not the keys.
- **`prodinfo` `decode failed (wrong keys?)`:** a `prod.keys` from
  another console or, with a red `BIS read failed` line, an eMMC read
  error.
- **`wlan_cal` FAIL:** the Wi-Fi calibration data is gone; radio parts
  will not bring it back.
- **`cal0_hash` FAIL:** PRODINFO decrypts but was corrupted or edited.
- **`cal0_model` WARN:** an eMMC from the other console family.
- **`cal0_panel` WARN:** the fitted panel is not the one the factory
  recorded, as after a screen replacement; see
  [display](display.md#cal0-lcd-vendor-against-the-fitted-panel).
- **`xc_emmc_mode` WARN:** HS400 on a narrow bus; `emmc_bus` FAILs with
  it.
- **`sd_bus` WARN or `sd_errors` FAIL:** the card or its slot; with
  `init fail`, some SD speed level did not come up. An `sd_errors` WARN
  on its own can come from eMMC retries.
- **`fuses_pkg1`:** the firmware's history, not the hardware.

## What it cannot tell

- Signal quality or a measured transfer speed.
- A missing SD card, as far as the verdict goes: no `sd_bus` key, a
  passing `sd_errors`, and a `prodinfo` that only says `no keys`. No key
  names the card; only printed lines do: `sd_initialize FAILED`,
  `SD not ready` on the content page, `skipped (SD not ready)` on the
  `BIS keys` line, the note on the error-counter page and the `[save]`
  line.
- The backup GPT, which is not read, or whether the layout, partition
  types and GUIDs match a stock console.
- Anything about pkg1 beyond the main header's timestamp: no signature or
  hash check, no backup pkg1, no BOOT1. The fuse table is applied without
  asking whether the unit is a production or a development one.
- Whether the BootROM would actually enter RCM.
- Bad or spare block counts beyond `PRE_EOL_INFO`; the wear figures are
  the vendor's own estimates.
