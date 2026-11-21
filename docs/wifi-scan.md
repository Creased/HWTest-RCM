# Wi-Fi Scan

A GUI tool, **Wi-Fi Scan** along the bottom of Home, next to Custom Check
and Last Results. It boots the CYW4356 with the firmware the console's own
HOS carries, asks the chip to scan, and lists the networks it heard. The
health check stops at enumeration, the chip's PCIe front end answering a
few config reads (see [wireless](wireless.md)). A scan goes through what
the orange screen points at: a firmware download of several hundred KB over
PCIe, the chip's ARM core running, the chip DMAing into console memory, the
radio itself. A chip that only fails under load gets a chance to fail here.

**Status: written against Linux's brcmfmac driver and not yet run on a
console.** Every step reports where it stopped, so a first run on hardware
says which part needs work. Nothing here sets a verdict key or goes in the
report.

## What it needs

- `sd:/switch/prod.keys`, as Lockpick_RCM writes it: `bis_key_02`,
  `header_key` and the `key_area_key_<kind>_<rev>` for the wlan NCA's key
  generation. Lockpick_RCM is on Home when its payload is on the card.
- A readable SYSTEM partition. Nothing is written to the eMMC or the SD
  card.
- A GUI built with an aarch64 compiler (CI's build is). Without one the
  screen says the image was not built.

## What it does

The left column logs each step as it goes; the debug console gets the
same lines (`gui: wifi: ...`).

### 1. The firmware and NVRAM, out of HOS (`gui/gui_wlanfw.c`)

HOS keeps neither as a file: both are embedded in the wlan sysmodule
(title `0100000000000016`), in the `.data` segment of its ExeFS `main`.
This is where nx-fwextract (perillamint/nx-fwextract), the extractor the
Switch Linux ports use, finds them:

| Blob | Where |
| --- | --- |
| NVRAM | the NUL-terminated text starting `NVRAMRev=` |
| firmware | from just after the last NUL in the 1 KB before the first `DBPP`, to the NUL after its `4356a3-roml...` version string |

The way there, once per GUI session (the result stays in memory):

1. `prod.keys` read from the SD card.
2. SYSTEM opened through bdk's BIS layer with `bis_key_02` (keyslots 4/5,
   as `nx_emmc_bis_init` picks for SYSTEM).
3. A read-only FAT32 walk of `Contents/registered`, long names and all,
   taking each `.nca` file, or the `00` inside an `.nca` directory.
4. Each NCA's header decrypted (AES-XTS, Nintendo tweak, `header_key`)
   until one is `NCA3`, a Program, title `0100000000000016`.
5. Its key area key unwrapped with `key_area_key_<kind>_<rev>`; section 0
   (the ExeFS, a PFS0 under AES-CTR) decrypted. CTR is done as ECB over
   counter blocks, so nothing depends on how the engine's own CTR mode
   counts.
6. `main`'s NSO header, its `.data` read and LZ4-decompressed, the two
   blobs found as above.

Keyslots 4, 5 and 8-11 are cleared before it returns. The NVRAM goes to the
chip in brcmfmac's binary form: `key=value` entries NUL-separated,
`boardrev=0xff` added when missing, padded to a word, the length token last.

The parsing is in `gui/wlanfw_parse.c`, kept free of bdk so the host can
test it: `host_tools/test_wlanfw.c`, run by CI.

### 2. CPU0 (`cpu_wifi/`, `wifi_mbox.h`)

The health check's PCIe bring-up runs on the BPMP exactly as for the Wi-Fi
pages (WL_REG_ON cold cycle included), at the payload's clocks. Then CPU0 is
released into its own image, built into the GUI only, which compiles the
health check's stub in (`cpu_stub/stub.c`, with `CPU_STUB_THEN`) for the
link training and enumeration and carries on from there, in brcmfmac's
order:

| Step | What |
| --- | --- |
| BARs | BAR0 (32 KB register window) at `0x13000000` and BAR2 (the TCM) at `0x14000000`, sized and verified; the root port's memory window widened over both; memory and bus master on |
| Chip ID | ChipCommon register 0 through BAR0 and its backplane window (config `0x80`): chip `0x4356`, its revision, AXI backplane |
| Cores | the EROM walked for ChipCommon, PCIe2, ARM CR4 and the D11 cores, with their wrappers |
| Halt, reset | CR4 halted and the D11 cores disabled; the chip's watchdog reset with ASPM off; the PCIe2 config shadow restored (PCIe2 rev <= 13); halted again; the BAR1 sizing rewrite |
| RAM | the CR4 bank registers (or the firmware's `SMAR` record) for the size; RAM at `0x180000` |
| Download | the firmware into TCM, read back (the first and last KB and every 64th word between), the NVRAM at the top of RAM |
| Boot | the reset vector at TCM 0, CR4 released, up to 5 s for the firmware's shared-RAM pointer at the top of RAM |
| Rings | the five common msgbuf rings in an uncached DMA block, TCM ring indices, the scratch buffers, host-ready |
| Ioctls | receive, event and ioctl-response buffers posted; `ver`, `cur_etheraddr`, `mpc` 0, `event_msgs` (ESCAN_RESULT), infra, UP |
| Scan | `escan` v1, active, every channel; ESCAN_RESULT events collected for up to 12 s, a BSS kept once per BSSID with its best RSSI |

Everything is polled, no interrupts. The DMA block (`0xA0400000`, 2 MiB)
is mapped Normal non-cacheable on CPU0 through an L2 table, so neither the
chip's writes nor the BPMP's reads of the mailbox need cache maintenance on
that side.

### 3. Watching

The BPMP reads the mailbox every 100 ms. CPU0 is stopped (the cluster
powergated) when it says done, when neither its heartbeat nor the stub's
has moved for 6 s, after 90 s in all, when it takes an exception, or when
the screen is left.

## Reading the result

On success the right column lists the networks, strongest first: the SSID
(or `(hidden)`, a shield for a protected one), the RSSI in dBm (green from
-60, yellow from -75, red below), the channel and band, the BSSID. Under
it: the die-level chip ID and revision (which the health check cannot
read), the firmware's own version string, its MAC, how long it took to
boot, the scan's length and how many events came in.

On failure the left column names the step it stopped at and why, and the
right shows the last lines of the firmware's own console when the firmware
got far enough to have one. What the steps tell a repair apart:

| Stopped at | Points at |
| --- | --- |
| prod.keys, SYSTEM, the NCA | the keys or the eMMC, not the chip |
| PCIe bring-up, enumeration | what the health check already reports |
| BAR0/BAR2, chip ID, cores | PCIe memory transactions to the chip, the first ones the health check never makes |
| firmware download (read back wrong) | writes reaching the chip corrupted or dropped |
| firmware starting | the chip's ARM core, its RAM, or its supplies |
| rings, first ioctls | DMA from the chip into console memory |
| scanning, no networks | the radio, the antenna, or nothing in range |

A pass is not a clean bill: one scan of an intermittent fault proves as
little as one enumeration, and the link still runs at Gen1. Run it a few
times on a suspect console.

## What it cannot tell

- It has only been checked against Linux's driver. Points most likely to
  need a hardware run to settle: whether this chip takes BAR assignments
  and backplane window writes the way the stub's notes doubt; whether the
  watchdog reset keeps the link up here as it does under Linux; whether the
  PCIe controller passes the chip's DMA to DRAM with the AFI set up as the
  stub leaves it.
- No association, no traffic: the scan is the whole test.
- The firmware HOS carries, not a known-good one: a console whose SYSTEM is
  damaged fails at step 1.
