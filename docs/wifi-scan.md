# Wi-Fi Scan

A GUI tool, **Wi-Fi Scan** along the bottom of Home, next to Custom Check
and Last Results. It boots the CYW4356 with the firmware the console's own
HOS carries, asks the chip to scan, and lists the networks it heard. The
health check stops at enumeration, the chip's PCIe front end answering a
few config reads (see [wireless](wireless.md)). A scan goes through what
the orange screen points at: a firmware download of several hundred KB over
PCIe, the chip's ARM core running, the chip DMAing into console memory, the
radio itself. A chip that only fails under load gets a chance to fail here.

**Status: run on a console.** It scans, joins a WPA2-PSK network, runs
the four-way handshake itself, takes an address from DHCP and reaches a
host on the internet. Nothing here sets a verdict key or goes in the
report.

## Joining a network

The scan proves the receive path. Associating adds the transmitter, the
rates and - on a private network - the handshake; an address and an echo
off the internet then prove the chip carries data.

The network comes from a tap on the list (a keypad takes the passphrase)
or from `wifi_ssid` / `wifi_psk` in `hwtest/hwtest.ini`. An open network
needs nothing else: `WLC_SET_SSID`, then the `E_LINK` event, and the page
says `associated (open)`.

That pair may be repeated, up to eight times, and the file then reads as a
list of the networks this console is tested against: the first is the one
a scan joins on its own, and picking any of the others in the list the
scan found opens the keypad with its passphrase already in the field and
Connect selected, so a network tested against every day is one press
rather than fourteen on an on-screen keypad. The passphrase is shown and
can be edited, and the step log says where it came from. Nothing is ever
written back to the file - a passphrase typed on the keypad lives until
the screen is left.

**Radio Off** puts back everything the run powered. It is in the header
only while there is something to put back - once a run has powered the
chip, until it is off again - so a screen that has run nothing shows
Scan and Close alone.

CPU0 first, since it is the one on the PCIe bus: `ccplex_powergate_cpu0()`
drops the three partitions and switches the CPU regulator off. Then
WL_REG_ON and the enable go low and the WLAN section is in reset. Then
what neither of those covers - PLLX and the MSELECT clock, which bdk
brings up for the cluster and never puts back, and the PCIe root port,
its clocks and its partition.

A scan otherwise leaves all of it running, and a join leaves the radio
associated and transmitting. On a console at rest that measures about
40 mA off the battery, and `!WIFI OFF` reports the figure: three runs
here read -45, -33 and -38 mA with the chip up, and 0 to -1 mA once it
was put back, which is the idle reading. Leaving the screen does the same
thing on its own if the chip was brought up while it was open.

PLLE and the lane mux are deliberately left alone: the UPHY they drive is
shared with USB, and the console is on it. Bluetooth is untouched as well
- the shared CBUCK stays up, as it does during the cold cycle. The next
scan brings the chip, the cluster and the root port up from cold; the
bring-up already powers the PCIe partition up from whatever state it
finds. The button is also how a run that is going nowhere is stopped.

A join draws its own card over the list, the five stages it goes
through one per line:

    Starting the chip        the firmware into the chip and the radio up
    Associating              the BSSID it settled on, and how long
    Key handshake            key frames in and out; skipped on an open one
    Getting an address       DHCP: the address, and how long the lease took
    Reaching the internet    ICMP echoes to 8.8.8.8, replies and best time

Each is pending, running, done or failed, with a bar over them; the run
stops at the first one that fails and the card stays up saying which,
until `Back to the list`. The last two are what tell a chip that
associated from a chip that carries data, and they are the ones a user
reads as "it works".

No escan runs in front of a join: the name came off a list a scan had
already drawn, and `WLC_SET_SSID` scans for the BSSID itself.

### The address and the echo (`cpu_wifi/net.c`)

Not a stack: DHCP (discover, offer, request, ack), ARP for the gateway,
then four ICMP echo requests, one outstanding at a time, four tries each
with a 1.2 s gap. No fragmentation, no TCP, no retransmission beyond
that, and addresses are kept in host order. Enough to prove a path, not
to use one.

It is a pure state machine over two calls - a frame that arrived, and
whatever is due to be sent - with no idea that msgbuf, PCIe or the
mailbox exist, so the whole of it runs on the host against a fake
network in `host_tools/test_net.c`, which CI runs. The emulator's AP
answers DHCP, ARP and echoes too (`RCM-Emulator/t210/wlan4356.cpp`), so
the screen can be watched going green end to end without an AP in the
room.

WPA2 is the chip's own supplicant's job on most Broadcom parts, through
`WLC_SET_WSEC_PMK`. Nintendo's firmware has none - it answers
`BCME_UNSUPPORTED` (-23), because HOS runs a supplicant in software - so
the payload does the four-way itself (`cpu_wifi/wpa.c`, and the handshake
in `cpu_wifi/wifi.c`):

