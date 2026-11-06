# Power and charging

Ten probes, one LCD page each, all named "Power & charging" (over UART each
prints the same `--- Power & charging ---` boundary, which the host viewer
merges into one page): main PMIC, MAX77812, PMIC GPIOs, regulators, 5 V
regulator, fuel gauge, charger, USB-PD controller, thermal sensor and fan,
in that order. They are the Power group ("Power / thermal" in the text
launcher's Custom test, "Battery, charging and power" in the GUI's Custom
Check) and run straight after Identity. The cross-checks on the Verdict
page run after all of them and re-read their chips live. The page table is
in the [README](../README.md).

Two facts apply across these pages, so they are stated once:

* Every regulator voltage (MAX77620, MAX77621, MAX77812) is the programmed
  register setting, never a measurement: the MAX77620 does not measure its
  outputs. The measured voltages are the fuel gauge's (`VCELL`, `OCV`,
  `Min volt`, `Max volt`).
* bdk's `i2c_recv_byte` returns 0 when a chip does not ACK, and 0 decodes
  as a healthy reading for the bucks, the charger and the thermal sensor
  alike. The MAX77621s, the BQ24193 and the TMP451 therefore get an
  explicit ACK check (the BQ24193 and the TMP451 before any other read)
  and share one no-ACK line, `<chip>: no ACK at 0x<addr> (dead?)`, with
  the reason `no I2C ack at 0x<addr>`. The MAX77812 is found by its
  `VERSION` value, the fuel gauge by `DevName`, and the MAX77620 gets no
  presence check at all.

## What hwtest has already changed

The pages show the console after hwtest's own start-up, not as it entered
RCM. bdk's `hw_init()`, the first call in `ipl_main`, switches LDO2 (the SD
card rail) off on both SoCs. On Erista it also sets LDO4 to 1.0 V, writes
`FPS_CFG0`-`FPS_CFG2` with time-period code 7, sets SD0 (`vdd_core`) to
1.125 V, disables the outputs of MAX77620 GPIO5/GPIO6 (the CPU and GPU buck
enables) and puts both MAX77621 in their POR configuration; `sdram_init`
then sets SD1 to 1.125 V. On Mariko it sets SD0 to 1.05 V and enables SD2 at
1.325 V, plus LDO8 at 2.8 V on a Lite, where LDO8 also powers the I2C1 pads.
SD card init then switches LDO2 back on at 3.3 V (1.8 V after a UHS
switch). The text payload does this before the sweep when Storage is
selected, the GUI always at start-up, so with a card that initialises,
LDO2 reads ON on the Regulators page; a failed init switches it off again.
Both front-ends call `tmp451_init()` first (the text payload when Power is
selected, the GUI once at start-up): alert off, 0-127 C range, a remote
offset of 0 on Erista and -12.5 C on Mariko, conversion rate 2. The GUI
also keeps the touchscreen powered for its own input (LDO6 at 2.9 V),
switching it on at start-up and again after every check.

Later pages change rails, so a pager refresh (`a`) or a GUI re-run can read
differently from the first sweep. The Wi-Fi PCIe page sets LDO1 and LDO7 to
1.05 V, enables them and leaves them on; the Bluetooth and game card probes
raise LDO1/LDO7 and LDO3/LDO5 and put them back as found; the Inputs pages
switch LDO6 on and off again; the Dock Test's TV output enables LDO7 and
LDO8 when it finds them off. Leaving the Dock Test only turns the dock's
LED off and exits DisplayPort mode: the PD contract and the data role it
negotiated stay, so a GUI re-run's USB-PD page can show them in
`Selected` and `Data role`.

## Main PMIC (`probe_pmic`)

The MAX77620 on I2C5 at `0x3C`: `CID3` (silicon revision), `CID4` (OTP
version) and `CID5` (ES revision, raw). The OTP differs per SoC generation,
`0x35` for Erista and `0x53` for Mariko, so `CID4` is compared with the
SoC's `HIDREV` major (1 = T210, 2 = T210B01). A mismatch points at a
swapped PMIC or a wired-in module.

