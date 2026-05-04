"""
hwtest_viewer.py - ImGui front-end for the hwtest UART output stream.

Connects to a serial port (default 115200 8N1, the rate hwtest's UART_B
is configured for in the non-JC build), reads the diagnostic dump, and
presents it as a clickable tab UI:

    [Verdict] [SoC] [Fuses] [PMICs] ... [Raw log]

Each tab shows the parsed key/value table for the selected page, with
heuristic colour highlighting (green/yellow/red) for OK/WARN/FAIL rows.
The toolbar has buttons that send the pager keys (n/p/r/s/q) over the
same UART so the user can drive the on-console pager remotely.

Dependencies:
    pip install imgui-bundle pyserial

Run:
    python hwtest_viewer.py [--port COM5] [--baud 115200] [--replay file.txt]

If --replay is given, the tool reads the file as a fake UART stream
(useful for offline development, no console required).

The `replay` mode honours newline-delimited timing if the file contains
lines starting with `# delay <ms>` (otherwise the whole file is fed in
one go on connect).
"""

from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
from pathlib import Path
from typing import Optional

# Allow running from inside host_tools/ without `python -m`.
sys.path.insert(0, str(Path(__file__).parent))

from parser import Parser, Report, Page, Section, Row  # noqa: E402

try:
    import serial  # pyserial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install pyserial",
          file=sys.stderr)
    raise

try:
    from imgui_bundle import imgui, hello_imgui, immapp
except ImportError:
    print("ERROR: imgui-bundle not installed. Run: pip install imgui-bundle",
          file=sys.stderr)
    raise


# ---------------------------------------------------------------------------
# Serial reader thread
# ---------------------------------------------------------------------------


