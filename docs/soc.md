# SoC identity and fuses

Three pages of the sweep identify the chip itself: `probe_soc` (page "SoC"),
then `probe_fuses` and `probe_kfuse` (both "Fuses"). They read the SoC's
revision register, the values burnt into its fuses at the factory and since,
and the state of the separate KFUSE block. Almost everything is printed for
the record; only KFUSE sets a verdict key. The terms (HIDREV, speedo, IDDQ,
SBK / DK, KFUSE) are in [terms](terms.md), the page list in the
[README](../README.md).

## Where it runs

All three pages are in `PROBE_G_IDENTITY`, which a custom run calls
"Identity / fuses" in the text launcher and "Identifying the console" in the
GUI. The text pager gives each probe its own LCD page. Over UART the sweep
prints a `--- Fuses ---` line before each of the two fuse probes, and the
host viewer, `host_tools/hwtest_viewer.py`
([README](../README.md#host-viewer-host_toolshwtest_viewerpy)), treats
same-named boundaries as one logical page; the GUI's capture does the
same, appending both probes' output to one "Fuses" page.
On the GUI's progress screen "Fuses" has no label of its own and counts as
part of the "Identifying the console" step.

Every line the probes print goes through `_log_emit`: to the debug
console, to the report buffer that `save_report` writes to the SD card, to
the GUI's page capture, and to the LCD whenever that is not suppressed.
That includes the SBK and DK when they are readable; see
[SBK / DK](#sbk--dk).

## SoC

`probe_soc` reads one register, `APB_MISC_GP_HIDREV` (offset `0x804` in
APB_MISC), and writes nothing. Under `[SoC]` it prints the raw value, the
chip ID (bits 15:8), and the major (bits 7:4) and minor (bits 19:16)
revision as `Major.Minor`. The name after the chip ID comes from the major
alone: 2 prints `T210B01 Mariko`, every other value prints `T210 Erista`.
The chip ID byte is shown but not used.

No verdict key is set here. The SoC identity is checked against other
sources in three places. Two set a verdict key: the MAX77620 OTP check
(`soc_pmic_otp`, see [Verdict](#verdict)) and the PRODINFO product model
on the Storage page ([storage](storage.md); `cal0_model`, a WARN when the
model's family disagrees with the SoC). The third, in `probe_boot0_pkg1`
on the same page, is printed only: when the pkg1 layout it finds in BOOT0
is not the SoC's, it prints a yellow
`Note : SoC=..., BOOT0=..., using BOOT0 layout` line.

The SoC name above, the product-model check, the BOOT0 note and the RCM
colouring below test for a major of exactly 2 and call every other value
Erista. bdk's `hw_get_chip_id`, which the fuse helpers below rely on,
treats any major of 2 or more as T210B01, so these differ from it only on
a major above 2. `soc_pmic_otp` needs a major of exactly 1 or 2, so it
fails one above 2 and also a major of 0, which bdk counts as T210.

## Fuses

`probe_fuses` reads the fuse cache registers (`FUSE()`, base `0x7000F800`).
It does not sense, program or re-clock the fuse block. The one place it
goes beyond a cache read is the RCM check below. It prints five sections.

### Identity

`[Fuses - identity]` opens with these lines, then the RCM and burnt-fuse
lines described next.

| Line | Source | Printed as |
|---|---|---|
| `PRODUCTION` | `FUSE_PRODUCTION_MODE` (`0x100`) | raw hex |
| `SKU_INFO` | `FUSE_SKU_INFO` (`0x110`) | raw hex, not decoded |
| `ODM4` | `fuse_read_odm(4)` (`0x1D8`) | raw hex, then `DRAM ID` |
| `HW state` | `ODM4` bits 1:0 | `Dev` when both are set, else `Prod` |
| `HW type` | `fuse_read_hw_type` | `0 (Icosa)`, `1 (Iowa)`, `2 (Hoag)`, `3 (Aula)` |
| `Keygen rev` | `fuse_read_odm_keygen_rev` | decimal |

`DRAM ID` is `fuse_read_dramid(true)`: `ODM4` bits 7:3, and on T210B01 also
`ODM4` bits 14:12 as ID bits 7:5. The raw form skips bdk's clamping of
out-of-range IDs, so the number is what is burnt, not what bdk would use.
`probe_dram` ([memory](memory.md)) and ramtest ([ramtest](ramtest.md))
read the same raw ID.

`HW type` on T210B01 is `ODM4` bits 19:16: 2 is Hoag, 4 is Aula, 1 and any
other value Iowa. On Erista bdk returns Icosa without reading a fuse, so
this line cannot tell Erista boards apart; the PRODINFO product model on
the Storage page is where models differ.

`Keygen rev` on T210B01 is `ODM2` bits 4:0. On Erista the same field is
used only when `ODM4` bit 11 is set and `ODM0` / `ODM1` hold the two values
bdk checks for (`0x8E61ECAE`, `0xF2BA3BB2`); otherwise it prints 0.

### Patched RCM

`fuse_check_patched_rcm` (bdk) answers yes straight away when
`FUSE_RESERVED_SW` bit 7 is set or the chip is T210B01. Otherwise it walks
the BootROM ipatch records down from the top of the fuse array (word 191),
sized by `FUSE_FIRST_BOOTROM_PATCH_SIZE`, and answers yes if any patch
targets address `0x769A`.

| Result | Line reads | Colour |
|---|---|---|
| not patched | `no (exploitable)` | green |
| patched, Mariko | `yes (Mariko - expected)` | default |
| patched, Erista | `yes (ipatch burnt)` | yellow |

The colours follow the source comment: every Mariko ships with the BootROM
RCM bug patched, so that is neutral; on Erista a patched unit is
interesting but not a hardware fault; an unpatched Erista is the state
expected of a console running this payload at all.

On an Erista with `FUSE_RESERVED_SW` bit 7 set, bdk answers yes without
looking at a single ipatch, yet the line still says `ipatch burnt`. The
`USB ctrl` line further down prints that same bit: `XUSB` there on an
Erista means the "patched" answer came from the bit.

The ipatch walk runs only on an Erista with bit 7 clear, and it is the one
part of `probe_fuses` that writes a register. Each `fuse_read` writes
`FUSE_ADDR` and a read command to `FUSE_CTRL`, then waits in
`fuse_wait_idle`, which has no timeout: the same kind of wait `probe_kfuse`
exists to avoid. `FUSE_ADDR` is left at the last word read.

### Burnt fuses

`Burnt fuses : N | M (ODM7 | ODM6)` gives the number of bits set in `ODM7`
(the anti-downgrade fuses) and in `ODM6`. The line is red only when `ODM7`
reads `0xFFFFFFFF`, all 32 burnt, and green otherwise, 0 included. `ODM6`
is printed and never judged. The colour does not compare the count with
any firmware; that is `fuses_pkg1` on the Storage page (see
[Verdict](#verdict)). The source comment above the line says 255 means
over-burnt; the code never produces 255 and does not test whether the
burnt bits are contiguous.

### Speedo, IDDQ and revisions

`[Fuses - speedo / IDDQ]` prints its values as decimal integers unless
the table says otherwise. None of them has a threshold, a colour or a unit
in the code.

| Line | Source |
|---|---|
| `CPU SPEEDO 0` | `FUSE_CPU_SPEEDO_0_CALIB` (`0x114`) |
| `CPU SPEEDO 1` | `FUSE_CPU_SPEEDO_1_CALIB` (`0x12C`) |
| `CPU SPEEDO 2` | `FUSE_CPU_SPEEDO_2_CALIB` (`0x130`) |
| `SOC SPEEDO 0` | `FUSE_SOC_SPEEDO_0_CALIB` (`0x134`) |
| `SOC SPEEDO 2` | `FUSE_SOC_SPEEDO_2_CALIB` (`0x13C`) |
| `CPU IDDQ` | `FUSE_CPU_IDDQ_CALIB` (`0x118`) times 4 |
| `GPU IDDQ` | `FUSE_GPU_IDDQ_CALIB` (`0x228`) times 5 |
| `SOC IDDQ` | `FUSE_SOC_IDDQ_CALIB` (`0x140`) times 4 |
| `BROM rev` | `FUSE_SOC_SPEEDO_1_CALIB` (`0x138`), in hex |
| `FT rev` | `FUSE_OPT_FT_REV` (`0x128`) |
| `CP rev` | `FUSE_OPT_CP_REV` (`0x190`) |
| `USB ctrl` | `FUSE_RESERVED_SW` bit 7: `XUSB` when set, `USB2` when clear |

The IDDQ multipliers are the ones Nyx uses. SOC speedo 1 is shown as the
BootROM revision rather than as a speedo. `FT rev` and `CP rev` print as
`major.minor (0xraw)`, major from bits 10:5 and minor from bits 4:0.

`USB ctrl` reports the fuse bit alone. Nyx also says XUSB for every
T210B01, and hwtest's own USB serial console (`gui/usbser.c`) picks the
XUSB controller on every Mariko from the chip ID. On a Mariko with the bit
clear this line would say `USB2` while hwtest's USB serial drives XUSB on
the same console.

### Lot and wafer

`[Fuses - lot / wafer]` is printed as read; nothing here is judged.

* `Vendor code`: `FUSE_OPT_VENDOR_CODE` (`0x200`), decimal.
* `FAB / LOT`: one base-36 character from `FUSE_OPT_FAB_CODE` (`0x204`),
  followed with no separator by five from `FUSE_OPT_LOT_CODE_0` (`0x208`),
  six bits each from bits 29:0, most significant first. A value of 36 or
  more prints `?`. `FUSE_OPT_LOT_CODE_1` is not read.
* `Wafer ID`: `FUSE_OPT_WAFER_ID` (`0x210`), decimal.
* `X / Y coord`: `FUSE_OPT_X_COORDINATE` (`0x214`) as a 9-bit signed value
  (bit 8 set subtracts 512); `FUSE_OPT_Y_COORDINATE` (`0x218`) as read,
  with no sign extension.

### Public key

`[Fuses - Public Key SHA-256]` prints `FUSE_PUBLIC_KEY0` to
`FUSE_PUBLIC_KEY7` (`0x164` to `0x180`), each word byte-swapped, as two
lines of 32 hex digits, `PK0..3` and `PK4..7`. The header calls it a
SHA-256; the probe does not compare it with anything.

### SBK / DK

Under `[Fuses - SBK / DK]` the probe reads `FUSE_PRIVATE_KEY0` to
`FUSE_PRIVATE_KEY4` (`0x1A4` to `0x1B4`). When all five read `0xFFFFFFFF`
it prints `SBK / DK : locked out (bootrom)` in the default colour: the
source comment calls this the bootrom locking the keys out post-pkg1, the
normal expected state and not a fault or a warning.

Otherwise it prints `SBK` (words 0 to 3, byte-swapped) and `DK` (word 4) in
full, uncoloured. Nothing is redacted, so a console whose keys are readable
puts its SBK and DK on the debug console, in the SD report and on screen.
A partial pattern, some words `0xFFFFFFFF` but not all five, also
prints every word.

Once the keys are locked out the probe sees only `0xFFFFFFFF`, so it cannot
say whether a key was burnt at all: bdk defines
`FUSE_PRIVATE_KEY0_NONZERO` to `FUSE_PRIVATE_KEY4_NONZERO` (`0x80` to
`0x90`), and the probe does not read them.

## KFUSE

KFUSE is the separate fuse block holding the HDCP key data; the page's
section for it is `[KFUSE (HDCP keys)]`. bdk's `kfuse_wait_ready` waits
for `KFUSE_STATE_DONE` in an unbounded loop. The source comment spells out
what that costs: if the block is not clocked, or the silicon is in a bad
state, the payload hangs, the BPMP watchdog fires, and a modchip setup
(picofly) re-injects hwtest into a reset loop. So `probe_kfuse` repeats
the clock sequence of bdk's `kfuse_read` and bounds the wait itself.

1. `clock_enable_kfuse`: put KFUSE in reset (`RST_DEV_H_SET`), gate and
   then enable its clock, wait 10 us, release reset, wait 20 us (bdk's
   comment: time for the KFUSE hardware to initialise).
2. Poll `KFUSE_STATE` (`0x7000FC00` + `0x80`) for up to 100 ms against
   `get_tmr_us`, with a wrap-safe signed compare. The first read with
   `KFUSE_STATE_DONE` (bit 16) set ends the poll, and
   `KFUSE_STATE_CRCPASS` (bit 17) from that same read is the result.
3. Read `KFUSE_STATE` once more. Every value printed comes from this second
   read, not from the one that was judged.
4. `clock_disable_kfuse`: assert reset and gate the clock.

KFUSE is left in reset with its clock off whatever its state before, and
nothing is restored; bdk's own `kfuse_read` ends the same way. The probe
never sets `KFUSE_KEYADDR` or reads `KFUSE_KEYS`, so no key word is read.

| Bits | Field (`kfuse.h`) | Meaning, from the source comment |
|---|---|---|
| 5:0 | `CURBLOCK` | block the decoder is on, or finished on |
| 13:8 | `ERRBLOCK` | block where the CRC failed; meaningful only with `DONE` set and `CRCPASS` clear |
| 16 | `DONE` | decode complete |
| 17 | `CRCPASS` | CRC verified across all 144 blocks |
| 24 | `RESTART` | controller in restart |
| 25 | `STOP` | controller stopped |
| 31 | `SOFTRESET` | block held in soft reset |

The comment gives about `0x00030030` (`DONE`, `CRCPASS`, `CURBLOCK` 48) as
what a healthy unit shows, with `CURBLOCK` varying by silicon revision.

The page always prints `KFUSE_STATE` and `CURBLOCK`. When `DONE` came with
the CRC failed it adds a red `ERRBLOCK : n (CRC failure at this block)`.
Each of `RESTART`, `STOP` and `SOFTRESET` that is set adds a yellow line;
these are informational and do not change the finding. The last line is
the status:

| Outcome | `status` line | `kfuse` finding |
|---|---|---|
| no `DONE` within 100 ms | red `TIMEOUT (block did not assert DONE)` | WARN, `timeout` |
| `DONE`, `CRCPASS` set | green `OK (CRC pass)` | PASS |
| `DONE`, `CRCPASS` clear | red `FAILED (CRC bad)` | FAIL, `CRC bad` |

A timeout prints red but is recorded only as a WARN.

KFUSE is part of the SoC, so a CRC failure is a fault in the SoC itself;
the source comment calls `ERRBLOCK` the only useful diagnostic short of
replacing the SoC. A `TIMEOUT` comes after the probe has clocked the block
and released its reset itself, so of the two causes the comment gives for
a hang it leaves the silicon in a bad state, not a missing clock.

## Verdict

`kfuse` is the only key these pages set. Two verdict rows concern them;
the rows in general are in [verdict](verdict.md).

`Identity` holds `kfuse` alone and is shown whenever the Identity group
ran: `pass`, `warn (timeout)` or `FAIL (CRC bad)`. It is a row of its own
rather than part of Storage because, as verdict.c's comment puts it, an
Identity-only custom run would otherwise drop a KFUSE CRC failure, and a
Storage run would claim to have covered a check it never ran.

`Boot integrity` holds the two checks that test these pages' values
against another chip or the firmware. It is shown only when both Power and
Storage ran, so an Identity-only run judges neither. On a Power run without
Storage, `soc_pmic_otp` is still set but no row shows it or counts it in
the totals: the only sign of a mismatch is the red `CID4 OTP` line on the
Power page, and the GUI lists the check as not picked.

* `soc_pmic_otp` (`probe_pmic`, Power): the MAX77620 CID4 OTP byte must be
  `0x35` with a HIDREV major of 1, or `0x53` with a major of 2. Anything
  else is a FAIL, detail `chip M<major>, otp 0x<cid4>`, and the line says
  `mismatch with SoC!`. The source comment reads a mismatch as a swapped
  PMIC or a wired-in module. There is no separate presence read, and bdk's
  `i2c_recv_byte` returns 0 on a NAK, so a PMIC that does not answer fails
  here too, as `otp 0x00`: not necessarily a wrong part. See
  [power](power.md).
* `fuses_pkg1` (`probe_boot0_pkg1`, Storage): the `ODM7` bit count against
  the count the BOOT0 pkg1 requires, looked up from its build timestamp.
  More burnt than required is a FAIL,
  `N burnt > M: OFW won't boot, CFW only`; fewer is a WARN,
  `N burnt < M: OFW burns on next boot`; equal passes. It is set only when
  the timestamp matches an entry of the table exactly, so a pkg1 newer than
  the table, or none at all, leaves it unset. See [storage](storage.md).

## In the GUI

| Key | Row name | What a finding says |
|---|---|---|
| `kfuse` | Security fuses | WARN: could not be read in time. FAIL: damaged, the official system will not start |
| `fuses_pkg1` | System version fuses | WARN: system software newer than the fuses, normal on a modified console, the official system updates them on its next start. FAIL: fuses newer than the system software, the official system will not start |
| `soc_pmic_otp` | Power chip model | FAIL only: a wrong power chip, most likely a wrong replacement part |

A check's entry in gui/gui_text.c may give a `soft` prefix: a WARN whose
detail starts with it is shown only as "Not checked on this console."
`kfuse` has none, so `timeout` shows as a real warning ("Worth a look").

All three sit in the "System and security" category, built from the
`Boot integrity` and `Identity` rows, with the SoC, Fuses and Storage
pages behind its technical details and "Fuses and system data are
consistent." when every check passes. When Power and Storage did not both
run, Boot integrity's checks are listed as not picked and an otherwise
clean category shows "Partly checked".

The category shows two readings from the Fuses page: `Fuses burnt` (the
`Burnt fuses` value up to its bracket, `N | M`) and `RCM` (the whole
`Patched RCM` value). The "Processor and memory" category's `Processor`
and `Revision` readings, from the SoC page, never appear
([gui](gui.md#results)).