* `soc_pmic_otp`: PASS when they match, otherwise FAIL
  `chip M<major>, otp 0x<cid4>`. The MAX77620 gets no separate presence
  read, so one that does not answer reads `CID4` 0x00 and fails here.

### MAX77621 (Erista only)

Erista has one single-phase MAX77621 buck per rail, CPU at `0x1B` and GPU at
`0x1C`; the later models use the MAX77812, so this block only appears when
the `HIDREV` major is 1. After both `CHIPID1` values, each rail is checked
by reading `VOUT` (0x00). A NAK prints the no-ACK line and sets
`max77621_cpu` or `max77621_gpu` FAIL; an ACK sets it PASS and decodes
`VOUT` and `VOUT_DVS` (bit 7 enable, then 606250 uV + N * 6250 uV).

The verdict is the ACK alone. The line shows the legal range, 0.606-1.400 V,
but the 7-bit field cannot encode anything outside it, so the red colour
never fires. OFF is normal in RCM. The range replaced 1.0 V (CPU) and 1.2 V
(GPU) floors that were bdk's default fields, not operating minima: hekate
starts CPU0 at 0.950 V and Horizon's DVFS tables go lower.

ON/off is the `VOUT` enable bit; the buck's physical enable is MAX77620
GPIO5 (CPU) or GPIO6 (GPU), on the GPIO page. `VOUT_DVS` can read ON while
`VOUT` is off, because bdk's voltage write also loads `VOUT_DVS` with its
enable bit set and switching off clears `VOUT` only. The Wireless page's
CPU0 bring-up sets the CPU rail to 0.950 V and later switches it off (see
[wireless](wireless.md)), so a re-run in the same boot can find `VOUT_DVS`
still ON at 0.950 V.

## MAX77812 (`probe_max77812`)

The multi-phase buck for CPU, GPU and memory VDDQ on Mariko, OLED and Lite,
on I2C5. On Erista the page says only

```
  N/A          : Erista uses dual MAX77621 (see PMIC page)
```

and sets no key; the header is kept so the page does not come out blank
like a crashed probe.

The fuse names the variant: bit 0 of `FUSE_RESERVED_ODM28_B01` set means
PHASE211 at `0x33` (2-phase M1 GPU, 1-phase M3 VDDQ, 1-phase M4 CPU: the
retail part), clear means PHASE31 at `0x31` (3-phase M1 high-power GPU plus
M4). bdk addresses the chip the same way. The probe tries the fused address,
then the other; a chip is present when `VERSION` (0x14) masked to 3 bits
reads 4 (ES2) or 5 (QS). Neither answering gives
`No MAX77812 at either 0x33 or 0x31` and `max77812` FAIL `no MAX77812 ack`.
Found only at the other address, the `Variant` line turns yellow with
` - fuse disagrees`: a board/fuse disagreement, not a dev kit, and colour
only, as it does not reach the verdict.

| Rail | Reg | Range judged | Variant |
|---|---|---|---|
| M1 (GPU) | `0x23` | 0.600-1.050 V | both |
| M3 (VDDQ) | `0x25` | 0.400-0.750 V | PHASE211 |
| M4 (CPU) | `0x26` | 0.600-1.235 V | both |

Each rail is 250 mV + N * 5 mV, enabled by its bit in `EN_CTRL`. M2 is not
read: on PHASE211 it reads back as the M1 master through the phase tying.
CPU and GPU get the broad normal-DVFS envelope rather than a stock value;
for M3, the LPDDR4X VDDQ, Horizon-OC validates 0.400-0.750 V and hekate
uses 0.600-0.650 V (VDD2 is MAX77620 SD1, a separate rail). In RCM the A57
cluster and GPU are offline and DRAM runs from SD1, so the expected picture
is every rail off, which adds
`(all rails dormant - normal in RCM, HOS brings them up)`.

