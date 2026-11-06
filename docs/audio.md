# Audio probe

Three pages, headed `[ALC5639 audio codec, I2C1 @ 0x1C]`,
`[Audio clocks - APE / AHUB / AUD_MCLK]` and
`[Audio test - melody through I2S1]` (`probe_audio`, `probe_audio_clocks`,
`probe_audio_beep`). The first identifies the ALC5639 codec on I2C_1
(`0x1C`) and, once it answers, walks the bus; the second brings up the
APE/AHUB power and clock domain plus `AUD_MCLK`. The third plays a melody:
the first phrase on the left speaker, the second on the right, then a third,
longer phrase on the headphone jack, split that way so one weak or dead
output can be picked out by ear without needing a meter.

Terms (ALC5639, I2S, APE/AHUB, `AUD_MCLK`) are defined in
[terms](terms.md); the [README](../README.md) has the overview.

## Codec identity (page 1)

The bus, address and control pins come from the Switch device tree: the
codec is `audio-codec@1c` on I2C_1, and the same node names `PZ4` as
`realtek,ldo1-en-gpios`, the enable for the codec's own internal LDO1.
Nothing configures `PZ4` under RCM, so LDO1 is off and the part does not
answer. The code records that as measured rather than assumed: a census of
all six I2C controllers finds every other device the device tree lists on
I2C_1 and nothing at `0x1C`. So the probe drives the enable itself:

1. Read the identity. If the codec already answers (for example on a re-run
   of the page), `PZ4` is not touched at all.
2. Take `PZ4` as a GPIO output, pinmux `PINMUX_INPUT_ENABLE |
   PINMUX_PULL_DOWN` with tristate clear. The input buffer is on only so the
   pad can be read back: with it off, `GPIO_IN` reads 0 whatever the pin is
   doing.
3. Drive `PZ4` to a physical HIGH and print `PZ4 drive HIGH : pad reads N`
   with the pinmux word. The readback separates two faults: `pad reads 1`
   means the pin moved, so a silence after it is on the codec side;
   `pad reads 0` means the pad itself never moved. The device tree flags
   the pin `GPIO_ACTIVE_LOW`, but the flag is inert for this part: Linux's
   rt5640 driver discards it and drives the pad high raw, and HOS's audio
   sysmodule asserts it high and only drives it low on shutdown.
4. Wait 400 ms, the settle both Linux and HOS use, and check the identity.
   If it is still silent, poll every 10 ms for another 1000 ms: HOS treats an
   early NoAck as expected and keeps retrying, so the codec is slow to wake
   rather than absent, and a single probe right after the drive proves
   nothing.
5. If HIGH got no answer, repeat steps 3 and 4 with LOW. That is tried only
   because the device tree's annotation leaves a doubt, and it is cheap once
   HIGH has failed; a codec that never answers costs about 2.8 s.

The identity is either of two registers: MX-FEh reading `0x10EC`, Realtek's
vendor code on every part of the family, or register `0xFF` reading
`0x6231`, the value Linux's rt5640 driver detects on. Accepting both keeps
the probe independent of which one this die populates. The `PZ4 ldo1_en`
line gives the level on entry and how the codec came up: already answering,
enabled by driving HIGH or LOW, or driven both ways with no answer.
`Codec ID` prints the value and the register that matched. `Wake time`
appears only when the probe drove `PZ4`: it counts from the drive at the
level that worked (400 ms at the earliest), and its text says "went high"
even when LOW was that level. `Device ID` gives the id in bits 2:1 of
MX-00h. This page only ever reads MX-00h: a write to it resets every codec
register to its default, which is how the melody page starts its own codec
set-up.

