# hwtest RCM

![hwtest running in the emulator](img/demo.png)

A read-only RCM (Recovery Mode) payload that probes every BPMP-side IC the
Switch boot ROM hands off to, then reports identity / status / faults to the
LCD and UART_B (Joy-Con right rail, 115200 8N1). The BPMP is the Boot and
Power Management Processor, the small ARM7 core that runs before the main CPU
comes up. That is what an RCM payload executes on, and it bounds what can be
tested. Output is also captured into a heap buffer that gets written to
`backup/<emmc_serial>/hwtest.txt` on the SD card.

The intended audience is repair techs working on Erista or Mariko consoles
(handheld, V2, OLED, Lite). The output is colour-coded so a unit with a
borderline reading or a real fault stands out without reading every line.

## Building

The Hekate source tree is vendored as a git submodule at
`third_party/hekate`. Initialize it before the first build:

```sh
git submodule update --init
export DEVKITARM=/opt/devkitpro/devkitARM   # or wherever yours lives
make                                        # produces both variants
```

`make` runs twice. The first pass uses `DEBUG_UART_PORT=1` and produces the
default build at `build/hwtest.bin`. The second pass uses `JC_PROBE=1` and
produces `build-jc/hwtest_jc.bin`. The two builds are mutually exclusive on
the wire because the Joy-Con stack needs UART_B at 1 Mbaud with hardware
flow control while the debug log uses 115200 8N1, so only one variant can
run at a time.

| Variant | UART_B owner | Use it when |
|---|---|---|
| `hwtest.bin` (default) | debug log @ 115200 8N1 | tech is capturing UART output for diagnosis |
| `hwtest_jc.bin` | Joy-Con stack @ 1 Mbaud | tech needs Joy-Con detection / FW version |

The default build is what you want 90% of the time.

## Running

Inject like any other RCM payload (fusee-launcher, TegraRcmGUI, modchip, RCM
jig). The unit boots into the diagnostic dump within ~3 seconds, writes the
SD report, and lands on the on-LCD pager.

If the SD slot is empty / faulty / the unit was injected without a card, the
payload still boots and runs. The save step is skipped cleanly with an `[save]
SD not ready` line on UART; the LCD pager remains usable.