* `max77812`: FAIL `<n> enabled rails out of band` when an enabled rail is
  outside its range (printed red with ` WRONG`), otherwise PASS. Rails that
  are off are not judged.

`EN_CTRL`, `BUCK_STAT` and `TOPSYS_STAT` are printed raw. The status bytes
are never judged: their bit meanings are not in the public MAX77812
material, and a non-zero live status is not a failed chip.

## PMIC GPIOs and FPS (`probe_pmic_gpios`)

Prints only, no key. Each of the eight GPIO registers (0x36-0x3D) is decoded
as direction (bit 1, 1 = input), drive (bit 0, 1 = push-pull), input level
(bit 2) and output latch (bit 3), beside its role: GPIO3 the 3.3 V rail,
GPIO5/GPIO6 the CPU and GPU bucks, GPIO7 LDO0 (DSI 1.2 V), the rest
generic. bdk documents those roles for T210; the Mariko names (MAX77812 M4
and M1) are the probe's own. GPIO3/5/6/7 belong to the Flexible Power
Sequencer (FPS), the PMIC's own rail sequencing, so they are coordination
signals: a low pin does not mean a rail is off. `MAX77620_REG_AME_GPIO`,
the alternate-function select, is not read, so a pin in its alternate
function is still decoded as a GPIO.

The FPS masters `FPS_CFG0`-`FPS_CFG2` follow: `slot` printed as
40 us << `TIME_PERIOD` (bits 5:3), `src` = `EN_SRC` (bits 2:1), `sw_en` =
bit 0. On Erista these are what `hw_init` has just written, so they say
nothing about the factory configuration.

## Regulators (`probe_regulators`)

The MAX77620's four step-down converters and nine LDOs, read only, judged
against two things kept apart. The supported domain is what bdk's regulator
table accepts: an enabled rail outside it is a FAIL. The reference is the
normal Switch/bdk setting where one is stable: an enabled rail set
otherwise is a WARN, because warm reset, boot stage and overclocking all
legitimately change programmed targets.

| Rail | Reg | Step | Domain | Reference (Erista / Mariko) |
|---|---|---|---|---|
| `SD0  (SoC)` | `0x16` | 12.5 mV | 0.600-1.400 V | none (variable) |
| `SD1  (MEM VDD2)` | `0x17` | 12.5 mV | 0.600-1.2375 V | 1.125 / 1.100 V |
| `SD2  (LDO src)` | `0x18` | 12.5 mV | 0.600-1.350 V | 1.350 / 1.325 V |
| `SD3  (1V8 gen)` | `0x19` | 12.5 mV | 0.600-1.800 V | 1.800 V |
| `LDO0 (Display)` | `0x23` | 25 mV | 0.800-1.200 V | 1.200 V |
| `LDO1 (XUSB+PCIE)` | `0x25` | 25 mV | 0.800-1.050 V | 1.050 V |
| `LDO2 (SDMMC1)` | `0x27` | 50 mV | 0.800-3.300 V | none |
| `LDO3 (GC source)` | `0x29` | 50 mV | 0.800-3.100 V | 3.100 V |
| `LDO4 (RTC)` | `0x2B` | 12.5 mV | 0.800-1.000 V | none |
| `LDO5 (GC I/O)` | `0x2D` | 50 mV | 0.800-1.800 V | none |
| `LDO6 (Touch+ALS)` | `0x2F` | 50 mV | 0.800-2.900 V | none |
| `LDO7 (XUSB)` | `0x31` | 50 mV | 0.800-1.050 V | none |
| `LDO8 (XUSB/DP)` | `0x33` | 50 mV | 0.800-2.800 V | none |

