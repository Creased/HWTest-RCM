# Wireless probes

The Wireless group runs three pages, in this order: the Bluetooth probe
(`probe_bt_radio`), the Wi-Fi bring-up (`probe_wifi_pcie`) and the Wi-Fi
link page (`probe_wifi_link`). The order matters, because of PH0 (see
below). The [verdict](verdict.md) note covers the Verdict rows they feed,
and the [README](../README.md) has the overview.

## Bluetooth radio probe

Most of hwtest reads. This probe writes, and it is worth knowing what it does.
(The GUI's [Bluetooth test](bluetooth.md) brings the radio up the same way
and then talks to a controller.)

The Switch's Broadcom CYW4356 carries WLAN on **PCIe** and Bluetooth on a
plain 4-wire UART (Tegra UART-D, `0x70006300`). The UART half is the cheap
measurement, and it is the one that runs first: both radios share one package
and its internal regulators (`BT_REG_ON` and `WL_REG_ON` are OR-gated into
the shared CBUCK/CLDO), so if Bluetooth answers HCI (Host Controller
Interface, the standard Bluetooth host-to-controller command protocol), the
module is powered and running, and a Wi-Fi fault is the PCIe link, the WLAN
core or the antenna. The [Wi-Fi probe](#wi-fi-radio-probe-pcie) then goes and
finds out which.

Before the radio is touched, the probe enables LDO1 (the XUSB and PCIe 1.05 V
rail) and LDO7 (XUSB 1.05 V) on the MAX77620. No Switch device tree says
which supply reaches the radio's `BT_VDDIO`, and these are the two that
plausibly feed it. Each CFG byte is saved first and written back after the
HCI exchange, so both rails end the probe as found. LDO3/5/6/8 are left alone
on purpose: LDO5's 3.1 V OTP default would land on the 1.8 V game-card
interface, and rails left on would change what later pages measure. The
probe also routes the PMIC's second 32 kHz output to GPIO4 (`AME_GPIO` bit 4)
and sets `32K_OUT0_EN` in `CNFG1_32K` when they are clear, and leaves both
set. The chip boots and answers HCI on its internal LPO, so this affects
low-power timing, not whether it talks. The `vdd_3v3` gate (PMIC GPIO3,
owned by the Flexible Power Sequencer) is only reported: a write there can
read back as success on a rail that is still gated.

To reach the radio the probe performs a real cold power cycle, called an
arm. Each arm runs:

1. Park every Bluetooth strap and control line in a known state, with
   `BT_REG_ON` (PH4) and `WL_REG_ON` (PH1) both driven low. PH0, which gates
   the WLAN RF path, is left as found.
2. Configure the UART pads (RX with a pull-down, CTS with a pull-up) and
   UART-D at 115200, before the edge. Arm B also asserts RTS here.
3. Hold for 20 ms. The datasheet minimum is 10 ms, and it only counts while
   both REG_ON pins are low.
4. Raise `BT_REG_ON` with one masked store, producing a genuine POR
   (Power-On Reset, the chip's internal reset, which is released when
   `BT_REG_ON` rises and can hold the part for up to 110 ms afterwards).
5. Write nothing on port H for 200 ms. The straps are sampled a few
   milliseconds after POR deassertion, so the worst-case sample instant is
   about 115 ms after the edge.
6. Read PH4, PH5 and PK0-PK2, the UART line state (RX still pulled down),
   and test PH3 for `SPI_INT` (below).
7. Swap RX to a pull-up, wait 2 ms and flush the FIFO.
8. Force RTS with MCR `0x02` (auto-CTS off), then wait up to 500 ms for the
   chip to drive its `RTS_N` low.
9. Send HCI_Reset up to three times, with a 700 ms deadline each.

The power cycle is not optional, because two of these pins are *straps*:
sampled once, a few milliseconds after POR deassertion, then latched until the
next one. Re-pulsing `BT_REG_ON` without a low period changes nothing; the
part keeps whatever mode it latched first.

* `BT_HOST_WAKE` (**PH5**, chip `BT_GPIO_1`) selects the host transport. Held
  low across the edge it latches **SPI**, and the part then ignores the UART
  entirely. The probe releases it (TRISTATE + input, no pull: the `0x50` that
  NVIDIA's board pinmux and Horizon program), so the Tegra never holds it
  low. That alone does not guarantee a high strap, because the pad has a bus
  keeper that can hold a level driven onto it earlier. Arm C therefore drives
  PH5 high for 2 ms as it parks the pads, then releases it before the edge.
* `BT_GPIO4` (**PK2**, on port K, not H) is the one strap the datasheet
  tabulates: driven high, the ROM hunts for an external serial flash instead
  of presenting HCI. The probe holds PK0-PK2 as inputs.

The remaining lines are not straps: `BT_REG_ON` (PH4), `WL_REG_ON` (PH1),
`BT_DEV_WAKE` (PH3, driven low) and `BT_GPIO5` (PH7, driven low). They still
have to be in a defined state across the edge, but nothing latches them as
configuration. PH3 doubles as the transport check: in SPI mode the chip
drives that ball as `SPI_INT`, an output, which is also why it is held low
rather than driven high (that would be a drive fight). Each arm releases it
and reads it under a pull-up and then a pull-down; a level that ignores both
pulls means the chip is driving it. On return `WL_REG_ON` is driven high,
where the vendor GPIO hog leaves it, and `BT_REG_ON` stays high from the last
arm's edge. PI4-PI7 stay handed to the UART and UART-D stays clocked at
115200, with MCR at the last value tried. After an arm, PH3 and PH7 also
stay driven low, PH5 stays a tristated input and PK0-PK2 keep their input
pinmux.

The probe avoids BDK's `uart_init()`, `uart_empty_fifo()` and `uart_recv()`:
`uart_empty_fifo()` zeroes MCR and never restores it, `uart_recv()` gives up
after 250 us and drops RTS while it listens, and `uart_init()` can spin
forever on a dark line (an unbounded RX drain against the break an unpowered
radio produces). Instead the probe programs UART-D directly with its own
bounded send, receive and flush routines, and clocks the block exactly once,
because `clock_enable()` resets it.

It also self-tests. A 16550 internal loopback runs first, proving the clock,
divisor, FIFOs and helper routines without involving a single pad. That also
means it passes with every pad in GPIO mode, so the probe hands PI4-PI7 back
to the UART by clearing their `GPIO_CNF` bits and logs port I's `CNF`. **A
radio that stays silent is only reported as a failure when that loopback
passed**; otherwise the fault is on our side of the pads: the `Result` line
reads `inconclusive - UART-D loopback failed`, and `bt_hci` is recorded as a
warning (`inconclusive (loopback failed)`), never a FAIL. A loopback failure
does not stop the arms, so a radio that answers anyway still passes.

The probe does not settle for one attempt before it records silence. It runs
up to three arms and stops at the first that answers: A, the retail
configuration; B, RTS asserted across the edge, which is how autobaud is
entered (autobaud still answers a plain HCI_Reset); C, the PH5 keeper driven
high before the edge. If all three stay silent it tries four MCR values
(`0x22`, `0x60`, `0x40`, `0x00`) without another power cycle, since the strap
is already latched. It sweeps rather than reasons because the RTS bit's
polarity is self-contradictory: BDK's `uart_recv()` clears it to listen,
which implies set = blocked, while the TRM names the field `FORCE_RTS_LOW`.
Every one of them runs at 115200 8N1 (DLL
221 from the undivided 408 MHz `PLLP_OUT0`, 115385 baud, +0.16%, inside the
+/-2% the chip tolerates), the only rate the probe configures. There is no
baud sweep. What that covers is the chip's power-up mode and the host's
flow-control setting. The baud rate is not covered, so a silent unit is
reported as silent at 115200 rather than at every rate. The code puts the
arms at about 9 s of waiting for a radio that stays silent. The HCI reads
look for the host's `R` about once a millisecond and the power-cycle waits
poll for it too, so the host can still reboot the console meanwhile.

The arm count and timings are the `BT_CFG_*` constants at the top of
probe_bt.c (arms, POR wait, the HCI MCR value, the MCR sweep, the HCI
deadline and tries). They are plain defines, not `EXTRA_DEFINES` knobs:
changing one means editing the file.

Only arm A prints its full detail on the LCD page. Each arm is about a dozen
rows, and three of them would overflow the panel, which wraps to the top
rather than scrolling. Arms B and C and the MCR sweep log in full to the UART
and the SD report and leave one `arm X : no HCI reply (detail on UART)` row
(and the `MCR sweep` summary) on screen.

An answer means a parsed Command Complete for HCI_Reset (`01 03 0C 00`):
event `0x0E`, opcode `0x0C03`, status 0. The bytes go through a
length-driven H4 framer that skips Broadcom's zero-length vendor packets
(`0x00`, `0x31`, `0x34`), consumes the fixed 63-byte `0x07` LM_DIAG packet
and resyncs over ACL/SCO/ISO, so a stray byte never counts, which matters
most in the MCR sweep. On a pass the probe waits 100 ms (the Command
Complete arrives before the part has finished resetting) and sends
Read_Local_Version_Information for the HCI/LMP versions, manufacturer and
`lmp_subver`. That read is informational and cannot change the verdict.

Order matters for a second reason. By default the Wi-Fi probe drives PH0
high and leaves it there, and the code records that on this board PH0 high
stops Bluetooth answering HCI at all, across power cycles. This probe leaves
PH0 as found, so only a Bluetooth result taken before the Wi-Fi probe has run
means anything: in practice, the first sweep after boot. Every later run of
this page comes after the Wi-Fi probe (paging back to it, `r`, `a`,
`G Wireless`, any later sweep) and overwrites `bt_hci`. Build with
`-DWIFI_CFG_DRIVE_PH0=0` to leave PH0 alone.

### Result and verdict

| `Result` line | `bt_hci` | When |
| --- | --- | --- |
| `radio is powered, running and speaking HCI` | PASS | a Command Complete on any arm or MCR value |
| `radio does NOT respond - not running` | FAIL, `no HCI response, radio not running` | the loopback passed and every arm and MCR value stayed silent |
| `inconclusive - UART-D loopback failed` | WARN, `inconclusive (loopback failed)` | the loopback failed and nothing answered; also printed when UART-D is not clocked, where the probe aborts before any arm |

The sequence is validated against known-good units, which answer HCI_Reset
with a valid Command Complete and identify as Broadcom (manufacturer
`0x000F`). A unit that stays quiet through every arm and every MCR value
while UART-D loops back cleanly is reported as not running. PRODINFO
calibration is judged separately ([storage](storage.md), verdict key
`wlan_cal`), and if it is intact this points at the module, its supply or
its solder, unless the part came up at a rate other than 115200, which this
probe does not test. An answer proves the HCI link and nothing
about the radio's RF side.

`bt_hci` and `wifi_pcie` make up the Verdict's `Wireless` row, which shows
the worse of the two (with the worst finding's reason) and appears only when
the Wireless group ran. The text Verdict counts the loopback WARN as a warn.
In the GUI, `bt_hci` is the *Bluetooth* check under *Wi-Fi and Bluetooth*,
and the loopback WARN shows there as not checked, because its reason starts
with `inconclusive`. The GUI's *Bluetooth* reading is the `Result` line, and
*Bluetooth chip* is the text in brackets on the `Manufacturer` line.

### Reading the Bluetooth output

In print order:

| Line | What it tells you |
| --- | --- |
| `port H entry` | GPIO port H as found. `OE` bit 5 set with `OUT` bit 5 clear is the Tegra itself holding `BT_HOST_WAKE` low, the SPI-strap condition |
| `PH5 pinmux`, `port I CNF` | PH5's pinmux as found, and port I's `GPIO_CNF`: PI4-PI7 reach the UART only while their bits are 0 |
| `PINMUX_GLOBAL` | bit 0 clamps every tristated input to 0, which would kill RX, CTS and PH5 at once (`INPUTS CLAMPED`) |
| `Rails up` | the power-mode field (0 = off) of the LDO1/LDO7 CFG bytes after the enable |
| `LPO 32k` | `AME_GPIO` before and after, `CNFG1_32K` and its `32K_OK` bit |
| `vdd_3v3 gate` | `AME_GPIO` bit 3 and `CNFG_GPIO3`, reported only |
| `CAR U` | UART-D's reset, clock-enable and clock-source registers. With the clock off the probe prints `UART-D : NOT CLOCKED - aborting` and goes straight to the restore |
| `IRDA_CSR` | non-zero means per-line inversion, which is otherwise symptomless: TX succeeds, LSR looks clean, nothing decodes |
| `UART-D loopbk` | the internal loopback of four bytes |
| `-- arm X --` | start of an arm |
| `pre-edge` | MCR, LSR, MSR and port I `CNF` just before the edge |
| `pads low` | PH1/PH3/PH4/PH5/PH7 with the pads parked |
| `PH4 BT_REG_ON` | read back after the 200 ms window; `(write did not latch)` if it is not high |
| `PH5 HOST_WAKE` | information only: once the ROM is up this is a programmable-polarity chip I/O, so its level is no transport verdict either way |
| `PK0/1/2 in` | `BT_GPIO2/3/4`, held as inputs |
| `line state` | LSR, read while RX still has the pull-down. The code reads BRK/FERR as nothing driving the chip's TXD (unpowered, or SPI mode, where that ball becomes `SPI_MISO`) and a clean LSR as the chip driving it idle-high, and calls this snapshot what tells the two apart. Until the chip drives its TXD the FIFO collects break-generated zeros, which is why step 7 flushes it |
| `PH3 SPI_INT` | PH3 read under a pull-up and a pull-down. Equal readings print `CHIP DRIVING - SPI latched`; the code calls this the decisive transport measurement |
| `transport rdy` | whether the chip drove its `RTS_N` (our CTS, `MSR` bit 4) low within 500 ms; `junk` is the byte count flushed in step 7. CTS has a pull-up on purpose, against the vendor pull-down, so `LOW` means the chip really drove it. It is a live flow-control output, so evidence, not a gate: HCI is sent either way |
| `HCI_Reset` | the event bytes received; `TX gated` if the transmitter never drained; or `no event` followed by `raw+NN` rows with whatever did arrive |
| `arm X : no HCI reply (detail on UART)` | the on-screen stand-in for arms B and C |
| `MCR=XX`, `MCR sweep` | one row per swept MCR value (`CC!` on a Command Complete), then the summary |
| `PH1 restored` | `WL_REG_ON` driven back high |
| `Rails restored` | the same field after the write-back |
| `HCI version`, `Manufacturer` | the version read after a pass, or `Local version : no usable reply` |

## Wi-Fi radio probe (PCIe)

The other half of the same package: the WLAN function, reached over PCIe,
which BDK does not support at all (see
[The bring-up sequence](#the-bring-up-sequence)).

### The BPMP cannot reach PCIe

Worth stating before the sequence, because every symptom of it looks like a
bring-up bug you could fix by writing one more register. You cannot: the
PCIe apertures do not answer the BPMP, and no register sequence changes that.

The Tegra X1 TRM's AHB chapter (19) puts the BPMP-Lite crossbar on AHB and
lists the AHB slaves: XBAR, DRAM, USB-OTG/USB2 and TZRAM. PCIe is not among
them. The PCIe aperture hangs off MSELECT instead, and MSELECT is documented
in chapter 16, *CPU Complex*: `MSELECT_CONFIG_0`, reset value `0x07ff4020`,
carrying the `ENABLE_PCIE_APERTURE` bit. The path runs the other way for
CPU0, which reaches the APB registers through MSELECT -> APC -> ARM7
crossbar; that is how the stub can program CAR and read the microsecond
timer.

What that looks like from a payload, with clocks, resets, power and PLLE all
correct (the BPMP column is measured on both an Erista and a Mariko):

| Access | From the BPMP | From CPU0 |
| --- | --- | --- |
| AFI window `0x01003800` | `0xFFFFFFFF` | decodes; the witness write reads back |
| `MSELECT_CONFIG_0` `0x50060000` | one constant, the same at every MSELECT offset | `0x07FF4020`, the TRM's reset value |

So the probe splits, and the PCIe-side accesses run on CPU0; see
[Running part of the probe on CPU0](#running-part-of-the-probe-on-cpu0).

(Aside if you cross-reference hekate: its `MSELECT_CFG_WRAP_TO_INCR_BPMP` for
bit 27 is a misnomer. The TRM calls that bit `WRAP_TO_INCR_SLAVE0 (APC)`,
and it is the APC bridge, not the BPMP.)

### Running it

It runs in the boot sweep by default (`WIFI_CFG_AUTORUN=1`), so the wireless
verdict is part of the report with no pager interaction required. Press `w`
on the pager, or send `W` over UART, to run it again. That runs both Wi-Fi
pages, not the Bluetooth one, and sends their output to the UART only; the
LCD page is not redrawn.

`make EXTRA_DEFINES=-DWIFI_CFG_AUTORUN=0` is meant to give a build where it
only runs when armed that way, but in the code as it stands it only quiets
the first page: `probe_wifi_pcie()` prints `Not armed` and returns, while the
next page, `probe_wifi_link()`, arms the bring-up for itself, runs it muted
and boots CPU0 anyway. So that build still does everything below in the
sweep; it only leaves the bring-up page unprinted.

The knob exists because the probe is *invasive* in a way the rest of the
sweep is not, not because it endangers the payload (a stall costs CPU0
alone, see
[Running part of the probe on CPU0](#running-part-of-the-probe-on-cpu0)).
It boots the CPU cluster (CPU rail up through the MAX77621/MAX77812, PLLX,
the CRAIL/C0NC/CE0 partitions) and cold-cycles `WL_REG_ON`, resetting the
WLAN section. hwtest is otherwise a read-only pass you can point at an
unknown console, and this costs 3-4 seconds on top. Only the cluster is put
back (powergated at the end). LDO1 and LDO7 stay on at 1.05 V, PH0 stays
high (by default), PH1 stays at `WIFI_CFG_PH1_LEVEL`, and the rest stays as
the bring-up left it: the PEX I/O pads out of deep power down and the
PEX_L1 pads unparked, the PCIE partition up with PCIE and AFI clocked and
out of reset (PCIEXCLK too, once the stub has released it), the pad
controller and the lane mux, PLLREFE and PLLE (PLLE under its hardware
sequencer), PLLX, the MSELECT clock, the FPCI timeout if the stub reached
the stage that arms it, and on Mariko the UPHY management clock.

Build knobs, all passed through `EXTRA_DEFINES`:

| Knob | Default | Effect |
| --- | --- | --- |
| `WIFI_CFG_AUTORUN` | 1 | see above |
| `WIFI_CFG_COLD_CYCLE` | 1 | 0 leaves PH0 and PH1 alone and only reports PH1 |
| `WIFI_CFG_DRIVE_PH0` | 1 | 0 leaves PH0 exactly as found |
| `WIFI_CFG_PH1_LEVEL` | 1 | the level PH1 is left at after the cold cycle. PH1 is named `wifi_rst` and the code notes that nothing establishes its polarity: if it is an active-high reset, a high PH1 holds the core in reset, which would look exactly like a link that trains with a dead core |
| `WIFI_CFG_X4_LANES` | 1 | include the unconnected x4 lanes `pcie-1..4` in the lane mux step and the B01 `CTL8` writes; the outright `USB3_PAD_MUX` write sets the final mux either way |
| `WIFI_CFG_EARLY_REGON` | 0 | drive PH1 high at the start of every sweep, as HOS keeps it; pair with `WIFI_CFG_COLD_CYCLE=0`. The Bluetooth probe, which runs before the Wi-Fi pages, still drops PH1 for each arm and then drives it high again |

The stub needs an aarch64 compiler: `A64_CC`/`A64_OC`, devkitA64 by default,
or any bare-metal aarch64 gcc. Without one the payload still builds and the
link page prints `CPU0 handoff : not built (devkitA64 absent at build
time)`; `REQUIRE_A64=1` turns that into a build error.

The link page always repeats the bring-up, with its output muted, before it
hands off: a bare handoff re-run reads `FFFF:FFFF`, because the endpoint
does not survive the PERST#/link re-train without the bring-up ahead of it.
So in the sweep and on `w` the cold cycle and the BPMP bring-up run twice,
and the printed bring-up lines (the POR window among them) come from the
first pass, not from the one the handoff follows. The mute covers the LCD
and the debug console only: the SD report and the GUI's page capture still
record the second pass, as a second `[Wi-Fi radio (...)]` block inside the
link page.

Every line it prints is flushed to the wire before the access it describes,
so if it does stall, the last line received names the access that did it.
During the muted pass only the `[step]` progress markers (listed under
[When it fails](#when-it-fails)) reach the debug console.

### The bring-up sequence

BDK has no PCIe support at all: no AFI (Address Framing Interface, the
controller's register window), no PLLE (the PCIe phase-locked loop), no UPHY
(Universal PHY) lane initialisation, not even a base address. So this probe
carries its own T210 root-complex bring-up, ported from U-Boot's
`drivers/pci/pci_tegra.c` plus its Tegra210 clock and XUSB pad-controller
code, and cross-checked against Linux's `pci-tegra.c` and
`phy/tegra/xusb-tegra210.c`.

One rule shapes the order: **a T210 block that is clocked but held in reset
does not error a read, it never completes one.** The BPMP hangs with no
abort, no timeout and no reset path. It holds for MSELECT, and for
`XUSB_PADCTL`, where a single read of `PADCTL_ELPG_PROGRAM` before its reset
clears hangs the console dead. A guard placed after the access it guards is
not a guard, and a read-modify-write like `REG |= bit` is a read.

The endpoint's own power-up is something this probe starts, so the CYW4356
datasheet's power-up timing (Figure 52 / Table 62, p147) applies end to
end. The "Table 62 clock" below counts from the moment the probe takes as
`WL_REG_ON` high (step 3):

| Interval | Name | Time |
| --- | --- | --- |
| `WL_REG_ON` high -> VDDC | `Tregontovdd` | 1 ms |
| -> internal POR | `Tvddtopor` | 57 ms |
| REFCLK enabled -> stable | `Trefclkstable` | 10 ms |
| -> PERST# high | `Tref2perst` | 100 us |
| -> first config | `Tperst2firstconfig` | 6 ms |
| `WL_REG_ON` high -> ready | `Tvregontofirstconfig` | 100 ms worst case |

The first five are minimums the host must honour: no config TLP before
about 58 ms, and the refclk running for 10 ms before PERST# rises. The last
is the other direction, a guarantee that the part is ready within 100 ms of
`WL_REG_ON`, so taking longer costs nothing.

On the BPMP, in order:

1. **Rails**, enabled *and set to the right voltage*, and left that way.
   LDO1 (`VDD_PEX_1V05`) and LDO7 (`AVDD_1V05_PLL`) are the two 1.05 V
   supplies the Switch device tree hangs off `pcie@1003000`. Enabling is not
   enough: LDO7 feeds `avdd-pll-uerefe`, the UPHY reference PLL's own analog
   supply, and a Mariko in RCM is still on its 1.000 V cold OTP default
   because HOS's `pcv`, which would program the 1.05 V the board specifies,
   never runs here. (Erista boots at 1.05 V, so this is Mariko-specific.)
2. **PEX_L1 pads unparked.** `PINMUX_AUX_PEX_L1_RST_N` resets to `0x460`,
   and bit 5 of that is PARK: a parked pad does not drive, so the controller
   can toggle PERST# all it likes and the endpoint never sees it. The probe
   clears PARK and TRISTATE on the reset line, and only PARK on `CLKREQ#`,
   which stays tristated as TRM 34.3.1 step 19a asks when L1 PM substates
   are off.
3. **`WL_REG_ON` cold cycle** on PH1, which starts the endpoint's own
   power-on reset. `BT_REG_ON` is left alone, so this resets the WLAN
   section without disturbing the Bluetooth result above. PH1 is driven low
   and, by default, PH0 (`wifi_en`) with it. After 20 ms PH0 is raised and
   left high, and the Table 62 clock starts; 10 ms later PH1 goes to
   `WIFI_CFG_PH1_LEVEL`, so it is low for about 30 ms in all. With PH0 left
   alone the clock starts at the same point. See the
   [Bluetooth probe](#bluetooth-radio-probe) for why PH0 high matters there.
4. **Tegra side**, interleaved into that reset window. PEX I/O pads out of
   deep power down. The PCIE power partition ungated if it is gated (with
   the REMOVE_CLAMPING quirk that swaps the PCIE and VDEC bits) and checked
   through `PWRGATE_STATUS`, a hard stop if it is not up. AFI is then
   released and clocked and the CML clock set (`PLLE_AUX` bit 0 set, bit 1
   clear). Then U-Boot's full powergate off/on cycle runs all the same, with
   PCIE, AFI and PCIEXCLK held in reset, after which PCIE and AFI are
   released and clocked again (PCIEXCLK waits for CPU0). Then the pad
   controller: its `RST_DEVICES_W` bit is cleared and read back
   before the first `XUSB_PADCTL` access, the probe refuses to go on if it
   is still set, and the ELPG clamps are released in order. Then the lanes:
   `FORCE_PCIE_PAD_IDDQ_DISABLE` for all five, P0-P4 each parked under TX/RX
   power and IDDQ overrides while the mux changes (TRM 34.3.1 steps 16-18),
   `pcie-0` muxed to `pcie-x1` and `pcie-1..4` to `pcie-x4`, then
   `USB3_PAD_MUX` written outright to `0x817FC1FE`, U-Boot's exact state,
   and the overrides released. On B01 each lane also gets its `CTL8`
   setting (why: see step 6). PLLE and the UPHY calibration are left to
   CPU0, where U-Boot's driver runs them. The AFI cross-bar and fuse
   configuration wait for CPU0 too: the BPMP cannot see those registers.

   **On Mariko there is one more clock, and without it the calibration can
   never finish.** T210B01's padctl PCIe pad takes a second clock that
   Erista's does not: NVIDIA's own tegra210b01 device tree gives it
   `clock-names = "pll", "uphy_mgmt"` where tegra210.dtsi has only `"pll"`,
   and L4T enables `uphy_mgmt` inside `tegra210_pcie_pad_probe()` behind
   `if (t210b01_compatible(padctl) == 1)`, ahead of every UPHY access. The
   probe does the same, before the pad controller leaves reset. It clocks
   the calibration state machine, so with it gated `CAL_EN` latches and
   `CAL_DONE` stays 0 for ever, while ELPG, the lane mux and CTL1/2/4 all
   read and write perfectly, because those are ordinary APB registers. Two
   CAR registers BDK never touches: `PLLP_MISC1` (0x680) carries the
   `pll_p_out_hsio` and `pll_p_out_xusb` branch gates, and
   `PEX_SATA_USB_RX_BYP` (0x6D0), which means something else entirely on
   Erista, is repurposed on B01 as this clock's 7.1 divider plus enable
   gate. 408 MHz over a divider value of 6 gives the 102 MHz NVIDIA's init
   table asks for. This is exactly the class of difference that lets the
   U-Boot sequence work on a Jetson TX1 and fail here, and no T210-only
   reference can show it to you.
5. **POR window.** Before anything asks the endpoint a question, the probe
   pads the time since the Table 62 clock started to at least 58 ms
   (`Tvddtopor` is 57 ms).

Then on CPU0, in the stub, after the hand-off described in the next section:

6. **PHY bring-up**, the stub's first block, before MSELECT: PLLREFE then
   PLLE (100 MHz, `CLK_SOURCE_PCIE` switched to PLLE, PLLE handed to its
   hardware sequencer), `AFI_PLLE_CONTROL` `PADS2PLLE_EN`, then UPHY PLL P0:
   brick configuration, frequency calibration, `LOCKDET`, resistor
   calibration, the XUSBIO sequencer and the x1 lane's IDDQ disable. It runs
   here because pre-calibrating on the BPMP leaves the UPHY sequencer-owned,
   which broke U-Boot's re-calibration. Every wait is bounded at 250 ms, and
   a failure is recorded for the report rather than stopping the stub.

   **And the UPHY PLL itself is configured a different way on B01.** Every
   public T210 driver writes `CAL_CTRL = 0x136` and `DCO_CTRL = 0x2a` before
   calibrating. L4T does that *only* on Erista; on T210B01 it instead loads a
   table through an indirect address/data port at `UPHY_PLL_P0_CTL10`
   (`usb3_pll_g1_init_data`), plus a per-lane setting through each lane's
   `CTL8`, which the BPMP writes in step 4. It is an either/or, not an
   addition. Take the T210 branch on a Mariko and the PLL is never
   configured: `CAL_EN` latches, `CAL_CTRL` reads back `0x136`, and
   `CAL_DONE` stays 0 for ever, which looks exactly like dead silicon and
   sends you hunting on the PCIe side, where nothing is wrong. Only L4T
   shows this; mainline has no B01 support at all.
7. **MSELECT and the AFI window.** The stub reads `MSELECT_CONFIG` and
   writes it back with `ERR_RESP_EN_PCIE` and `ENABLE_PCIE_APERTURE`, both
   already set in the reset value, then proves the AFI window with a
   write-read-restore of `AFI_AXI_BAR5_START`. If the witness does not read
   back, it stops there.
8. **Translations and controller enable.** AFI BARs byte-identical to
   U-Boot's `tegra_pcie_setup_translations` (configuration aperture at
   `0x02000000`, `FPCI_BAR0` `0xFE100000`), the cross-bar as `X4_X1` with
   both controllers enabled, `AFI_FUSE`'s GEN2-disable bit cleared,
   PCIEXCLK out of reset (the root ports live behind it), `EN_FPCI`, and
   every AFI interrupt off. Then root port 0, the unconnected x4 bundle,
   gets a full enable and up to three PERST# retries of 200 polls 2 ms
   apart before it is disabled again, because that is U-Boot's order.
9. **Link training** on root port 1, honouring the CYW4356's own ordering:
   the reference clock runs for 12 ms before a 2 ms PERST# pulse, and 8 ms
   plus link training pass before the first configuration access, both
   above the datasheet's 10 ms `Trefclkstable` and 6 ms `Tperst2firstconfig`
   minimums (Table 62, p147). Neither upstream driver waits for either; they
   drive add-in cards whose rails came up long before the driver loaded. In
   between, the stub writes `PADS_REFCLK_CFG0` (`0x90B890B8`), enables PCA
   and forces the presence map to "endpoint present", since there is no
   presence-detect pin. It then polls up to about 400 ms for `DL_UP` and as
   long again for DL active. TRM 34.4.8's sequence is deliberately skipped:
   no public driver runs it, and with its writes in place the root port's
   own config space reads back garbage.
10. **Enumeration.** The bridge is programmed first as U-Boot leaves it (bus
    numbers, memory and I/O windows, Device Control `0x2810`, command), its
    own config window is walked, and CRS software visibility is turned on,
    so a still-booting endpoint's CRS completes as `0xFFFF0001` instead of
    being retried silently by the root port. Then the endpoint. The values
    that matter (ID, class, BAR0, window, the command write) are read in
    pairs, because on the Erista test unit every second endpoint config
    read returns a stale per-boot constant; the verdict uses whichever of
    the ID pair holds the known ID. `14E4:43EC` is the
    CYW4356's WLAN function, class `0x028000` (network controller). Only
    after those baseline reads is the FPCI completion timeout armed; then
    the stub reads BAR0 and the backplane window, writes the command
    register (memory and bus master), the one config write the ROM-phase
    chip accepts, verifying it for up to 25 rounds 20 ms apart, and re-reads
    the ID to see the path is still alive. **Enumeration is the verdict**:
    the WLAN core answered config reads with the right ID, so it is present
    and on the bus. The die-level ChipID would need a firmware download (the
    backplane is firmware-gated in ROM phase), which is the OS's job, not
    the probe's.

`wifi_pcie` joins `bt_hci` on the Verdict's `Wireless` row (see
[Result and verdict](#result-and-verdict)). The CPU0 boot also sets
`cpu_complex`, which has its own `CPU complex` row (shown whenever the
Wireless group runs), so a dead CPU rail reads as that rather than only as
a Wi-Fi fault. In the GUI, `wifi_pcie` is the *Wi-Fi* check under *Wi-Fi
and Bluetooth*, and `cpu_complex` is *Main processor* under *Processor and
memory*. The readings are *Wi-Fi* (the `Result` value), *Wi-Fi chip*
(`EP config`) and *Wi-Fi link* (`RP1 link`, up to the first " (") under
*Wi-Fi and Bluetooth*, and *CPU start-up* (the first `CPU0` line, cut the
same way) under *Processor and memory*. As the code stands none of them
appears: a reading is looked up only inside the block that opens with
`[Wi-Fi radio - PCIe`, and the muted bring-up's own `[Wi-Fi radio (...)]`
header, which the capture keeps (see [Running it](#running-it)), ends that
block before the handoff prints them.

| `wifi_pcie` | Reason | When |
| --- | --- | --- |
| PASS | | the endpoint reads `14E4:43EC` |
| WARN | `endpoint is VVVV:DDDD` | the link is up and the endpoint reads any other ID. `FFFF:FFFF` is config reads returning all ones (what a bare handoff re-run reads) and `0001:FFFF` the CRS completion `0xFFFF0001` of an endpoint still in its ROM-phase init, so neither names another chip |
| FAIL | `PCIE power partition would not ungate` | step 4 |
| FAIL | `XUSB_PADCTL would not leave reset` | step 4 |
| FAIL | `CPU0 could not be powered` | the checked CPU0 boot failed (`cpu_complex` FAIL too) |
| FAIL | `CPU0 did not start` | the stub never wrote its magic word |
| FAIL | `CPU0 fault ESR ... at ...` | the stub took an exception |
| FAIL | `link up, endpoint config read hung` | a stall with the bridge already programmed |
| FAIL | `CPU0 stalled at stage N` | any other stall |
| FAIL | `PCIe does not decode even from CPU0` | the AFI witness failed |
| FAIL | `no link on root port 1` | the link did not train |

A build without the stub sets `wifi_pcie` only for the two step 4 failures,
so its `Wireless` row otherwise reflects `bt_hci` alone.

| `cpu_complex` | Reason | When |
| --- | --- | --- |
| PASS | | CPU0 was released from reset: the CPU regulator answered on I2C and took the enable, CRAIL, C0NC and CE0 powered up, and the RAM repair finished within 10 ms. It is set before the stub runs, so it does not show that the stub ran |
| FAIL | the `CPU0 boot` reason: `CPU regulator does not answer on I2C`, `CPU regulator did not take the enable`, `CRAIL partition did not power up` (or `C0NC`, `CE0`), `CPU RAM repair never completed` | any checked boot, the retries and the diagnostic re-boot included, so a later failure overwrites an earlier PASS |

Without the stub, or when the bring-up stopped before the handoff, the key
is never set and the `CPU complex` row reads `not tested`.

The emulator models all of this, including the timing gates, so a bring-up
that skips a step fails on the host with a `[pcie] root port 1 cannot train:`
line naming what was missed. `--wifi-radio healthy|faulty|absent` selects
which console you are testing against.

### Running part of the probe on CPU0

The PCIe apertures answer a CPU-complex master and nobody else, so the probe
splits in two. The BPMP does steps 1-5: rails, the PEX_L1 pads,
`WL_REG_ON`, the PEX I/O pads, the PCIE partition, PCIE/AFI clocks and
resets, the pad controller and the lane mux, all of it APB and CAR. Then it
hands over:

1. Copies a small **AArch64 stub** (`cpu_stub/`, linked at `0xA0000000`)
   into DRAM, clears a mailbox at `0xA0030000`, and cleans its own cache so
   CPU0, which starts with its MMU off, reads them from DRAM. The BPMP
   refuses an image over its 64 KiB copy window, but the real limit is
   32 KiB: `stub.ld` also asserts that the image ends below `0xA0008000`,
   where `stub.S` builds its L1 page table (the PHY dump follows at
   `0xA0008800`, and the stack tops out at `0xA0020000`).
2. Runs its own copy of bdk's `ccplex_boot_cpu0()` sequence, which brings
   up the CPU rail, PLLX, the CPU clock domain and the CRAIL/C0NC/CE0
   partitions, and releases CPU0 at the stub, in EL3, AArch64, MMU off.
   Its own copy because bdk's spins on the RAM repair with no timeout, and
   a console with a dead CPU rail would hang there; this one reads the
   regulator back and bounds the wait, so the fault is reported instead.
   MSELECT's clock is enabled here and its reset cleared just before CPU0
   is released, which is the rule above again. A failure names the step
   (`CPU0 boot : ...`), sets `cpu_complex` to FAIL and powers the cluster
   down, and is not retried: a rail that will not come up does not come up
   on the second ask.
3. The stub installs its own exception vectors, marks its accesses
   non-secure (`SCR_EL3.NS`, as U-Boot does), and turns on the I-cache, an
   identity MMU and the D-cache, with I/O mapped Device-nGnRnE
   **non-shareable**: the MMU-off default is inner-shareable, and a
   shareable read to the AFI goes down the coherency path and wedges after
   a few. Then it runs steps 6-10. It stores a **stage breadcrumb before
   each block of accesses** and bumps a heartbeat at every stage and inside
   its longer waits (the PHY locks, the root port 0 polls, the
   command-register retries). A fault lands in its vectors, which record
   `ESR_EL3`, `FAR_EL3`, `ELR_EL3` and the stage in the mailbox and park.
   The other rules its source sets for changes:
   - no loop without a bound: a stall costs the cluster, a spin the run;
   - the PHY dump leaves out the PADS block (`0x01003000`): the code's
     comment records that touching it before the first endpoint access
     drops the root port's `0x88` readback from `00002810` to 0 and the
     endpoint read then hangs. The `PADS_REFCLK_CFG0` write and readback
     in step 9 is the one PADS access the stub makes before enumeration;
   - endpoint config writes are absolute, never read-modify-write (the
     read side is what is under test), and each is verified by a pair of
     readbacks;
   - `AFI_FPCI_BAR0` is written once, at the translation stage: re-poking
     it after config traffic has flowed disturbs the path;
   - `stub.S` parks every core but CPU0, and sets the MMU and D-cache
     enables in the same `SCTLR_EL3` write, because M=1 with C=0 wedges
     this console's fetch path.
4. The BPMP watches the mailbox. Once the magic word is there, a heartbeat
   that stops moving for 2 s before stage 10, or for 300 ms from stage 10
   on, is a stall. The hard ceiling is 40000 polls 500 us apart, about
   20 s, which is also what a CPU0 that never reports in costs; the loop
   looks for the host's `R` on every pass, as do the bring-up's own delays
   and register waits. A stall on the first two of three attempts prints
   `stalled (attempt n/3, hb=..) - resetting AFI`, powergates the cluster,
   pulses PCIE, AFI and PCIEXCLK through reset, clears the mailbox and the
   PHY dump, and boots CPU0 again: some CYW4356s wedge the fabric on an
   early config read, and a fresh run can land a good one. After a final
   stall at stage 10 or later with `AFI_INTR_CODE` still 0, the BPMP boots
   CPU0 once more with `diag_mode` set, to read the AFI's latched error,
   which only a master that is not wedged can read. The stub never looks
   at `diag_mode`, so that is a full re-run of the stub, and its
   breadcrumbs overwrite the first pass's. Then the BPMP prints the results
   and calls `ccplex_powergate_cpu0()` to put the cluster away again,
   whatever the outcome.

**This is the safer arrangement, which is the real argument for it.** An
access to a block held in reset, or to an aperture that does not decode,
never completes (the rule at the head of
[The bring-up sequence](#the-bring-up-sequence)). Made from the BPMP it
takes the whole console down, because the BPMP is also what runs the UART,
the pager and the reset path, so recovery means a manual power cycle.
Made from CPU0, the same stall costs CPU0 alone: the BPMP sees the heartbeat
stop, prints the last breadcrumb, powergates the cluster and carries on with
the sweep.

The emulator models CPU0 (AArch64 at EL3, in lockstep with the BPMP) and
the PCIe root complex, so the stub runs to completion there and enumerates
the endpoint; `--fault vdd_cpu` or `--fault max77621_cpu` leaves the rail
dead and exercises the supervisor's refusal path instead, which is worth
running since that path is the safety net.

### Reading the output

Every line is a register read printed next to what it means, so a bring-up
that stops somewhere tells you where. This is a healthy Erista, end to end;
`<...>` marks values that vary from unit to unit and run to run:

```
--- Wireless ---
[Wi-Fi radio (BCM/CYW4356 WLAN core over PCIe)]
  entry clocks  : PCIE=0 AFI=0  rst PCIE=1 AFI=1 PCIEXCLK=1
  entry power   : PWRGATE PCIE=1  PLLE lock=0 PLLREFE lock=0
  CCPLEX amap   : 00020000  PCIe apertures A1=MMIO A2=MMIO A3=MMIO
  LDO1 VDD_PEX_1V05 : en 3->3  1050->1050 mV
  LDO7 AVDD_1V05_PLL: en 3->3  1050->1050 mV
  PEX_L1 pads   : RST 00000460->00000440  CLKREQ 00000470->00000450
  PH0 wifi_en   : 0 on entry (driven high by this probe)
  PH0/PH1       : 1,1 (cold cycle, t=0 for Table 62)
  PEX I/O pads  : DPD already awake
  PCIE partition: ungated (was already up, left alone, bit 3 = 1)
  PCIe partition: powergate cycle (U-Boot's power_on)
  PLLREFE/PLLE  : left to the CPU0 side
  lane power    : P0_CTL_2=00053131 (IDDQ/PWR overrides released)
  lane mux      : USB3_PAD_MUX=817FC1FE (pcie-0 -> pcie-x1)
  UPHY PLL P0   : left to the CPU0 side
  POR window    : 62 ms since WL_REG_ON (Tvddtopor 57 ms)

--- Wireless ---
[Wi-Fi radio - PCIe link and endpoint]
  -- CPU0 handoff (PCIe needs a CPU-complex master) --
  stub          : 8132 bytes -> A0000000, mailbox A0030000
  CPU rail      : before: <state> <n> mV
  CPU rail      : enabled: on 950 mV
  CPU RAM repair: done in <n> us
  CPU rail      : powered: on 950 mV
  CPU rail      : running: on 950 mV
  CPU0          : ran to completion (hb=<n>)
  MSELECT (CPU) : 07FF4020 -> 07FF4020 (TRM reset 07FF4020)
  AFI (CPU)     : cfg=00103025 witness=A5A50000 -> APERTURE LIVE
  RP1 link      : UP, DL active (LNKSTA=3011 gen1 x1)
  RP1 id        : 10DE:0FAF
  EP config     : 14E4:43EC Broadcom BCM/CYW4356 WLAN
  EP class/rev  : 028000 rev 03  BAR0=00000004 (default)
  CFG baseline  : id=43EC14E4/43EC14E4 cmd0=00100000 class=02800003/02800003
  CFG write chk : bar0=00000004/00000004 cmd=00100006/00100006
  CFG window    : 18003000/18003000
  CFG retries   : cmd=0 SECSTS 00000101->00000101
  CFG recheck   : id=43EC14E4/43EC14E4 (path alive at the end)
  AFI INTR      : code=0000 sig=0000
  FPCI timeout  : TO=FFFF
  Result        : WLAN core enumerated on the PCIe bus
  CPU0          : powergated
```

Three lines decide `wifi_pcie`, in this order. A failed AFI witness ends
the report there (only `CPU0 : powergated` follows). A link that is down
still prints `RP1 id`, the `Result` line with its note and `PHY (CPU0)`.
An unexpected endpoint ID still prints the `EP class/rev`, `CFG` and
`AFI INTR` lines that follow, but no `Result`:

- `AFI ... APERTURE LIVE` needs a `cfg` other than `FFFFFFFF` and the
  witness, a write-read-restore of `AFI_AXI_BAR5_START`, reading back
  `A5A50000`. The witness has to be a genuinely writable register:
  `AFI_PCIE_CONFIG` reads a plausible `00103025`, but the bit a toggle
  would test (the disable bit of a third root port this SoC does not have)
  never changes.
- `RP1 link : UP, DL active` is the Tegra root port. `LNKSTA=3011` decodes
  as gen1 x1.
- `EP config : 14E4:43EC` is the enumeration verdict: the WLAN function
  answered config reads with the right ID, so the core is present and on
  the bus. For a vendor ID other than `14E4` the stub skips the BAR and
  command stage, so `CFG write chk`, `CFG window` and `CFG retries` show
  what those mailbox slots held before (root port probes and registers,
  the PHY status). `CFG recheck` is not printed, and nor is
  `FPCI timeout`, which was never armed.

`MSELECT (CPU)` is a readback, not a verdict: `07FF4020` from CPU0, where
the BPMP reads one constant at every offset, is the measurement the handoff
exists to take, and the line is printed red only when it reads all ones.

The other lines:

| Line | What it tells you |
| --- | --- |
| `entry clocks`, `entry power` | the PCIE/AFI clock and reset bits, the partition and the PLL locks as the payload found them |
| `CCPLEX amap` | PMC `GLB_AMAP_CFG` bits 1-3: whether the CPU complex decodes each PCIe aperture as MMIO or DRAM. One flipped to DRAM sends CPU0's config reads to DRAM, which looks exactly like a bus that never answers. Nothing in the payload writes it |
| `LDO1 ...`, `LDO7 ...` | enable field and voltage before and after; green only when enabled at exactly 1050 mV, followed by `(set_voltage refused ...)` if the driver refused |
| `PEX_L1 pads` | the two pinmux words before and after the PARK clear |
| `PH0 wifi_en`, `PH0/PH1` | PH0 on entry, then both pins after the cold cycle (`PH1 WL_REG_ON` instead with `WIFI_CFG_COLD_CYCLE=0`). `PH0/PH1` is red when PH1 reads back 0 |
| `PEX I/O pads` | whether the PEX pads were in deep power down. `(STILL PARKED)`, in red, means they did not leave it within 250 ms; it is reported and does not stop the bring-up |
| `PCIE partition`, `PCIe partition` | `PWRGATE_STATUS` after the ungate, then the U-Boot cycle |
| `uphy mgmt clk` | Mariko only: `PLLP_MISC1` and `PEX_SATA_USB_RX_BYP` once the management clock is on |
| `lane power`, `lane mux` | P0's `CTL_2` after the overrides are released, and `USB3_PAD_MUX` |
| `POR window` | time since the Table 62 clock started, padded to at least 58 ms |
| `CPU rail` | the CPU regulator's enable bit and programmed voltage before the boot, after the enable, once the partitions are up, and after CPU0 has run. Erista's MAX77621 is set to 950 mV and Mariko's MAX77812 to 800 mV; Mariko adds the MAX77812's `BUCK_STAT` and `TOPSYS` bytes, raw, to compare across the snapshots |
| `PLLX` | printed only when PLLX has not locked after its wait: a warning, the boot goes on and `cpu_complex` is not affected |
| `CPU RAM repair` | how long the cluster's RAM repair took (the limit is 10 ms) |
| `CPU0 : ran to completion` | the stub reached stage 12. `hb` counts every breadcrumb store and wait step: at least 613 when the x4 port, which has nothing attached, never trains (13 breadcrumb stores, stage 10 twice, plus 600 polls) |
| `RP1 id` | the root port's own vendor:device, `10DE:0FAF`, which the stub's comment says turns to `0000:0000` with TRM 34.4.8's writes in place (step 9) |
| `CFG baseline` | the ID pair, the command/status baseline (`cmd0`, one read) and the class pair. Good values per the code: id `43EC14E4`, class `02800003` |
| `CFG write chk` | the BAR0 pair, then the command register's write-verify pair (good: `xxxx00x6`). The code's comment expects BAR0 `FF9F800x`, the ROM-phase default, where the sample reads `00000004`; BAR0 does not feed the verdict |
| `CFG window` | the backplane window register (`0x80`), paired. The code's comment expects `18000000`, the sample reads `18003000`; nor does this feed the verdict |
| `CFG retries` | rounds the command write needed, and the root port's secondary status before and after; its RMA/RTA bits mean the endpoint errored a downstream request |
| `CFG recheck` | the ID re-read at the end: the path is still alive |
| `AFI INTR`, `FPCI timeout` | the AFI's latched error code and signature after the config traffic (0 = nothing), and the armed FPCI timeout read back |

### When it fails

The stub's stages, as the stall and exception reports print them:

| Stage | Block |
| --- | --- |
| 1 | entry and the PHY bring-up |
| 2, 3 | MSELECT read, write |
| 4, 5 | AFI read, witness |
| 6 | address translations |
| 7 | controller enable, root port 0 included |
| 8 | root port 1 refclk and PERST# |
| 9 | link wait |
| 10 | bridge programming and the endpoint's config reads |
| 11 | BAR and window reads, the command write |
| 12 | done |

- `CPU0 boot : <why> (PWRGATE_STATUS=.. RAM_REPAIR=..)`, with the `before`
  and `enabled` rail lines: the regulator did not answer on I2C or did not
  take the enable, a CPU partition would not power up, or the RAM repair
  did not finish in 10 ms. On the first attempt nothing PCIe has been
  tried yet; a failure on a retry or on the diagnostic re-boot follows a
  stalled run.
- `CPU0 : never reported in (magic=..)`: the cluster did not start or never
  reached the stub. A `stub fault` line gives ESR/FAR/ELR if it faulted
  before writing its magic word.
- `CPU0 : EXCEPTION at stage N, not a stall`, then `ESR_EL3` and `FAR/ELR`:
  the stub faulted rather than hung. EC `0x25` with DFSC `0x10` is a
  synchronous external abort: the access was answered, with an error, the
  fabric saying no rather than never answering.
- `CPU0 : link trained, endpoint config access hung`: the bridge had been
  programmed, which only happens after `DL_UP`, so the link was up and the
  endpoint's config space never completed a read. The lines after it dump
  the first reads, the root port 0 probes, the AFI latch, the root port
  state and the PHY registers the stub saw, with `PADS_REFCLK_CFG0`
  (wanted `90B890B8`).
- `CPU0 : STALLED at stage N (hb=.., no exception)`: any other stall; the
  stage names the block.
- After any of those three reports, `CPU0 state`, `CPU0 saw`, `CPU0 BAR0`,
  `CPU0 bridge`, `CPU0 port en` and `CPU0 FPCI-to` print what the stub
  recorded. From stage 10 on, `CPU0 AFI intr` names the latched code
  (`FPCI_TIMEOUT`, `TGT_DECERR(master-abort/UR)` and so on; code 0 means
  the read is outstanding with neither a completion nor an error, the
  endpoint not answering). `CPU0 link` follows once the link wait was
  reached. A stall before stage 10 with no bridge programmed adds a note
  that an MSELECT (2/3) or AFI (4/5) access that never returns is one that
  is not decoding.
- `AFI (CPU) : ... -> not decoding`: even CPU0 cannot reach the AFI.
- `Result : no PCIe link on root port 1`, then `PHY (CPU0) : plle=..
  uphy=..`: SoC side and aperture are proven and the link did not train.
  The PHY line says whether PLLE or the UPHY calibration failed in step 6.
  The code notes this is intermittent on its test console: retry before
  suspecting the module, its supply or its solder.
- `Result : PCIE partition down, refusing to read AFI` and `Result :
  XUSB_PADCTL stuck in reset, refusing to read`: the bring-up stopped
  before an access that would never complete.
- `No handoff : bring-up incomplete`: the link page's muted bring-up
  stopped early, at one of those two, so CPU0 was not booted.
- `CPU0 handoff : not built (devkitA64 absent at build time)` and `stub :
  N bytes exceeds window`: build problems, no handoff.

If the console itself freezes, the BPMP hung, and the last
`[step] Wireless: ...` marker on the debug console and the LCD status row
names the block it was in. `status_step()` sends the marker synchronously
and cleans the status row out of the cache, so neither sink lags a step
behind.

| Marker | Block |
| --- | --- |
| `Bluetooth radio` | the whole Bluetooth probe |
| `Wi-Fi PCIe setup` | entry state, LDO1/LDO7, the PEX_L1 pads, PH0 |
| `WLAN power cycle` | the PH1 cycle, the PEX I/O pads, the partition ungate, AFI release and the CML clock |
| `PCIe partition` | U-Boot's powergate cycle |
| `PCIe pad controller` | the Mariko UPHY clock, the pad controller's reset and ELPG clamps, the lane overrides |
| `PCIe lanes` | the lane mux, `USB3_PAD_MUX`, the B01 `CTL8` writes, the POR window |
| `Wi-Fi link (CPU0)` | the link page; the muted bring-up's markers follow it |
| `CPU0 stub to DRAM` | the stub copy and the mailbox clear |
| `CPU rail` | the CPU regulator, read, set and enabled over I2C |
| `CPU clocks` | PLLX, the MSELECT and CPU clocks |
| `CPU partitions` | CRAIL, C0NC and CE0 |
| `CPU RAM repair` | the bounded RAM repair wait |
| `CPU0 out of reset` | the reset vector and the release |
| `waiting for CPU0` | the supervisor loop |
| `CPU0 diagnostic boot` | the re-boot that reads the AFI latch |

### What it cannot tell

- Enumeration shows that the WLAN function is present and answers config
  reads. The probe loads no firmware (step 10) and puts nothing on the
  air, so it says nothing about the firmware, the RF path or the antenna,
  and a PASS is not proof that Wi-Fi works.
- PH1's polarity is not established (see `WIFI_CFG_PH1_LEVEL` above), so
  a link that trains does not by itself show the core is out of reset.
- The code records the stub's enumeration path as verified on the Erista.
