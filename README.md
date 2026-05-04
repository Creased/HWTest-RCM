# hwtest RCM

![hwtest running in the emulator](img/demo.png)

A read-only RCM payload that probes every BPMP-side IC the Switch boot ROM hands
off to, then reports identity / status / faults to the LCD and UART_B (Joy-Con
right rail, 115200 8N1). Output is also captured into a heap buffer that gets
written to `backup/<emmc_serial>/hwtest.txt` on the SD card.

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
| `s` / `S` | Re-write the SD report file |
| `q` / POWER | Power off the console |

## What's reported

Nine logical pages (some span multiple LCD-sized sub-pages):

| Page | Probes |
|---|---|
| Verdict | top-level cross-checks across SoC / fuses / charger / display / touch / battery / eMMC / thermal / USB-PD / fuel-gauge |
| SoC | HIDREV chip ID + major/minor |
| Fuses | identity + speedo / IDDQ + lot/wafer + public key + SBK/DK + KFUSE |
| Power & charging | MAX77620 + GPIOs + regulators + 5V + battery + charger + USB-PD + thermal + fan |
| Memory & clocks | LPDDR4 mode regs + CLK_RST raw + decoded rates |
| Storage | SD + eMMC + Switch serial + partitions + health + GPT + BOOT0/pkg1 + AutoRCM + SD content scan |
| Display | DSI panel ID + backlight PWM + GPIO state |
| Inputs | touch FW + Joy-Con rails + buttons + AC adapter |
| Raw state | GPIO pin census + UART debug port + reset reason + PMC scratch |

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
main.c               - probe sequence + sinks + pager + UART command loop
emmcsn.c             - Hekate-style backup/<emmc_serial>/<sub>/<file> path
diskio.c, ffconf.h   - FatFS glue + config (vendored from the Hekate bootloader)
exception_handlers.S - boot relocator (vendored from the Hekate bootloader)
gfx/                 - LCD driver (vendored from the Hekate bootloader)
stubs.c              - minerva_deinit no-op
link.ld              - link script
Makefile             - devkitARM build for both variants
host_tools/          - ImGui + pyserial UART viewer (parser, GUI, replay)
```

## Acknowledgements

Heavy reliance on the work of:

- [Hekate / BDK](https://github.com/CTCaer/hekate)
- [Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM)
- [switchbrew.org](https://switchbrew.org/wiki/)