The setting is the masked register times the step plus the domain's floor,
so "outside the domain" can only mean above it. ON is the power-OK status
from bdk's `max77620_regulator_get_status` (a `STATSD` bit for SD rails, the
`POK` bit of `CFG2` for LDOs), not the power-mode bits the Wi-Fi PCIe page
prints as `en` (see [wireless](wireless.md)). A line reads
`off  <v> V cfg` in grey, or `ON   <v> V cfg` with
`(outside supported domain)` in red, `(reference <v> V)` in yellow, or
`(reference)` / `(variable)` in green.

* `pmic_rails`: FAIL `<n> enabled rails outside domain`; otherwise WARN
  `<n> programmed targets differ from reference`; otherwise PASS.

The reference match is exact, with no tolerance, and two references mostly
confirm hwtest's own writes took: SD1 1.125 V on Erista and SD2 1.325 V on
Mariko. Rails that are off are never judged. LDO5 is the one to know: its
OTP default is 3.1 V, above its 1.8 V domain, and nothing programs it under
RCM, so expect `off  3.100 V cfg` and no verdict; left on, it would FAIL. A
MAX77620 that does not answer reads `STATSD` as 0, so the SD rails read ON
at 0.600 V and `pmic_rails` comes out only WARN; the real failure is the
`soc_pmic_otp` FAIL.

## 5 V regulator (`probe_5v`)

`FAN`, `Joy-Con L` and `Joy-Con R`, each `enabled` or `off`. No key and no
hardware access: `regulator_5v_get_dev_enabled` returns bdk's record of
which consumers have asked this binary for the 5 V supply. On the text
payload `Joy-Con R` reads `enabled` because start-up raises it for UART-B
RX, whose level shifter is powered from that rail. `FAN` normally reads
`off`: only `fan_set_duty` raises it, and the fan probe restores the duty
it found, 0 in RCM. The GUI, a separate binary, keeps its own flags: its
UART-C debug port raises `Joy-Con L`, and the Joy-Con driver switches both.

## Fuel gauge (`probe_battery`)

The MAX17050 on I2C1 at `0x36`, read only. bdk's `max17050_get_property`
reports success whether or not the gauge answered, with zeroed reads on a
NAK, so a dead gauge prints a page of zeros; presence is judged by
`DevName` instead.

| Line | Colour |
|---|---|
| `SOC` | red below 10 %, yellow below 30 % |
| `VCELL` | red below 2500 or above 4500 mV, yellow below 3300 or above 4350 mV |
| `Temp` | red below -20.0 or above 70.0 C, yellow below 0 or from 45.0 C |
| `FullCap` | yellow when DesignCap or FullCap is 0 or below 80 % of design |
| `dQacc/dPacc` | yellow when both are 0, `(no learning activity)` |
| `DevName` | green at 0x00AC, otherwise yellow |
| `STATUS` | yellow when any bit other than POR or BI is set |
| `FSTAT` | yellow when DNR (data not ready) is set |

`FSTAT` also names RelDt2 (long relax reached), FQ (full charge seen),
EDet (empty detected) and RelDt (relaxed now), or prints
`no documented flags set`. RelDt means the current is near zero and the
readings are at their most trustworthy; DNR means the gauge is still
starting up.

`VCELL` is charge-state evidence, not a rail tolerance: hekate counts about
3.1 V on battery and 3.3 V charging as enough to boot, and 2.5-4.5 V is the
broad range an older repair test used. `Current` and `AvgCurrent` are in
mA (the gauge reports uA), positive when charge flows into the pack and
negative when the console runs from it; the GUI's "Current", the fan's
current test and `xc_charge_dir` all read this sign. `Cycles` is
informational; a high count on a healthy pack is normal and FullCap
against DesignCap is the wear signal. `OCV`, `RepCap`, `Min volt`,
`Max volt`, `V_empty`, `Age` (a percentage) and `ManName` are printed
uncoloured and not judged. `TTE` reads `invalid / charging` at 0xFFFF and
otherwise gives minutes; a 0 or 0xFFFF that persists on a discharging unit
means the gauge cannot predict. `QH/QL`, the coulomb counter, is worth a
look when both halves sit at zero. `(gauge learning)` on a non-zero
`dQacc/dPacc` proves little: bdk's gauge set-up code (which hwtest never
calls) writes fixed non-zero values there.

