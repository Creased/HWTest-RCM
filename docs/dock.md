# Dock Test

Dock Test is the GUI's screen behind the home screen's Dock button. On
the left, under "The USB-C port's controller", is a live view of the USB-C
port's PD controller (Rohm `BM92T36`), read only, from the moment the
screen opens, docked or not, so what the controller does on its own can be
watched. On the right, under "Docking, step by step", is the handshake the
console performs when it docks, in eight rows covering its steps, with what
each found and where it stopped if it did. Two buttons: Run (the
handshake, then the TV picture when a TV is on the dock) and Reset Chip
(HOS's boot-time reset of the controller, for one that does not answer).
The hint under the rows asks to dock the console or plug a USB-C PD
charger, then press Run.

The handshake changes the port's state, so nothing of it runs from the
health check: the screen sets no verdict keys and feeds no row of the GUI's
results (the "USB-C power chip" row, `usb_pd`, is `probe_usbpd` in
`probe_power.c`). While the screen is open the GUI runs at the payload's
clocks, not its boost: the code records that the PD controller answers
there and did not at the GUI's boost (see
[internals](internals.md#the-guis-clocks)). Its comments record runs on
hardware with two docks, Nintendo's and a Ugreen one.

## The live view

The controller is on `I2C_1` at `0x18`. A snapshot every 500 ms (about
twenty I2C reads) starts with the ids, `FW_TYPE` (`0x4B`), `MAN_ID`
(`0x4D`) and `DEV_ID` (`0x4E`). A chip that has dozed off may let the
first transfer go by, so they get six tries 20 ms apart: three in packet
mode (one transaction with a repeated start, as bdk reads this chip), then
three in normal mode (two transactions, as the fuel gauge is read). The
chip answers when `MAN_ID` reads `04B5` and `DEV_ID` `03B0`, and later
reads, the handshake's too, use the mode that worked.

| Line | Read from | Shows |
|---|---|---|
| `BM92T36` | `FW_TYPE`, `FW_REVISION` (`0x4C`), `DEV_CAPS` (`0x07`) | the firmware type in bdk's nibble form (`36` for the raw `0603` the silent view expects), revision, capabilities; packet or normal mode |
| `Alert` | `ALERT_STATUS` (`0x02`) | see below |
| `Status1` | `STATUS1` (`0x03`) | cable in or out, sink, vsafe or "no power"; data role, busy, fault; the last command's result |
| `Status2` | `STATUS2` (`0x04`) | the live CC side (`STATUS1` bit 11), VCONN, e-marked cable, accessory, OTG |
| `DP` | `DP_STATUS` (`0x18`), `0x1C`, `0x1D` | DisplayPort mode on (bit 15), inserted (14), pins configured (1), signal (7); two unnamed registers HOS reads around DisplayPort |
| `Config` | `CONFIG1` (`0x06`), `CONFIG2` (`0x17`), `SYS_CONFIG1` (`0x26`), `SYS_CONFIG2` (`0x27`) | raw |
| `Offer` | `READ_PDOS_SRC` (`0x08`) | up to seven source profiles, or "nothing" |
| `Contract` | `CURRENT_PDO` (`0x28`), `CURRENT_RDO` (`0x2B`) | voltage and current, and "up to" the operating current requested; or "none" |
| `HPD` | the hot plug detect line | high or low, read by `GPIO CC1`, or by `DPAUX1` once the picture has taken the pad (see [The AUX channel](#the-aux-channel-dpaux1)) |
| `Pins` | `PV5`, `PK4`, `PK5` | reset, interrupt and VCONN-enable lines: `sfio` (the pad on its special function, not a GPIO), `out N` or `in N` |
| `Switch` | nothing | the `PI3USB30532`, "(inferred)": see [What it cannot tell](#what-it-cannot-tell) |

Reading the alert register may clear it, so while Run is going the
snapshot leaves it alone and the line shows the bits the handshake has seen
and not yet used, "held by the handshake". Otherwise it shows the register,
with "bits stay set across reads" when a bit was set in two snapshots in a
row, or "cleared by the read" when the one before saw bits and this one
none. Until DPAUX1 owns it, the HPD pad (GPIO CC1) is set to a GPIO input
for each read: the only write the view makes.

When the ids do not come back, the view shows the last ids and firmware
type read against the expected `04B5` / `03B0` / `0603`, whether the
transfers reported an error, the three pins, and the fuel gauge on the
same bus, whose device name (`0x36`, register `0x21`, expected `00AC`)
tells a silent controller from a dead bus. "Answers" means the bus works
and the controller alone is silent, which is what Reset Chip is for (the
view ends by saying what it does); "silent too" points at `I2C_1` itself.

## Run: the handshake

Run performs the handshake as CTCaer's `bm92txx.c` in the switchroot L4T
kernel does it, which is what Linux on the Switch docks with; HOS does the
same from its usb sysmodule. The screen advances it every 40 ms, and each
wait gives up after 2.5 s unless said otherwise.

Run disappears while the handshake goes and comes back when it ends;
there is no cancel, and Reset Chip is refused meanwhile. Two waits have no
end: with no cable the first step waits for ever, and a dock whose hub
keeps answering busy keeps step 12 going for ever. Leaving the screen ends
the run, with the clean-up given at the end of
[The rows and the verdict](#the-rows-and-the-verdict).

### What it asks for

As L4T asks. Exactly two profiles, 5 V 0.5 A then 15 V at 2.6 to 3 A, is
the Nintendo dock's offer: it gets the 15 V profile, drawing 1.2 A, the
`BQ24193`'s limit at that voltage. Two profiles starting with 5 V 0.5 A
and any other second one fail as "it offers power profiles, but none this
console takes". Any other source gets its most powerful fixed 5, 9, 12 or
15 V profile at 3 A or less (more than 3 A is passed over), with the same
failure when none qualifies. The current drawn is the profile's less a
reserve for a USB device (4.5 W worth above 5 V, 2.5 W at 5 V), capped at
1.2 A at 15 V, 1.5 A at 12 V and 2 A below, rounded down to the charger's
steps (100, 150, 500, 900, 1200, 1500, 2000, 3000 mA). The request object
carries the profile's current as its maximum and this as the operating
current.

### Commands and messages

A command goes to the command register (`0x05`) and is done when the
alert's command-done bit (2) comes up with `STATUS1`'s busy bit (13) clear
and its last-command field (bits 4-6) at 0, "complete". No done bit in
time fails the step as "no answer to" the command, another result as
"... was refused (last command ...)" with the command and the result:
aborted, invalid, rejected, terminated, or "?" for the codes it has no
name for. The command is one of "sink ready", "the data role swap",
"DisplayPort mode" and "the DisplayPort configuration".

A message (a VDM) goes to `OUTGOING_VDM` (`0x60`) and out with `SEND_VDM`
(`0x1111`); the step waits for the alert's VDM-received bit (11), answers
`ACCEPT_VDM` (`0x1616`) and reads the reply from `INCOMING_VDM` (`0x50`).
Its failures: "the message could not be written to the controller", "no
reply to the message", "the reply could not be read".

### The sequence

1. **Waiting for a cable.** The alert pending at the start is read and
   dropped. The step waits, with no limit, for `STATUS1`'s insert bit (7),
   then 300 ms for the source to announce itself, then reads its profiles.
   None within 2.5 s of the plug: "no power profiles offered: a plain
   charger or cable, not a PD source".
2. **Asking for power.** The request object to `SET_RDO` (`0x30`), then
   `SEND_RDO` (`0x0707`). The alert's contract bit (12) must follow, else
   "no power contract came back"; the contract is read from `CURRENT_PDO`.
3. **Sink ready.** `PS_RDY` (`0x0505`), then the data role, `STATUS1` bits
   8-9: the host (DFP) goes on to step 5, a device (UFP) to step 4, any
   other role (none or accessory) fails as "no data role after the
   contract".
4. **Asking to be the data host.** `DR_SWAP` (`0x1818`); the role must then
   read DFP, else "still not the data host after the swap".
5. **Asking who it is.** Discover Identity, a structured VDM on SVID
   `FF00`; anything but its acknowledgement fails as "the other end did not
   say who it is". The reply sorts the device three ways (below).
6. **The dock's mode, or DisplayPort support.** A Nintendo dock gets Enter
   Mode on Nintendo's SVID `057E` ("the dock refused its own mode" unless
   acknowledged on that SVID). Anything else gets Discover SVIDs ("it does
   not list any alternate modes" unless acknowledged; the list is not
   searched for `FF01`), then Discover Modes on the DisplayPort SVID
   `FF01`, whose reply must say UFP_D capable with DisplayPort signalling,
   else "it does not do DisplayPort".
7. **Entering DisplayPort.** `DP_ENTER_MODE` (`0x3131`); from here the way
   out leaves DisplayPort mode. The answer, the capabilities object in
   `INCOMING_VDM`, is read no sooner than 100 ms after the command was
   sent, as L4T gives the controller.
8. **Configuring DisplayPort.** The capabilities' pin assignments come from
   the third byte for a Nintendo dock or a receptacle, the second for a
   plug (a dock reports a plug but answers on the receptacle assignments,
   the code notes). Pin assignment D, 2 lanes, as L4T runs the dock, which
   keeps USB 3 on the other pair; C, 4 lanes, if that is all it offers;
   neither, or a reply that is not four bytes, fails as "no usable pin
   assignment (..)", reported as "Stopped while entering DisplayPort"
   because the reply is read in step 7's state. The configuration object,
   as L4T builds it, goes out with `DP_CFG_AND_START_HPD` (`0x3636`): byte
   0 `0x06` makes the other end the sink (UFP_U as UFP_D) with DisplayPort
   signalling, and the pin goes in byte 1, or byte 2 for the Nintendo dock.
   The code records that a Nintendo dock refuses anything else and a
   generic adapter let a wrong one through.
9. **Waiting for the TV's hot plug detect.** The dock's HDMI side raises
   HPD a moment after the lanes are set, so up to 1.5 s for the HPD line or
   `DP_STATUS`'s signal bit (7). It never fails. Anything but a Nintendo
   dock is done here.
10. **Asking the dock's state.** Steps 10 to 12 are the Nintendo dock's own
    messages, unstructured VDMs on its SVID. Command `0x16`, a read; a
    `0x17` reply carries four state bytes, bit 1 of the third marking old
    dock firmware. Another reply is no failure.
11. **Lighting the dock.** Command `0x01`, a write, fade 64 and duty 128
    (full brightness, a short fade in). Then 500 ms for the hub's own
    power-up.
12. **Powering the dock's hub.** Command `0x20`, a write. While the `0x21`
    reply says busy, 250 ms and ask again: the code's ten-try limit never
    binds, because every resend resets the count. A reply without the busy
    bit marks the hub up; any other leaves it unconfirmed.

### Three kinds of device

Discover Identity gives the vendor id (the ID header's low 16 bits), the
product type (its bits 27-29) and the product id (the product object's high
16 bits).

- **A Nintendo dock**, `057E:2003` with product type 5 (an alternate mode
  adapter): every step (4 only when the console starts as the device).
- **A Nintendo charger**, `057E:2004`: done after step 5, power only.
- **Anything else**: the generic DisplayPort path, done after step 9, with
  no state, light or hub.

### What ends a run at any step

Every tick reads the alert register (adding its bits to those not yet
used), `STATUS1`, `STATUS2` and `DP_STATUS`. Past the cable wait, a cleared
insert bit ends the run: "the controller went quiet: every register reads
zero; Reset Chip is HOS's answer" when `STATUS1`, `STATUS2` and the device
id all read zero, "the cable went away" otherwise (why a controller goes
quiet is in [Talking to the controller](#talking-to-the-controller)).
Either of `STATUS1`'s fault bits (0-1) ends it as "the controller reports
an internal fault (N)".

### The rows and the verdict

Steps 3 and 4 share the Data host row, 6 the Dock mode row, 7 and 8 the
DisplayPort row, 10 to 12 the last. A running row shows the step's name, a
failed one the reason, and those after it "not reached".

| Row | When done | Warning when |
|---|---|---|
| Cable and power on offer | the number of profiles, "the dock's 5 V then 15 V" for the dock's offer | |
| Power contract | the contract's voltage and current, "drawing up to" the current asked for | |
| Data host | "this console is the USB host" | |
| Who is there | "Nintendo dock", "Nintendo charger ..., not a dock", or "another device" with a vendor name for a dozen known ids and the product type; VID:PID | anything but a Nintendo dock |
| Dock mode | "entered" (Nintendo dock), "DisplayPort offered" (anything else) | |
| DisplayPort | lanes and pins, "configured by the controller", ", mode not on" when `DP_STATUS` bit 15 is clear | `DP_STATUS` bit 1 clear: "NOT configured" |
| TV on the dock | "a TV is on the dock's HDMI (HPD high)", when HPD is high or `DP_STATUS` bit 7 set | neither: "nothing on the dock's HDMI (HPD low)" |
| Dock light and hub | light on or off, hub up or "not confirmed", the four state bytes, "(old dock firmware)" | hub not confirmed |

For a Nintendo charger the Dock mode, DisplayPort and TV rows say "not a
dock"; for anything but a Nintendo dock the last row says "not a Nintendo
dock". The DisplayPort row's lanes are the controller's pin assignment;
the SoC's link is trained on at most 2.

The verdict is "Stopped while" the step, then the reason, on a failure.
Done, it is "The dock answers all the way through: power, its own mode,
DisplayPort, light and hub." for a Nintendo dock, or a warning: "A
Nintendo charger, not a dock: the port and its controller negotiated with
it." or "Not a Nintendo dock: it negotiated power and DisplayPort as a
generic adapter." The hint then reads "Done. Run does it again; ..." or
"Stopped. Run tries again from the top; Reset Chip if the controller went
quiet."

Leaving the screen stops the TV picture, sends the dock's light-off message
(fade 128, duty 0) if the light was lit, and `STOP_HPD_EXIT_DP` (`0x3B3B`,
then 100 ms) if DisplayPort mode was asked for (step 7 reached). The power
contract and the data role stay, as they do when the console is docked.
Run again takes the picture down and starts over from the cable step;
nothing goes to the dock for the last run, and leaving acts only on what
the latest run reached: after a later run that stops earlier, no light-off
message and no `STOP_HPD_EXIT_DP` go out for the earlier one.

## Reset Chip

Reset Chip does to the controller what HOS's usb sysmodule does at every
boot of a console with a battery (read from its 17.0.0 build). It does
nothing while Run or another reset is going, takes the TV picture down
first, and the live view takes no snapshot until it ends, so the view's
reads do not land between its steps. Run is not refused during a reset:
pressed then, it reads the alert register at once, and its steps wait for
the reset to end. The pins, as HOS's gpio service names them: `PV5`
`PdRstN` (reset), `PK4` `CradleIrq` (interrupt, active low), `PK5`
`PdVconnEn` (VCONN enable).

1. The pins as found are logged. `PK4` becomes an input, pulled up and
   tristated as L4T has it; `PV5` an output, pulled down as L4T's device
   tree has the pad, driven high.
2. `PV5` low again at the first of the screen's 40 ms ticks past 30 ms,
   so the pulse is at least 30 ms. Then 100 ms.
3. Up to 2 s for `PK4` to fall. HOS waits for ever; here a line that never
   falls is reported and the reset goes on.
4. `DEV_ID` polled in packet mode until non-zero, for up to 10 s from the
   start of this stage, else "no device id within 10 s, the reads reporting
   errors" (or "none"). HOS polls every 10 ms, this once a tick.
5. The alert read, as HOS reads it here; `SYS_RESET` (`0x0D0D`); 100 ms,
   L4T's wait after it; the alert and both status registers read.
6. `DP_ALERT_EN` (`0x19`) written `0x00FF`, the value HOS writes, and read
   back; `VENDOR_CONFIG` (`0x1A`) read and its over-current protection
   disable bit (2) cleared if set, as HOS does. A failed write ends the
   reset as "the DP alert enable register did not take the write" or "the
   vendor config register did not take the write".

The result replaces the hint: "Reset done: the interrupt line fell after N
ms" (or "never fell"), when the device id came back and whether reads
reported errors before, what `SYS_RESET` left in the alert and status
registers, the DisplayPort alerts and vendor config read back, and "(its
over-current protection was off)" when the bit was cleared. A failure reads
"Reset stopped while" the stage and the reason, then "The pins before
it:" with `PV5`, `PK4` and `PK5` as found. The pins stay as the reset set
them, `PK4` an input pulled up and `PV5` low; leaving mid-pulse drives
`PV5` low too, where HOS leaves the line. The controller keeps what the reset
gave it, the DisplayPort alerts on and its over-current protection on, the
values HOS sets.

## The TV picture

When Run ends with the handshake done, DisplayPort mode entered and the HPD
line high, it goes straight on to show the Dock Test screen itself on the
TV, the GUI's 1280x720 frame at 60 Hz. The TV on the dock row also turns
green on `DP_STATUS`'s signal bit alone, so it can say a TV is there while
no picture is tried and no log is saved. A Nintendo charger never enters
DisplayPort mode; a generic adapter does.

The SoC side follows switchroot's L4T display driver, with coreboot's
Tegra210 driver for the link's dividers, the AUX retries and pad settings,
and the PLL clamp while locking. The Switch uses SOR1 (the SoC block that
drives the DisplayPort lanes), DPAUX1 and the second display controller
(DC1), as its device tree says.

### The AUX channel (DPAUX1)

DPAUX1 lives in the SOR partition, which holds DSI too and so is on
whenever the panel runs; gated, it is powered up ("the SOR partition did
not power up" otherwise), and host1x, which any DPAUX access needs, is
clocked if it is not. The AUX pads are set as coreboot sets them for
DisplayPort: AUX mode, 50 ohm, 0.70 V common mode, the receiver on, then
powered up.

The hot plug pad (`hdmi_int_dp_hpd_pcc1`) then moves from GPIO to SFIO
mode, its DisplayPort function, because in GPIO mode DPAUX1 does not see
the line: the code records both docks showing HPD high by the GPIO and
unplugged by DPAUX1. This is never undone; from the first picture attempt
to the end of the session every HPD read, the live view's included, goes
through DPAUX1.

The sink, the dock's DisplayPort converter, is then probed. DPAUX1 must see
HPD ("DPAUX1 does not see HPD: no DisplayPort sink on the dock, or the
lanes are not routed to it") and the sink's DPCD (its registers, read over
AUX) must answer at `000`..`00F` ("the sink does not answer the AUX
channel", with the AUX error and AUXSTAT); then the sink count, lane
status, sink and branch OUI and id, and the TV's EDID through the
converter (I2C over AUX at `50h`). An AUX transaction carries up to 16
bytes and has 40 ms (the DisplayPort specification's limit). A native one
is retried as coreboot does, twice on an error, seven times on a defer;
the EDID's I2C-over-AUX ones get up to 32 tries, errors and defers alike,
as L4T's `dpaux.c`.

### SOR1 and the second head

1. NVIDIA's OUI `00044B` into DPCD `300`, as L4T's `tegra_dp_dpcd_init`.
   The link: the port's 2 lanes (1 if the sink offers only one), the
   sink's highest rate up to 5.4 Gbps (HBR2), enhanced framing and
   training pattern 3 when it has them. Its dividers (transfer unit, active
   symbols, watermark, blanking) are computed exactly as coreboot computes
   them; a mode the link cannot carry fails as "the mode does not fit the
   link", as 720p does on one lane at 1.62 Gbps.
2. The pads' regulators on the MAX77620, `LDO7` `vdd-gen-pll-edp` and
   `LDO8` `vdd-hdmi-dp`: one found on is left exactly as it is, one found
   off is set to 1.05 V and enabled, and stays on. `LDO8` is software
   controlled and normally off; hekate turns it on only on the Lite, where
   it is the I2C1 pad supply at 2.8 V. `LDO7` is normally up from the
   power-up sequence.
3. `PLLDP` at 270 MHz, the SOR macro's reference, from the 38.4 MHz
   oscillator: 19.2 MHz x (55 + 0.5 + 6144/8192) = 1080 MHz, divided by 4.
   `PLLD2` at 74.25 MHz, the pixel clock: 891 MHz divided by 12, code 9 of
   the quasi-linear post-divider table (code 8 is /10, which ran the head
   at 89.1 MHz on the first hardware runs). Each is clamped while it starts
   and released once locked, and left alone if it already runs at that
   setting.
4. The DISB partition if gated, `DISP2` clocked from `PLLD2`, SOR1's block
   on its safe clock and out of reset.
5. DC1 set up and held stopped: CEA 1280x720 timing, window A the frame as
   it lies in memory, landscape, 32 bits a pixel.
6. The SOR macro as L4T's DP enable has it: the differential DP clock, the
   link rate and SOR1's DisplayPort prods from switchroot's SoC device trees
   (Erista's `prod_c_dp`, Mariko's with its rate prods); a termination
   calibration, a binary search on `TMDS_TERMADJ` against the comparator;
   the pads powered up. SOR1 moves to the macro's feedback clock.
7. The lanes powered through the lane sequencer, the link parameters set,
   and the crossbar between the SOR's lanes and its pads set as the
   Tegra210 SoC device tree has it for sor1, `<2 1 0 3 4>`: DP lane i goes
   out on the pad the list gives at position i (lane 0 on pad 2, lane 1 on
   pad 1), which also picks the lane's power bit and its byte in the drive
   registers.
8. An external panel on the macro clock. A sink that can use the alternate
   scrambler reset, ASSR (DPCD `00D` bit 0), gets it on both sides, DPCD
   `10A` and the SOR's internal-panel bit, as L4T does: training patterns
   are not scrambled, so a mismatch would only show once video flows.
9. The sink woken to D0 (DPCD `600` = 1); HPD must still be there.
10. Link training (below).
11. The attach, in the order of L4T's `tegra_dc_sor_attach`: the SOR told
    the raster, attached asleep and safe, then normal; the head started and
    its clock to SOR1 enabled; the SOR awake. The same values go into the
    head's assembly copies, so a later general update cannot take them
    away. The SOR must report the head awake within 200 ms.

### Link training and the margin check

Training runs against the sink's own status registers, at the chosen rate
first, then at 2.7 and 1.62 Gbps as trainings fail. The lanes are
precharged, 8b/10b coding and no spread (`PLLDP` has none) set, then the
rate and lane count, which the sink must take. Clock recovery runs on
training pattern 1 with scrambling off, the drive levels following the
sink's requests (never below the swing floor, swing plus pre-emphasis at
most 3), until every lane locks, for up to 20 rounds or until five requests
in a row leave the swing unchanged. Equalisation runs on pattern 3 with
post-cursor2 when the sink has pattern 3, pattern 2 otherwise (a choice
made once, kept at the lower rates), for up to four rounds, until every
lane reports equalisation done and symbol lock and the lanes are aligned,
clock recovery holding throughout. The drive levels are the Switch's
device tree's "Default T210 lt-data", with the Mariko tree's three swing
level 3 overrides.

A trained link then gets a margin check, because the Nintendo dock showed a
picture only on runs without symbol errors on DP lane 0. The sink's symbol
error counters (DPCD `210`..`213`) are read, which clears them, and read
again after 20 ms of idle pattern; the 15-bit counts of lanes 0 and 1 are
added. With any errors the link is trained again with the swing floored
one step higher, at most twice (floor 2), and then goes on with the errors
reported. A training at a raised floor that fails drops the rate, and the
floor goes back to 0.

### The frame on the TV

The TV is fed its own copy of each finished frame, so it never shows the
screen half drawn: LVGL draws in place, background first, so a head
scanning the drawing buffer shows every change half drawn (the code records
the Nintendo dock blinking that way, 2026-11-05). The copy is allocated at
the first picture and kept for the session; each start fills it with the
whole frame, and while the picture is on each finished frame copies only
the rows it drew. Without memory for the copy the head scans the drawing
buffer itself, and the half-drawn frames come back.

### What it reports

The result replaces the hint at the bottom of the right column, and the
handshake's verdict line is cleared. A failed probe gives "No DisplayPort
sink on the AUX channel:" with its reason and HPD as DPAUX1 sees it, and
"Run first: the picture follows once the DisplayPort step is green."
Otherwise:

| Line | Shows | Green when |
|---|---|---|
| Sink | DPCD revision; downstream port (DP, VGA, HDMI or other); the TV's name from the EDID's name descriptor (`0xFC`), its three-letter maker code without one, or "no EDID." | always |
| Link | lanes and rate, ", ASSR", lane 0's swing, the margin check's errors, ", NOT in sync" (", no sync bit" on an STDP2550) | no margin errors, and port 0 in sync unless the converter has no sync bit |
| Head | frames per 100 ms, pixel clock in MHz, ", UNDERFLOW" or ", NOT 74.25", "SOR sees it" or "sees nothing" | frames counted, no underflow, pixel clock within 1 percent (73.5 to 75 MHz, exclusive) |

In sync is DPCD `205` bit 0, read 85 ms after the attach and again a moment
later; either counts, because some converters drop sync for a moment when
video starts and lock again (the Ugreen dock did, every time). The STDP2550
of the V1 / V2 Nintendo docks (branch OUI `0080E1`) never sets the bit, not
even while it shows the picture: an Erista on a known-good dock and a working
Mariko on another both trained 2 lanes at HBR2, swing 2, 0 errors, picture
on, with `205` reading `00` (2026-11-11). For that branch the line says
", no sync bit" and the bit does not colour it; the Realtek converters do set
it, and for them "NOT in sync" still counts. The frames
are vsyncs counted on host1x syncpoint 11, which needs no interrupt logic;
the pixel clock is the SOR's `DP_DEBUG_MVID` against `N_VAL` (`DP_MN0`) of
the link symbol clock; the underflow is window A's, DC `INT_STATUS` bit 8.
"SOR sees it" is the valid bit of the SOR's CRC of the raster it receives
and does not change the colour. The head's CRC, the sink's test CRC and the
symbol errors after the attach go to the log only: the test CRC counted 0
frames while the Nintendo dock showed the picture.

A start that fails gives, after the Sink line, "No picture (step N):" with
the reason, the lanes (DPCD `202`..`204` as the last training left them),
whether `LDO7` and `LDO8` are on, and the time taken. N is the start's own
failure code, not the numbering of the list under
[SOR1 and the second head](#sor1-and-the-second-head).

| Step | Why |
|---|---|
| 2 | "the mode does not fit the link", at the first rate or after a drop |
| 3 | "PLLDP did not lock" |
| 4 | "PLLD2 did not lock" |
| 5 | "the DISB partition did not power up" |
| 7 | "the lane sequencer did not finish powering the lanes" |
| 8 | "HPD went away" |
| 9 | training failed down to 1.62 Gbps, with the last training's reason: "the sink did not take the link rate" (or "lane count"), "clock recovery failed after N tries", "clock recovery lost during equalization", "equalization failed after N tries" |
| 11 | "the head did not wake up behind the SOR" |

Steps 1 and 6 exist in the code but cannot occur here: the start only
follows a probe that got an answer, and the macro's bring-up has no failure
path. There is no step 10. Step 2 at the first rate comes before the
regulators are looked at, so it reports both off. A failed start stays as
far as it got until the next stop.

### The log

Every line of the picture goes to the debug console and into a 48 KiB
buffer, emptied at each attempt (lines past its end go to the console
only). It is saved to the SD card after every attempt, success or failure,
as `hwtest/tvlink_YYYYMMDD_HHMMSS.txt` from the PMIC's real-time clock,
and the report's last line gives the path. A save that fails for any
reason (no card, a failed mount or write, no memory for the buffer)
reports "Log not saved: no SD card."

Lines starting `gui: tv link:` come from the probe: its failure (HPD by
DPAUX1 and by the live view's last read, AUXSTAT), or a few EDID bytes (0,
1, 8 to 11, 18, 19) with maker and name, DPCD `000`..`00F` with the sink
count and lane status, and the branch and sink OUI and id. Lines starting
`gui: tv:` come from each stage of the start: the link and its dividers,
the regulators, the clocks, the termination value, every clock recovery
round, each margin check and rate drop, the attach and the readings behind
the report. A SOR that never reports itself attached is logged ("L4T only
logs this") and the start goes on.

Once the start got as far as SOR1's block, a dump follows between
`dump: start` and `dump: end`: CAR's `PLLD2` and `PLLDP`, clock sources,
enables and resets; PMC's power gate and clamp status; every SOR1 register
(`0x00`..`0x77`); DC1's command, CRC, pin output, display and window A
registers (active copies); checksums of rows 100, 360 and 700 of the GUI's
buffer and of the one the head scans (identical right after the start
unless the copy or the address is wrong; all zero would be a black
screen); DPAUX1's interrupt status and AUXSTAT; DPCD blocks `000`, `2200`,
`300`, `080`, `100`, `200`, `210`, `240` and `600`.

### Taking it down

Run, Reset Chip and leaving the screen all take the picture down first,
undoing whatever the start reached, in the TRM's order: the SOR safe,
asleep and detached; the head stopped in its assembly copies first, then
its active ones, because the attach mirrored its settings there and a
general update would bring them back (it did: the head kept scanning the TV
buffer at 60 Hz after leaving); after 20 ms for the frame in flight,
`DISP2` gated and held in reset; the lanes, the macro's port, PLL and
bandgap down; SOR1 reset and gated; ASSR cleared in DPCD `10A`, the sink
put to D3 (DPCD `600` = 2); `PLLD2` and `PLLDP` off. What stays is the HPD
pad with DPAUX1, the regulators as the start left them, and NVIDIA's OUI
in the sink's DPCD. DPAUX1 itself stays clocked, out of reset and with its
pads up for the rest of the session, the SOR partition and host1x's clock
stay on if the AUX set-up turned them on, and the DISB partition stays
powered if the start powered it up.

## Talking to the controller

Reads go in the mode the snapshot found, except those over eight bytes (the
profiles and the message replies), always in packet mode. Writes of up to
seven bytes (commands, the request object, the structured VDMs, the
DisplayPort configuration) go in normal mode, which ends with a STOP. The
longer ones, the Nintendo dock's own messages, go through a packet write
of the payload's own whose header asks for the STOP, as HOS's Start|Stop
option does, and which waits for the packet to complete before switching
packet mode off.

Both avoid bdk's packet write, which ends with no STOP: its header asks the
controller to carry on, and nothing else in hekate uses it. The code
records that the PD controller took the request object that way once and
read zero in every register from then on (a photo of 2026-11-04), the state
HOS's reset pulse is for, and that it went quiet on the first message
written by a packet write that stopped at the byte counter bdk polls, which
reaches the last byte while it is still on the wire, so the STOP was cut.
That state is the "went quiet" failure, and Reset Chip is the answer to it.

## Debug console

The screen writes to the debug console (see the [README](../README.md)):

- `gui: dock <step>: alert .. s1 .. s2 .. dp ..` at every change of step,
  the end included, with the registers as the last tick read them.
- `gui: pd alert .. s1 .. s2 .. dp .. 1c .. 1d .. hpd N (read in N us)`
  every second and whenever `STATUS1`, `DP_STATUS` or HPD change, with
  "(alert bits held)" when alert bits stayed set across two snapshots;
  while Run goes the alert is the handshake's.
- `gui: pd controller answers` (or `silent`) `at N s since boot` the moment
  that changes, so a chip that dozes off shows up.
- `gui: pd reset: ...` once per stage of Reset Chip: the pins before, the
  interrupt line, the device id, the `SYS_RESET` result, the ids and
  configuration read back, or the failure.
- The picture's `gui: tv link:` and `gui: tv:` lines.

## What it cannot tell

The USB 3/DisplayPort switch (`PI3USB30532`) between the SoC and the port
answers on none of the six I2C buses the SoC reaches (the census of all six
finds no address for it), and neither L4T nor its device tree drive one.
The code's inference is that the PD controller sets it as part of the
DisplayPort pin assignment, and the live view says so. It is tested only
through that step: the DisplayPort row warns "NOT configured by the
controller" when `DP_STATUS`'s pin-configuration bit stays clear.

The hot plug wait gives up after 1.5 s without failing, so "nothing on the
dock's HDMI" is a warning, not a fault, and a TV that raises HPD after it
gets no picture. The hub is confirmed by the dock's reply alone.

The mode is always CEA 1280x720 at 60 Hz: the EDID is read for the TV's
maker and name only, not to choose it. The picture's report reads what
the head sends, what the SOR receives and what the sink's DPCD says;
nothing reads what the TV shows. A clean margin check is what the Nintendo
dock's picture needed and the sink's test CRC is no guide, so whether the
picture is on the TV is for the eye to judge.
