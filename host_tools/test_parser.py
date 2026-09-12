"""Smoke test for parser.py against a saved capture.

Run:
    python test_parser.py
"""

from pathlib import Path
from parser import Parser, parse_file, classify


def test_classify_keywords() -> None:
    assert classify("MISMATCH (board reworked?)") == "err"
    assert classify("WRONG chip_id 0x1234") == "err"
    assert classify("aging (eol 2)") == "warn"
    assert classify("OK (eol 1, life A=1 B=1)") == "ok"
    assert classify("4188 mV") == "info"
    assert classify("100% of design") == "info"
    print("PASS test_classify_keywords")


def test_classify_word_boundary_only() -> None:
    """The classifier used to do plain substring match, which fired
    false positives like 'IC default 120' tripping the 'fault' keyword
    because 'default' contains 'fault'. With word-boundary matching,
    only standalone 'fault' / 'faults' should match."""
    # The exact line from the BQ24193 charger probe that was being
    # rendered red in the GUI.
    assert classify("120 C (charge-throttle cutoff, IC default 120)") == "info"
    # Other words that contain fail-keyword substrings but aren't faults.
    assert classify("default behaviour") == "info"
    # Real fail keyword still triggers.
    assert classify("BOOST fault detected") == "err"
    # Plural still triggers.
    assert classify("3 faults latched") == "err"
    print("PASS test_classify_word_boundary_only")


def test_gpio_census_neutral_descriptions() -> None:
    """GPIO/PINMUX census rows describe pin state with terms like
    'no-pull', 'n/a' (no PINMUX entry), 'tristate' / 'tri'. These are
    routine descriptive terms, not warnings, the C code never colours
    them yellow. The classifier must NOT treat them as warn."""
    # Exact rows from probe_gpio_census that were lighting up orange.
    assert classify("fn=1 no-pull drv ie=0 GPIO OE=1 OUT=1 IN=0  (5V regulator EN)") == "info"
    assert classify("pmx=(n/a)              GPIO OE=1 OUT=1 IN=0  (LCD AVDD CH2 EN)") == "info"
    assert classify("fn=0 no-pull tri ie=1 GPIO OE=0 OUT=0 IN=1  (JC-R attach detect)") == "info"
    assert classify("fn=0 pull-up drv ie=1 SPIO OE=0 OUT=0 IN=0  (UART2_RX (JC-R))") == "info"
    print("PASS test_gpio_census_neutral_descriptions")


def test_classify_negated_fail_keywords() -> None:
    """Lines like '0x00 (no SD-rail power faults)' or 'no carrier' use a
    FAIL keyword in a negated context. The classifier must recognise that
    'no <kw>' / '0 <kw>' / '(none)' flips the meaning. Without this guard
    the word 'fault' alone trips ERR even when the value is reporting
    the *absence* of faults."""
    # The exact PMIC IRQSD line that was triggering false-red in the GUI.
    assert classify("0x00 (no SD-rail power faults)") == "ok"
    # Other phrasings that should NOT be err.
    assert classify("no thermal/low-batt events") == "info"
    assert classify("0 fault, system OK") == "ok"
    # And the legit failure case must STILL trigger.
    assert classify("VBUS present but PG=0 (BQ24193 fault)") == "err"
    # 'patched' was overloaded, removed from FAIL_KEYWORDS, so the
    # AutoRCM / Patched RCM info lines aren't flagged red.
    assert classify("yes (Mariko - expected)") == "ok"
    assert classify("RCM patched in BootROM - AutoRCM has no effect") == "info"
    print("PASS test_classify_negated_fail_keywords")