`STATUS` names its set bits (BR, SMX, TMX, VMX, BI, SMN, TMN, VMN,
`BST(absent)`, POR); POR alone or with BI adds
`(relearn pending, readings advisory)`. POR is the gauge's power-cycle
marker, expected after a battery swap or disconnect and on bench consoles.
Neither `STATUS` nor `FSTAT` reaches the verdict, `BST(absent)` included.

* `batt_health`: WARN `DesignCap unavailable` or `FullCap unavailable` when
  either is 0, else WARN `<p>% of design (wear grade)` below 80 %, else
  PASS. Never FAIL: the ratio is a service wear grade, not an electrical
  threshold, and zero capacity data is advisory after a gauge reset or
  relearn.
* `batt_ntc`: FAIL `<t> C implausible` below -20.0 C or above 70.0 C
  (exactly -20.0 and 70.0 are WARN); else WARN `<t> C cold` below 0 or
  `<t> C warm` from 45.0 C; else PASS. A negative reading is valid in a
  cold room and does not mean the NTC is disconnected.
* `fuel_devname`: PASS at 0x00AC, else FAIL `DevName 0x<v> != MAX17050`
  (its line is yellow, not red). A NAK reads 0x0000, so this is the gauge's
  presence check.

A dead gauge: every value 0, `VCELL` red, `batt_health` WARN
`DesignCap unavailable`, `fuel_devname` FAIL, and `batt_ntc` PASS, since 0
decodes as 0.0 C. It also blinds two other checks: the fan probe sees no
current change, so a spinning fan with a silent tach reads FAIL
`no tach and no current draw`, and `xc_charge_dir` passes on 0 mA.

## Charger (`probe_charger`)

The BQ24193 on I2C1 at `0x6B`, read only.

1. `VendorPart` (0x0A). A NAK prints the no-ACK line, sets `charger_id` FAIL
   and ends the page, so `charger_pg` and `charger_fault` stay unset rather
   than passing. Anything but 0x2F from a part that answers is a swapped-in
   replacement: `(not BQ24193?)` and WARN `VendorPart 0x<v>`.
2. `STATUS` (0x08) once and `FAULT` (0x09) twice. The first `FAULT` read
   returns what latched since the last read, the second the state now; a
   fault gone by the second read is benign, usually VBUS dipping during PD
   negotiation. Nothing else in hwtest reads `FAULT` (the GUI's own
   charge status reads `STATUS`), so only the first charger page after RCM
   entry carries latches from before hwtest; on a refresh or re-run,
   `FAULT (since last)` covers only the time since the previous charger
   page's reads.
3. `VBUS_STAT` (none, USB-SDP, Adapter, OTG), `CHRG_STAT` (not charging,
   pre, fast, done) and `PG_STAT`, red when VBUS is present and PG is 0.

* `charger_pg`: FAIL `VBUS present but PG=0` when `VBUS_STAT` is non-zero
  (OTG included) and PG is 0, otherwise PASS. Power-good must follow VBUS;
  when it does not, the BQ or the circuitry around it is at fault.
* `charger_fault`: FAIL `FAULT 0x<v> latched` when the second read has
  BOOST (bit 6), `CHRG_FAULT` (bits 5:4: input, thermal, safety timer) or
  `BATT_OVP` (bit 3), otherwise PASS.

Bit 7, the I2C watchdog, is left out: nothing writes to the BQ in RCM, so
its watchdog expires by design after about 40 s and latches on every
healthy console once a sweep runs that long (the page says so). Bits 2:0,
the NTC class, are decoded but not judged. Both reads print as
`FAULT (since last)` and `FAULT (now      )`, green at 0, otherwise yellow
and red respectively, so a `now` line with only the watchdog or an NTC code
is red while `charger_fault` passes.