Once the codec answers, the census re-walks I2C_1, since anything sharing
the codec's enable only answers now. It reads one byte (register 0) at every
address from `0x08` to `0x77` and prints the first 16 that answer on the
`I2C_1 census` line, or `(none)`. The expected inhabitants are `0x18`
USB-PD, `0x1C` the codec, `0x36` fuel gauge, `0x4C` TMP451 thermal and
`0x6B` charger. The census is informational and sets no verdict key. A
codec that stays silent ends the page before the census, so it gets no
`I2C_1 census` line; the warning's hint (below) points at the Power page
instead to show whether I2C_1 is up.

`PZ4` is left driven when the page ends, because the melody page relies on
the enable staying asserted. A codec that never answered leaves `PZ4`
driven LOW, the last level tried.

## Audio clocks (page 2)

The page does not depend on the codec: it runs and sets `audio_clocks` even
when page 1 found nothing at `0x1C`; only the melody page needs the codec
as well.

hekate leaves the whole domain off: `hw_init()` gates every audio clock and
holds APE in reset, and the AUD power partition is whatever the boot ROM
left. The order below exists because of one hazard. On this SoC a block that
is clocked but still held in reset does not fault on a read, it never
completes, and the BPMP hangs with no way back (the same hang MSELECT and the
PCIe apertures produce, see [wireless](wireless.md)). So nothing inside APE
is touched until its reset is released. Everything before that is CAR, PMC
or APB_MISC, which always answer, and the state is checked by reading those
back.

1. Latch the AUDIO_HV pad rail as 1.8 V: `PWR_DET` bit 18 set,
   `PWR_DET_VAL` bit 18 clear, 130 us, then `NO_IOPOWER` `AUDIO_HV`
   cleared. Route the DAS: `DAS_DAP_CTRL_SEL` (APB_MISC `0xC00`) = bit 31,
   `DAS_DAC_INPUT_DATA_CLK_SEL` (`0xC40`) = 0. `PWR_DET`, `PWR_DET_VAL` and
   both DAS registers are printed on the `Audio rail` line. Both steps come
   from shinyquagsire23's hekate PR #990 (`misc_audio_init` /
   `rt5639_init`).
2. Ungate the AUD power partition, which powers the whole APE island,
   before any clock work. `AUD partition` prints its state before and after
   and the return code. If it stays off the page stops here: every clock
   after it would be pointless and any later APE access would hang.
3. Enable the clocks of every block that hangs off AHUB at once: APE, AHUB,
   APB2APE, I2S1 to I2S5 and SPDIF. NVIDIA's guidance, quoted in coreboot,
   is that reading an AHUB device hangs unless all of them are clocked.
4. After 2 us, release `SWR_APE_RST` (`RST_DEVICES_Y` bit 6), which the
   `APE reset` line names as the only audio reset on T210, and print it as
   released or `STILL ASSERTED`. I2S1 and AHUB come out of power-on reset
   already released, which is why they need nothing here.
5. Bring up `AUD_MCLK` (below).
6. Only if APE is out of reset, make the first read inside APE: the five
   `I2S_CTRL` registers, on one `I2S1-5 CTRL` row. hekate's
   `_mbist_workaround_bl()` makes the same access after the same sequence on
   this console. Only I2S1 carries the codec.

`AUD_MCLK` is `EXTPERIPH1`, sourced here from `PLLP_OUT0` (408 MHz) with
divisor 66. The encoding is "divide by (N/2)+1", so 408/34 = 12 MHz, which
is what coreboot computes for the same pad. hekate's
`clock_enable_extperiph1()` is not used as it stands: it points
`EXTPERIPH1` at `PLLA_OUT0`, and nothing has enabled PLLA at this point, so
the pad would carry no clock. The divider is written inside a reset bracket
(assert reset, disable, set source, enable, 2 us, release), as hekate's
`clock_enable()` and HOS's `audio.elf` both do. The page then sets PMC
`CLK_OUT_CNTRL` CLK1 to source CAR with `FORCE_EN`, muxes the `AUD_MCLK`
pad `PULL_NONE`, and raises the pad's drive strength. `EXTPERIPH1` reports
the module reset as released or `STILL ASSERTED`; `AUD_MCLK` prints the
source, pinmux, pad-control and `CLK_OUT_CNTRL` words. The melody page
later re-points this clock at 24 MHz, so a run that does not play the
melody leaves `AUD_MCLK` at 12 MHz.