def test_classify_bis_keys_states() -> None:
    """The PRODINFO probe emits three BIS-keys states. The host viewer
    has to surface them as ok / warn / warn so the tech can tell apart
    'all good', 'no keyfile', and 'wrong-console keyfile' from the row
    colour alone."""
    # All good - keys loaded AND CAL0 decoded.
    assert classify("loaded from sd:/switch/prod.keys") == "ok"
    # Loaded but the keyfile is for a different console.
    assert classify(
        "loaded but invalid for this console "
        "(prod.keys from a different unit?)") == "warn"
    # No keyfile at all - tech needs to run Lockpick first.
    assert classify("skipped (no sd:/switch/prod.keys, run Lockpick first)") \
        == "warn"
    # CAL0 mismatch tag - "invalid" hits WARN, not FAIL.
    assert classify("???? (invalid - decrypt produced gibberish)") == "warn"
    # Serial line on the two failure paths.
    assert classify(
        "skipped (gibberish, prod.keys not for this device)") == "warn"
    assert classify(
        "skipped (encrypted, run Lockpick to populate"
        " sd:/switch/prod.keys)") == "warn"
    print("PASS test_classify_bis_keys_states")


def test_classify_verdict_summary() -> None:
    """The verdict summary line ('N pass, M warn, K fail') used to be
    coloured red by a naive substring match on the literal 'fail' --
    even when the count was 0. Make sure the counts drive the colour."""
    assert classify("8 pass, 0 warn, 0 fail") == "ok"
    assert classify("7 pass, 1 warn, 0 fail") == "warn"
    assert classify("5 pass, 0 warn, 2 fail") == "err"
    assert classify("0 pass, 0 warn, 1 fail") == "err"
    # The classifier should still flag a free-form 'fail' that ISN'T part
    # of the counter summary.
    assert classify("smoke test FAILED on rev D boards") == "err"
    print("PASS test_classify_verdict_summary")


def _rows_by_section(page):
    return {
        section.name: {row.key: row for row in section.rows}
        for section in page.sections
    }


def test_runtime_uart_messages() -> None:
    p = Parser()
    p.feed_chunk(
        "[boot] display_init ok\r\n"
        "[boot] launcher enter\r\n"
        "[save] backup/ABCDEF/hwtest.txt (27123 bytes)\r\n"
    )
    assert "Runtime" in p.report.pages
    sections = _rows_by_section(p.report.pages["Runtime"])
    assert sections["Boot"]["State"].value == "launcher enter"
    assert sections["Boot"]["State"].severity == "info"
    assert sections["Save"]["Report"].value.endswith("(27123 bytes)")
    assert sections["Save"]["Report"].severity == "ok"

    # Every other [save] message emitted by report.c is a failure path.
    p.feed("[save] f_open failed (4): backup/ABCDEF/hwtest.txt")
    sections = _rows_by_section(p.report.pages["Runtime"])
    assert sections["Save"]["Report"].severity == "err"
    print("PASS test_runtime_uart_messages")


def test_ram_uart_success_protocol() -> None:
    """Exercise every stable success-path shape emitted by ramtest_main.c."""
    p = Parser()
    p.feed_chunk(
        "[ram] mode=advanced patterns=7 mask=7F\r\n"
        "Platform      : Erista/T210\r\n"
        "DRAM size     : 4096 MiB\r\n"
        "DRAM ID       : FF\r\n"
        "Target        : 1600 MHz\r\n"
        "Training...\r\n"
        "Training      : PASS\r\n"
        "DRAM ID       : 00\r\n"
        "SMMU window   : PASS\r\n"
        "walking data bits: PASS\r\n"
        "[ram] write address pattern 0% errors=0\r\n"
        "[ram] write address pattern 40% errors=0\r\n"
        "[ram] verify address pattern 100% errors=0\r\n"
        "[ram] pass=address pattern result=PASS new_errors=0 seconds=31\r\n"
        "DRAM result : PASS\r\n"
        "platform    : Erista/T210\r\n"
        "mode        : advanced\r\n"
        "DRAM size   : 4096 MiB\r\n"
        "scope/pass  : 1020 / 1024 regions (4080 MiB)\r\n"
        "passes done : 7 / 7\r\n"
        "errors seen : 0\r\n"
        "elapsed     : 221 s\r\n"
        "MTC flags   : 00000000\r\n"
    )
    assert p.ram_active
    assert "RAM test" in p.report.pages
    sections = _rows_by_section(p.report.pages["RAM test"])
    assert sections["Run"]["Mode"].value == "advanced"
    assert sections["Run"]["Patterns"].value == "7"
    assert sections["Run"]["Pattern mask"].value == "0x7F"
    assert sections["Training / setup"]["Training"].severity == "ok"
    assert sections["Training / setup"]["walking data bits"].severity == "ok"
    # Periodic progress must be a live snapshot, not 3 appended copies.
    assert sections["Live progress"]["Phase"].value == "verify"
    assert sections["Live progress"]["Progress"].value == "100%"
    assert sections["Live progress"]["Errors"].severity == "ok"
    assert sections["Result"]["DRAM result"].severity == "ok"
    assert sections["Result"]["errors seen"].severity == "ok"
    assert sections["Result"]["MTC flags"].severity == "ok"
    passes = next(s for s in p.report.pages["RAM test"].sections
                  if s.name == "Passes")
    assert len(passes.rows) == 1 and passes.rows[0].severity == "ok"
    print("PASS test_ram_uart_success_protocol")


