"""
Parser for the hwtest UART output stream.

The payload prints a stable, line-oriented format:

    === hwtest full UART dump ===

    --- <Page name> ---
    [<Section name>]
      <Key>        : <Value>
      <Key>        : <Value>
    [<Section name>]
      ...
    --- <next page> ---
    ...
    === end of dump - pager active on LCD ===

Pager pages later print as:

    hwtest  -  page 3/27  -  Battery
    VOL+ / n next | ...
    =============================================================
    <page body>

This module turns a stream of bytes/lines into a `Report` object:

    Report
      .pages: dict[name, Page]
      .raw_lines: list[str]
      .latest_page: name of the most recent --- page ---

    Page
      .name: str
      .sections: list[Section]
      .raw: list[str]   # every line in this page, unprocessed

    Section
      .name: str
      .rows: list[Row]    # parsed `key : value` lines
      .notes: list[str]   # other indented continuation lines

    Row
      .key: str
      .value: str
      .severity: 'ok' | 'warn' | 'err' | 'info'

The parser is incremental: feed it one line at a time via
`Parser.feed(line)`. A no-op for empty lines and the banner.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional


# Match `--- Page Name ---`. Page boundaries are emitted with NO leading
# whitespace (run_all_probes -> log_color "\n--- %s ---\n"). Internal
# separators (e.g. `  --- decoded ---` inside the Clocks page) have a
# leading indent and must not be confused for page breaks.
_RE_PAGE = re.compile(r"^---\s+(.+?)\s+---\s*$")

# Match `[Section name]`, no leading whitespace, square-bracket wrapped.
_RE_SECTION = re.compile(r"^\[([^\]]+)\]\s*$")

# Match `  KEY      : VALUE` (>=2 spaces for the indent, key has no colon).
# We allow letters, digits, common punctuation in the key and require at
# least one space before the colon so we don't mis-classify free text.
_RE_ROW = re.compile(r"^  ([^:]{1,30}?)\s*:\s*(.*)$")

# Markers that signal severity. We match against lowercased text. The colour
# information that hwtest prints over the LCD is NOT in the UART stream
# (UART only carries plain ASCII), so we re-derive it heuristically from
# value text. The rules mirror the same pass/warn/fail ramps probe_verdict
# uses internally.
_FAIL_KEYWORDS = (
    "mismatch", "wrong", "fault", "stuck", "missing", "fail",
    "broken", "shorted", "unpowered", "abnormal",
    "hard fail", "no carrier", "framing", "overflow",
)

_WARN_KEYWORDS = (
    # Strictly diagnostic words. "no-pull", "n/a", "tristate" used to be
    # here but they're routine descriptive terms in GPIO/PINMUX dumps
    # (e.g. PA5 "fn=1 no-pull drv ie=0 ..."), the C code never colours
    # them yellow, and matching them here turned every GPIO census row
    # with no-pull or n/a orange in the host viewer.
    "warn", "advisory", "aging", "suspect", "unknown",
    "not present", "not read", "skipped",
    "downgrade", "floating", "unverified", "cannot",
    # PRODINFO / BIS-keys probe failure modes. "invalid" covers
    # "loaded but invalid", "gibberish" + "encrypted" cover the two
    # CAL0-decoded-wrong cases.
    "invalid", "gibberish", "encrypted",
)

_OK_KEYWORDS = (
    "ok", "match", "present", "enabled", "attached", "normal",
    "consistent", "healthy", "exploitable", "responded", "active",
    "alive", "connected", "done", "yes (exploitable)", "expected",
    # "loaded" greens the BIS-keys success line ("loaded from sd:/...").
    # Falls below the WARN check so "loaded but invalid" still surfaces
    # yellow.
    "loaded",
    # "pass" comes from the new macro-grouped verdict ("Charger: pass").
    "pass",
)

# Counter-summary line emitted by probe_verdict: '8 pass, 0 warn, 0 fail'.
# A naive substring search hits the literal 'fail' / 'warn' words and
# colours the line red even when the counts are zero. Detect this shape
# explicitly and pick the colour from the actual numbers instead.
_RE_VERDICT_COUNTS = re.compile(
    r"(\d+)\s*pass\D+(\d+)\s*warn\D+(\d+)\s*fail", re.IGNORECASE)

# A FAIL keyword preceded by a negation/zero-count token is NOT a failure.
# e.g. "no SD-rail power faults" -> ok, "0 fail" -> ok, "(none)" -> info.
# The qualifier may sit immediately before the keyword or be wrapped in
# parens: "0x00 (no power faults)". Word-boundary on both sides; the
# keyword can carry a trailing 's' so 'faults' / 'fails' both match.
_RE_NEGATED_FAIL = re.compile(
    r"\b(?:no|none|zero|without|0)\s+[\w\-/ ]*?\b("
    + "|".join(re.escape(k) for k in _FAIL_KEYWORDS)
    + r")s?\b",
    re.IGNORECASE)


def _kw_match(text: str, keywords: tuple) -> bool:
    """Word-boundary match. Substring matching used to false-positive on
    things like 'default' tripping the 'fault' keyword. We require a
    word boundary BEFORE the keyword (so 'default' doesn't match 'fault'),
    and allow trailing word characters so common derived forms still
    match, 'faults', 'failed', 'failing', 'mismatched' etc. all hit
    their base keyword."""
    pat = (r"\b(?:"
           + "|".join(re.escape(k) for k in keywords)
           + r")\w*\b")
    return re.search(pat, text, re.IGNORECASE) is not None


def classify(value: str) -> str:
    """Heuristic severity from a raw VALUE string."""
    # Counter summaries first: 'N pass, M warn, K fail', pick by counts,
    # not by the literal words. '0 fail' must NOT trigger the FAIL keyword.
    m = _RE_VERDICT_COUNTS.search(value)
    if m:
        _, warn_n, fail_n = (int(g) for g in m.groups())
        if fail_n > 0:
            return "err"
        if warn_n > 0:
            return "warn"
        return "ok"

    # If the value reports the *absence* of a fail condition ("no faults",
    # "0 fail", "no carrier"), classify as ok directly, the row is good
    # news. Without this short-circuit, the negated text would get stripped
    # and fall through to "info" (grey), losing the visual signal that
    # things are healthy.
    negated = _RE_NEGATED_FAIL.search(value)
    if negated:
        # If anything OUTSIDE the negated phrase still trips a fail
        # keyword (e.g. "no SD fault, but BOOST fault"), err wins.
        v_for_fail = _RE_NEGATED_FAIL.sub("", value)
        if _kw_match(v_for_fail, _FAIL_KEYWORDS):
            return "err"
        return "ok"

    if _kw_match(value, _FAIL_KEYWORDS):
        return "err"
    if _kw_match(value, _WARN_KEYWORDS):
        return "warn"
    if _kw_match(value, _OK_KEYWORDS):
        return "ok"
    return "info"


@dataclass
class Row:
    key: str
    value: str
    severity: str = "info"
    # True for sub-bullet rows whose key was emitted with a leading '- '
    # in the C source, e.g. the per-bit breakdown of PMIC IRQSD or
    # NVERC. The host viewer indents these so they render as a nested
    # list under the parent register row.
    indent: int = 0


@dataclass
class Section:
    name: str
    rows: list[Row] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


@dataclass
class Page:
    name: str
    sections: list[Section] = field(default_factory=list)
    raw: list[str] = field(default_factory=list)

    def section(self, name: str) -> Section:
        for s in self.sections:
            if s.name == name:
                return s
        s = Section(name=name)
        self.sections.append(s)
        return s


@dataclass
class Report:
    pages: dict[str, Page] = field(default_factory=dict)
    page_order: list[str] = field(default_factory=list)
    raw_lines: list[str] = field(default_factory=list)
    latest_page: Optional[str] = None

    def page(self, name: str) -> Page:
        if name not in self.pages:
            self.pages[name] = Page(name=name)
            self.page_order.append(name)
        return self.pages[name]

    def reset(self) -> None:
        self.pages.clear()
        self.page_order.clear()
        # Keep raw_lines so the user can still see the full transcript
        # across reset boundaries. The viewer can offer an explicit "clear"
        # action if they want a totally fresh start.


class Parser:
    """Line-by-line state machine."""

    def __init__(self) -> None:
        self.report = Report()
        self._cur_page: Optional[Page] = None
        self._cur_section: Optional[Section] = None
        # Pending tail of a chunk that ended mid-line. Critical for live
        # serial input, the OS RX buffer is read in arbitrary slices, so
        # `manfid       : 0x03\n` may arrive as ("manfid       :", " 0x03\n")
        # across two reads. Without this buffer the parser would emit a row
        # with empty value plus an orphan note line, badly corrupting the
        # GUI's section table.
        self._line_buf = ""

    def feed_chunk(self, data: str) -> None:
        # Normalise line endings then split, but ALWAYS keep the last frag
        # (the unterminated tail) for the next chunk, even if the chunk
        # happens to end on a newline, str.split leaves an empty string at
        # the tail in that case, which is what we want (consumed entirely).
        data = self._line_buf + data.replace("\r\n", "\n").replace("\r", "\n")
        parts = data.split("\n")
        self._line_buf = parts.pop()      # incomplete trailing line, if any
        for raw in parts:
            self.feed(raw)

    @property
    def pending_line(self) -> str:
        """The unterminated tail currently buffered. The raw-log view
        in the GUI surfaces this so the user sees in-flight bytes
        instead of an empty pane while waiting for a newline."""
        return self._line_buf

    def flush(self) -> None:
        """Emit any partial line still buffered. Call at end-of-stream."""
        if self._line_buf:
            self.feed(self._line_buf)
            self._line_buf = ""

    def feed(self, line: str) -> None:
        # Strip trailing whitespace but preserve indent on the left.
        line = line.rstrip()
        self.report.raw_lines.append(line)

        m = _RE_PAGE.match(line)
        if m:
            name = m.group(1)
            # `--- name ---` boundary. The C side emits this for every
            # LCD page render, when a multi-LCD-page domain (Storage
            # has 7 pages, Power has 7, Diagnostics has 4) emits its
            # boundary, only ONE of its sections refreshes. Clearing all
            # sections here would wipe data the host already has from
            # other LCD pages in the same group.
            #
            # Refresh semantics moved down to the section level: see
            # `[section]` handling below where rows are cleared on
            # re-emit. The page boundary just selects/creates the page.
            if name not in self.report.pages:
                self._cur_page = self.report.page(name)
            else:
                self._cur_page = self.report.pages[name]
            self.report.latest_page = name
            self._cur_section = None
            self._cur_page.raw.append(line)
            return

        # `=== ... ===` overall-stream banners. Skip but record raw.
        if line.startswith("===") and line.endswith("==="):
            return

        if self._cur_page is None:
            # Pre-page free text (boot banner). Stash under a synthetic page.
            self._cur_page = self.report.page("Boot banner")
            self.report.latest_page = "Boot banner"

        self._cur_page.raw.append(line)

        m = _RE_SECTION.match(line)
        if m:
            sec_name = m.group(1)
            # Section re-emit replaces rows: when the C-side pager
            # re-runs a probe (refresh / re-navigation), the same
            # `[section]` header arrives again followed by the fresh
            # rows. Clear the section's old rows so the GUI shows the
            # latest snapshot, not appended duplicates.
            existing = next((s for s in self._cur_page.sections
                             if s.name == sec_name), None)
            if existing is not None:
                existing.rows.clear()
                existing.notes.clear()
                self._cur_section = existing
            else:
                self._cur_section = self._cur_page.section(sec_name)
            return

        m = _RE_ROW.match(line)
        if m:
            if self._cur_section is None:
                # No section yet (e.g. raw rows directly under a page name).
                self._cur_section = self._cur_page.section("(unnamed)")
            key = m.group(1).strip()
            value = m.group(2).strip()
            sev = classify(value)
            # Sub-bullet rows carry a leading '- ' in the key text, the C
            # source emits per-bit register breakdowns that way (see PMIC
            # IRQSD / NVERC / INTLBT in probe_reset). Strip the dash and
            # mark the row indented so the host viewer renders it nested
            # under its parent register row.
            indent = 0
            if key.startswith("- "):
                key = key[2:].strip()
                indent = 1
            self._cur_section.rows.append(
                Row(key=key, value=value, severity=sev, indent=indent))
            return

        # Free-form continuation/note line. Indented further or no colon.
        if line.strip():
            if self._cur_section is None:
                self._cur_section = self._cur_page.section("(unnamed)")
            self._cur_section.notes.append(line)


# Convenience: read a saved capture file and return the Report.
def parse_file(path: str) -> Report:
    p = Parser()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            p.feed(line)
    return p.report


if __name__ == "__main__":
    # Quick smoke when invoked directly.
    import sys
    if len(sys.argv) < 2:
        print("Usage: python parser.py <hwtest-uart-capture.txt>")
        sys.exit(2)
    rep = parse_file(sys.argv[1])
    for name in rep.page_order:
        page = rep.pages[name]
        print(f"=== {name} ({len(page.sections)} section(s)) ===")
        for sec in page.sections:
            print(f"  [{sec.name}] ({len(sec.rows)} row(s))")
            for row in sec.rows[:5]:
                print(f"    {row.severity:>4} {row.key:<14}: {row.value}")
            if len(sec.rows) > 5:
                print(f"    ... +{len(sec.rows)-5} more")