The page leaves the partition ungated, the clocks running and APE out of
reset, which is what the melody page needs.

## Melody (page 3)

Samples reach the codec by PIO, not DMA: the CPU writes straight into the
ADMAIF TX FIFO (`FIFO_CTRL` bit 31), the crossbar routes ADMAIF1 to I2S1, and
I2S1 clocks them out on DAP1. ADMA is powered and reset but never runs, which
keeps the DMA buffer and its cache-coherency hazard out of the path entirely.
Each frame is written to `FIFO_WRITE` (ADMAIF `+0x32C`) once bit 0 of
`TX_ACIF_FIFO_FULL` (`+0x744`) clears.

The register sequence is HOS's own, recovered from the audio sysmodule
(`audio.elf` 0x1a610 power and global enable, 0x1a840 path config), and so
is the codec init table. Two run-control registers are not in it: ADMAIF1
`TX_ENABLE` and I2S1's AXBAR `RX_ENABLE` (`+0x00`, separate from the block
enable at `+0x80`). HOS's CPU code never writes either, because its audio
DSP owns them; with no DSP, the payload writes them itself.

PIO and the clock recipe follow shinyquagsire23's hekate PR #990, the one
known working bare-metal Switch beep. The device tree cannot say which port
carries the codec: it has no sound node and repurposes the DAP4 pins as the
touchscreen enable. `audio.elf` can: it maps exactly one I2S window,
`0x702D1000`, its crossbar write routes ADMAIF1 to the I2S1 RX port, and the
clock it enables is `CLK_SOURCE_I2S1`.

Two checks come first, and either one skips the page without setting a key.
If the codec does not answer a read of `0xFE`, the page prints
`Skipped : codec not answering, nothing to drive`. If `SWR_APE_RST` is
still asserted it prints `Skipped : APE still in reset, run the audio clocks
page`: the pager can re-run this page alone (`r`), and an APE access in that
state would hang the BPMP until a power cycle. Then, in order:

1. PLLA: VCO 38.4 MHz / 2 x 50 = 960 MHz (`DIVM` 2, `DIVN` 50, `DIVP` 0),
   and `PLLA_OUT0` divides by 8 to 120 MHz. The values are in Erista's PLLA
   layout; PR #990's own PLLA writes are the Mariko layout and cannot be
   copied literally. `DIVP` is an exponent, not a plain divider, so a `DIVP`
   of 1 halves everything downstream. The `PLLA` line prints both registers
   and `LOCKED -> 120 MHz OUT0` or `NO LOCK`; a missing lock is only
   printed.