def test_ram_uart_abort_and_error_protocol() -> None:
    p = Parser()
    p.feed_chunk(
        "[ram] mode=custom patterns=3 mask=43\n"
        "[ram] verify PRBS + retention 60% errors=3\n"
        "[ram] pass=address pattern result=PASS new_errors=0 seconds=30\n"
        "DRAM result : ABORTED\n"
        "platform    : Mariko/T210B01\n"
        "mode        : custom\n"
        "scope/pass  : 1020 / 1024 regions (4080 MiB)\n"
        "passes done : 1 / 3\n"
        "stopped on  : pass 2/3, PRBS + retention, verify\n"
        "phase prog  : 123 / 1020 regions (492 MiB)\n"
        "errors seen : 3\n"
        "MTC flags   : 00000002\n"
        "bad @ 0000000080401000 exp DEADBEEF got DEADAEEF (PRBS + retention)\n"
        "+ 9 additional failing words\n"
    )
    sections = _rows_by_section(p.report.pages["RAM test"])
    assert sections["Result"]["DRAM result"].severity == "warn"
    assert sections["Result"]["errors seen"].severity == "err"
    assert sections["Result"]["MTC flags"].severity == "err"
    assert sections["Live progress"]["Errors"].severity == "err"
    assert sections["Errors"]["0x0000000080401000"].severity == "err"
    assert sections["Errors"]["Additional failing words"].value == "9"
    print("PASS test_ram_uart_abort_and_error_protocol")


def test_ram_rerun_replaces_structured_state() -> None:
    p = Parser()
    p.feed_chunk(
        "[ram] mode=advanced patterns=7 mask=7F\n"
        "[ram] pass=address pattern result=FAIL new_errors=2 seconds=10\n"
        "DRAM result : FAIL\n"
        "errors seen : 2\n"
        "[ram] mode=quick patterns=1 mask=01\n"
        "[ram] write address pattern 10% errors=0\n"
    )
    page = p.report.pages["RAM test"]
    sections = _rows_by_section(page)
    assert sections["Run"]["Mode"].value == "quick"
    assert sections["Run"]["Patterns"].value == "1"
    assert "Passes" not in sections and "Result" not in sections
    assert sections["Live progress"]["Errors"].value == "0"
    # Raw history deliberately keeps both runs for forensic/replay use.
    assert sum(1 for line in p.report.raw_lines if line.startswith("[ram] mode=")) == 2
    print("PASS test_ram_rerun_replaces_structured_state")