class SerialReader:
    """
    Background thread that reads from a serial port (or replay file) and
    pushes decoded text chunks into a queue.

    The GUI main thread drains the queue each frame and feeds the parser.
    Keeping serial I/O off the GUI thread is essential, a stalled
    pyserial read would freeze the window otherwise.
    """

    def __init__(self) -> None:
        self.q: queue.Queue[str] = queue.Queue()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._ser: Optional[serial.Serial] = None
        self._replay_path: Optional[Path] = None
        self.last_error: Optional[str] = None
        self.connected = False
        # Cumulative byte counter, surfaced in the toolbar so the user
        # can tell at a glance whether bytes are actually arriving. If
        # this stays at 0 while "connected", another process (picocom /
        # screen / minicom) is racing us for the TTY.
        self.rx_bytes = 0

    # Serial mode -----------------------------------------------------------

    def open_serial(self, port: str, baud: int = 115200) -> None:
        self.close()
        try:
            # 8N1, no flow control. timeout=0.05 keeps the reader thread
            # responsive without burning CPU.
            #
            # We deliberately do NOT pass exclusive=True. On Linux that
            # uses TIOCEXCL which is advisory, some USB-UART drivers
            # (esp. FTDI's vendor branch) honour it inconsistently, and
            # if it succeeds it can leave the TTY in a state that makes
            # subsequent picocom sessions fail until reboot. The "is
            # something else holding this port?" diagnosis is shown in
            # the toolbar; we don't try to enforce it.
            self._ser = serial.Serial(port, baudrate=baud, timeout=0.05,
                                      bytesize=serial.EIGHTBITS,
                                      parity=serial.PARITY_NONE,
                                      stopbits=serial.STOPBITS_ONE,
                                      xonxoff=False, rtscts=False,
                                      dsrdtr=False)
        except serial.SerialException as e:
            self.last_error = f"open {port} @ {baud}: {e}"
            self.connected = False
            return
        self.last_error = None
        self.connected = True
        self.rx_bytes = 0
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._serial_loop, daemon=True, name="hwtest-serial-reader")
        self._thread.start()

    def _serial_loop(self) -> None:
        # Pyserial gotcha: `Serial.in_waiting` on some USB-UART drivers
        # (FTDI in particular) under-reports because bytes sit in the
        # chip's internal buffer until the latency timer fires (~16 ms
        # default). Reading `read(1)` works but is slow.
        #
        # The robust pattern is to call `read(N)` with a generous N and
        # let timeout govern the loop cadence: read returns whatever
        # bytes have arrived within the timeout, up to N. With timeout
        # 0.05 s and 4096-byte reads we drain promptly and spend the
        # rest of each tick blocked in the kernel rather than spinning.
        assert self._ser is not None
        try:
            self._ser.reset_input_buffer()
        except (OSError, serial.SerialException):
            pass
        while not self._stop.is_set():
            try:
                data = self._ser.read(4096)
            except (OSError, serial.SerialException) as e:
                self.last_error = f"read: {e}"
                self.connected = False
                return
            if data:
                self.rx_bytes += len(data)
                self.q.put(data.decode("utf-8", errors="replace"))

    def send(self, byte: bytes) -> None:
        if self._ser is None or not self.connected:
            return
        try:
            self._ser.write(byte)
        except (OSError, serial.SerialException) as e:
            self.last_error = f"write: {e}"
            self.connected = False

    # Replay mode -----------------------------------------------------------

    def open_replay(self, path: Path) -> None:
        self.close()
        if not path.exists():
            self.last_error = f"replay file not found: {path}"
            return
        self._replay_path = path
        self.last_error = None
        self.connected = True
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._replay_loop, daemon=True, name="hwtest-replay-reader")
        self._thread.start()

    def _replay_loop(self) -> None:
        assert self._replay_path is not None
        try:
            text = self._replay_path.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            self.last_error = f"replay read: {e}"
            self.connected = False
            return
        # Honour `# delay <ms>` directives so a recorded session can play
        # back at its original cadence.
        for line in text.splitlines(keepends=True):
            if self._stop.is_set():
                return
            stripped = line.strip()
            if stripped.startswith("# delay "):
                try:
                    ms = int(stripped.split()[2])
                    time.sleep(ms / 1000.0)
                except (ValueError, IndexError):
                    pass
                continue
            self.q.put(line)
        # Replay file done. Stay "connected" so the user can re-open it
        # by hitting the reload button without a teardown.

    # Common ----------------------------------------------------------------

    def close(self) -> None:
        self._stop.set()
        if self._ser is not None:
            try:
                self._ser.close()
            except OSError:
                pass
            self._ser = None
        self._thread = None
        self.connected = False
        self._replay_path = None


# ---------------------------------------------------------------------------
# Colours used for the severity highlight. ImGui takes RGBA in [0,1].
# These mirror hwtest's COL_OK / COL_WARN / COL_ERR / COL_DEFAULT.
# ---------------------------------------------------------------------------

COL_OK   = (0.59, 1.00, 0.00, 1.00)   # 0xFF96FF00
COL_WARN = (1.00, 0.87, 0.00, 1.00)   # 0xFFFFDD00
COL_ERR  = (1.00, 0.31, 0.31, 1.00)   # 0xFFFF5050
COL_INFO = (0.85, 0.85, 0.85, 1.00)   # plain key/value text
COL_KEY  = (0.65, 0.80, 1.00, 1.00)   # cool blue for the key column
COL_HDR  = (1.00, 0.82, 0.45, 1.00)   # warm orange for section headers

SEV_TO_COLOR = {
    "ok":   COL_OK,
    "warn": COL_WARN,
    "err":  COL_ERR,
    "info": COL_INFO,
}


# ---------------------------------------------------------------------------
# Application state
# ---------------------------------------------------------------------------