bdk's decode follows (`IN current`, `IN voltage`, `System min`,
`FastCharge`, `CV target`, `Therm thresh`), then `Batt NTC`, the BQ's live
battery-temperature class from a third `FAULT` read, which is the better
source for warm, cool, cold and hot: yellow for warm or cool, red for cold
or hot, colour only. `DevID` is printed only. Last, `IPRECHG`, `ITERM`,
`Safety timer`, `I2C watchdog` and `BATFET` are informational on purpose:
in RCM HOS has not configured the charger, they may hold power-on
defaults, and no fixed threshold tells the two apart. A real BATFET-off
fault only shows once HOS has configured the charger.

## USB-PD controller (`probe_usbpd`)

The BM92T36 on I2C1 at `0x18`, read only. No contract is negotiated: the
handshake changes the port's state (a 15 V request, a data role swap,
DisplayPort mode), so it belongs to the GUI's Dock Test ([dock](dock.md)).

* `usb_pd`: PASS when bdk's `bm92t36_get_version` sees firmware type 0x36,
  the ROHM manufacturer ID and the BM92T device ID; otherwise
  `Version read FAILED` and FAIL `BM92T36 not responding`, which also
  covers a chip that answers with other IDs. `Version` is printed only on a
  pass, so it can only read `0x03B00036`.

bdk's source information is read regardless, into zeroed buffers, so a dead
chip reads `not inserted` with no PDOs: `Cable` (`STATUS1` bit 7),
`PDO count` and one `PDO[i]` per fixed-supply PDO, and `Selected` with a
cable in. `STATUS2` (0x04) follows as two raw bytes, with `Data role` and
`CC orient.` decoded from bits 4 and 5 of its first byte to catch a port
that works one way round only. The Dock Test reads those from `STATUS1`
instead: `usbpd.h`, following CTCaer's `bm92txx.c`, puts the data role in
bits 9:8 and the live CC side in bit 11. If the `STATUS2` read fails, the
page prints `STATUS2 read FAILED (cable disconnected?)` in yellow instead
of those three lines. It is an I2C read from the same chip as the version,
so a silent BM92T36, already shown by `Version read FAILED`, fails this
one too and still prints the cable hint. None of this reaches the verdict.

## Thermal sensor (`probe_thermal`)

The TMP451 on I2C1 at `0x4C`, read only. Presence is a read of the
manufacturer ID (0xFE): a NAK sets `tmp451_id` FAIL and ends the page,
leaving the temperature keys unset; otherwise PASS, the ID itself unused.