def test_ram_training_failure_and_pre_mode_messages() -> None:
    p = Parser()
    p.feed_chunk(
        "[ram] mode=quick patterns=1 mask=01\n"
        "1600 MHz training: FAIL\n"
        "DRAM ID       : 00\n"
        "error flags   : 00000004\n"
        "training mask : 00000010\n"
        " - memory PLL did not lock\n"
        "Power-cycle before trusting memory state after a HW timeout.\n"
    )
    sections = _rows_by_section(p.report.pages["RAM test"])
    setup = sections["Training / setup"]
    assert setup["1600 MHz training"].severity == "err"
    assert setup["error flags"].severity == "err"
    assert setup["training mask"].severity == "err"
    assert setup["Recovery"].severity == "warn"

    # Unsupported-silicon output occurs before the [ram] mode marker.
    q = Parser()
    q.feed("Unsupported Tegra HIDREV 12345678 (major 9); no RAM training will run.")
    assert q.ram_active and "RAM test" in q.report.pages
    qrows = _rows_by_section(q.report.pages["RAM test"])
    assert qrows["Training / setup"]["Platform"].severity == "err"
    print("PASS test_ram_training_failure_and_pre_mode_messages")


def test_section_state_machine() -> None:
    p = Parser()
    p.feed_chunk("""
=== hwtest full UART dump ===

--- SoC ---
[SoC]
  HIDREV       : 0x00012127
  Chip ID      : 0x21 (T210B01 Mariko)
  Major.Minor  : 2.01

--- Fuses ---
[Fuses - identity]
  PRODUCTION   : 0x00000001
  HW state     : Prod
""")
    rep = p.report
    assert "SoC" in rep.pages
    assert "Fuses" in rep.pages
    soc = rep.pages["SoC"]
    assert len(soc.sections) == 1
    assert soc.sections[0].name == "SoC"
    assert len(soc.sections[0].rows) == 3
    assert soc.sections[0].rows[0].key == "HIDREV"
    assert soc.sections[0].rows[0].value == "0x00012127"
    print("PASS test_section_state_machine")


def test_indented_separator_not_a_page() -> None:
    """Internal `  --- decoded ---` separators inside a page must NOT
    be parsed as a page boundary, they have leading whitespace."""
    p = Parser()
    p.feed_chunk("""
--- Clocks ---
[Clocks]
  PLLP_BASE    : 0x48115408
  --- decoded ---
  OSC          : 38.400 MHz
""")
    rep = p.report
    assert "Clocks" in rep.pages
    assert "decoded" not in rep.pages
    # The separator becomes a free-form note in the section.
    sec = rep.pages["Clocks"].sections[0]
    assert any("decoded" in n for n in sec.notes)
    print("PASS test_indented_separator_not_a_page")


def test_partial_chunk_buffering() -> None:
    """Live serial reads arrive in arbitrary slices. The parser must stitch
    a line back together when it spans multiple feed_chunk calls; otherwise
    `manfid       : 0x03\\n` arriving as ("manfid       :", " 0x03\\n") creates
    a row with empty value + an orphan note line."""
    p = Parser()
    p.feed_chunk("--- Storage ---\n[SD card (SDMMC1)]\n  manfid       :")
    # Mid-row split. With the bug, this would already record manfid=""
    sec = p.report.pages.get("Storage")
    if sec and sec.sections:
        assert all(r.value != "" for r in sec.sections[0].rows), \
            "premature row emitted from partial chunk"
    p.feed_chunk(" 0x03\n  serial       : 0xF08BDF22\n")
    sec = p.report.pages["Storage"].sections[0]
    rows = {r.key: r.value for r in sec.rows}
    assert rows.get("manfid") == "0x03", f"manfid stitched wrong: {rows!r}"
    assert rows.get("serial") == "0xF08BDF22", f"serial stitched wrong: {rows!r}"
    print("PASS test_partial_chunk_buffering")