class App:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.parser = Parser()
        self.reader = SerialReader()

        # UI state
        self.selected_port = args.port or ""
        self.baud = args.baud
        self.selected_page: Optional[str] = None
        self.show_raw = False
        self.auto_scroll_raw = True
        self.raw_filter = ""

        # Wireshark-style row selection: set when a row is left-clicked
        # (table_id + index). Used for the highlight + as the anchor for
        # the right-click copy context menu.
        self.selected_row_id: Optional[str] = None

        # If --replay was passed, kick it off immediately on first frame.
        self._pending_replay: Optional[Path] = (
            Path(args.replay) if args.replay else None)

    # -----------------------------------------------------------------------
    # Per-frame work
    # -----------------------------------------------------------------------

    def drain_serial(self) -> None:
        """Pull any new chunks out of the reader queue and feed the parser."""
        # Tight loop with a non-blocking get so we don't introduce GUI lag.
        try:
            while True:
                chunk = self.reader.q.get_nowait()
                self.parser.feed_chunk(chunk)
        except queue.Empty:
            pass

    def _refresh_selected_group(self) -> None:
        """Refresh just the host viewer's currently-selected group.

        Sends the C-side 'G<name>\\n' command which re-runs every probe
        whose page_group name matches and re-emits the data over UART
        WITHOUT touching the LCD. The on-console operator stays where
        they were; we just get fresh data for our pane.

        Falls back to a plain 'r' (refresh whatever the LCD is showing)
        if no real page is selected."""
        target = self.selected_page
        if not target or target == "__raw__" or target == "Boot banner":
            self.reader.send(b"r")
            return
        # Group name plus '\n' terminator. The C-side reader stops on \n
        # or after 200 ms; the explicit terminator avoids the timeout.
        self.reader.send(b"G" + target.encode("utf-8") + b"\n")

    def render(self) -> None:
        # Process any pending replay request first (handled here rather than
        # __init__ so the GUI is already alive when the worker starts).
        if self._pending_replay is not None:
            self.reader.open_replay(self._pending_replay)
            self._pending_replay = None

        self.drain_serial()

        # The selected_page deliberately stays put across UART updates.
        # The host viewer does NOT auto-follow LCD navigation, if the
        # operator hits VOL+/VOL- on the Switch and the console emits a
        # different page boundary, our pane stays where the user clicked.
        # Refresh-group sends 'G<name>\n' which re-emits without changing
        # LCD state, so this is symmetric.

        self._render_toolbar()
        imgui.separator()
        self._render_main_split()

    # -----------------------------------------------------------------------
    # Toolbar: connect, pager keys, save, status
    # -----------------------------------------------------------------------

    def _render_toolbar(self) -> None:
        # Port selector. Re-enumerate every frame so freshly-plugged USB
        # serial adapters appear without a manual refresh.
        ports = [p.device for p in list_ports.comports()]
        if self.selected_port and self.selected_port not in ports:
            ports.insert(0, self.selected_port)

        imgui.set_next_item_width(180)
        if imgui.begin_combo("##port",
                             self.selected_port or "(no port)"):
            for p in ports:
                is_sel = (p == self.selected_port)
                if imgui.selectable(p, is_sel)[0]:
                    self.selected_port = p
            imgui.end_combo()
        imgui.same_line()

        imgui.set_next_item_width(80)
        changed, self.baud = imgui.input_int("baud", self.baud, 0)
        imgui.same_line()

        if not self.reader.connected:
            if imgui.button("Connect") and self.selected_port:
                self.reader.open_serial(self.selected_port, self.baud)
        else:
            if imgui.button("Disconnect"):
                self.reader.close()
        imgui.same_line()

        # Pager command keys. Disabled when not connected.
        #
        # n/p (next/prev) intentionally NOT exposed here, the user
        # said the LCD-side switching is interesting on the Switch
        # itself, not via remote control. Navigation stays on the
        # physical buttons / the operator's hands.
        #
        # Refresh sends 'G<group>\n' which re-runs only the probes
        # for the host's currently-selected group and re-emits them
        # over UART, WITHOUT touching the LCD page the operator is
        # viewing. Refresh ALL re-runs every probe.
        imgui.begin_disabled(not self.reader.connected)

        if imgui.button("Refresh group"):
            self._refresh_selected_group()
        imgui.same_line()
        if imgui.button("Refresh ALL"):
            self.reader.send(b"a")
        imgui.same_line()
        if imgui.button("Save SD report"):
            self.reader.send(b"s")
        imgui.same_line()
        if imgui.button("Power off console"):
            self.reader.send(b"q")
        imgui.same_line()
        imgui.end_disabled()

        # Status spacer + clear/save buttons.
        imgui.new_line()
        if imgui.button("Clear parsed view"):
            self.parser = Parser()
            self.selected_page = None
        imgui.same_line()
        if imgui.button("Save raw log..."):
            ts = time.strftime("%Y%m%d-%H%M%S")
            out = Path(f"hwtest_uart_{ts}.txt")
            out.write_text("\n".join(self.parser.report.raw_lines),
                           encoding="utf-8")
            print(f"Wrote {out.resolve()}")
        imgui.same_line()
        if self.reader.last_error:
            imgui.text_colored(COL_ERR, f"  err: {self.reader.last_error}")
        elif self.reader.connected:
            rx = self.reader.rx_bytes
            if rx == 0:
                # Open succeeded but nothing's arrived yet. Most common
                # causes (in order): another picocom / screen / minicom
                # / tio session is racing us for bytes; the boot dump
                # finished BEFORE the viewer attached and the pager is
                # idle (hit 'r: refresh' to nudge it); wrong baud rate.
                imgui.text_colored(COL_WARN,
                    "  connected (0 B - hit 'r: refresh' or close other terminals)")
            else:
                pages = len(self.parser.report.page_order)
                imgui.text_colored(COL_OK,
                    f"  connected ({rx} B, {pages} page{'s' if pages != 1 else ''})")
        else:
            imgui.text_colored(COL_INFO, "  idle")

    # -----------------------------------------------------------------------
    # Main split: page list on the left, content on the right.
    # -----------------------------------------------------------------------

    def _render_main_split(self) -> None:
        report = self.parser.report
        avail = imgui.get_content_region_avail()

        # Left pane: list of pages.
        imgui.begin_child("##pages", imgui.ImVec2(220, avail.y),
                          imgui.ChildFlags_.borders.value)
        # Plain text for the static UI label - COL_HDR (warm orange) is
        # reserved for parsed-from-stream headers (page / section names
        # mirroring the firmware's COL_HEADER). Tinting the static
        # "Pages" caption in the same orange used by warning rows below
        # makes the entire list look like it's in a warning state.
        imgui.text("Pages")
        imgui.separator()

        # "Raw log" pseudo-page is always present.
        if imgui.selectable("Raw log",
                            self.selected_page == "__raw__")[0]:
            self.selected_page = "__raw__"

        # Real pages from the parsed stream, in arrival order. Colour the
        # entry by the worst severity any of its rows reached: red if any
        # row is `err`, yellow if any row is `warn`, default otherwise.
        # No leading [!] / [~] glyph, the colour itself signals status.
        for name in report.page_order:
            page = report.pages[name]
            has_err = any(r.severity == "err"
                          for s in page.sections for r in s.rows)
            has_warn = any(r.severity == "warn"
                           for s in page.sections for r in s.rows)
            row_color = (COL_ERR if has_err
                         else COL_WARN if has_warn
                         else None)
            sel = (self.selected_page == name)
            if row_color is not None:
                imgui.push_style_color(imgui.Col_.text.value, row_color)
            clicked = imgui.selectable(name, sel)[0]
            if row_color is not None:
                imgui.pop_style_color()
            if clicked:
                self.selected_page = name

        # Auto-select the first page if nothing chosen yet.
        if self.selected_page is None and report.page_order:
            self.selected_page = report.page_order[0]

        imgui.end_child()
        imgui.same_line()

        # Right pane: page content.
        imgui.begin_child("##content", imgui.ImVec2(0, avail.y),
                          imgui.ChildFlags_.borders.value)
        if self.selected_page == "__raw__":
            self._render_raw_log()
        elif self.selected_page and self.selected_page in report.pages:
            self._render_page(report.pages[self.selected_page])
        else:
            imgui.text_colored(COL_INFO,
                "Connect to a serial port (or pass --replay) to start.")
        imgui.end_child()

    # -----------------------------------------------------------------------
    # Page renderer: header + per-section tables.
    # -----------------------------------------------------------------------

    @staticmethod
    def _section_header_redundant(page_name: str, section_name: str) -> bool:
        """True if the section header repeats info already in the page
        title. Used to suppress visual noise like

            Page: 'SoC'
              [SoC]
                HIDREV : ...

        where the '[SoC]' bracket adds nothing."""
        # Exact match, '[Verdict]' under page 'Verdict'
        if section_name.strip() == page_name.strip():
            return True
        # Synthetic placeholder created by the parser for free-form text
        # before the first page boundary (the boot banner). Has no useful
        # content of its own.
        if section_name.strip() == "(unnamed)":
            return True
        return False

    def _render_section_table(self, section: Section, table_id: str) -> None:
        """Render the key/value table for one section. Each row is
        wrapped in a SpanAllColumns Selectable so the user can highlight
        it like a Wireshark packet, and right-click brings up a context
        menu with Copy key / Copy value / Copy row entries (writing to
        the system clipboard via imgui.set_clipboard_text). Selectable
        IDs are scoped by table_id + row index so two rows with
        identical key text in different sections don't collide.

        Sub-rows (Row.indent > 0) get a leading-spaces + '+ ' prefix
        in the key column so register-bit breakdowns visually nest
        under their parent row.

        section.notes (free-form lines the parser couldn't classify)
        is intentionally NOT rendered here. Per the user's request,
        only parseable rows belong in the panes; raw text is in the
        Raw log pane."""
        if not section.rows:
            return
        table_flags = (imgui.TableFlags_.row_bg.value |
                       imgui.TableFlags_.borders_inner.value |
                       imgui.TableFlags_.sizing_stretch_prop.value)
        if not imgui.begin_table(table_id, 2, table_flags):
            return
        imgui.table_setup_column("Key",
            imgui.TableColumnFlags_.width_fixed.value, 220)
        imgui.table_setup_column("Value")
        sel_flags = (imgui.SelectableFlags_.span_all_columns.value
                   | imgui.SelectableFlags_.allow_overlap.value)
        for i, row in enumerate(section.rows):
            row_id = f"{table_id}#{i}"
            is_sel = (self.selected_row_id == row_id)
            imgui.table_next_row()
            imgui.table_set_column_index(0)
            if imgui.selectable(f"##{row_id}", is_sel, sel_flags)[0]:
                self.selected_row_id = row_id
            if imgui.begin_popup_context_item(f"ctx{row_id}"):
                self.selected_row_id = row_id
                if imgui.menu_item(f"Copy key  ({row.key})", "", False)[0]:
                    imgui.set_clipboard_text(row.key)
                if imgui.menu_item("Copy value", "", False)[0]:
                    imgui.set_clipboard_text(row.value)
                if imgui.menu_item("Copy row (key : value)", "", False)[0]:
                    imgui.set_clipboard_text(f"{row.key} : {row.value}")
                imgui.end_popup()
            imgui.same_line(0, 0)
            # Sub-bullet rows render with leading-spaces + '+ ' prefix
            # to look nested under their parent row.
            key_label = (("    " * row.indent) + "+ " + row.key
                         if row.indent > 0 else row.key)
            imgui.text_colored(COL_KEY, key_label)
            imgui.table_set_column_index(1)
            imgui.text_colored(SEV_TO_COLOR[row.severity], row.value)
        imgui.end_table()

    def _render_page(self, page: Page) -> None:
        imgui.text_colored(COL_HDR, page.name)
        imgui.separator()

        # Single-section pages: no point in a tree node, and the section
        # header `[Name]` is often just a restatement of the page title.
        # Render the table inline so the user sees data right away.
        if len(page.sections) == 1:
            sec = page.sections[0]
            if not self._section_header_redundant(page.name, sec.name):
                # Surface the chip identity / bus location subtitle even
                # without a collapsible tree, it's useful context (e.g.
                # 'BQ24193 charger, I2C1 @ 0x6B') but it's not redundant
                # with the page title.
                imgui.text_colored(COL_INFO, f"[{sec.name}]")
            self._render_section_table(sec, f"##{page.name}-{sec.name}")
            return

        # Multi-section pages keep the collapsible tree-node layout so
        # the user can fold up the sub-sections they don't care about
        # (the Fuses page in particular has 5 sections of dense data).
        for section in page.sections:
            flags = (imgui.TreeNodeFlags_.default_open.value |
                     imgui.TreeNodeFlags_.open_on_arrow.value)
            if not imgui.tree_node_ex(f"[{section.name}]", flags):
                continue
            self._render_section_table(section, f"##{page.name}-{section.name}")
            imgui.tree_pop()

    # -----------------------------------------------------------------------
    # Raw log view.
    # -----------------------------------------------------------------------

    def _render_raw_log(self) -> None:
        rx = self.reader.rx_bytes
        lines = len(self.parser.report.raw_lines)
        imgui.text_colored(COL_HDR,
            f"Raw UART log  ({rx} bytes, {lines} complete lines)")
        imgui.same_line()
        _, self.auto_scroll_raw = imgui.checkbox("auto-scroll",
                                                 self.auto_scroll_raw)
        imgui.same_line()
        imgui.set_next_item_width(180)
        _, self.raw_filter = imgui.input_text("filter", self.raw_filter)
        imgui.separator()

        # If we're connected but nothing parsed and nothing buffered, give
        # the user actionable advice. Empty rx_bytes most commonly means
        # another terminal program (picocom / screen / minicom / tio) is
        # holding the port. Wrong baud is the second-most-common cause.
        pending = self.parser.pending_line
        if self.reader.connected and rx == 0:
            imgui.text_colored(COL_WARN,
                "Connected, but no bytes received yet.")
            imgui.text_colored(COL_INFO,
                "  - Close any picocom/screen/minicom/tio session on this port,")
            imgui.text_colored(COL_INFO,
                "    then reconnect (Disconnect / Connect).")
            imgui.text_colored(COL_INFO,
                "  - Confirm the baud rate (115200 for default hwtest build).")
            imgui.text_colored(COL_INFO,
                "  - Confirm the cable is plugged into the right Joy-Con rail")
            imgui.text_colored(COL_INFO,
                "    and the level shifter has 1.8 V on the Switch side.")

        imgui.begin_child("##rawscroll", imgui.ImVec2(0, 0),
                          imgui.ChildFlags_.borders.value)
        # ImGui can struggle with thousands of text_colored calls per frame.
        # We're typically <2k lines, which is fine. If we ever blow past
        # that we should switch to ImGui's clipper helper.
        f = self.raw_filter.lower()
        for line in self.parser.report.raw_lines[-5000:]:
            if f and f not in line.lower():
                continue
            imgui.text_unformatted(line)
        # Surface the partial-line buffer so bytes that haven't yet been
        # newline-terminated still appear (the original raw log only showed
        # complete lines, which made slow trickles look like a hang).
        if pending and (not f or f in pending.lower()):
            imgui.text_colored(COL_WARN, pending + "  (partial)")
        if self.auto_scroll_raw:
            imgui.set_scroll_here_y(1.0)
        imgui.end_child()


# ---------------------------------------------------------------------------
# Entry point: build the immapp window and dispatch frames to App.render.
# ---------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="Serial port (e.g. COM5 or /dev/ttyUSB0)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--replay",
        help="Replay a captured UART log instead of opening a serial port")
    args = ap.parse_args()

    app = App(args)

    runner_params = hello_imgui.RunnerParams()
    runner_params.app_window_params.window_title = "hwtest viewer"
    runner_params.app_window_params.window_geometry.size = (1280, 800)
    runner_params.imgui_window_params.show_menu_bar = False
    runner_params.imgui_window_params.default_imgui_window_type = (
        hello_imgui.DefaultImGuiWindowType.provide_full_screen_window)
    runner_params.callbacks.show_gui = app.render
    # We need 30+ FPS so serial chunks render promptly without burning CPU.
    runner_params.fps_idling.fps_idle = 30.0

    immapp.run(runner_params)


if __name__ == "__main__":
    main()
