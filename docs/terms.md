# Terms used in the report

The abbreviations and terms of art used in the report, the
[README](../README.md#whats-reported) and the notes in this folder, grouped
by subject. Where a note explains a term at its first use, that note has
the detail. Some words mean two things (SD, SOC, DLL, CID, IDDQ, PD,
sink, partition); both meanings are listed.

## hwtest's own words

| Term | Meaning |
|---|---|
| **hwtest.bin, text payload** | The RCM payload itself: the sweep, the LCD pager and the text launcher, built by `make` |
| **gui.bin, GUI stage** | The LVGL front end, too large to inject: hwtest.bin loads `hwtest/gui.bin` from the SD card into DRAM and jumps to it |
| **text launcher** | hwtest.bin's physical-key menu (Hardware test, RAM test, Custom test, Power off), shown when the GUI does not start |
| **pager** | hwtest.bin's page viewer after a sweep, worked with VOL+, VOL- and POWER or with keys over the UART |
| **probe** | One `probe_*` function: it reads, or exercises, one part of the console and prints one page |
| **page** | One entry of the page table `g_pages` (42 of them); pages that share a name make one of the report's 11 logical pages |
| **group** | What a custom run picks (`PROBE_G_*`): each page belongs to one, and each Verdict row runs on its groups |
| **sweep** | One run of every page whose group is selected, in table order, the Verdict last (`run_probe_mask`) |
| **health check, Custom Check, Custom test** | The GUI's full sweep, and the GUI's and the text launcher's runs of a chosen set of groups |
| **finding, verdict key** | A named result a probe records with `dx_set`: PASS, WARN or FAIL, with a short detail, the reason |
| **row** | One line of the Verdict page, rolling a fixed list of keys up into their worst state and one reason |
| **category, check** | One entry of the GUI's results, built from one or two Verdict rows, and one key as that category lists it |
| **reading** | A value a GUI category shows, copied from a line the probes printed |
| **soft prefix** | The start of a WARN detail (`no keys`, `inconclusive`, `not run`) that makes the GUI show the check as not checked |
| **cross-check** | A Verdict check (`xc_*` keys) that reads the hardware again and compares readings that each look healthy on their own |
| **breadcrumb, `[step]` line** | A progress line a long probe sends straight to the debug console, so that a hang names the step it was in |
| **debug console** | The serial port the payload logs to: UART-B on the right rail, or in the GUI the port Custom Check picks |
| **report** | The text of a sweep, captured in a 128 KiB buffer and saved as `backup/<eMMC serial>/hwtest.txt` on the SD card |
| **census** | A read-only listing that judges nothing: the pin census, the I2C census, the input census |
| **snapshot** | The MAX77620's clear-on-read latches, read once per boot before anything active runs and reported by every sweep after |
| **stub, handoff, mailbox, heartbeat** | The Wi-Fi probe's AArch64 program, the BPMP starting it on CPU0, the DRAM block the two share, and the counter that shows it still runs; the loader's unpacking stub and bdk's SMMU stub are other programs |
| **host viewer** | `host_tools/hwtest_viewer.py`, the PC program that reads the debug console and shows the pages |
| **JC-L, JC-R, BL, EN, CH** | In printed labels: the left and right Joy-Con rails, backlight, enable, channel |

## Consoles, firmware and tools

| Term | Meaning |
|---|---|
| **Tegra X1, T210, Tegra210** | NVIDIA's SoC in the Switch; T210 is its original revision |
| **T210B01, B01** | The later revision of the Tegra X1, fitted to every Mariko console |
| **Erista** | A console with the original T210: the first standard model |
| **Mariko** | The T210B01 and the consoles built on it: the V2, the Lite and the OLED |
| **Icosa, Iowa, Hoag, Aula** | Board codenames: the Erista standard model, the Mariko standard model (V2), the Lite and the OLED |
| **Nx, Copper, Calcio** | PRODINFO product models besides those: the Erista retail model, and the Erista and Mariko development units |
| **V2, Lite, OLED** | The Mariko standard model, the handheld-only Switch Lite and the Switch OLED model |
| **SKU** | A product variant; `SKU_INFO` is the fuse word that records it |
| **HOS, Horizon** | The Switch's own operating system |
| **sysmodule** | A HOS background service; the notes cite `pcv` (regulators), FS (file systems, the game card), `usb`, `gpio` and `audio` |
| **OFW, CFW** | Official firmware, HOS as Nintendo ships it, and custom firmware such as Atmosphere |
| **RCM** | Recovery Mode: the BootROM's USB recovery mode, through which a payload is injected; "patched RCM" means its exploitable bug is fixed (every Mariko, some Erista) |
| **BootROM, BROM** | The SoC's built-in boot code, the first thing that runs at power-on |
| **ipatch** | A BootROM patch burnt into the fuses; an Erista with one at `0x769A` has patched RCM |
| **payload** | A program sent over RCM, or chainloaded, and run by the BPMP from IRAM |
| **chainload** | One payload loading and starting another, as hwtest.bin does with the RAM test |
| **RCM injector** | The PC program that sends a payload: fusee-launcher, TegraRcmGUI, TegraRcmSmash (which truncates past 130392 bytes) |
| **RCM jig** | A plug for the right Joy-Con rail that makes the console start in RCM |
| **modchip** | A chip fitted to the board that injects a payload at every power-on reset (picofly is one) |
| **hekate** | CTCaer's bootloader, on whose BDK hwtest is built |
| **Nyx** | hekate's touch GUI; hwtest's GUI looks like it and is started the way hekate starts it |
| **BDK, bdk** | hekate's bootloader development kit: the SoC, storage, display and input drivers hwtest uses (`third_party/hekate/bdk`) |
| **Minerva** | hekate's DRAM training and clock-switching module, `libsys_minerva.bso`, which the GUI loads from the SD card |
| **Atmosphere** | A HOS custom firmware; its public MTC code and timing tables drive the RAM test |
| **Lockpick_RCM, prod.keys** | A payload that dumps the console's keys, and the file it writes, `sd:/switch/prod.keys`; hwtest reads only `bis_key_00` from it |
| **emuMMC** | A copy of the eMMC kept on the SD card, which CFW can boot instead of the real one |
| **HATS** | A prebuilt SD card pack whose `/manifest.json` and `/HATS_VERSION.txt` carry the Atmosphere version |
| **Horizon-OC** | An overclocking project for HOS, cited for the MAX77812 VDDQ range |
| **L4T, switchroot** | Linux for Tegra, and the Switch Linux project whose kernel (CTCaer's) is the reference for the dock, display and PCIe code |
| **U-Boot, coreboot** | Open-source boot firmware whose Tegra210 drivers are references for the PCIe, audio clock and DisplayPort code |
| **Jetson TX1** | NVIDIA's T210 development board, on which U-Boot's PCIe sequence works |
| **device tree, dtsi** | The hardware description Linux boots with; the Switch's trees are the source for many pins, rails and tuning values |
| **TRM** | NVIDIA's Tegra X1 Technical Reference Manual |
| **devkitARM, devkitA64** | The 32-bit ARM and the AArch64 cross-compilers; devkitA64 builds the CPU0 stub |
| **LVGL** | The embedded graphics library (version 5, carried by bdk) the GUI draws with |
| **ImGui** | The GUI library the host viewer uses |
| **RCM-Emulator, emulator** | The host-side emulator the code is tested against; flags such as `--fault`, `--oem` and `--prod-keys` choose the console it models |
| **switchbrew** | The community wiki on Switch hardware and software |

## SoC and fuses

| Term | Meaning |
|---|---|
| **SoC** | System on chip: the Tegra X1, holding the CPU cluster, the BPMP and every controller the probes use |
| **BPMP** | The Boot and Power Management Processor, the ARM7 core that runs the payload; the main CPU cluster stays off unless a probe starts it |
| **CCPLEX, CPU complex, A57** | The main Cortex-A57 CPU cluster, off in RCM |
| **CPU0** | The cluster's first core; the Wi-Fi probe runs its PCIe half there, and the RAM test uses it to switch the SMMU |
| **CRAIL, C0NC, CE0** | The power partitions CPU0 needs: the CPU rail, the cluster's non-core part and core 0 |
| **RAM repair** | A power-up step of the CPU cluster's own RAMs that must finish before CPU0 is released (bounded at 10 ms here) |
| **IRAM** | The SoC's internal SRAM; an RCM payload is loaded at `0x40010000` and runs from it |
| **TZRAM, TrustZone** | A small protected internal RAM, and the ARM secure world it belongs to; some registers (`MC_SMMU_CONFIG`) take only a secure write |
| **AHB, APB** | The SoC's peripheral buses; most controller registers sit on APB |
| **APB_MISC** | A block of miscellaneous registers: `HIDREV`, the pinmux and pad controls, the DAS |
| **MSELECT** | The CPU complex's bridge to the rest of the SoC; the PCIe apertures are reached only through it |
| **APC** | A bridge on CPU0's path from MSELECT to the BPMP's crossbar and the APB registers |
| **crossbar, XBAR** | A switch between several sources and destinations: the BPMP's AHB crossbar, the audio AXBAR, the PCIe and SOR lane maps |
| **CAR** | The Clock and Reset controller, the same block as CLK_RST: clock enables, resets and clock sources per block |
| **CLK_RST** | Tegra's Clock and Reset Controller |
| **PMC** | Power Management Controller, the Tegra block holding scratch registers and reset reason |
| **power partition, powergate** | A part of the SoC the PMC can switch off (PCIE, AUD, SOR, DISB, VDEC the video decoder, the CPU partitions); ungating powers it up |
| **EMC, MC** | The External Memory Controller that drives the DRAM, and the Memory Controller in front of it |
| **SMMU, IOVA, ASID** | The memory controller's System MMU, an address as a device sees it through it, and its per-master address-space number |
| **SE, keyslot** | The Security Engine, the SoC's crypto block, and its write-only key registers |
| **fuse, fuse cache** | One-time-programmable bits burnt at the factory and by HOS updates; the probes read their cached copies (`FUSE()`) |
| **ODM, ODM fuses** | Original design manufacturer: fuse words kept for the device maker (`ODM4` holds the DRAM ID and model, `ODM7` the anti-downgrade count) |
| **HIDREV** | Tegra chip identity register: chip ID plus major/minor revision |
| **IDDQ** | Quiescent supply current, a factory-measured leakage figure burnt into fuses |
| **speedo** | Factory-measured silicon speed grade, also fuse-burnt |
| **SBK / DK** | Secure Boot Key / Device Key, per-console fuse-burnt secrets |
| **KFUSE** | Separate fuse block holding the HDCP/display key data |
| **HDCP** | High-bandwidth Digital Content Protection, the copy protection of digital video links, whose keys KFUSE holds |
| **lot, wafer, X/Y coordinate** | The fused manufacturing record of the die: its batch, the silicon wafer it was cut from and its place on it |
| **BROM rev, FT rev, CP rev** | Fused revisions: the BootROM's, and those of the factory's final-test and chip-probe test programs |
| **public key (fuses)** | The SHA-256 of the console's boot public key, burnt into `FUSE_PUBLIC_KEY0-7` |
| **anti-downgrade fuses** | `ODM7` bits HOS burns as it updates, so that older boot firmware refuses to start |
| **VIC** | Video Image Compositor: the block that rotates the GUI's landscape frame into the panel's portrait framebuffer |
| **DC, head** | A display controller and the output it scans; DC1, the second, drives the TV |
| **SOR** | The SoC block that drives the DisplayPort lanes; SOR1 is the dock's |
| **host1x, syncpoint** | Tegra's host interface block, which every DPAUX access needs, and its hardware event counters |
| **XUSB** | The SoC's USB 3 controller with its pad controller (`XUSB_PADCTL`); also the name of the rails that feed it |
| **USB2 controller** | The SoC's older USB 2 device controller, which USB serial uses on Erista |
| **SDMMC1, SDMMC2, SDMMC4** | The SD/MMC host controllers of the SD card, the game card ASIC and the eMMC |
| **PTO** | Pulse Tag Observer: a CAR counter that measures a clock against the 32.768 kHz clock |

## Clocks and PLLs

| Term | Meaning |
|---|---|
| **PLL** | Phase-locked loop: multiplies a reference clock up; `ENABLE` (bit 30) and `LOCK` (bit 27) say it is on and settled, and an enabled one that never locks is "hunting" |
| **PLLP, PLLP_OUT0** | The 408 MHz peripheral PLL and its main output, the source of the BPMP's clock, the UARTs and `AUD_MCLK` |
| **PLLA** | The audio PLL (`PLLA_OUT0` at 120 MHz for I2S1 here) |
| **PLLC, PLLC4** | PLLC feeds the BPMP's boost clocks; PLLC4 is another PLL the lock check leaves out |
| **PLLD, PLLD2, PLLDP** | The built-in panel's pixel clock, the TV's pixel clock (74.25 MHz) and the SOR macro's reference (270 MHz) |
| **PLLE, PLLREFE** | The PCIe PLL (100 MHz) and the reference PLL in front of it (PLLRE in one comment) |
| **PLLM, PLLMB** | The DRAM clock PLLs |
| **PLLU** | The USB PLL |
| **PLLX** | The CPU cluster's PLL |
| **OSC, CLK_M** | The 38.4 MHz crystal oscillator, the SoC's reference clock, and its name as a clock source |
| **SCLK** | The system clock, which the BPMP runs on (408 MHz in the sweep) |
| **CCLK_G** | The A57 cluster's clock; 0 unless the cluster has been started |
| **VCO, DIVM, DIVN, DIVP** | A PLL's oscillator, and its input divider, feedback multiplier and post-divider (`DIVP` is an exponent on PLLA) |
| **EXTPERIPH1** | The CAR clock source behind the `AUD_MCLK` pad |
| **32 kHz clock, LPO** | The 32.768 kHz clock the PMIC supplies, and the CYW4356's internal low-power oscillator, which it can run on instead |
| **boost clock** | hekate's raised BPMP clock (576 MHz on Erista, 589 MHz on Mariko) the GUI draws at; the probes run at 408 MHz |

## Pins and pads

| Term | Meaning |
|---|---|
| **GPIO** | General-purpose I/O: a pin read or driven by software rather than by a controller |
| **PH0, PBB3, ...** | A Tegra pin by port and number, as bdk spells it: PH0 is port H pin 0, PBB3 port BB pin 3, PD0 port D pin 0 (not USB-PD); the MAX77620's GPIO0-GPIO7 are the PMIC's own pins |
| **CNF, OE, OUT, IN** | A GPIO port's bits for each pin: GPIO mode, output enable, output level, and the pad's input level |
| **pinmux** | A pad's configuration word: function, pull, tristate, park, input enable and drive |
| **SFIO, SPIO** | A pad on its special function, owned by its controller (UART, PWM) rather than by the GPIO block; SPIO is bdk's name |
| **tristate** | A pad with its output driver off, so that it does not drive the line |
| **park** | A pad's `PARK` bit: a parked pad does not drive, whatever its other settings |
| **input enable** | A pad's input buffer; with it off the GPIO `IN` bit reads 0 whatever the line does |
| **pull-up, pull-down** | A weak resistor in the pad that holds an undriven line high or low |
| **active low, act-low** | A signal asserted at the low level, such as a pressed volume key or a panel held in reset |
| **strap** | A pin a chip samples once just after reset to choose a mode, then latches until the next reset |
| **bus keeper** | A weak pad circuit that holds the last level driven onto a line |
| **Schmitt trigger** | An input with hysteresis, which ignores slow or noisy edges |
| **drive strength** | How hard a pad drives (`DRIVE_1X`, `DRIVE_4X`, `CAL_DRVUP` and `CAL_DRVDN`); too weak and the far end sees nothing |
| **DPD** | Deep power down: a low-power state a pad group has to leave before it works (the PEX pads) |
| **pad rail, AUDIO_HV, PWR_DET** | The I/O voltage a pad group runs at, such as AUDIO_HV; the PMC's `PWR_DET` latch tells the pads which voltage it is |
| **clamp** | A gate that holds a block's signals at a safe level while it is off: power partition clamps, the XUSB ELPG clamps |
| **ELPG** | The XUSB pad controller's power-gated state, whose clamps are released before its pads are used |
| **masked write** | A GPIO store that names the pins it changes (`GP_MWR`), so the other pins of the port do not move |
| **MOSFET** | A transistor used as a switch, such as the one PK3 turns on between the Tegra and the right rail |

## Power and battery

| Term | Meaning |
|---|---|
| **PMIC** | Power management IC; the main one is the MAX77620 |
| **MAX77620** | The main PMIC, on I2C5: four step-down converters, nine LDOs, eight GPIOs, the power key, the RTC and the reset latches |
| **MAX77621** | Erista's CPU and GPU buck regulators, one per rail |
| **MAX77812** | The multi-phase buck for CPU, GPU and DRAM VDDQ on Mariko; its outputs M1-M4 are wired as PHASE211 (retail) or PHASE31 |
| **ES, ES2, QS** | Silicon versions a chip reports: engineering sample (the MAX77620's `CID5`, the MAX77812's ES2) and qualification sample |
| **buck, step-down converter** | A switching regulator that steps a voltage down |
| **SD0-SD3** | The MAX77620's four step-down rails: SoC core, DRAM VDD2, LDO source and 1.8 V (nothing to do with the SD card) |
| **LDO** | Low-dropout linear regulator; the MAX77620's LDO0-LDO8 feed the display, PCIe and XUSB, SD card, game card, RTC, touch and DisplayPort |
| **rail** | One supply line, named by its regulator (LDO5) or by its device-tree name (`vdd_core`, `vdd_3v3`) |
| **CFG, CFG2** | An LDO's configuration register (voltage and power mode) and its second one, which holds the power-OK bit |
| **POK, power-OK** | A regulator's status bit saying its output is up |
| **OTP** | One-time-programmable memory: the PMIC's factory defaults and OTP version, and the fuses the game card ASIC burns |
| **FPS** | The MAX77620's Flexible Power Sequencer, which brings rails up and down in order |
| **DVFS** | Dynamic voltage and frequency scaling: the CPU and GPU rail voltage for each clock, from HOS's tables |
| **VDDQ, VDD2** | The DRAM's I/O supply (MAX77812 M3 on Mariko) and its core supply (MAX77620 SD1) |
| **5 V regulator** | The FAN5333 behind the 5 V of the fan and the Joy-Con rails, enabled by PA5 |
| **fuel gauge** | The MAX17050, which measures the battery's voltage, current and temperature and estimates its charge |
| **SOC (gauge)** | State of charge, the battery percentage (`RepSOC` is the figure it reports); not the SoC |
| **VCELL, OCV** | The measured cell voltage, and the gauge's open-circuit voltage estimate |
| **FullCap, DesignCap, RepCap** | The gauge's learnt full capacity, the pack's design capacity, and the remaining capacity it reports |
| **TTE** | Time to empty, the gauge's estimate in minutes |
| **QH/QL, coulomb counter** | The gauge's running count of the charge in and out of the pack |
| **dQacc, dPacc** | The gauge's learning accumulators behind FullCap |
| **BR, BI, SMX, SMN, TMX, TMN, VMX, VMN, BST** | MAX17050 `STATUS` bits: battery removed, inserted; charge, temperature and voltage over the maximum or under the minimum; battery absent |
| **relearn** | The gauge rebuilding its model after a reset (its `POR` bit), while its readings are only advisory |
| **charger** | The BQ24193 on I2C1, between the USB-C input, the system and the battery |
| **VBUS** | The USB-C port's supply line, present when a charger or a dock powers it |
| **ACOK** | The MAX77620's input that says external power is present |
| **PG, power-good** | The charger's flag that its input is valid; it must follow VBUS |
| **USB-SDP** | Standard downstream port: a plain USB host port, limited to 500 mA |
| **OTG** | On-The-Go: the charger powering VBUS for an attached device, and the USB role that goes with it |
| **CV, IPRECHG, ITERM** | The charger's constant-voltage target, pre-charge current and charge-termination current |
| **BATFET** | The charger's switch between the battery and the system |
| **NTC** | The battery's thermistor (negative temperature coefficient), which the gauge and the charger both read |
| **watchdog** | A timer that resets or flags a part unless software services it: the BQ24193's (expires about 40 s into RCM by design), the PMIC's, the BPMP's |
| **TMP451** | The temperature sensor on I2C1: a local channel on the board and a remote one on a diode inside the SoC |
| **PCB** | Printed circuit board: the main board, where the TMP451's local channel measures |
| **under-fill** | The resin under the SoC package |
| **tach, rpm** | The fan's speed signal, two pulses a turn by bdk's arithmetic, and the turns per minute worked out from it |
| **VBAT, coin cell** | The PMIC's backup supply that keeps the RTC calendar; when it drops, the calendar resets to 2000-01-01 |
| **reflow** | Re-melting a board's solder, a repair step that can drop VBAT |
| **POR** | Power-on reset: a chip's reset at power-up, or the full power cycle of hwtest's reboot; also the bits that record one (PMC `RST_STATUS` 0, the gauge's `POR`) |
| **MAIN_RST, warm reset** | A reset of the SoC by the PMC without a power cycle; on a modchipped Mariko it lands in patched RCM |
| **SFT_RST** | The MAX77620's software reset, a full power cycle through POR, which hwtest's `R` reboot uses |
| **PMIC latches** | `NVERC` (why the last power cycle ended), `INTLBT` (thermal and low-battery events) and `IRQSD` (rail fault thresholds), all cleared on read |

## Buses and serial links

| Term | Meaning |
|---|---|
| **I2C** | Inter-Integrated Circuit, the two-wire bus most chips sit on; buses I2C1-I2C6 (bdk's `I2C_1`, the README's I2C-1): I2C1 the power and audio chips, I2C2 the light sensor, I2C3 touch, I2C5 the PMICs |
| **ACK, NAK** | An I2C device acknowledging or refusing a transfer (also NACK, and HOS's NoAck); bdk returns 0 for a read that got no ACK |
| **STOP, repeated start** | I2C framing: a STOP ends a transaction, a repeated start begins the next without releasing the bus |
| **SPI** | Serial Peripheral Interface: the Joy-Con's flash bus, and the transport a CYW4356 latched with PH5 low speaks instead of the UART |
| **UART** | A serial port: UART-A (test pads), UART-B (right rail, the debug console), UART-C (left rail), UART-D (Bluetooth); pin names count from 1, so UART2 is UART-B, and `UART_B` is UART-B too |
| **8N1, baud** | 8 data bits, no parity, 1 stop bit; and the line rate, 115200 on every debug UART |
| **16550** | The standard UART register layout the Tegra's UARTs follow |
| **FIFO** | First-in first-out buffer, such as a UART's receive queue or the audio TX FIFO |
| **LCR, MCR, LSR, MSR, THR** | UART registers: line control, modem control, line status, modem status, transmit holding |
| **DLAB, DLL, DLM** | The UART's divisor-latch access bit and the divisor's low and high bytes (not the DRAM's DLL) |
| **RDR, THRE, TMTY, BRK, FERR, PERR, OVRF, FIFOE** | `LSR` flags: data ready, transmit holding empty, transmitter empty, break, framing, parity and overrun errors, an error in the FIFO |
| **RTS, CTS** | Request to send and clear to send, the flow-control pair; the radio's `RTS_N` is the host's CTS |
| **flow control** | The receiver pacing the sender over RTS/CTS; bdk switches a UART between hardware and manual modes |
| **break, mark** | A line held low for longer than a byte, as an unpowered sender leaves it, and the idle high level |
| **loopback** | A UART mode that feeds TX straight back to RX inside the controller, testing it without the pads |
| **autobaud** | A mode in which the radio works out the host's baud rate from its first bytes |
| **level shifter, USB-UART adapter** | The part that converts the 1.8 V UART to another logic level, and the PC cable that carries it (USB-TTL) |
| **PIO, DMA** | Programmed I/O, the CPU moving each word, and direct memory access, a controller moving the data alone |
| **MMIO** | Memory-mapped I/O: registers reached at memory addresses |

## Bluetooth and Wi-Fi

| Term | Meaning |
|---|---|
| **CYW4356** | The Broadcom combo chip: WLAN on PCIe and Bluetooth on UART-D, in one package with shared regulators |
| **WLAN** | Wireless LAN, the chip's Wi-Fi function |
| **BT_REG_ON, WL_REG_ON** | The radio's Bluetooth and WLAN power enables (PH4, PH1); either one powers the shared regulators |
| **CBUCK, CLDO** | The radio's internal regulators, a buck and an LDO, which both REG_ON pins switch |
| **VDDC, VDDIO** | The radio's core supply and its I/O supply (`BT_VDDIO`) |
| **HCI** | Host Controller Interface, the standard command protocol between a Bluetooth host and controller |
| **HCI_Reset, Command Complete, opcode** | The reset command (`01 03 0C 00`), the event (`0x0E`) that answers a command, and a command's 16-bit number |
| **H4** | HCI over a UART: each packet starts with a type byte |
| **ACL, SCO, ISO** | HCI data packet types: data, voice and isochronous traffic |
| **LMP** | Link Manager Protocol; the controller reports its version beside the HCI version |
| **LM_DIAG** | A Broadcom vendor packet (`0x07`) with a fixed 63-byte payload |
| **BD MAC** | Bluetooth device address (the radio's MAC-equivalent identifier) |
| **ROM phase** | The radio running from its built-in ROM before the OS loads its firmware; it then takes only the command-register write |
| **backplane, ChipID** | The radio's internal bus and the die's own ID on it, both out of reach until a firmware download |
| **RF** | Radio frequency: the antenna side of the radio, which no probe tests |
| **inquiry, page** | Searching for devices that answer (inquiry), and calling one by its address to connect (paging); a page that gets no answer ends in "page timeout" (`0x04`) |
| **RSSI** | Received signal strength, in dBm, as the radio reports it for each device found |
| **class of device** | Three bytes a device announces when found; a Joy-Con or a Pro Controller is `0x002508` (peripheral, gamepad) |
| **pairing mode, sync button** | A controller ready to pair with a new host, shown by its sweeping lights; the small button that puts it there |
| **SSP, just works, IO capability** | Secure Simple Pairing, Bluetooth 2.1's pairing; its mode for devices with no screen or keys; and what each side says it can show or type |
| **link key, bonding** | The key pairing produces, and keeping it so the pair can reconnect without pairing again |
| **L2CAP, PSM, CID** | The layer that multiplexes channels over a link; the number a channel is opened to (`0x11` HID control, `0x13` HID interrupt); a channel's id at each end |
| **echo request** | An L2CAP ping a device must answer, whatever is above it |
| **HID DATA, SET_REPORT, HANDSHAKE** | HID over Bluetooth: a report sent on the interrupt channel; one sent on the control channel; the control channel's answer to it |
| **basic rate, EDR** | Bluetooth's original packet types (DM1, DH1, DH3 and so on) and the faster Enhanced Data Rate ones (2-DH1 and so on), which the radio's ROM does not receive |
| **sniff mode** | A low-power link mode in which a device listens only every so many slots |
| **slot, poll interval** | Bluetooth's 625 us time unit; how often the master gives a slave its turn to send (40 slots by default) |

## PCIe

| Term | Meaning |
|---|---|
| **PCIe, PEX** | PCI Express, the serial bus to the WLAN; PEX is Tegra's name for it in pads, rails and clocks |
| **root complex, root port, endpoint** | The SoC's PCIe controller, its two ports (RP0 the unconnected x4, RP1 the WLAN's x1), and the device at the far end (EP) |
| **lane, x1, x4, gen1, GEN2** | One transmit and receive pair; a link's width in lanes; the first and second PCIe speeds |
| **AFI** | The PCIe controller's register block on the SoC side, with the translations from CPU addresses (`AFI_AXI_BAR*`, `AFI_FPCI_BAR0`) |
| **FPCI** | The PCIe controller's internal bus behind AFI (`EN_FPCI`, the FPCI completion timeout) |
| **PADS** | The PCIe controller's pad register block (`0x01003000`) |
| **UPHY, PHY** | The SoC's high-speed lane PHY (Universal PHY), muxed between USB 3 and PCIe, with its own PLL and calibration |
| **aperture** | An address window the CPU complex maps to PCIe (A1-A3, decoded as MMIO or DRAM by PMC `GLB_AMAP_CFG`) |
| **link training, DL_UP, DL active** | The two ends agreeing a working link, and the data link layer reporting it up and active |
| **LNKSTA** | The root port's link status register: speed and width |
| **PERST#, CLKREQ#, REFCLK** | The endpoint's reset (active low), its clock request line, and the reference clock |
| **L1 PM substates** | PCIe low-power link states; off here, so `CLKREQ#` stays tristated |
| **config space, TLP** | An endpoint's standard registers (ID, class, BARs, command), reached by configuration requests sent as transaction-layer packets |
| **enumeration** | Reading an endpoint's config space to find what it is (`14E4:43EC` is the CYW4356's WLAN) |
| **vendor:device ID, class** | A PCIe function's identity pair and its class code (`0x028000`, network controller) |
| **BAR** | Base address register: where a function's memory window lives (the endpoint's BAR0; the AFI's own BARs are translations) |
| **command register, bus master** | The config register that turns on a function's memory decode and its own bus requests |
| **CRS** | Configuration Request Retry Status: an endpoint still starting up asks for a config read to be retried; made visible, it reads `0xFFFF0001` |
| **UR, master abort, TGT_DECERR, RMA/RTA** | A request no device claimed, or one refused: unsupported request, master abort, target decode error, received master or target abort |
| **SECSTS** | The root port's secondary status register, whose RMA/RTA bits record errored downstream requests |
| **IDDQ (PCIe lanes)** | A pad's quiescent, powered-down state; the lanes' `IDDQ` overrides hold it while the mux changes |
| **CML clock** | An auxiliary PLLE output enabled by `PLLE_AUX` bit 0, named after U-Boot's `pci_tegra.c` |
| **PCA** | A root-port vendor-control enable bit (`RP_VEND_CTL2_PCA_ENABLE`) the stub sets before link training; the code does not expand the name |

## CPU0 and the ARM side

| Term | Meaning |
|---|---|
| **AArch64, EL3** | The A57's 64-bit state, and its highest exception level, where the stub runs |
| **ARM7** | The BPMP's 32-bit core |
| **MMU, identity map** | The memory management unit, and a mapping in which every virtual address equals the physical one |
| **I-cache, D-cache, clean, invalidate** | The instruction and data caches; a clean writes dirty lines back to memory, an invalidate drops cached copies |
| **write-back, write-through** | A cache that writes to memory later, on a clean, or with every store |
| **Device-nGnRnE** | The strictest ARM device memory type: no gathering, no reordering, no early write acknowledgement |
| **shareable, inner-shareable** | ARM coherency attributes; a shareable access to the AFI takes the coherency path and wedges |
| **SCR_EL3.NS, SCTLR_EL3** | EL3's secure configuration register, whose NS bit marks accesses non-secure, and its system control register (MMU and cache enables) |
| **ESR, FAR, ELR** | The exception syndrome, fault address and exception link registers, which the stub records on a fault |
| **EC, DFSC** | Fields of ESR: exception class and data fault status code (`0x25` with `0x10` is a synchronous external abort) |
| **synchronous external abort** | A bus access answered with an error, as opposed to one that never completes |
| **undefined instruction, abort, SWI** | BPMP exceptions (SWI is a software interrupt); bdk's handlers start the program over |
| **R0, R1, LR** | ARM registers: the first two argument registers, and the link register holding the return address |
| **BSS, .data, .text** | The zero-filled, the initialised-data and the code sections of an image |
| **magic word** | A fixed value at a known place that proves an image or a message is the expected one |

## DRAM and the RAM test

| Term | Meaning |
|---|---|
| **DRAM, SDRAM** | The console's main memory, 4, 6 or 8 GiB, behind the EMC |
| **LPDDR4 / LPDDR4X** | The DRAM standards of Erista and of Mariko |
| **channel, rank, die** | A DRAM interface of its own (bdk: each EMC channel is one chip), a set of dies addressed together, and one piece of silicon |
| **mode register, MRR** | A DRAM chip's own identity and status registers (MR5 maker, MR6 and MR7 revision, MR8 density, MR4 refresh and temperature), read with a mode register read |
| **density** | One die's capacity as MR8 encodes it |
| **training, MTC** | Calibrating the DRAM interface's timings for a clock, needed before running it fast; MTC (memory training) is the name the Atmosphere code and Minerva's interface use |
| **timing table** | The EMC settings for one DRAM ID at one clock, which training starts from |
| **FSP** | LPDDR4 frequency set point; bdk's FSP workaround switches to 800 MHz on the way to 1600 MHz |
| **periodic training, compensation** | The re-calibration a trained link needs as it drifts (every 250 ms in the GUI, 100 ms in the RAM test) |
| **DLL (DRAM)** | The memory interface's delay-locked loop (not the UART's divisor latch) |
| **CA, QUSE, VREF training** | Training steps: command/address timing, the read-strobe gate, the reference voltages; the write and read steps train the data |
| **refresh, retention** | The DRAM's periodic recharge of its cells, and a cell keeping its data in between (the PRBS pass holds 2 s with refresh running) |
| **PRBS, xorshift** | Pseudo-random bit sequence, and the shift-and-XOR generator the RAM test makes it with |
| **alias** | Two addresses that reach the same cells, the fault an address-decoder defect causes |
| **carveout** | A DRAM region set aside at boot, which the RAM test skips |
| **self-extracting loader** | `loader/`: a stub that unpacks the LZ-packed image in IRAM, so that the injected file stays under the 130392-byte RCM limit |
| **LZ, LZ77, BCL, LZ4** | Compression: the payload packer is the LZ77 coder of the Basic Compression Library (BCL) that bdk carries; the Mariko DRAM tables are LZ4 |
| **relocator** | `chain_reloc`: a tiny copy loop in upper IRAM that moves a staged payload to `0x40010000` and starts it |

## Storage and the game card

| Term | Meaning |
|---|---|
| **eMMC** | The soldered-down internal flash storage |
| **SD card, SDMMC** | The removable card, on SDMMC1; SDMMC is the Tegra's SD/MMC host controller |
| **MMC** | MultiMediaCard, the command set the eMMC and the game card ASIC speak |
| **CID (card)** | An SD card's or the eMMC's identification register: maker, OEM, product name, revision, serial and date; not the MAX77620's `CID` registers |
| **OEM** | Original equipment manufacturer: a field of a card's CID, and in "OEM clone" a third-party part |
| **EXT_CSD** | The eMMC's extended card-specific data register: sizes, supported modes, wear and features |
| **CMD, ACMD, R1** | SD/MMC commands by number (CMD6 switch, CMD21 tuning block, ACMD13 SD status; CMD60-63 the game card's vendor commands) and the standard short response |
| **tuning, tuning block** | The host finding its sampling point at a high clock; CMD21 returns the fixed JEDEC pattern used for it |
| **HS25, HS52, HS100, HS200, HS400** | Bus speed modes named by rate; HS400 is the eMMC's fastest (8-bit, DDR), HS100 the game card ASIC's |
| **SDR82, SDR104, DDR208** | SD UHS bus modes: two single-data-rate ones, and a double-rate one hwtest's build leaves out |
| **DDR** | Double data rate: data on both clock edges |
| **UHS** | Ultra High Speed: the SD card's 1.8 V bus modes |
| **speed class, UHS grade, video class, application class** | An SD card's claimed floors: C10 and U1/U3 (sequential write, MB/s), V30 (video recording), A1/A2 (random IOPS) |
| **AU** | Allocation unit, a size the SD card reports in its status |
| **IOPS** | Input/output operations per second |
| **LBA, sector** | Logical block address, the number of a 512-byte sector |
| **USER, GPP, BOOT0, BOOT1, RPMB** | eMMC hardware partitions: the main user area (general purpose, holding the GPT), the two boot partitions, the replay-protected memory block |
| **BOOT0 / pkg1** | eMMC boot partition and the first-stage bootloader package in it |
| **package1** | pkg1 written out, as HOS and hekate name it |
| **BCT** | Boot configuration table: the BootROM's boot parameters at BOOT0 byte 0x200, carrying an RSA modulus |
| **AutoRCM** | A deliberately corrupted boot0 that forces the console into RCM at power-on |
| **GPT** | GUID Partition Table, the eMMC partition layout |
| **GUID** | A 128-bit identifier: a partition's type or its own ID |
| **CRC, CRC32, CRC-8** | Cyclic redundancy check, a checksum: 32-bit over the GPT header and entries, 8-bit on the IR camera commands; KFUSE and the display have their own |
| **SHA-256** | A cryptographic hash; CAL0's body carries one, and so do the public-key fuses |
| **PRODINFO** | The factory calibration partition, encrypted with per-console keys |
| **CAL0** | The structure inside PRODINFO holding that calibration data |
| **BIS, bis_key_00** | Built-in storage, HOS's encrypted eMMC partitions, read through bdk's decrypting layer; `bis_key_00` is PRODINFO's key |
| **AES-XTS, tweak** | The disk-encryption mode PRODINFO uses, and the second of its two key halves (crypt and tweak) |
| **SSL, eTicket keys** | Per-console keys and certificates CAL0 also carries, which is why hwtest wipes it from memory |
| **donor board** | A board fitted with another console's eMMC, whose PRODINFO then disagrees with the SoC |
| **PRE_EOL_INFO, life time estimate** | The eMMC's reserved-block state (normal, warning, urgent) and its two wear estimates, A and B, in 10% steps |
| **JEDEC** | The standards body behind eMMC, LPDDR4 and the tuning pattern |
| **TRIM, sanitize, HPI, bkops** | eMMC features: freeing unused blocks, erasing them for good, interrupting a long operation, background maintenance |
| **wear levelling** | The eMMC spreading writes over its blocks, which needs TRIM to reclaim space |
| **FatFs, `0:`** | The FAT file-system library bdk uses (`f_mount`, `f_open`, `f_read`), and its name for the SD card volume |
| **Lotus3, GCBRG** | The game card bridge ASIC between the slot and SDMMC2: switchbrew's name and the board marking |
| **ASIC** | Application-specific integrated circuit |
| **CD#** | The game card slot's card-detect pin, low with a cartridge seated |

## USB-C, USB-PD and USB serial

| Term | Meaning |
|---|---|
| **USB-C** | The console's reversible port: power, USB and, through the dock, DisplayPort |
| **USB-PD** | USB Power Delivery, the charging negotiation protocol |
| **BM92T36** | Rohm's USB-C PD controller, on I2C1 at `0x18` |
| **CC** | Configuration channel: the USB-C pins PD talks over; which one is live is the cable's orientation (`CC orient.`) |
| **VCONN** | Power for an e-marked cable or an accessory, carried on the CC pin not in use |
| **e-marked cable** | A USB-C cable with a chip that reports its ratings |
| **source, sink** | The side that supplies power and the side that takes it; in DisplayPort, the sender and the receiver |
| **PDO, RDO, contract** | Power data object, a voltage and current a source offers; request data object, the sink's pick; and the agreement that follows |
| **DFP, UFP, data role** | Downstream- and upstream-facing port, the USB host and device sides; `DR_SWAP` exchanges them |
| **VDM, structured VDM** | Vendor-defined message, a PD message for anything beyond power; structured ones carry the standard commands |
| **SVID** | The standard or vendor ID a VDM belongs to: `FF00` PD's own, `FF01` DisplayPort, `057E` Nintendo |
| **Discover Identity, Discover SVIDs, Discover Modes, Enter Mode** | Structured VDM commands: who the other end is, which SVIDs it supports, which modes, and entering one |
| **alternate mode** | Running another protocol, here DisplayPort, over the USB-C pins |
| **UFP_D, UFP_U, pin assignment** | DisplayPort alternate mode's sink role, the USB-C port that takes it, and the lane layout (C: 4 lanes; D: 2 lanes plus USB 3) |
| **VID:PID** | A USB vendor and product ID pair (`057E:2003` is the Nintendo dock) |
| **HPD** | Hot plug detect: the line a DisplayPort sink raises when a display is there |
| **PI3USB30532** | The USB 3 and DisplayPort switch between the SoC and the port, which no probe can reach |
| **over-current protection** | The PD controller's guard against excess current, which Reset Chip makes sure is on |
| **CDC ACM** | The USB serial-port class of the GUI's USB serial console, which Windows 10 and later, Linux and macOS drive without an installed driver |
| **DTR, line coding** | The PC's "port is open" signal, and the baud rate and framing it sets (`SET_LINE_CODING`) |
| **endpoint (USB), bulk, interrupt** | A USB data channel, and two of its transfer types; the serial data runs on bulk endpoints `0x01` and `0x81` |
| **high speed, full speed** | USB 2's 480 and 12 Mbit/s rates (512- and 64-byte bulk packets) |
| **interface association** | A descriptor that binds the CDC communications and data interfaces into one function |
| **pid.codes** | An open registry of USB IDs; `1209:0001` is its test pair |
| **UMS** | USB mass storage: hekate offering a partition to a PC as a drive |
| **COMx, /dev/ttyACM0** | The names Windows and Linux give the USB serial port |

## DisplayPort and the TV picture

| Term | Meaning |
|---|---|
| **DisplayPort, DP** | The video link from the SOR to the dock, which turns it into HDMI for the TV |
| **AUX channel, DPAUX** | DisplayPort's side channel for configuration, and the SoC block that drives it (DPAUX1 for the dock) |
| **native, I2C-over-AUX, defer** | AUX transactions to the sink's own registers, AUX transactions carrying I2C (the EDID), and a sink's "try again" |
| **DPCD** | DisplayPort configuration data: the sink's registers, read and written over AUX |
| **OUI** | The IEEE vendor identifier kept in DPCD (NVIDIA's `00044B` is written as the source's) |
| **EDID** | Extended display identification data: the TV's name and timings, read through the dock's converter |
| **branch** | A DisplayPort converter or hub; the dock's converter is the sink and a branch |
| **link rate, HBR2** | The bit rate per lane: 1.62, 2.7 or 5.4 Gbps, the last called HBR2 |
| **enhanced framing** | A DisplayPort framing option the sink advertises |
| **training pattern, clock recovery, equalisation** | Link training: fixed patterns (1, 2, 3) sent while the sink locks to the clock, then tunes its receivers |
| **symbol lock, alignment** | A lane decoding symbols, and the lanes lined up with each other |
| **swing, pre-emphasis, post-cursor2** | The transmitter's drive levels, which the sink asks for during training |
| **8b/10b, spread** | The link's line coding, and spread-spectrum clocking (off: `PLLDP` has none) |
| **scrambling, ASSR** | Data whitening on the link, and the alternate scrambler seed reset the SOR pairs with its internal-panel mode |
| **D0, D3** | Sink power states: awake and asleep |
| **SOR macro, lane sequencer** | The SOR's analogue part (pads, PLL, bandgap) clocked from PLLDP, and the logic that powers its lanes in order |
| **bandgap** | An on-chip voltage reference |
| **termination calibration** | Trimming the pads' on-chip termination (`TMDS_TERMADJ`) against a comparator |
| **prods** | Per-SoC register tuning values from the device tree, such as Erista's `prod_c_dp` |
| **transfer unit, active symbols, watermark, blanking** | The link dividers that fit a video mode into the link's bandwidth |
| **window A, raster** | The display controller's first plane, which carries the frame, and the scan timing it generates |
| **assembly and active copies, general update** | A display controller's shadow registers and its live ones, which a general update copies across |
| **CEA 1280x720** | The CEA-861 720p TV timing at 60 Hz, with a 74.25 MHz pixel clock |
| **pixel clock, vsync** | The rate pixels are sent at, and the once-a-frame vertical sync |
| **underflow** | The display controller running out of pixel data during a scan |
| **MVID, N_VAL** | The M and N values from which the pixel clock is worked out against the link symbol clock |
| **DISB, DISP2** | The second display controller's power partition and its clock |
| **HDMI, VGA** | The TV interface the dock outputs, and an older analogue one a converter may report |

## Display panel

| Term | Meaning |
|---|---|
| **DSI** | Display Serial Interface, the MIPI link to the built-in panel, the OLED on an Aula as well as the LCD |
| **MIPI DCS** | MIPI Display Command Set, the commands sent over DSI (`GET_DISPLAY_ID`, `SET_BRIGHTNESS`) |
| **BTA** | Bus turn-around: the panel taking the DSI link to reply |
| **long read, short read** | DSI reply packets: a payload of any length, or one or two bytes |
| **LCD, OLED** | The backlit panels of the standard models and the Lite, and the Aula's self-emissive panel with no backlight |
| **WLED** | The white-LED backlight driver of an LCD panel |
| **AVDD** | A panel's analogue supply (the LCD's +5.4 V and -5.4 V channels, PI0 and PI1) |
| **JDI, AUO, InnoLux, Sharp, Samsung** | The panel makers bdk's ID table names |
| **OEM clone** | A third-party replacement panel or touch controller, answering an ID of its own or a copied one |
| **sentinel** | A value written first so that finding it still there shows a read never happened (`0xCCCCCC` for the panel ID); in the GUI, the invisible newest object of a screen that moves LVGL's draw buffer |
| **PWM** | Pulse-Width Modulation, used here for the LCD backlight's brightness and the fan's speed |
| **duty** | The share of each PWM period the output is on, from 0 to 255 here |
| **framebuffer** | The memory a display controller scans out |

## Audio

| Term | Meaning |
|---|---|
| **ALC5639** | The Realtek audio codec: DACs, class-D speaker amplifier and capless headphone driver |
| **codec** | The chip that converts between digital audio and the analogue outputs |
| **rt5640, rt5639** | Realtek family names, as Linux's driver and hekate's audio PR use them |
| **MX-xxh** | Realtek's register notation: MX-FEh is register `0xFE` |
| **private register** | A codec register reached indirectly, outside the main register map |
| **I2S** | Inter-IC Sound, the serial audio link carrying samples from the SoC to the codec |
| **BCLK, LRCK, frame, fs** | I2S's bit clock and its left/right (frame) clock, one stereo sample pair, and the sample rate |
| **master (I2S)** | The side that generates the bit clock, I2S1 here |
| **MCLK, SYSCLK** | The codec's master clock, fed by `AUD_MCLK`, and its internal system clock taken from it |
| **AUD_MCLK** | The master clock the SoC feeds the codec to run its converters |
| **APE / AHUB** | Tegra's Audio Processing Engine and the Audio Hub crossbar inside it |
| **ADMA, ADMAIF** | The audio DMA engine and its interface into the AHUB, whose TX FIFO hwtest fills by PIO |
| **AXBAR** | The AHUB's crossbar, which routes ADMAIF1 to I2S1; a destination's source mux spans three registers, its partitions |
| **CIF** | The format word of an AHUB port's client interface: channels, packing, FIFO threshold |
| **DAS, DAP** | The Digital Audio Switch, and the digital audio ports (pin groups DAP1-DAP4) it connects to the I2S controllers |
| **DSP** | The audio processor HOS runs, which owns some run-control registers there |
| **DAC** | Digital-to-analogue converter |
| **class-D** | A switching speaker amplifier |
| **HPO, capless, HPVOL** | The codec's headphone output, driven with no coupling capacitor, and the headphone volume stage that feeds it |
| **charge pump, depop** | The on-chip supply that makes the headphone output's negative rail, and the start-up sequence that avoids a click |
| **EQ, DRC** | Equalisation and dynamic range control coefficients from board calibration, left out here |
| **SPDIF** | A digital audio interface on the AHUB, clocked with the rest |
| **underrun** | A FIFO emptying before new data arrives |

## Touch, light sensor and Joy-Con

| Term | Meaning |
|---|---|
| **FTS, FTS4** | STMicroelectronics' FingerTip S touch controller on I2C3, as bdk's driver names it |
| **ITO, ITO test** | Indium tin oxide, the panel's transparent sensor lines, and the controller's production test for open or shorted lines |
| **force line, sense line** | The touch grid lines the controller drives, and the ones it listens on |
| **mutual sense, self sense** | Sensing by the coupling between force and sense lines, or by each line alone |
| **autotune, compensation data, CX** | The controller re-trimming its sensing, and the result it stores in its EEPROM (`FTS4_CMD_SAVE_CX_TUNING`) |
| **EEPROM** | Electrically erasable non-volatile memory, here the touch controller's |
| **FPC** | Flexible printed circuit, the panel's ribbon cable |
| **VCM, GND** | The common-mode reference and ground, nets the ITO test checks lines against (hekate's tool prints VDD for VCM) |
| **TSP** | Touch screen panel |
| **GiS, NISSHA** | Touch panel makers in bdk's vendor table |
| **ALS, BH1730** | Ambient light sensor: the Rohm BH1730 on I2C2, which drives HOS's auto-brightness (the Lite has none) |
| **lux, visible, IR** | The light level bdk works out, and the sensor's two channels it comes from |
| **IMU** | Inertial Measurement Unit, the console's accelerometer/gyro |
| **accelerometer, gyroscope, g, dps** | The Joy-Con's motion sensors, read in g (gravity) and in degrees per second |
| **Joy-Con rail** | The slide on each side of the console: a UART, 5 V, a charge enable and an attach line |
| **IsAttached, attach detect** | A rail's detect line (PH6 right, PE6 left), pulled low by a Joy-Con |
| **chassis adapter** | A UART tap that slides onto a rail in place of a Joy-Con |
| **Sio** | bdk's name for the Lite's built-in controls, run by the Joy-Con driver on UART-C |
| **HID, report, subcommand** | The Joy-Con's protocol, as Bluetooth carries it: numbered input and output reports, and the commands in output report `0x01` |
| **SPI flash (Joy-Con)** | The Joy-Con's own memory, holding the stick calibration and the body colour |
| **user, factory calibration** | The stick calibration a user saved, and the one written at the factory |
| **MCU (NFC/IR)** | The right Joy-Con's microcontroller behind its IR camera |
| **rumble** | The Joy-Con's vibration motor |
| **D-pad, SL, SR, ZL, ZR** | Joy-Con buttons: the direction pad, the two buttons on the rail side, the triggers |
| **Hori** | A third-party pad maker whose wired pads bdk handles specially |
| **drift** | A stick resting away from its centre |
| **debounce** | Ignoring a switch's contact bounce |

## Reset and boot state

| Term | Meaning |
|---|---|
| **RST_STATUS** | The PMC's reset-reason field (POR, WATCHDOG, SENSOR, SW_MAIN, LP0, AOTAG), which bdk rewrites at start-up |
| **LP0** | The SoC's deepest sleep state |
| **scratch registers** | PMC registers that survive resets, used to pass boot state (`SCRATCH0` reboot mode, `SCRATCH20` configuration, `SCRATCH200` reset cause) |
| **RTC** | The MAX77620's battery-backed real-time clock |
| **epoch offset, DST** | The offset HOS and hekate add to the RTC calendar (`timeoffset` in `nyx.ini`), and the daylight saving rule |
| **nyx.ini** | Nyx's settings file, read for its time offset |
| **kgdb** | A kernel debugger, which the `SCRATCH20` debug-console flag would have HOS enable |
| **clear-on-read** | A latch that resets when it is read, so only the first read sees it |

## GUI drawing and fonts

| Term | Meaning |
|---|---|
| **focus group** | LVGL's list of the objects the keys move between |
| **message box, long press, label** | LVGL's pop-up with buttons, a press held past 5 s, and a text object |
| **button matrix** | LVGL's grid of buttons (tab bars, message box buttons), whose control bytes are 0x80-0xBF |
| **recolour markup** | LVGL's inline colour codes in a label's text, which carry the report's colours |
| **BMP, bpp** | The bitmap file format of the screenshots, and bits per pixel |
| **glyph, advance, ink** | A character's image, the width it takes in a line, and the part of it that is drawn |
| **Inter, Source Code Pro, Font Awesome (FA)** | The GUI's text, monospaced and icon fonts |
| **res.pak** | hekate's resource file, which holds Nyx's own glyphs |
| **UTF-8** | Unicode's byte encoding, which bdk's LVGL is built without |

## Units and notation

| Term | Meaning |
|---|---|
| **u** | Micro, written u in ASCII: us, uV, uA |
| **KiB, MiB, GiB** | 1024-based sizes; KB, MB and GB appear where the code or a standard uses them |
| **C** | Degrees Celsius |
| **Gbps, Mbaud** | Billions of bits a second (DisplayPort lanes), millions of symbols a second (the Joy-Con UART) |
| **mg, mdps** | Thousandths of a g and of a degree per second, the Joy-Con's raw motion units |
| **px** | Screen pixels |
| **0x, h** | A hexadecimal number, prefixed `0x` or suffixed `h` (`50h`, MX-FEh) |
| **bits a:b** | A register field from bit a down to bit b |
| **XOR** | Exclusive or: the bits in which two values differ |
| **LF, CR LF** | Line endings: line feed alone, or carriage return then line feed |
| **FW** | Firmware |
