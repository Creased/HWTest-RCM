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
    test_section_state_machine()
    test_indented_separator_not_a_page()
    test_partial_chunk_buffering()
    test_page_reemit_replaces_content()
    test_real_capture()
    print("all OK")
