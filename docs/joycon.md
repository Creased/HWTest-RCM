# Joy-Con tests

The Joy-Con screens exist only in the GUI: the text payload's sweep never
talks to a Joy-Con. **Joy-Con** on Home opens the Joy-Con Test
(`gui_joycon_test`, `gui/gui_tests.c`): both Joy-Cons, or a Lite's built-in
controls, with every button that can be pressed on the rail, each stick
drawn live and judged, and a count of connections that dropped. Its header
has Rumble, which buzzes each Joy-Con in turn on this screen, and Motion
and IR Camera, which open screens of their own; none of the three is on a
Lite. The header's Bluetooth button, on a Lite too, opens
[the Bluetooth test](bluetooth.md), for SL and SR and for a Pro
Controller. A Joy-Con slid onto a rail is also greeted, on any screen where
the driver is being polled, by a glow in its body colour.

None of these screens calls `dx_set` or writes to the report: every
judgement below is text on the screen, gone when the screen is left; two
fingers on the touchscreen save a screenshot, the only record (see
[Screenshots](gui.md#screenshots)). The one Joy-Con reading that reaches
the results is the rail detect, see
[In the health check](#in-the-health-check). The overview is in the
[README](../README.md).

## Talking to a Joy-Con

bdk's driver (`third_party/hekate/bdk/input/joycon.c`) brings a Joy-Con up
on its rail, the left on UART-C and the right on UART-B (side 0 and side 1
in the GUI code), and polls it for buttons, sticks and battery. That is all
it exposes: its packet code is static. The GUI polls it through `_jc_poll`
(`gui/gui_main.c`), which runs `gui_dbg_rails()`, `joycon_poll()`, then the
attach watcher. It has to be polled at the input rate all the time: the
driver brings a Joy-Con up over several polls (handshake, reply, status
requests 15 ms apart) and starts over whenever 1.8 s pass without a reply.
How the debug console shares the rails is in
[Rails and Joy-Cons](gui.md#rails-and-joy-cons).

For the rest of the protocol, `gui/gui_jcwire.c` speaks the driver's wired
format itself: a UART header (magic `19 01 03`, `total_size`), a command
header with `cmd` `0x92` (HID) and `payload_size`, then the HID report as
Bluetooth would carry it, offsets counted from the report id as in
dekuNukem's notes and CTCaer's jc_toolkit, where the sequences come from.
It does so two ways:

* **Alongside the driver**: one report dropped into the driver's polling,
  such as a rumble report. The driver reads the reply as any other.
* **Instead of it**: `gui_jc_own` switches the side's UART to hardware flow
  control (`UART_AO_TX_HW_RX`), and the GUI sends and reads on it itself.
  The replies are longer than the driver's manual flow control lets
  through at once (bdk's `uart.h` notes that mode as "Up to 36 bytes
  read"), and the driver switches the same way for its own pairing read.
  `gui_jc_release` drains the FIFO, so nothing is left for the driver to
  trip over, and puts the manual mode (`UART_AO_TX_MN_RX`) back. The flash
  reads are short exchanges done this way; Motion and IR Camera own their
  rails for as long as they are open. When such a screen closes, the
  driver, which was not told, finds the Joy-Con silent and brings it up
  again, which takes a few seconds.

`gui_jc_subcmd` sends output report `0x01`: neutral rumble bytes, the
subcommand at byte 10, its arguments from byte 11, always 49 bytes
(`JC_RPT_LEN`, a Bluetooth-sized report as jc_toolkit sends them). It makes
up to four tries, each waiting 64 ms for a `0x21` reply whose byte 14 names
the subcommand and whose byte 13 has bit 7, the acknowledgement, set.
Anything else in that window is discarded, so an unanswered subcommand
costs about 256 ms.

`gui_jc_recv` buffers 0x200 bytes per side, as a report can arrive in
pieces or several at once, resynchronises on the `19 81 03` reply magic
and skips replies that are not HID. When a header gives a packet length no
Joy-Con sends (under 12 or over 0x200 bytes), it drops what it holds and
starts over. The first reply after each
`gui_jc_own` goes to the debug console as
`gui: jc R reply total T payload P cmd XX first YY`, with
` (sizes disagree)` appended when `5 + total_size` and `12 + payload_size`
differ: the driver never checks the two against each other, so the line is
there for the bench to confirm the framing.

The input request `gui_jc_poll` sends is the driver's `_jc_nx_pad_status`
byte for byte, checksums included. Every other packet the GUI builds
leaves the wired header's `crc_payload` and `crc_hdr` at zero.

## The Joy-Con Test

### Moving around

Every Joy-Con button is under test here, so `gui_jc_nav(false)` stops the
Joy-Con from navigating, and B does not go back. VOL+, VOL- and POWER still
drive the header and touch still works: leave with POWER on the focused
Close button, or a tap. The next screen switch (`_switch_screen`) turns
Joy-Con navigation back on. The header reads, left to right, Rumble,
Motion, IR Camera, Close; on a Lite only Close, because a Lite's controls
do not speak the protocol the deeper tests need.

`_jc_tick` runs every 20 ms, and once straight away. Each tick polls the
driver through `gui_jc_state()` and updates both panels. Leaving
(`_jc_leave`) deletes the task and stops a rumble burst still running.
Motion and IR Camera lead back here, and the screen is built afresh: each
panel starts again from nothing (buttons, stick, drop count, calibration).

### Buttons

Nine per side, by bit of `jc_gamepad_rpt_t.buttons` (bdk `input/joycon.h`):

| Side | Buttons (bit) |
|---|---|
| Left | Up 17, Down 16, Left 19, Right 18, L 22, ZL 23, `-` 8, Capture 13, Stick 11 |
| Right | A 3, B 2, X 1, Y 0, R 6, ZR 7, `+` 9, Home 12, Stick 10 |

"Stick" is the stick click (`l3`, `r3`). SL and SR are left out: they sit
inside the rail once a Joy-Con is on it, and this test only reads Joy-Cons
on the rails. Each button is drawn as a theme button: plain while never
pressed, pressed while held, and the theme's toggled look once let go, so
the plain ones are what is left to press. The title line counts them,
`N of 9 buttons`, then `All 9 buttons` in the OK colour. The pressed set
survives drops and is cleared only when the screen is rebuilt.

### Connection, battery and drops

The status line reads `Not attached: slide it onto the rail` until the side
connects, then `Connected, battery <level>`, followed by `, N drop(s)` in
the warning colour when there are any and `, buzzing` during a rumble
burst. When the driver lets a side go it reads
`Connection lost: reseat it on the rail`.

The level is the driver's `batt_info >> 1`: 0 to 4 are named empty, very
low, low, half and full, anything higher `unknown`. bdk's comments put its
levels 1 to 4 at 3300-3599, 3600-3759, 3760-3899 and 3900-4200 mV. The
level is taken once, at connect, and not refreshed while the side stays
connected.

A drop is counted when a side that was connected before connects again.
The driver lets a Joy-Con go for two reasons: no report for 1.8 s
(`_jc_conn_init`, whose comment gives the official timeout as 2000 ms), or
the rail's attach line saying it is gone (`_jc_conn_check`, through
`_jc_rail_detect`, which reads PH6 for the right rail and PE6 for the left
with the UART TX pins released). The count cannot tell those apart, nor
from a Joy-Con pulled off and put back or another one slid on: each is a
connection that went and came back. A Joy-Con that leaves and never
returns shows "Connection lost" but adds no drop. The stick model, the
calibration and the pressed buttons are kept across a drop.

The sensor screens take the rails over, and the driver needs a few seconds
to find the Joy-Cons again once they close. So Motion and IR Camera, when
they took a rail over, call `gui_jc_test_resumed()` on the way out: for
the next 8 s (`JC_RESUME_MS`)
no reconnect counts as a drop, and a side not yet back reads
`Reconnecting after a sensor test (a few seconds)`. After that a side that
never came back reads `Not attached` again.

### The calibration from the flash

At a connect, unless one has already been read on this screen, and never
on a Lite, `gui_jc_cal_read` reads the stick calibration from the
Joy-Con's SPI flash, so the stick is judged as the firmware judges it.
Each read is subcommand `0x10` with a 4-byte little-endian address and a
length; the reply must repeat those five bytes at byte 15, and the data
follows at byte 20. In order:

1. The user calibration: left `0x8010`, right `0x801B`, 11 bytes, the magic
   `B2 A1` then nine bytes. Taken when the read is acknowledged, the magic
   matches and the nine bytes decode.
2. Otherwise the factory calibration: left `0x603D`, right `0x6046`, nine
   bytes.
3. Otherwise none, and the stick is judged on its own travel (below).

Every three bytes hold two 12-bit values. The left stick's six are max,
centre, min; the right's are centre, min, max, each an X,Y pair. Max and
min are travels from the centre (right and up, left and down), not
positions. A decode is accepted only when each travel is within
`0x200`..`0xA00` and the centre lies strictly between `0x400` and `0xC00`;
erased flash reads `0xFFF` and fails.

The subcommand's own four tries are the only retries, so a pad with no
flash to read (a third-party pad) holds the screen for about half a
second, both reads failing, at every connect. A successful read is not
repeated. The debug console gets
`gui: jc L cal user centre CX,CY travel lN rN uN dN` (`factory` for the
second; `l` is `xmin`, `r` `xmax`, `u` `ymax`, `d` `ymin`), or
`gui: jc L cal: none read`, which a Lite prints too.

### The stick model

`gui/gui_stick.c` is plain arithmetic on the raw 12-bit readings, with no
LVGL in it. A Joy-Con stick does not rest at `0x800`: each unit rests up to
a tenth of its travel away, and its travel is its own too. With a
calibration, each direction is judged against its calibrated travel and
the resting point against the calibrated centre, which is what the
console's firmware does. Without one both are learnt, and each direction
is judged against the stick's own best one, because wear is lopsided: a
tired stick comes up short one way, not evenly all round.

* **Feed** (`gui_stick_feed`, every tick). The first reading stands in as
  rest, minimum and maximum. A new minimum or maximum on either axis
  raises `STICK_GREW`. A move of more than 40 counts (`STICK_STILL`) from
  where it last stopped restarts a timer; staying within 40 counts for
  over a second takes that reading as the resting point (`STICK_RESTED`),
  provided both axes lie strictly inside `0x400`..`0xC00`, since held
  against an edge is not resting. The rest is taken again at every settle.
* **Dot.** A 28 px dot in a 220 px ring, clamped to 96 px on each axis
  separately. With a calibration it is offset from the calibrated centre
  and scaled by that direction's travel, so a drifting stick's dot rests
  off centre, as the firmware sees it. Without one it is offset from the
  learnt rest and scaled by `STICK_SPAN` (`0x480`, a good stick's travel),
  so it rests in the middle. The 56 px inner circle is only drawn.
* **Range** (`gui_stick_range`). The reach each way is measured from the
  calibrated centre, or from the rest point without a calibration. A
  direction is covered once it reaches `STICK_COVER` (`0x280`) raw counts,
  which a stick drifting a third of the way out still reaches on its short
  side. The share is the reach over the calibrated travel, or without one
  over the best direction's reach (at least `0x280`), so the best
  direction then reads 100%. Shares are capped at 100.
* **Drift** (`gui_stick_drift`). The rest point's distance from the
  calibrated centre over the mean of the four travels; without a
  calibration, from the middle of the travel seen over half the larger
  extent (at least `0x280`), so a rest point off to one side does not
  stretch its own yardstick. In percent, capped at 100.

Extremes never shrink; the model is cleared only when the screen is
rebuilt.

### What the stick lines say

The centre line starts as "Let go of the stick for a second, then roll it
all the way around." At a rest with fewer than four directions covered it
reads "Resting point noted. Now roll it all the way around." Once all four
are covered, the drift is judged:

| Drift | Line |
|---|---|
| 8% or less | OK: `Centred when let go` |
| 9% to 15% | WARN: `Rests N% off centre, slight drift possible` |
| over 15% | FAIL: `Rests N% off centre: the stick drifts` |

The code waits for the four directions with a calibration too, although
`gui_stick.h` calls the drift meaningful at once with one. In practice the
drift first appears at the next rest after the roll, and is judged again
at every rest after that.

The range line stays empty until a rest point is known, then is rewritten
at each new extreme: `Roll it all the way around: N of 4 directions so far`
until all four are covered, then the first that applies of:

1. No calibration and drift over 15%: `Range N-100% from where it rests`,
   in plain text, with no OK or WARN mark. Measured from a resting point
   that is off centre, a drifting stick reaches less one way; that is the
   drift's doing, which the centre line already says.
2. No direction reaching `STICK_SHORT` (`0x400`) raw counts: WARN
   `Short travel all round, worn or not pushed to the edge`. This is
   checked before the shares, with or without a calibration.
3. The smallest share 85% or more: OK `Reaches the edge all round`, with
   ` (by its calibration)` when there is one.
4. Otherwise WARN `Short to the <direction>: N% of its [calibrated ]travel,
   worn or not pushed to the edge`.

A stick rolled all the way round before it ever rested shows no range
line until its next new extreme. Each update with four directions covered
also prints `gui: jc L stick rest X,Y reach lN rN uN dN`: the raw figures,
there for the bench to learn what a good stick reads; the GUI does not use
them.

### On a Lite

The driver runs a Lite's built-in controls (the Sio) instead: one
controller, both halves always present. Both panels follow `conn_l`, the
left panel takes the left stick and the right panel the right one, and
both read the same button word, laid out as a Joy-Con's, so the table
above applies (bdk reads Home from PV3). The Lite's two sticks sit half a
turn apart compared with a Joy-Con's: the left reads Y upside down and the
right X backwards, as switchroot's joycon-serdev driver maps them, so those
axes are flipped before the model. There is no calibration read and no
glow. The driver puts its connected flag in `batt_info_l` (`joycon.h`:
"Also Sio Connected status"), so by the code a Lite's panels read
`battery empty` whatever the charge; it is not a reading. The 1.8 s
timeout applies to the Sio too, so a drop counts on both panels at once.

## Rumble

Rumble opens no screen: it runs on the Joy-Con Test itself. The left
Joy-Con buzzes for half a second, then after a 250 ms gap the right one
does, so a dead motor is told from a working one by feel. A press while a
burst runs is ignored, and a side that is not connected is left out.

At the start of each side's buzz, `gui_jc_rumble_enable` sends subcommand
`0x48` with argument 1, alongside the driver, which reads the reply. The
driver enables rumble once, at the first connection after the rail was
found empty (bdk clears `rumble_sent` only then), so a Joy-Con it brought
up again after a 1.8 s timeout, which is what follows a sensor screen, has
lost that. While on, the rumble report (`0x10`, `C2 C8 03 72` twice: full
strength on both bands, as the console plays it when a Joy-Con is
attached, and what hekate sends) goes out every tick, because the
driver's own reports could mute it; one off report (`00 01 40 40` twice)
ends it. The status line shows `, buzzing`
meanwhile. Leaving mid-burst sends off to any side still on. Rumble stays
enabled afterwards.

## Motion sensors

Each Joy-Con's accelerometer and gyroscope arrive in every input report
once subcommand `0x40` turns them on. The driver never asks and parses only
buttons and sticks, so `gui/gui_jcmotion.c` takes both rails over:

1. At open, `gui_jc_state()` says which sides are connected; a side absent
   then shows `Not attached: attach it and come back` and is not picked up
   later, as the driver is not polled again.
2. Per connected side: `gui_jc_own`, then `0x40` with argument 1. The
   status reads `Connected, sensor on`, or WARN
   `The sensor did not answer` when it is not acknowledged, and polling
   goes on regardless.
3. Every 15 ms: an input request to each side, then its reports are
   drained. A `0x30` report holds up to three 12-byte samples from byte 13
   (accelerometer X, Y, Z then gyroscope X, Y, Z, signed 16-bit), 5 ms
   apart; the last complete one is used.
4. On leaving: `0x40` with argument 0, `gui_jc_release`, and the resume
   window of the Joy-Con Test.

The raw units are dekuNukem's: 4096 counts per g (the +/-8 g range) and
70 mdps per count (the +/-2000 dps range). The code converts with
`raw * 244 / 1000` mg and `raw * 70 / 1000` deg/s, applies no calibration,
and never sets the range. A needle shows accelerometer X and Y, full length
at 1 g, and three bars the gyroscope at +/-2000 dps full scale. Under the
bars a monospaced line gives all six values, signed: accelerometer X, Y, Z
in g, then gyroscope X, Y, Z in deg/s; it is the only place accelerometer
Z shows. As the screen's help says, held upright the needle points down
and laid flat it shrinks to a dot, gravity then being on Z. The verdict,
per side:

* FAIL `No readings. The sensor sends nothing.` when no sample that is not
  all zero has arrived 1.5 s after polling began.
* OK `The sensor works. At rest it reads X.YY g` once the magnitude stays
  within 750..1250 mg with every axis below 100 deg/s for 600 ms. It
  latches, the figure is the live magnitude, and ", and it felt the turn"
  is added once any axis has gone over 250 deg/s. A turn is not needed to
  pass.
* WARN `Readings stopped` when, before a pass, the stored sample has been
  all zero for over 1.5 s after real data.
* Otherwise `Reading: hold it still a moment`.

The debug console gets `gui: motion L sensor on` (or `no answer`), then
the first sample with a non-zero accelerometer, in raw counts, as
`gui: motion L first sample acc X Y Z gyr X Y Z`.

## IR Camera

The right Joy-Con carries a 320 x 240 infrared camera behind its own
controller, the NFC/IR MCU, driven through output report `0x11` and read
through input report `0x31`. bdk never touches it. `gui/gui_jcir.c` owns
the right rail and runs the sequence from CTCaer's jc_toolkit on the wire,
with jc_toolkit's checks on each reply. Without a right Joy-Con at open it
says so and does nothing. The left Joy-Con is not polled while it runs.

| Step | Sent | Waits for |
|---|---|---|
| 1 asking for camera reports | subcommand `0x03`, `0x31` | ack of `0x03` |
| 2 waking the camera's controller | subcommand `0x22`, `0x01` | ack of `0x22` |
| 3 waiting for it to be ready | `0x11` status request | MCU report: standby |
| 4 switching it to camera mode | `0x21`: mode IR (`21 00 05`) | ack, `p[15]` and `p[22]` = 1 |
| 5 confirming camera mode | `0x11` status request | MCU report: IR mode (`p[56]` = 5) |
| 6 setting the image size | `0x21`: image transfer, last fragment's number, firmware 5.18 or later | ack, `p[15]` = `0x0B` |
| 7 setting the sensor up | `0x21`: nine register writes, then a mode query | ack, `13 00 07` |
| 8 setting the lights up | `0x21`: eight register writes | ack, `p[15]` = `0x23` or `13 00 07` |

Step 7 sets resolution, a 300 us exposure, the lights, gain 1, no external
light filter and the white pixel threshold; the mode query follows at once
because the controller answers it with the write's acknowledgement in
front. Step 8 sets both lights full, de-noise, the buffer update time, and
applies. Subcommand `0x21` carries a CRC-8 (polynomial `0x07`) over bytes
12..47 in byte 48; the `0x11` mode query and fragment acknowledgements
carry one over bytes 11..46 in byte 47, with byte 48 `0xFF`. The status
request goes without one.

While it starts, the status reads `Starting: step N of 8, <name>`. Each
step is resent every 64 ms without a match, up to 8 sends (about 0.5 s),
or 40 (about 2.6 s) for steps 3 and 5, because the controller takes a
moment to boot. Then it fails: FAIL `Could not start: step N of 8,
<name>`, with `Nothing came back, after N tries.` or `The Joy-Con answered
XX:` and eight bytes of the last report, and the note that a controller
that does not answer at all may have an older firmware, or no camera. It
stays there until Try Again or Image Size: nothing retries on its own.

| Size | Fragments | Register `0x2E00` |
|---|---|---|
| 80 x 60 (first) | 16 | `0x64` |
| 160 x 120 | 64 | `0x50` |
| 320 x 240 | 256 | `0x00` |
| 40 x 30 | 4 | `0x69` |

Image Size steps through them in that order and Try Again restarts; both
suspend the controller first and begin again at step 1. A frame arrives
300 pixels per `0x31` report; each fragment is acknowledged, a repeated
fragment (a missed acknowledgement) is acknowledged again and not counted,
and after 200 ms without one the last is acknowledged again. A frame is
complete when its last fragment arrives, and is drawn at 480 x 360,
nearest neighbour. The status reads `Started, waiting for the first
<size> frame`, then `Streaming <size>, measuring the rate`, then
`Streaming <size> at X.X frames a second`, the rate taken over windows of
at least a second. The detail line, `Brightness N%, N frames so far`,
gives the controller's own brightness figure (byte 53) as a share of 255
and the frame count. The verdict, the first that applies:

* WARN `No frames are coming. Is it still on the rail?` after 3 s without
  a fragment.
* OK `The camera sees.` once the brightness figure at a completed frame is
  above 5 of 255, which the screen shows as 2% or more. It latches until a
  restart.
* WARN `Frames come, but black.` after 10 frames with none brighter: dead
  lights or sensor, or a covered window. The help text calls a dark
  picture with nothing in front normal, so this can show with nothing
  wrong: hold a hand in front, 10 to 30 cm away, as the help text says,
  before reading anything into it.

On leaving, the controller is suspended (`0x22` with 0), subcommand `0x03`
with `0x30` asks for ordinary reports again, the rail is released and the
Joy-Con Test's resume window starts. The screen sends nothing of its own
to the debug console; only the framing line appears.

## Attach glow

`_jc_attach_watch` (`gui/gui_main.c`) runs inside every `_jc_poll`: from
the keypad read while Joy-Con navigation is on, and through `gui_jc_state()`
on the Joy-Con Test, the Screen test and as Motion and IR Camera open. A
side whose rail was found empty (not connected, battery byte 0) and then
connects is shown. The battery byte is the point: bdk clears it only when
the rail reads empty, so the driver bringing a Joy-Con up again after a
timeout or a sensor screen does not glow. Nothing is shown in the first
3 s after `jc_init_hw` (`JC_ATTACH_QUIET_MS`), so Joy-Cons already on the
rails when the GUI starts are not greeted. Not on a Lite.

The watcher reads the body colour from flash `0x6050` (three bytes, under
`gui_jc_own`, up to about 256 ms) and keeps grey (`82 82 82`) when nothing
answers or the bytes are all `0xFF`, never written. `gui/gui_jcattach.c`
lifts a colour whose luminance, `(3R + 6G + B) / 10`, is below `0x60`
towards white, at most half way, so a black Joy-Con still shows on the
dark background. The glow is a 96 px strip on that side's screen edge, on
LVGL's top layer: alpha 150 of 255 at the edge, falling as the square of
what is left of the 96 px, to zero at its inner end, and softened over
160 px at the top and bottom. It fades in over 300 ms, rests 700 ms, fades
out and is deleted.

## What the tests change

* UART-B and UART-C are switched to hardware flow control while the GUI
  owns them, and back to the driver's manual mode after a drain.
* Rumble stays enabled after a burst; the motor is left off.
* Motion turns the motion sensor off again, and IR Camera suspends the
  controller and restores the `0x30` report mode, each as one subcommand
  whose acknowledgement is not checked, so a Joy-Con that does not answer
  may keep the sensor on, or the camera's controller awake and the `0x31`
  mode. The camera's registers are not reset.
* Nothing is written to a Joy-Con's flash: the only flash access is
  subcommand `0x10`, a read.

The rails' 5 V comes from `jc_init_hw` at GUI start. While it is polled,
bdk's driver also looks at each Joy-Con's battery level every 5 s and, when
the charge mode should change, sends subcommand `0x51`: a full Joy-Con gets
its rail's 5 V switched off, a low one charges at 100 mA. The tests neither
report nor undo this.

## Debug console lines

Every `gui: jc` and `gui: motion` line goes through `gui_uart_puts`, and
once a Joy-Con has been seen on the console's rail nothing more is sent
there for the session, so with the default console on UART-B and a right
Joy-Con attached none of these lines reaches the bench, the left side's
included. Which port carries them is in
[Rails and Joy-Cons](gui.md#rails-and-joy-cons).

## In the health check

The sweep reads the rails in `probe_joycon` (`probe_inputs.c`): the two
attach lines, PH6 and PE6, and no Joy-Con protocol. It sets no verdict
key; its `ATTACHED` or `empty` shows as the **Left rail** and
**Right rail** readings of the **Touch and sensors** category. See
[Inputs](inputs.md#joy-con-rails).

## What a failure points at

* Drops or `Connection lost`: the Joy-Con's connection on its rail, which
  is why the status line asks for it to be reseated. Reconnects after a
  sensor screen are left out of the count as not the rail's doing.
* A Joy-Con that stays still during its half of a Rumble burst: its motor,
  which the burst is there to tell by feel.
* A button that stays plain: the driver never reported it pressed; the
  test names no cause.

The stick, Motion and IR Camera verdicts carry their own pointers, quoted
with them above.

## What they cannot tell

* Whether a motor turns: `buzzing` only means hwtest is sending, and the
  `0x48` acknowledgement is not checked.
* SL and SR, Joy-Con charging, or the battery beyond five coarse levels
  taken at connect; nothing about a Lite's.
* A worn stick from one not pushed to the edge, as the wording says.
* Whether the flash calibration is right; it is only checked for
  plausible sizes.
* A stick that never stays within 40 counts of one point for a second (a
  noisy one), or whose rest lies outside `0x400`..`0xC00`: it is never
  taken as resting, so the centre line keeps its first hint, the range
  line stays empty, and no verdict is given. One stray reading, likewise,
  stays in the extremes until the screen is rebuilt: it can cover a
  direction, or without a calibration shift the middle the drift is
  measured from.
* A different Joy-Con slid onto the same rail without leaving the screen:
  it counts as a drop and is judged on the first one's calibration and
  learnt travel. Leave the test and open it again after a swap.
* Whether the gyroscope works: one that reads zero on every axis looks
  still, so the side passes; look for `, and it felt the turn` or the bars
  swinging. An accelerometer that never reads 750..1250 mg at rest, while
  the sample is not all zero, gets no WARN or FAIL either: the line stays
  on `Reading: hold it still a moment`.
* A Joy-Con that stops sending on the Motion screen: the screen keeps
  judging its last sample, so it neither fails nor says the readings
  stopped.
* Missing camera fragments: a frame completes when its last fragment
  arrives, and gaps keep the previous frame's pixels. "The camera sees"
  only means a frame was not black; seeing the hand is left to the eye.
* Whether a camera that will not start has older controller firmware or
  no camera at all, or whether black frames are the lights or the sensor.
* Whether a Hori pad answers any of this: bdk computes the wired header
  checksum only for Hori pads (`JC_ID_HORI`), noting it is only checked
  there, and the packets the GUI builds leave it at zero for every pad.