`SoC die temp` is the remote channel (a diode in the SoC, including the
-12.5 C offset on Mariko) and `PCB temp` the local one. `Status` (0x02)
flags OPEN (bit 2, red), BUSY, LHIGH, LLOW, RHIGH and RLOW; OPEN is the
repair fault, a loose or unsoldered remote-diode line on the SoC's
under-fill. `Cnv rate` is read at 0x04 (bdk's 0x0A is the write address);
after `tmp451_init` expect 0x02, `1/4 Hz`. The alert limits
(`PCB limits`, `SoC limits`) are printed as signed degrees and not judged;
bdk's init does not program them. Low 0 C / high 85 C are the power-on
defaults, meaning nothing has configured the sensor since it powered up.

* `soc_die_temp`: FAIL `remote diode OPEN`; otherwise WARN
  `<t> C above service ceiling` from 82 C or `<t> C warm in RCM` from 70 C;
  otherwise PASS.
* `pcb_temp`: WARN `<t> C above service ceiling` from 90 C or
  `<t> C warm in RCM` from 60 C, otherwise PASS. Never FAIL.

Whole degrees are compared. 82 C and 90 C are service ceilings used by
Switch hardware tests; the lower marks only flag an unusually hot RCM
bench. Heat is never a FAIL because it may reflect the state before RCM
entry; read it with the fan test and the reset history. There is no
SoC/board/battery agreement check: the sensors sit at different points with
different thermal mass and legitimately disagree in a cold-boot RCM state.

## Fan (`probe_fan`)

Every model has its fan on PWM channel 1 with the tach on `GPIO_PORT_S`
pin 7 (PS7). In RCM the fan is off, and a healthy stopped fan reads 0 rpm
like a dead one, so this is the one Power probe that drives hardware: it
reports what it found, spins the fan, measures, then puts the found duty
back. `PWM_CSR_1` is decoded fully (bit 31 enable, bit 24 the absolute 0 %
override, bits 23:16 the inverted duty); bdk's `fan_get_speed` skips the
first two and reports a disabled fan as full duty.

1. Print the duty found; if it is above 0, count the tach for 1 s, on top
   of the timing below (`As found`, red at 0 rpm, no key).
2. Baseline battery current: the mean of 8 gauge samples 25 ms apart.
3. Duty 236 for 600 ms, then duty 100 and 2 s to settle. Duty 100 keeps a
   turning fan turning but does not reliably start one from standstill,
   and a turning fan's rpm climbs run over run, so absolute rpm is not
   repeatable: only "turning at all" is judged.
4. Count twice: bdk's `fan_get_speed` (2 s, rpm = pulses / 2 * 30) and the
   probe's own 1 s counter, which also counts transitions and is armed from
   the line's starting level so a line that never moves counts zero. Two
   independent numbers tell a silent tach from wrong arithmetic
   (`Tach detail`).
5. Current again at duty 100 (`Fan current`). A further 25 mA or more
   drawn from the battery (`motor is loaded`) shows the motor turning even
   with a flat tach. It compares battery current before and after, so any
   other load change in those seconds counts too, and a dead gauge reads
   0 mA both times, so it never shows the draw.
6. Print the tach pad, line and GPIO state (`Tach pad`, `Tach line`,
   `Tach gpio`; without `CNF` set the GPIO controller does not own the pad
   and reads a constant), read `PWM_CSR_1` back, restore the found duty,
   and print `PWM readback`.

The probe configures the tach pad itself (`CAM1_PWDN` to SFIO 1, input
enabled, tristated, pull-up on Erista only, PS7 an input) because bdk does
so only on its first `fan_set_duty`, which also returns early on an
unchanged duty; an unconfigured pad reads a constant, exactly like a dead
fan.

| Condition, in code order | `fan_stalled` |
|---|---|
| readback is not 100 | FAIL `PWM readback <d>`: the PWM block failed, the fan is not judged |
| bdk 0 rpm, own counter saw transitions | WARN `only <n> tach pulses`: the tach toggles, the fan turns |
| bdk 0 rpm, no transitions, drop of 25 mA or more | WARN `spins (<d> mA) but tach silent`, also printed as `Fan verdict  : SPINNING ...`: a broken tach wire or a fan without one |
| bdk 0 rpm, no transitions, no extra draw | FAIL `no tach and no current draw`: seized, unplugged or unpowered |
| bdk rpm above 0 | PASS |

There is no rpm band, and the PASS `Tach RPM` line pairs bdk's 2 s rpm
with the own counter's 1 s edge count. Restoring duty 0 makes bdk write the
enable plus the 0 % override, drop the fan's claim on the 5 V supply and
park the PWM pad. The tach pad, bdk's one-time set-up (the PWM clock on
Aula, `PV4` switched to SPIO for fan power) and the raw register are not
put back: a channel found disabled comes back as `[absolute 0% override]`
on the next sweep. The probe takes about 6 s, 5 of them at duty 100 (the
printed `3000 ms` counts only settle and own window). The kick and settle
waits honour a reboot request (an `R` on the debug console); the two
counting windows do not.

## Cross-checks

