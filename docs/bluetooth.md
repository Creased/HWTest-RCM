# The Bluetooth test

The GUI's Bluetooth screen (`gui/gui_btscreen.c`) finds a Joy-Con or a Pro
Controller in pairing mode, connects to it over the console's own radio and
shows every button it has, SL and SR included, with its sticks. The rail
cannot show SL and SR (they sit inside it), and a Pro Controller has no
rail. It opens from the **Bluetooth** button in the
[Joy-Con](joycon.md) window's header, on a Lite too. Nothing of it runs from
the health check, and nothing is written to the report.

Under the screen is a small Bluetooth host (`gui/gui_bt.c`): HCI over
UART-D, L2CAP's two HID channels and the controllers' own report protocol,
one controller at a time, all of it polled from the screen's 10 ms task.
The health check's [Bluetooth probe](wireless.md#bluetooth-radio-probe)
only proves the radio answers; this is the part that talks to a
controller.

## Using it

1. Put the controller in pairing mode: hold its small sync button until
   its lights sweep (on a Joy-Con's rail side, on top of a Pro Controller
   by its USB-C port). With Joy-Cons on the rails, **Pair from rails** does
   it for them, see [Pairing mode from the rails](#pairing-mode-from-the-rails),
   then they are slid off.
2. **Search**, about ten seconds, then each controller found is asked its
   name. The list keeps gamepads only (class of device major Peripheral,
   not a keyboard or a mouse); a Joy-Con or a Pro Controller is
   `0x002508`.
3. Pick one. A box asks first, because pairing may make the controller
   forget the console it was paired with: a Joy-Con pairs with the console
   again when slid onto its rail, a Pro Controller from Change Grip/Order
   or over its USB cable.
4. The right panel shows the steps as they happen, then the controller:
   one stick ring for a Joy-Con, two for a Pro Controller, and one pill per
   button that lights while held and stays marked once pressed. The type
   comes from the controller's own answer (subcommand `0x02`), else from
   its name.

Close powers the radio down and puts back the two regulators and the
32 kHz clock it changed. Disconnect ends the link and leaves the list.

## Bringing the radio up

The radio comes up exactly as the probe's first arm brings it up
(`bt_attempt` in `probe_bt.c`, copied, not called, because the probe puts
everything back when it is done): straps parked, both REG_ON pins low,
`BT_REG_ON` raised for a cold power-on reset, 200 ms, then `HCI_Reset` at
115200. LDO1 and LDO7 are switched on and the PMIC's 32 kHz output to the
radio enabled, as the probe does, and `WL_REG_ON` goes back high so Wi-Fi
is not left dark.

**No vendor command is sent.** The radio runs its ROM firmware as it is:
HOS loads a firmware patch over HCI first, which is not ours to ship. It
keeps the address the ROM comes up with, `AA:AA:AA:AA:AA:AA`, a filler (HOS
gives it the console's own from PRODINFO with a vendor command), and it
stays at 115200. The log says so.

Then, each answer checked and any refusal logged: Read_BD_ADDR,
Read_Buffer_Size (8 ACL buffers on the console), the firmware's version and
whether it does Simple Pairing (LMP features byte 6 bit 3), the event mask
(only defined bits, as BlueZ sets them: the default leaves out the Simple
Pairing events, and a mask with reserved bits may be refused whole), Simple
Pairing mode, inquiry results with RSSI, and the default link policy (role
switch and sniff allowed, see below).

From there the UART runs with hardware flow control both ways, its receive
trigger at 8 bytes, so the radio holds what it has while the GUI draws.

## Connecting

Pairing comes first, as BlueZ pairs a controller before it opens anything.
Asked for an HID channel before that, a Joy-Con on the console hung up with
`0x05` (authentication failure) before any pairing event reached the host.

1. Create_Connection, basic-rate packet types only (`0xFF1E`, see below),
   the controller allowed to take the master role.
2. Its supported features read, for the trace; the flush timeout read
   (0, never: a packet the radio counts as done was delivered); the packet
   types set again on the link; a QoS request for 2.5 ms latency.
3. Authentication_Requested: Secure Simple Pairing, "just works". The
   link-key request is answered with none (or with the key from earlier in
   the session), the IO capability request with NoInputNoOutput and general
   bonding, the confirmation with yes. A PIN request (legacy pairing) gets
   `0000`; a passkey request is refused. If pairing fails, the host calls
   the controller again once, on a fresh link.
4. Encryption on.
5. L2CAP: the HID control channel (PSM `0x11`), then the interrupt channel
   (PSM `0x13`), each configured both ways. Information requests are
   answered, echo requests too, and a controller may open the channels
   itself.
6. Three subcommands in output report `0x01`, padded to the 49 bytes its
   report descriptor gives it: full reports (`0x03 0x30`), what it is
   (`0x02`), the first player light (`0x30 0x01`). Each is tried twice as
   HID DATA on the interrupt channel, then twice as SET_REPORT on the
   control channel, and the screen goes on without an answer after the
   fourth.

Input reports `0x30` and `0x21` carry the buttons in bdk's
`jc_gamepad_rpt_t` layout (SR and SL of the right Joy-Con at bits 4 and 5,
of the left at 20 and 21), the two sticks as 12-bit pairs and the battery.
The stick dot is drawn from where the stick sat in the first report.

## What it took on the console

Each of these was found on the user's console, driven from a PC over USB
(below), with a trace of every HCI command, event and L2CAP signal:

- **Basic rate only.** The ROM firmware does not receive EDR packets.
  Everything a Joy-Con sent that fits a basic-rate DH1 arrived (its plain
  `0x3F` reports, echo replies, signalling), every full report and every
  subcommand answer (about 54 bytes, a 2-DH1) was lost, while what it was
  sent was carried out (its player light came on). With the EDR types
  forbidden on the connection the reports come through.
- **Sniff allowed.** After a reset the link policy allows neither role
  switch nor sniff, so the radio refuses, with no event to the host, the
  sniff a Joy-Con asks for to save power; the link died seconds after the
  channels opened. Once the controller has gone to sniff, the host takes
  the link back to active and forbids sniff on it: in sniff it woke every
  15 ms and carried far fewer reports.
- **Reading the UART long enough.** With flow control the radio stops when
  the FIFO fills and takes well over 300 us to start again once it has
  been read. A read loop that gave up after 300 us of quiet took about a
  FIFO per call, 13 reports a second of a Joy-Con's 60, and the screen ran
  3-4 s behind. The loop now reads until the line has been quiet for 2 ms,
  at most 8 ms a call; on the console that gives about 89 reports a second.

Signal strength on the console read -77 to -100 dBm, weak for a Joy-Con in
hand next to a Switch; it did not stop any of the above.

## Pairing mode from the rails

**Pair from rails** sends subcommand `0x06` ("set HCI state"), argument
`0x02`, to each Joy-Con on a rail, over the rail (`gui_jc_subcmd`): it
reboots into Bluetooth pairing mode, as the sync button does. The rails are
polled for up to 3 s first, so the driver has a Joy-Con slid on a moment
ago up, then not polled at all until the button is pressed again or the
screen is left, so the driver does not take a rebooted Joy-Con back. Both
of the user's Joy-Cons took it, and a search found them 12 s later. Not on
a Lite.

## From a PC

With the USB serial port open ([The GUI](gui.md#commands-from-the-pc)), the
test can be driven without touching the console:

```sh
python host_tools/hwtest_push.py cmd BT OPEN
python host_tools/hwtest_push.py cmd BT RAILPAIR
python host_tools/hwtest_push.py cmd BT SCAN --follow 15
python host_tools/hwtest_push.py cmd BT LIST
python host_tools/hwtest_push.py cmd BT CONNECT 0 --follow 20
python host_tools/hwtest_push.py cmd BT STATE
```

`BT STATE` gives the step, the last failure, the buttons, the reports and
their rate, line errors, ACL packets sent and completed, and the bytes the
read loop took in how many calls. CONNECT skips the confirmation box.

## Reading the log

Each step goes to the screen's list (the last ten, while no controller is
shown) and to the debug console as `gui: bt <step>`. The wire goes to the
debug console only, as `gui: bt> <what> <code> len <n>: <first bytes>`:
`cmd` for HCI commands, `ev` for events (not inquiry results or completed
packets outside a connection), `l2 out` and `l2 in` for L2CAP signals,
`hid out` and `hid set` for subcommands, and `data in` for the first 40
HID frames of a connection.

Three L2CAP echoes go out around the set-up, 4 bytes and 50 bytes once the
channels are open and 4 bytes again at the end, logged as `echo sent` and
`echo answered`. A controller's L2CAP answers them whatever its HID layer
makes of the host, so they tell a link that does not carry a frame of that
size from a controller that ignores what reaches it (a Joy-Con answers with
no data).

## The emulator

RCM-Emulator's radio model has two fake controllers (`t210/btpeer.cpp`): a
Pro Controller that needs pairing with bonding and fails its first pairing
of a run, and a right Joy-Con that needs no security. Both answer the
subcommands and stream reports, pressing each button in turn and sweeping
their sticks. They prove the host against this reading of the protocol,
not against a controller: none of the three findings above shows there.

## Limits

- One controller at a time.
- The radio keeps the ROM's filler address, so a controller paired here
  remembers a host the console never is.
- A Pro Controller has only been through the emulator.
- Rumble, motion and the IR camera stay rail-only.
