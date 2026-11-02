#!/usr/bin/env python3
"""Pack what goes on the SD card into one zip, laid out as it goes there.

Unzipping the result at the root of the card gives:

    bootloader/payloads/hwtest.bin       the payload (hekate's Payloads menu;
                                         inject it over RCM as usual otherwise)
    bootloader/payloads/hwtest_ram.bin   the RAM test, where hwtest looks first
    hwtest/gui.bin                       the GUI stage hwtest.bin hands over to
    hwtest/libsys_minerva.bso            hekate's DRAM module, for faster drawing

Usage: mksdzip.py <out.zip>   (run from the repository root after
`make`, `make ram` and `make gui`; `make sdzip` does all of that)
"""
import os
import sys
import zipfile

FILES = [
    ("build/hwtest.bin",              "bootloader/payloads/hwtest.bin"),
    ("build-ram/hwtest_ram.bin",      "bootloader/payloads/hwtest_ram.bin"),
    ("build-gui/gui.bin",             "hwtest/gui.bin"),
    ("build-gui/libsys_minerva.bso",  "hwtest/libsys_minerva.bso"),
]


def main(out):
    missing = [src for src, _ in FILES if not os.path.isfile(src)]
    if missing:
        sys.exit("mksdzip: not built yet: " + ", ".join(missing))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for src, dst in FILES:
            z.write(src, dst)
            print("  %-36s %8d bytes" % (dst, os.path.getsize(src)))
    print("   built %s (%d bytes) - unzip at the root of the SD card"
          % (out, os.path.getsize(out)))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mksdzip.py <out.zip>")
    main(sys.argv[1])