2. `AUD_MCLK` re-pointed at PLLP/17 = 24 MHz (divisor `0x20`). `EXTPERIPH1`
   is held in reset and gated across the switch, because changing the
   source mux of a live divider can leave the output dead or glitched while
   every register reads back as programmed. The `AUD_MCLK` line reads back
   the source and pinmux words; its `24000000 Hz` is the intended rate
   printed as a constant, and the reset release is not read back here
   (page 2's `EXTPERIPH1` line is the only check of it).
3. ADMA and ADMAIF: global soft reset and enable, then ADMAIF1's TX CIF and
   FIFO allocation (see the gotchas). Every soft reset on this page is
   polled for up to 5 ms, then forced to 0, the shape HOS uses; whether it
   cleared is neither printed nor judged.
4. I2S1: the DAP1 pads, the CAR divisor `0x9E` (`PLLA_OUT0` / 80 = 1.5 MHz
   BCLK) with `I2S1_MASTER_CLKEN` kept, a soft reset of the RX datapath
   (without it a re-run starts the port from wherever the last melody left
   it), master mode, `TIMING` = 31 (64 bit clocks per frame), standard I2S
   data offset, the crossbar and the AXBAR RX enable.
   `I2S_AXBAR_RX_SLOT_CTRL` is written 0, matching the reference and the
   L4T driver, which leave it at 0 in basic I2S mode.
5. The codec: software reset, a Realtek private-register write and the
   analogue bias, then HOS's 53-entry init table, then what HOS does on top
   to enable the speakers, then the headphone side brought back up. HOS's
   speaker route also writes 21 private-register EQ and DRC coefficients
   from board calibration; they shape the response rather than gate it and
   are left out, so the melody's tone and loudness are no guide to what HOS
   plays. The `MCLK_DET` and `ADDA_CLK` lines read back `0xFA` and `0x73`
   after they are changed from the table's values, and should show `3601`
   (the table's `3E01` less bit 11) and `1114`. They are the page's only
   codec readbacks, and a failed read prints `0000`, so zeros there mean
   the codec stopped answering during set-up.
6. The melody (below), then `TX_ENABLE` and the PIO bit cleared and the
   codec outputs muted, on the success path and the stall path alike.

That is all the page puts back. Everything else stays as it was set up:
PLLA running, `AUD_MCLK` at 24 MHz, I2S1 enabled as master with its RX path
on, ADMA and ADMAIF enabled, the crossbar routed, the DAP1 pads muxed, and
the codec configured with its DACs, headphone amplifier and charge pump
powered (`_codec_mute_all()` only sets the two port mutes). If the melody
reached the jack phrase, the class-D amplifier and the speaker mixers are
also left powered down: `_codec_hp_on()` drops them for that phrase and
nothing powers them up again.

The clock recipe (`AUD_MCLK` from PLLP/17 = 24 MHz, BCLK 1.5 MHz from
`PLLA_OUT0`, `TIMING` = 31) gives fs = 23437.5 Hz. That is the *actual*
sample rate, and the note pitches and durations are computed from it; using
a nominal 48 kHz instead just detunes and slows the melody. The codec takes
SYSCLK straight from MCLK (`0x80` = 0, no codec PLL), its reset default and
what HOS relies on. MCLK is 1024 x fs, which `0x73` = `0x1114` accepts;
HOS's table sets `0x8814` there, and the code says which of the two is right
for this frame rate has not been settled on hardware.

Durations follow fs, but each note's half-period is rounded down to a whole
number of samples (`TONE_RATE_HZ / (2 x freq)`, with `TONE_RATE_HZ` =
23437), so every note plays sharp, by an uneven 2 to 8 %: 784 Hz gets a
half-period of 14 samples and plays at about 837 Hz. The intervals between
notes are slightly off as a result.

The `I2S1` line prints the frame length read back from `I2S1_TIMING` and
fs = 1.5 MHz / frame length; a good run prints
`64 bclk/frame, fs 23437 Hz`. The 1.5 MHz there is the nominal BCLK, not a
measurement, so the line catches a wrong `TIMING` but not a wrong clock.

| Phrase | Notes + rests | Length | Channels | Amplitude | Lead-in |
|---|---|---|---|---|---|
| Left speaker | 7 + 3 | 2.4 s | left only | `0x0200` | 300 ms |
| Right speaker | 10 + 2 | 2.4 s | right only | `0x0200` | 150 ms |
| Headphone jack | 15 + 3 | about 4.0 s | both | `0x0A00` | 300 ms |

During the speaker phrases the headphone is muted (`0x02` = `0xC8C8`), both
speaker outputs are unmuted and the channel not under test gets zero
samples. For the jack phrase the speakers are muted, `_codec_hp_on()`
brings the headphone amplifier up afresh, and both channels play at five
times the amplitude, because the HP amp is far less efficient than the
class-D. The lead-in is silence, so the output's mute-release ramp finishes
before the first note instead of clipping it. A `Melody` line is printed as
each phrase starts.

## Gotchas

Most of the bring-up is unremarkable; these are not, because getting them
wrong leaves the registers reading back as programmed, and all but
`I2S1_MASTER_CLKEN` (which shows as a FIFO stall) leave the status bits
looking normal too. On the SoC side:

| Gotcha | Why it matters |
|---|---|
| ADMAIF CIF `FIFO_THRESHOLD` = 3, not 0 (`CIF_CTRL` = `0x43113300`) | A zero threshold forwards to the crossbar before real data is held, so the port clocks out stale or empty words: the FIFO drains (PIO sees no stalls) yet no waveform reaches the codec |
| DAS routing (`DAS_DAP_CTRL_SEL`, `DAS_DAC_INPUT_DATA_CLK_SEL`), set on page 2 | The Digital Audio Switch is what connects the DAP1 pin group to I2S1. Miss it and everything upstream looks perfect |
| PMC `PWR_DET` latch for the AUDIO_HV rail, set on page 2 | The DAP1 and `AUD_MCLK` pads sit on AUDIO_HV. Pads on a rail whose voltage is never latched can drive at the wrong level, or not at all |
| `EXTPERIPH1` module reset (`RST_DEVICES_V` bit 24) is asserted at power-on | Nothing else in the payload or in hekate clears it. Held in reset, the source, divider, enable bit, PMC gate and pad mux all read back as programmed while `AUD_MCLK` stays dead |
| `AUD_MCLK` `CFGPADCTRL` resets to minimum drive | A clock driven too weakly to reach the codec looks identical to no MCLK. `CAL_DRVUP` and `CAL_DRVDN` are set to `0x10`, as coreboot's Switch mainboard does |
| `I2S1_MASTER_CLKEN` (`CLK_SOURCE_I2S1` bit 28, reset value 1) | It must survive the divisor write. Cleared, the controller generates no BCLK or LRCK: the ADMAIF FIFO stays full, with no underrun and every other register looking configured |
| A crossbar destination's mux spans three registers, `0x200` apart | Only partition 0 selects ADMAIF1 and the other two are zeroed: nothing resets the crossbar, and a stale bit in another partition would sum a second source into the port |
| ADMAIF1 TX `FIFO_CTRL` = `0x02000300` | The ten TX channels carve up one shared FIFO, and a wrong size or an overlapping start address moves data internally while emitting nothing, so the allocation is written rather than trusted to its reset value |
| DAP1 pads written `DRIVE_4X` with a pull-down | These are AUDIO_HV pads, which the TRM says keep `DRV_TYPE` at `0b11`. A whole-word `PULL_NONE` write forces `DRIVE_1X` and clears the Schmitt trigger, the kind of change that leaves every register reading back as programmed while the pins stop being able to drive a load |
| One 32-bit FIFO word per stereo *frame* | Under the CIF's `UNPACK16`, the low half is left and the high half right. A 16-bit push per frame leaves the high half as sign extension: the right DAC sees DC and the pitch comes out an octave low |

On the codec side:

| Gotcha | Why it matters |
|---|---|
| HOS's init table is exactly 53 entries | Its shutdown table sits right after it in `audio.elf`. Overrunning lands on writes that mute both outputs and power off I2S1, both DACs and the class-D amplifier, which fails exactly like a wiring fault |
| DAC1 digital volume (`0x19`) | The table leaves it at `0x0000`, which on this part is the minimum: a powered, unmuted, silent DAC. It is set to `0xAFAF` (0 dB) |
| `MCLK_DET` (`0xFA` bit 11) cleared | An interlock that mutes the DAC when it sees no MCLK. HOS leaves it set because its MCLK always arrives; clearing it removes a failure that no readback can tell apart from a dead converter |
| The jack is the codec's own capless HPO | Reached through the HPVOL leg (`HPO_MIXER` = `0xC000`), not a DAC1-direct tap. Its negative rail comes from an on-chip charge pump, so the pump and depop sequence have to be walked in order before the amplifier can move the pin. `HP_SG_EN` (`0x8E` bit 2) is cleared too: the soft generator holds a capless pin at its reference and lets only transients through |

## Verdict keys

Three keys feed the `Audio` row of the [verdict](verdict.md), and they do
not prove the same amount. `audio_codec` passes when the codec answers with
its identity, either already on entry or after the probe drives LDO1_EN on
`PZ4` itself (HIGH first, then LOW). Silence is only a WARN (`silent at
0x1C after LDO1_EN + 1400 ms`), never a fail, because nothing here
separates a faulty codec from a board that was never fitted with one; HOS
treats it as optional too. The page adds a hint: if the charger and gauge
answer on the Power page ([power](power.md)), I2C_1 is up and the fault is
the codec's (not populated, unpowered or faulty).

`audio_clocks` fails if the AUD power partition will not ungate (`AUD power
partition would not ungate`) or APE stays in reset (`APE still in reset`),
and passes when the island is up. An `EXTPERIPH1` reset that will not
release is printed in red but does not fail the key.

`audio_beep` is the one to read carefully: it is a **completion marker, not
an acoustic measurement**. Nothing in the payload can hear the console, so
it passes as soon as every frame of the sequence has been accepted into the
ADMAIF FIFO: a console whose speakers are dead, disconnected or swapped
still reports PASS. I2S1 is the bit-clock master, so the FIFO drains at its
own rate whatever is at the far end, and PASS equally covers no `AUD_MCLK`
at the codec pin (nothing on the SoC side can see past the pad), a broken
DAP1 link, or a codec that ignored its set-up, since no codec write or
readback feeds the key. The verdict says the SoC side did its job; your
ears are the instrument for the rest. One silent or weak output points at
that output's own path; silence on all three points at what they share:
the DAP1 link, the codec's MCLK and the codec itself.

It fails (`ADMAIF TX FIFO stalled`, printed as `Result : ADMAIF FIFO stayed
full for >5 ms`) when any push waits more than 5 ms for room. One frame
drains in about 43 us, so a FIFO that stays full means nothing downstream is
taking samples, which is the symptom the code gives for an I2S1 that
generates no bit clock (`I2S1_MASTER_CLKEN` cleared, for one). It is not
set at all when the page skips.

The keys come from two run groups. `audio_codec` and `audio_clocks` belong
to Audio hardware (the GUI's Custom Check calls it "Sound chip"),
`audio_beep` to Sound output ("Sound output (plays a short tone)"). The
`Audio` row appears when either group runs and shows the worst finding with
its detail; a key whose page did not run, or skipped, is left out. The text
launcher's Custom test couples the two groups: picking Sound output adds
Audio hardware, and dropping Audio hardware drops Sound output. The GUI's
Custom Check does not; picked without Sound chip, the melody page plays only
if the codec already answers and APE is already out of reset, and otherwise
skips. Picked alone and skipped, it sets no key: the text verdict's `Audio`
row reads `not tested`, and the GUI's Sound category reads "Not checked"
("These checks did not run.") with no "Speakers and jack" reading, since a
skip prints `Skipped` rather than `Result`.

In the GUI the `Audio` row is the Sound category ([gui](gui.md)), and its
keys read "Audio chip" (`audio_codec`), "Audio system" (`audio_clocks`) and
"Sound playback" (`audio_beep`). Their findings read "The audio chip did
not answer. The speakers and headphones may be silent." (`audio_codec`
WARN), "The audio part of the processor could not be started."
(`audio_clocks` FAIL) and "Sound could not be sent to the audio chip."
(`audio_beep` FAIL). A codec WARN shows there as a real warning, not as
"not checked". When everything that ran passed, the category says "The
audio chip and sound path work." That is the same completion marker, not
proof that sound came out, and with only Sound chip picked it appears
although no melody played: a key the row did not set is skipped rather
than shown as not checked. The category also shows two readings, "Audio
chip" and "Speakers and jack", taken from the `Result` lines of the codec
and melody pages ("codec is powered and answering", "melody played - left,
right, then jack").