0. `sup_wpa` is set to 1 on the way in, so a part that does have a
   supplicant takes the passphrase, and back to 0 where the PMK was
   refused. On this console's firmware all three of `sup_wpa`,
   `WLC_SET_WSEC_PMK` and the internal supplicant answer
   `BCME_UNSUPPORTED`: it has no supplicant at all, which is why HOS
   runs one in software and why this does too.
1. PBKDF2-HMAC-SHA1, 4096 iterations, gives the PMK from the passphrase
   and the SSID. SHA-1 in software: the security engine is the BPMP's and
   this runs on CPU0, where it costs a few tens of milliseconds.
2. Message 1 carries the AP's nonce. With the BSSID from the link event
   and the chip's own address, the PMK expands into KCK | KEK | TK.
3. Message 2 goes back with the station's nonce, its RSN element and a
   MIC over the frame - which means transmitting, and transmitting over
   msgbuf means creating a flow ring for the AP first. The RSN element
   is the one the firmware actually put in the association request,
   read back with `assoc_info` and `assoc_req_ies`, not one built here:
   the AP compares it byte for byte with what it was sent and answers a
   mismatch with silence, which reads exactly like a wrong passphrase.
4. Message 3's MIC is checked against the same key, message 4 answers it,
   and the TK goes into the chip with `WLC_SET_KEY`.

The group key in message 3 arrives wrapped and is left alone: nothing
here listens to broadcast traffic, and unwrapping it would want an AES
the CPU0 image does not carry.

An AP that dislikes message 2's MIC says nothing at all, so a wrong
passphrase reads as `no answer to msg 2: bad key?` rather than anything
more definite. That is what the air looks like.

`Key frames  N in, M out, K data frames up` under the chip's details
counts what the receive path carried while the handshake ran, for the
runs where it did not: no key frames with data frames arriving is the
firmware holding them back, nothing at all is a receive path that never
delivered. On a failure the debug console also gets how many receive
buffers the chip took and where each ring stood when the run ended.

### Four things that had to be right

The join failed on a console for a while, and each of these looked like
a wrong passphrase from the outside. They are written down because the
next person to port this will hit them in the same order.

**The key frames do not come up the receive ring.** They arrive as
`WLC_E_EAPOL_MSG` events (type 25), which has to be in the event mask or
nothing arrives at all. This was measured rather than guessed: the chip
took all 32 receive buffers posted to it (`rxpost 32/32`) and produced
not one completion (`rx 0/0`), while the control ring carried 61
messages - so the rings, the DMA and the index arrays were all fine and
the firmware simply had nothing to put there. It fits: a firmware with
no supplicant of its own still has to get EAPOL to its host somehow.

**Message 2's RSN element must be the firmware's, not ours.** See step 3
above.

**The key descriptor version is message 1's, not a constant.** Messages
2 and 4 echo the low three bits of the key info word the AP sent.

**`brcmf_wsec_key_le` is not packed the way it reads.** The peer's
address is at offset 156, after `iv_initialized`, the receive IV and two
pad words - not 152 - and `flags` is 0 for a pairwise key;
`BRCMF_PRIMARY_KEY` is for the group key, the one with no peer. With the
address written into the padding the firmware answered `BCME_BADARG`.

### And one for the address

The DHCP request leaves the broadcast flag clear. A broadcast answer is
encrypted with the group key, and this station never installs one - the
group key arrives wrapped in message 3 and is left alone - so a
broadcast offer is a frame the chip throws away before the host sees it.
Clear, the server unicasts under the pairwise key, and the lease
arrives.

## The firmware, without the keys

The blobs normally come out of the console's own HOS, which needs
`sd:/switch/prod.keys` and a readable eMMC. Where that is not possible -
a console whose eMMC will not read, a bench without that console's keys,
or the emulator, which has no HOS at all - the card can carry them
instead:

    hwtest/wlan_fw.bin     the firmware, as nx-fwextract writes it
    hwtest/wlan_nvram.txt  the NVRAM text

Both are needed: the chip does not start without its NVRAM, so a card
with the firmware alone stops there and says so. With both, they are used
as they are and the eMMC is not touched; the Steps column says `firmware from the SD card` so a run is
never ambiguous about where its firmware came from.

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

- The group key is never unwrapped, so nothing broadcast or multicast
  can be read: no mDNS, no broadcast DHCP, no IPv6 router advertisements.
  Everything here is unicast under the pairwise key by design.
- The echo proves the path to one host, once. It is not a throughput
  test and it says nothing about the link's rate.
- The firmware HOS carries, not a known-good one: a console whose SYSTEM is
  damaged fails at step 1.