| Key | Action |
|---|---|
| `n` / space / VOL+ | Next page |
| `p` / `b` / VOL- | Previous page |
| `r` | Refresh current LCD page |
| `a` / `A` | Refresh ALL probes (re-emit full UART dump) |
| `G<name>\n` | Refresh just the named group (host viewer uses this) |
| `w` / `W` | Re-run the [Wi-Fi PCIe probe](#wi-fi-radio-probe-pcie) (it already runs in the sweep) |
| `s` / `S` | Re-write the SD report file |
| `R` | Reboot (PMIC power cycle, so a modchip re-injects) |
| `q` / POWER | Power off the console |

## What's reported

Ten logical pages (some span multiple LCD-sized sub-pages). The Verdict
page is index 0, so the pager opens on the summary and you only page into
the detail when something needs chasing:

| Page | Probes |
|---|---|
| Verdict | rolled-up pass/warn/fail per subsystem, plus cross-checks across SoC / fuses / charger / display / touch / battery / eMMC / thermal / USB-PD / fuel-gauge. Shown first on the pager and written first into the SD report |
| SoC | HIDREV chip ID + major/minor |
| Fuses | identity + speedo / IDDQ + lot/wafer + public key + SBK/DK + KFUSE |
| Power & charging | MAX77620 + GPIOs + regulators + 5V + battery + charger + USB-PD + thermal + fan |
| Memory & clocks | LPDDR4 mode regs + CLK_RST raw + decoded rates |
| Storage | SD + eMMC + partitions + health + GPT + BOOT0/pkg1 + AutoRCM + SD content scan, plus PRODINFO: serial, CAL0 body SHA-256, config ID, product model vs SoC, LCD vendor vs fitted panel, WLAN/BD MACs, battery lot, USB-C PD rev, touch IC, IMU, sticks, region, speaker calibration |
| Wireless | Bluetooth radio: full CYW4356 power-up, then HCI over UART-D. Reports whether the radio answers and identifies itself. Then the WLAN half: a complete T210 PCIe bring-up, link training and endpoint enumeration (`14E4:43EC`) — enumeration is the verdict, the die-level ChipID is firmware-gated and left to the OS |
| Display | DSI panel ID + backlight PWM + GPIO state |
| Inputs | touch FW + Joy-Con rails + buttons + AC adapter |
| Raw state | GPIO pin census + UART debug port + reset reason + PMC scratch |

### Terms used in that table

| Term | Meaning |
|---|---|
| **HIDREV** | Tegra chip identity register: chip ID plus major/minor revision |
| **IDDQ** | Quiescent supply current, a factory-measured leakage figure burnt into fuses |
| **speedo** | Factory-measured silicon speed grade, also fuse-burnt |
| **SBK / DK** | Secure Boot Key / Device Key, per-console fuse-burnt secrets |
| **KFUSE** | Separate fuse block holding the HDCP/display key data |
| **PMC** | Power Management Controller, the Tegra block holding scratch registers and reset reason |
| **DSI** | Display Serial Interface, the MIPI link to the LCD panel |
| **PWM** | Pulse-Width Modulation, used here for backlight brightness |
| **LPDDR4** | The DRAM standard the Switch uses |
| **CLK_RST** | Tegra's Clock and Reset Controller |
| **GPT** | GUID Partition Table, the eMMC partition layout |
| **BOOT0 / pkg1** | eMMC boot partition and the first-stage bootloader package in it |
| **AutoRCM** | A deliberately corrupted boot0 that forces the console into RCM at power-on |
| **PRODINFO** | The factory calibration partition, encrypted with per-console keys |
| **CAL0** | The structure inside PRODINFO holding that calibration data |
| **BD MAC** | Bluetooth device address (the radio's MAC-equivalent identifier) |
| **IMU** | Inertial Measurement Unit, the console's accelerometer/gyro |
| **USB-PD** | USB Power Delivery, the charging negotiation protocol |
| **eMMC** | The soldered-down internal flash storage |

## Bluetooth radio probe

Most of hwtest reads. This probe writes, and it is worth knowing what it does.

The Switch's Broadcom CYW4356 carries WLAN on **PCIe** and Bluetooth on a
plain 4-wire UART (Tegra UART-D, `0x70006300`). The UART half is the cheap
measurement, and it is the one that runs first: both radios share one package
and one supply, so if Bluetooth answers HCI (Host Controller Interface, the
standard Bluetooth host-to-controller command protocol), the module is
powered, clocked and executing code, and a Wi-Fi fault has to be downstream
of the chip. The [Wi-Fi probe](#wi-fi-radio-probe-pcie) then goes and finds
out which.

To get there the probe performs a real cold power cycle: it parks every radio
control line in a known state, drives `BT_REG_ON` (PH4) and `WL_REG_ON` (PH1)
low together, holds them, then raises `BT_REG_ON` to produce a genuine POR
(Power-On Reset, the chip's internal reset, which is released when
`BT_REG_ON` rises and can hold the part for up to 110 ms afterwards).

The power cycle is not optional, because two of these pins are *straps* --
sampled once, a few milliseconds after POR deassertion, then latched until the
next one. Re-pulsing `BT_REG_ON` without a low period changes nothing; the
part keeps whatever mode it latched first.

* `BT_HOST_WAKE` (**PH5**, chip `BT_GPIO_1`) selects the host transport. Held
  low across the edge it latches **SPI**, and the part then ignores the UART
  entirely. The probe leaves it high-Z with no pull so the chip's own
  internal pull-up wins and UART is selected.
* `BT_GPIO4` (**PK2**, on port K, not H) is the one strap the datasheet
  tabulates: driven high, the ROM hunts for an external serial flash instead
  of presenting HCI. The probe holds PK0-PK2 as inputs.

The remaining lines (`BT_REG_ON`, `WL_REG_ON`, `BT_DEV_WAKE` on PH3) are
ordinary control and wake signals, not straps. They still have to be in a
defined state across the edge, but nothing latches them as configuration.
`WL_REG_ON` is restored before the probe returns.

It also self-tests. A 16550 internal loopback runs first, proving the clock,
divisor, FIFOs and helper routines without involving a single pad. **A radio
that stays silent is only reported as a failure when that loopback passed** --
otherwise the fault is on our side of the pads and nothing is recorded against
the radio.

The probe does not settle for one attempt before it records silence. It runs
three power-cycle arms and four flow-control (MCR) variants, every one of them
at 115200 -- the CYW4356's default HCI rate, and the only rate the probe
configures. There is no baud sweep. What that covers is a host-side transport
or flow-control mismatch; the host baud is not covered, so a silent unit is
reported as silent at 115200 rather than at every rate.

Verified against four consoles spanning both SoC generations. Three healthy
units, two Mariko and one Erista, each answer with a valid Command Complete
(`04 0E 04 01 03 0C 00`) and identify as Broadcom (`0x000F`). One faulty
Erista, reporting error 2110-1118 on Wi-Fi, stays quiet through every arm and
every MCR variant: it never releases its internal pull-up on `BT_HOST_WAKE`
and never asserts the transport-ready handshake. Its PRODINFO calibration is
intact, which points at the module, its supply, or its solder -- unless that
module came up at a rate other than 115200, which this probe does not test.

## Wi-Fi radio probe (PCIe)

The other half of the same package, and the only probe in hwtest that brings
up an entire bus from nothing.

### The BPMP cannot reach PCIe

Worth stating before the sequence, because every symptom of it looks like a
bring-up bug you could fix by writing one more register. You cannot: the
PCIe apertures do not answer the BPMP, and no register sequence changes that.

The Tegra X1 TRM chapter 19 (AHB) describes the BPMP-Lite crossbar as "the
path to IRAM from AHB devices, and also the BPMP-Lite and CPU path to AHB
devices", and lists the AHB slaves: XBAR, DRAM, USB-OTG/USB2 and TZRAM. PCIe
is not among them. The PCIe aperture hangs off MSELECT instead, and MSELECT
is documented in chapter 16, *CPU Complex* — `MSELECT_CONFIG_0`, reset value
`0x07ff4020`, carrying the `ENABLE_PCIE_APERTURE` bit. The block diagram says
the same thing: MSELECT's master is CCPLEX/CPUCIF, and its 32-bit
`AXI>ARM7 (APC)` port is a bridge *into* the ARM7 world, not out of it. The
BPMP-Lite is an AHB master; PCIe lives on the CPU's AXI, behind MSELECT.

What that looks like from a payload, with every precondition it controls
already satisfied — PCIE and AFI clocked and out of reset, the PCIE power
partition ungated, PLLE and PLLREFE locked, MSELECT's own reset released:

| Access | From the BPMP | From CPU0 |
| --- | --- | --- |
| AFI window `0x01003800` | all-ones, and writes do not stick | decodes, writes stick |
| `MSELECT_CONFIG_0` `0x50060000` | `0xEAFFFFFE`, and the same word at every other MSELECT offset | `0x07FF4020`, the TRM's reset value |
| Display controller `0x54200000`, TSEC `0x54500000` | read fine | — |

The third row is the control: this is not a general MMIO fault on the way to
some fix, it is PCIe and MSELECT specifically. So the probe splits, and the
PCIe-side accesses run on CPU0 — see
[Running part of the probe on CPU0](#running-part-of-the-probe-on-cpu0).

(Aside if you cross-reference hekate: its `MSELECT_CFG_WRAP_TO_INCR_BPMP` for
bit 27 is a misnomer. The TRM calls that bit `WRAP_TO_INCR_SLAVE0 (APC)`,
and it is the APC bridge, not the BPMP.)

### Running it

It runs in the boot sweep by default, so the wireless verdict is part of the
report with no pager interaction required. Press `w` on the pager, or send
`W` over UART, to run it again. For a build where it only ever runs when
armed that way:

```sh
make EXTRA_DEFINES=-DWIFI_CFG_AUTORUN=0
```

That knob exists because the probe is *invasive* in a way the rest of the
sweep is not, not because it endangers the payload: with the PCIe half on
CPU0, an access that never completes takes out CPU0 alone, and the BPMP sees
the mailbox heartbeat stop, prints the last breadcrumb the stub wrote,
powergates the cluster and carries on. What it does do is boot the CPU
cluster — CPU rail up through the MAX77621/MAX77812, PLLX, the CRAIL/C0NC/CE0
partitions — and cold-cycle `WL_REG_ON`, resetting the WLAN section. hwtest
is otherwise a read-only pass you can point at an unknown console, and this
costs 3-4 seconds on top.

Every line it prints is flushed to the wire before the access it describes,
so if it does stall, the last line received names the access that did it.

### The bring-up sequence

BDK has no PCIe support at all — no AFI (Address Framing Interface, the
controller's register window), no PLLE (the PCIe phase-locked loop), no UPHY
(Universal PHY) lane initialisation, not even a base address. So this probe
carries its own T210 root-complex bring-up, ported from U-Boot's
`drivers/pci/pci_tegra.c` plus its Tegra210 clock and XUSB pad-controller
code, and cross-checked against Linux's `pci-tegra.c` and
`phy/tegra/xusb-tegra210.c`. In order:

0. **MSELECT out of reset.** The step with nothing to do with PCIe that stops
   everything without it. MSELECT is the fabric between every master — the
   CPU cluster *and the BPMP* — and the AXI slaves. `hw_init()` enables its
   clock but never clears its reset; the only BDK code that does is the path
   that boots the A57s, which this payload never takes. Setting
   `MSELECT_CFG_ERR_RESP_EN_PCIE` at the same time is what makes the rest of
   the probe safe: an unserviceable PCIe access then comes back as a data
   abort, which BDK's handler survives, instead of stalling the bus.

   This is one instance of the rule every step below obeys: **a T210 block
   that is clocked but held in reset does not error a read, it never
   completes one.** The BPMP hangs with no abort, no timeout and no reset
   path. It holds for MSELECT, and for `XUSB_PADCTL`, where a diagnostic
   register dump alone is enough to hang the console. So every block is
   released, then its `RST_DEVICES_*` bit is read back and checked, *before*
   the first access. A guard placed after the access it guards is not a
   guard, and a read-modify-write like `REG |= bit` is a read.
1. **Rails**, enabled *and set to the right voltage*. LDO1 (`VDD_PEX_1V05`)
   and LDO7 (`AVDD_1V05_PLL`) are the two 1.05 V supplies the Switch device
   tree hangs off `pcie@1003000`. Enabling is not enough: LDO7 feeds
   `avdd-pll-uerefe`, the UPHY reference PLL's own analog supply, and a
   Mariko in RCM is still on its 1.000 V cold OTP default because HOS's
   `pcv` — which would program the 1.05 V the board specifies — never runs
   here. (Erista boots at 1.05 V, so this is Mariko-specific.)
2. **`WL_REG_ON` cold cycle** on PH1, which starts the endpoint's own
   power-on reset. `BT_REG_ON` is left alone, so this resets the WLAN
   section without disturbing the Bluetooth result above.
3. **Tegra side**, interleaved into that reset window: PEX I/O pads out of
   deep power down, the PCIE power partition ungated (with the REMOVE_CLAMPING
   quirk that swaps the PCIE and VDEC bits), PCIE/AFI clocks and resets,
   PLLREFE then PLLE, the pad controller out of reset, lane `pcie-0` muxed to
   `pcie-x1`, and the UPHY PLL P0 frequency- and resistor-calibration
   sequence. The AFI cross-bar and fuse configuration waits for the CPU0
   half: the BPMP cannot see those registers.

   **On Mariko there is one more clock, and without it the calibration can
   never finish.** T210B01's padctl PCIe pad takes a second clock that
   Erista's does not: NVIDIA's own tegra210b01 device tree gives it
   `clock-names = "pll", "uphy_mgmt"` where tegra210.dtsi has only `"pll"`,
   and L4T enables `uphy_mgmt` inside `tegra210_pcie_pad_probe()` behind
   `if (t210b01_compatible(padctl) == 1)`, ahead of every UPHY access. It
   clocks the calibration state machine, so with it gated `CAL_EN` latches
   and `CAL_DONE` stays 0 for ever — while ELPG, the lane mux and CTL1/2/4
   all read and write perfectly, because those are ordinary APB registers.
   Two CAR registers BDK never touches: `PLLP_MISC1` (0x680) carries the
   `pll_p_out_xusb` branch gate, and `PEX_SATA_USB_RX_BYP` (0x6D0) — which
   means something else entirely on Erista — is repurposed on B01 as this
   clock's 7.1 divider plus enable gate. 408 MHz over a divider value of 6
   gives the 102 MHz NVIDIA's init table asks for. This is exactly the class
   of difference that lets the U-Boot sequence work on a Jetson TX1 and fail
   here, and no T210-only reference can show it to you.

   **And the UPHY PLL itself is configured a different way on B01.** Every
   public T210 driver writes `CAL_CTRL = 0x136` and `DCO_CTRL = 0x2a` before
   calibrating. L4T does that *only* on Erista; on T210B01 it instead loads a
   table through an indirect address/data port at `UPHY_PLL_P0_CTL10`
   (`usb3_pll_g1_init_data`), plus a per-lane table through each lane's
   `CTL8`. It is an either/or, not an addition. Take the T210 branch on a
   Mariko and the PLL is never configured: `CAL_EN` latches, `CAL_CTRL` reads
   back `0x136`, and `CAL_DONE` stays 0 for ever — which looks exactly like
   dead silicon and sends you hunting on the PCIe side, where nothing is
   wrong. Only L4T shows this; mainline has no B01 support at all.
4. **Link training** on root port 1, honouring the CYW4356's own ordering:
   reference clock stable for 10 ms *before* PERST# is released, then 6 ms
   before the first configuration access (datasheet Table 62, p147). Neither
   upstream driver waits for either — they drive add-in cards whose rails
   came up long before the driver loaded.
5. **Enumeration.** The root port's own identity, then the endpoint's:
   `14E4:43EC` is the CYW4356's WLAN function, class `0x028000` (network
   controller), and the command register takes the one config write the
   ROM-phase chip accepts. **Enumeration is the verdict**: the WLAN core
   answered config reads with the right ID, so it is present and on the
   bus. The die-level ChipID would need a firmware download (the backplane
   is firmware-gated in ROM phase), which is the OS's job, not the probe's.

The BT and WLAN halves each have their own verdict — a radio that answers
HCI and a WLAN function that enumerates means the module is powered,
clocked and on its busses; one that does neither has nothing on the bus at
all. Both are reported distinctly, and both feed the `Wireless` verdict
alongside `bt_hci`.

The emulator models all of this, including the timing gates, so a bring-up
that skips a step fails on the host with a `[pcie] root port 1 cannot train:`
line naming what was missed. `--wifi-radio healthy|faulty|absent` selects
which console you are testing against.

### Running part of the probe on CPU0

The PCIe apertures answer a CPU-complex master and nobody else, so the probe
splits in two. The BPMP does everything it can legitimately reach — rails,
`WL_REG_ON`, the PEX I/O pads, the PCIE partition, PCIE/AFI clocks and
resets, PLLREFE and PLLE, the pad controller, the lane mux and UPHY PLL P0,
all of it APB and CAR. Then it hands over:

1. Copies a small **AArch64 stub** (`cpu_stub/`, built with devkitA64, linked
   at `0xA0000000`) into DRAM, and clears a mailbox at `0xA0030000`.
2. Calls bdk's `ccplex_boot_cpu0()`, which brings up the CPU rail, PLLX, the
   CPU clock domain and the CRAIL/C0NC/CE0 partitions, and releases CPU0 at
   the stub — in EL3, AArch64, MMU off.
3. The stub does MSELECT, the AFI translations (byte-identical to U-Boot's
   `tegra_pcie_setup_translations`), the controller enable, root port 1's
   refclk and PERST# (with the CYW4356's own 10 ms and 6 ms waits), link
   training, and the endpoint's configuration space with paired-readback
   verification. It stores a **breadcrumb before each access** and bumps a
   heartbeat.
4. The BPMP watches the heartbeat, prints the results, and calls
   `ccplex_powergate_cpu0()` to put the cluster away again.

**This is the safer arrangement, which is the real argument for it.** An
aperture that does not decode, or a block held in reset, does not error a
transaction on this SoC — it never completes one. Such an access from the
BPMP takes the whole console down, because the BPMP is also what runs the
UART, the pager and the reset path, so recovery means a manual power cycle.
Made from CPU0, the same stall costs CPU0 alone: the BPMP sees the heartbeat
stop, prints the last breadcrumb, powergates the cluster and reboots itself.

The emulator has no CCPLEX model, so there the stub never reports and the
supervisor's timeout path runs instead — which is worth having exercised on
every emulator run, since that path is the safety net.

### Reading the output

Every line is a register read printed next to what it means, so a bring-up
that stops somewhere tells you where. This is a healthy Erista, end to end:

```
[Wi-Fi radio (BCM/CYW4356 WLAN core over PCIe)]
  entry clocks  : PCIE=0 AFI=0  rst PCIE=1 AFI=1 PCIEXCLK=1
  entry power   : PWRGATE PCIE=1  PLLE lock=0 PLLREFE lock=0
  CCPLEX amap   : 00020000  PCIe apertures A1=MMIO A2=MMIO A3=MMIO
  LDO1 VDD_PEX_1V05 : en 3->3  1050->1050 mV
  LDO7 AVDD_1V05_PLL: en 3->3  1050->1050 mV
  PEX_L1 pads   : RST 00000460->00000440  CLKREQ 00000470->00000450
  PH0 wifi_en   : 0 on entry (left as found)
  PH0/PH1       : 0,1 (cold cycle, t=0 for Table 62)
  PEX I/O pads  : DPD already awake
  PCIE partition: ungated (was already up, left alone, bit 3 = 1)
  PLLREFE/PLLE  : refe locked, plle locked
  lane power    : P0_CTL_2=00053131 (IDDQ/PWR overrides released)
  lane mux      : USB3_PAD_MUX=817FC03E (pcie-0 -> pcie-x1)
  UPHY PLL P0   : calibrated and locked
  POR window    : 62 ms since WL_REG_ON (Tvddtopor 57 ms)

[Wi-Fi radio - PCIe link and endpoint]
  -- CPU0 handoff (PCIe needs a CPU-complex master) --
  stub          : 5872 bytes -> A0000000, mailbox A0030000
  CPU0          : ran to completion (hb=12)
  MSELECT (CPU) : 07FF4020 -> 17FF4020 (TRM reset 07FF4020)
  AFI (CPU)     : cfg=00103025 witness=A5A50000 -> APERTURE LIVE
  RP1 link      : UP, DL active (LNKSTA=3011 gen1 x1)
  RP1 id        : 10DE:0FAF
  EP config     : 14E4:43EC Broadcom BCM/CYW4356 WLAN
  EP class/rev  : 028000 rev 03  BAR0=00000004 (default)
  CFG write chk : bar0=00000000/00000004 cmd=00000000/00100046
  CFG window    : 00000000/18003000
  CFG retries   : cmd=0 SECSTS 00000101->00000101
  Result        : WLAN core enumerated on the PCIe bus
  CPU0          : powergated
```

Four lines carry the verdict, and they fail independently:

- `MSELECT (CPU)` reading back `07FF4020` rather than all-ones is the proof
  the handoff worked at all. If this is `EAFFFFFE` the stub is running but
  the aperture is not, and nothing after it can be trusted.
- `AFI ... APERTURE LIVE` is a write-then-read-back witness, not just a
  non-zero read, because an undecoded window can return plausible garbage.
- `RP1 link : UP, DL active` is the Tegra root port. `LNKSTA=3011` decodes as
  gen1 x1, which is what this endpoint negotiates.
- `EP config : 14E4:43EC` is the enumeration verdict: the WLAN function
  answered config reads with the right ID, so the core is present and on
  the bus. (Reading the die-level ChipID would need a firmware download -
  the backplane is firmware-gated in ROM phase - which is the OS's job.)

The `uphy entry`/`uphy ungated` pairs bracket the pad controller coming out
of reset, and exist because the register values on either side are how you
tell a gated clock from a mis-programmed one. They are printed before the
access they describe, so a stall leaves the last line on the wire naming
what stalled.

## BDK's SDRAM parameter scratch

BDK hands out a few fixed IRAM addresses no section knows about, and
`SDRAM_PARAMS_ADDR` (`memory_map.h`) is one of them: `sdram_init()`, reached
from `hw_init()` on the first line of `ipl_main`, decompresses the DRAM
parameter blob there and writes `0x838` bytes, unconditionally, on every
boot. Any static that lands in that window is destroyed during `hw_init` —
after `start.S` zeroes `.bss`, before any probe runs.

Nothing in the payload can defend against that, so the link script stays out
of the way: `link.ld` starts `.bss` above the scratch window and asserts that
`.text`/`.data` never reach it. The gap costs no image bytes, since `.bss`
occupies none. Such a collision is harmless for any variable written before
it is first read, which is most of them, so it surfaces only as a static read
before assignment coming up with a garbage value.

## Host viewer (`host_tools/hwtest_viewer.py`)

ImGui front-end that connects to the same UART, parses the line-oriented dump,
and shows it as a clickable page list with green, yellow, or red severity
colouring. The toolbar exposes the pager keys (`n`, `p`, `r`, `s`, `q`) plus a
"Refresh group" button that sends a `G<group>\n` command. That refreshes just
the selected group on the host side without touching the LCD.

```sh
pip install -r host_tools/requirements.txt
python host_tools/hwtest_viewer.py --port /dev/ttyUSB0
# or replay a saved capture (no console needed):
python host_tools/hwtest_viewer.py --replay path/to/uart_capture.txt
```

The Switch UART runs at 1.8 V. Use a level-shifter between the Tegra rail
and any USB-TTL cable that doesn't natively support 1.8 V. For wiring the
Joy-Con right rail as a UART tap, see this gbatemp guide:
<https://gbatemp.net/threads/how-to-make-a-joycon-rail-uart-to-capture-boot-log.625784/>.

## Layout

```
hwtest.h             - shared header: logging + finding APIs, probe prototypes
main.c               - boot flow, page table, pager + UART command loop
log.c                - three-sink logging (LCD + UART_B + report capture)
dx.c                 - diagnostic-finding registry consumed by the verdict
report.c             - SD report writer (backup/<emmc_serial>/hwtest.txt)
verdict.c            - cross-checks + per-subsystem verdict aggregation
probe_soc.c          - SoC identity, fuses, KFUSE
probe_power.c        - PMIC, regulators, battery, charger, USB-PD, thermal, fan
probe_storage.c      - SD, eMMC, partitions, health, GPT, BOOT0/pkg1, PRODINFO
probe_bt.c           - Bluetooth radio (CYW4356 HCI over UART-D)
probe_wifi.c         - Wi-Fi radio (CYW4356 WLAN over PCIe)
probe_display.c      - DSI panel ID + backlight
probe_inputs.c       - touch, ambient light, Joy-Con rails, buttons
probe_memclk.c       - DRAM identity + clock registers
probe_lowlevel.c     - GPIO census, UART debug port, reset reason, PMC scratch
cpu_mbox.h           - BPMP <-> CPU0 mailbox, shared by both toolchains
cpu_stub/            - AArch64 stub that does the PCIe half on CPU0
emmcsn.c             - Hekate-style backup/<emmc_serial>/<sub>/<file> path
diskio.c, ffconf.h   - FatFS glue + config (vendored from the Hekate bootloader)
exception_handlers.S - boot relocator (vendored from the Hekate bootloader)
gfx/                 - LCD driver (vendored from the Hekate bootloader)
stubs.c              - minerva_deinit no-op
link.ld              - link script; also keeps .bss off BDK's SDRAM scratch
Makefile             - devkitARM build for both variants, devkitA64 for the stub
host_tools/          - ImGui + pyserial UART viewer (parser, GUI, replay)
```

## Acknowledgements

Heavy reliance on the work of:

- [Hekate / BDK](https://github.com/CTCaer/hekate)
- [Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM)
- [switchbrew.org](https://switchbrew.org/wiki/)
