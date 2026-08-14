/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: log probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* Logging                                                                  */

/* Three-sink output:
 *  1. gfx LCD console  (suppressible via _log_uart_only)
 *  2. UART_B           (always on, CRLF translated)
 *  3. report buffer    (when _log_capture is non-NULL, used by save_report)
 *
 * Everything funnels through `_log_emit(color, "...")`. The LOG macro picks
 * `COL_DEFAULT`; `log_color(color, ...)` overrides for a single line. */

char _log_buf[512];
bool _log_uart_only = false;
bool _log_no_uart   = false;    /* used by save_report to avoid duplicating */
char *_log_capture     = NULL;   /* NULL = don't capture */
u32 _log_capture_pos = 0;
u32 _log_capture_cap = 0;

static void _log_capture_append(const char *s, u32 n)
{
    if (!_log_capture || _log_capture_pos + n + 1 > _log_capture_cap)
        return;
    memcpy(_log_capture + _log_capture_pos, s, n);
    _log_capture_pos += n;
    _log_capture[_log_capture_pos] = 0;
}

/* Emit to the LCD, folding anything wider than the panel onto a continuation
 * line instead of letting it run off the right edge.
 *
 * The LOG/log_color _Static_assert only measures the format-string LITERAL;
 * a runtime %s / %d substitution can still push the rendered line past 80
 * columns, and gfx_putc does not wrap, so a wide line is truncated mid-word
 * on the panel. Folding here catches every such case, and only affects the
 * LCD - UART and the SD report keep the full untouched line. */
static void _gfx_puts_folded(const char *s)
{
    char line[LCD_COLS + 1];
    u32 col = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') {
            line[col] = 0;
            gfx_puts(line);
            gfx_puts("\n");
            col = 0;
            continue;
        }
        if (col == LCD_COLS) {
            line[col] = 0;
            gfx_puts(line);
            gfx_puts("\n");
            col = 0;
            /* Indent the continuation so it reads as a wrapped row. */
            line[col++] = ' ';
            line[col++] = ' ';
        }
        line[col++] = *p;
    }
    if (col) {
        line[col] = 0;
        gfx_puts(line);
    }
}

/* Emit `_log_buf` to all sinks. Caller has already filled the buffer via
 * s_printf / s_vprintf. `color` only matters for the gfx sink. */
void _log_emit(u32 color)
{
    u32 n = strlen(_log_buf);
    if (!_log_uart_only) {
        u32 prev = gfx_con.fgcol;
        gfx_con.fgcol = color;
        _gfx_puts_folded(_log_buf);
        gfx_con.fgcol = prev;
    }
    if (!_log_no_uart)
        uart_send_crlf(UART_B, (u8 *)_log_buf, n);
    _log_capture_append(_log_buf, n);
}

/* Translate \n to \r\n on the way to UART so terminals don't stair-step
 * the output. The gfx and report-buffer sides keep bare \n. (In the
 * JC_PROBE build this compiles out: the header provides an inline no-op
 * because there is no DEBUG_UART_PORT and UART_B is never initialised.) */
#ifndef JC_PROBE
void uart_send_crlf(u32 idx, const u8 *buf, u32 n)
{
    static const u8 cr = '\r';
    u32 start = 0;
    for (u32 i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (i > start)
                uart_send(idx, buf + start, i - start);
            uart_send(idx, &cr, 1);
            uart_send(idx, &buf[i], 1);
            start = i + 1;
        }
    }
    if (start < n)
        uart_send(idx, buf + start, n - start);
    uart_wait_xfer(idx, UART_TX_IDLE);
}
#endif  /* !JC_PROBE */

void _log_color_impl(u32 color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s_vprintf(_log_buf, fmt, ap);
    va_end(ap);
    _log_emit(color);
}

/* Silence both sinks for a stretch of work whose output would be noise -
 * re-running a prerequisite so a later page can stand on its own, say.
 * Nests once, which is all any caller needs; the capture buffer is left
 * alone so the saved report still has the full story. */
static bool _log_mute_prev_gfx, _log_mute_prev_uart;
void log_mute(bool on)
{
    if (on) {
        _log_mute_prev_gfx  = _log_uart_only;
        _log_mute_prev_uart = _log_no_uart;
        _log_uart_only = true;
        _log_no_uart   = true;
    } else {
        _log_uart_only = _log_mute_prev_gfx;
        _log_no_uart   = _log_mute_prev_uart;
    }
}