`xc_vbus_acok` and `xc_charge_dir` hold these chips against each other on
the Verdict page; they are described in [verdict](verdict.md#cross-checks).

With nothing plugged in, `charger_pg`, `xc_vbus_acok` and `xc_charge_dir`
all pass without testing anything. The input path is only exercised with a
charger connected, and `xc_charge_dir` only judges on an adapter whose
input limit is above 500 mA.

## Verdict rows and the GUI

| Key | Verdict row | GUI check | GUI category |
|---|---|---|---|
| `soc_pmic_otp` | Boot integrity | Power chip model | System and security |
| `max77621_cpu` | PMIC | Processor power chip | Power system |
| `max77621_gpu` | PMIC | Graphics power chip | Power system |
| `max77812` | PMIC | Memory and graphics power chip | Power system |
| `pmic_rails` | PMIC | Internal voltages | Power system |
| `charger_id` | Charger | Charging chip | Charging |
| `charger_pg` | Charger | Charger power input | Charging |
| `charger_fault` | Charger | Charging faults | Charging |
| `usb_pd` | Charger | USB-C power chip | Charging |
| `xc_vbus_acok` | Charger | USB-C power detection | Charging |
| `batt_health` | Battery | Battery wear | Battery |
| `batt_ntc` | Battery | Battery temperature | Battery |
| `fuel_devname` | Battery | Battery level chip | Battery |
| `xc_charge_dir` | Battery | Charge reaching the battery | Battery |
| `tmp451_id` | Thermal | Temperature sensor | Temperature and fan |
| `soc_die_temp` | Thermal | Processor temperature | Temperature and fan |
| `pcb_temp` | Thermal | Board temperature | Temperature and fan |
| `fan_stalled` | Thermal | Fan | Temperature and fan |

A row shows its worst state with the reason of its first FAIL, else its
first WARN, cut to 47 characters; keys never set are ignored (no
`max77812` on Erista, no `max77621_*` on Mariko). The GUI shows each key's
own reason one tap away, under its plain text, which matters because each
key has at most one FAIL and one WARN text. `fan_stalled`'s FAIL text,
that the fan did not spin, also covers the PWM readback case, and its WARN
text (the speed signal is missing) also covers `only <n> tach pulses`,
where the tach does toggle. `batt_health`'s worn-battery WARN text also
covers `DesignCap unavailable` and `FullCap unavailable`, which is missing
gauge data, not wear.

Charger, Battery and Thermal need Power. The PMIC row runs with Power or
Raw state, as it also carries `pmic_nverc`, `pmic_irqsd` and `pmic_intlbt`,
the PMIC latches the Raw state page reports ([raw state](raw-state.md)).
Boot integrity needs Power and Storage both: on a Power-only custom run a
`soc_pmic_otp` FAIL is recorded but shown on no Verdict row, and the GUI
lists it as not picked.

The GUI categories also show readings taken from the printed lines:

| Category | Readings (source line) |
|---|---|
| Battery | Charge (`SOC`), Capacity (`FullCap`), Design capacity, Charge cycles (`Cycles`), Voltage (`VCELL`), Current, Temperature (`Temp`) |
| Charging | Charger (`CHRG_STAT`), Power input (`VBUS_STAT`), Charger faults (`FAULT (now`), Input limit (`IN current`), Charge current (`FastCharge`), USB-C cable (`Cable`) |
| Power system | Processor supply (`SD0`), Memory supply (`SD1`), 1.8 V supply (`SD3`), Display supply (`LDO0`), Touch supply (`LDO6`), CPU/GPU regulator (MAX77812 `Variant`, absent on Erista) |
| Temperature and fan | Processor (`SoC die temp`), Board (`PCB temp`), Battery (gauge `Temp`), Fan speed (rpm) (`Tach RPM`) |

"Charger faults" is the current `FAULT` read, so past about 40 s it can
show `watchdog` on a healthy console. "Processor supply" is SD0, the SoC
core rail `vdd_core`, not the CPU cluster's buck (MAX77621 CPU or MAX77812
M4). No other line is taken as a reading; the full pages are behind each
category's report view.