def test_page_reemit_replaces_content() -> None:
    """Refresh semantics: when a probe re-emits its `[section]` header
    + rows, the section's old rows are REPLACED with the fresh data.
    The page itself is not cleared on `--- name ---` re-emit, because
    multiple LCD pages with the same display name can each refresh
    independently without wiping the others."""
    p = Parser()
    # Initial emission of two sections under one page.
    p.feed_chunk(
        "--- Power & charging ---\n"
        "[MAX17050]\n"
        "  SOC          : 50.0 %\n"
        "  VCELL        : 3800 mV\n"
        "--- Power & charging ---\n"
        "[BQ24193]\n"
        "  VBUS         : present\n"
    )
    page = p.report.pages["Power & charging"]
    assert len(page.sections) == 2, \
        f"expected 2 sections, got {[s.name for s in page.sections]}"

    # Only the [MAX17050] section refreshes. [BQ24193] data must persist.
    p.feed_chunk(
        "--- Power & charging ---\n"
        "[MAX17050]\n"
        "  SOC          : 51.5 %\n"
        "  VCELL        : 3810 mV\n"
        "  Temp         : 32.0 C\n"
    )
    page = p.report.pages["Power & charging"]
    sections = {s.name: s for s in page.sections}
    assert "MAX17050" in sections and "BQ24193" in sections, \
        "BQ24193 section was wiped by Power & charging boundary re-emit"

    fresh = {r.key: r.value for r in sections["MAX17050"].rows}
    assert fresh == {"SOC": "51.5 %", "VCELL": "3810 mV", "Temp": "32.0 C"}, \
        f"MAX17050 not refreshed cleanly: {fresh!r}"
    bq = {r.key: r.value for r in sections["BQ24193"].rows}
    assert bq == {"VBUS": "present"}, f"BQ24193 corrupted: {bq!r}"

    assert p.report.page_order == ["Power & charging"]
    print("PASS test_page_reemit_replaces_content")


def test_real_capture() -> None:
    """Parse a recent emulator capture and confirm the grouped layout
    is intact: ~10 logical pages (down from 28+ in the legacy layout),
    with all storage probes folded into a single Storage page."""
    sample = Path(__file__).parent / "sample_emu_capture.txt"
    if not sample.exists():
        print(f"SKIP test_real_capture (no {sample})")
        return
    rep = parse_file(str(sample))

    # Required pages in the new grouped layout.
    expected_pages = ["SoC", "Fuses", "Power & charging",
                      "Memory & clocks", "Storage", "Display",
                      "Inputs", "Raw state", "Verdict"]
    for name in expected_pages:
        assert name in rep.pages, \
            f"expected page missing from grouped layout: {name}"

    # Storage page must contain the probes that used to be separate pages.
    storage = rep.pages["Storage"]
    section_names = [s.name for s in storage.sections]
    must_have = ("SD card", "eMMC", "partitions", "GPT", "BOOT0", "AutoRCM")
    for needle in must_have:
        assert any(needle in n for n in section_names), (
            f"Storage page missing section matching '{needle}'; "
            f"got: {section_names}")

    # Verdict page should have at least a few `ok`-classified rows after
    # the negation/SoC-aware fixes.
    verdict = rep.pages["Verdict"]
    ok_count = sum(1 for s in verdict.sections for r in s.rows
                   if r.severity == "ok")
    assert ok_count >= 4, f"expected verdict ok rows, got {ok_count}"
    print(f"PASS test_real_capture ({len(rep.page_order)} pages, "
          f"{len(storage.sections)} storage sections, "
          f"{ok_count} verdict ok rows)")


if __name__ == "__main__":
    test_classify_keywords()
    test_classify_word_boundary_only()
    test_gpio_census_neutral_descriptions()
    test_classify_negated_fail_keywords()
    test_classify_bis_keys_states()
    test_classify_verdict_summary()
    test_runtime_uart_messages()
    test_ram_uart_success_protocol()
    test_ram_uart_abort_and_error_protocol()
    test_ram_rerun_replaces_structured_state()
    test_ram_training_failure_and_pre_mode_messages()
    test_section_state_machine()
    test_indented_separator_not_a_page()
    test_partial_chunk_buffering()
    test_page_reemit_replaces_content()
    test_real_capture()
    print("all OK")
