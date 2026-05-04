/*
 * hwtest - one-shot Switch hardware probe / repair report.
 *
 * Inits the BPMP-side peripherals via Hekate's BDK and walks every chip the
 * payload knows about. Output is mirrored three ways:
 *
 *   - LCD (paged, with health-coloured values)
 *   - UART_B (Joy-Con right port, 115200 8N1, 1.8 V, CRLF line endings)
 *   - SD card text file at backup/<emmc_serial>/hwtest.txt
 *
 * Use it on a console with a faulty screen by capturing the UART output, or
 * pull the SD card afterwards if no UART rig is at hand.
 *
 * Navigation:
 *   VOL+ / 'n'   next page
 *   VOL- / 'p'   previous page
 *   'r'          refresh current page (re-reads chips)
 *   's'          re-save SD report
 *   POWER / 'q'  power off the console
 */

#include <bdk.h>
#include <gfx_utils.h>
#include <libs/fatfs/ff.h>
#ifdef JC_PROBE
#include <input/joycon.h>
#endif

#include <stdarg.h>
#include <string.h>

extern void pivot_stack(u32 stack);
extern char *emmcsn_path_impl(char *path, char *sub_dir, char *filename,
                              sdmmc_storage_t *storage);

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */

#define COL_HEADER  0xFFFF8000   /* orange */
#define COL_DEFAULT 0xFFCCCCCC   /* light grey */
#define COL_OK      0xFF96FF00   /* green */
#define COL_WARN    0xFFFFDD00   /* yellow */
#define COL_ERR     0xFFFF5050   /* red */
#define COL_INFO    0xFF00DDFF   /* cyan, for section subtitles */
#define COL_BG      0xFF1B1B1B

/* Three-sink output:
 *  1. gfx LCD console  (suppressible via _log_uart_only)
 *  2. UART_B           (always on, CRLF translated)
 *  3. report buffer    (when _log_capture is non-NULL, used by save_report)
 *
 * Everything funnels through `_log_emit(color, "...")`. The LOG macro picks
 * `COL_DEFAULT`; `log_color(color, ...)` overrides for a single line. */

static char _log_buf[512];
static bool   _log_uart_only = false;
static bool   _log_no_uart   = false;    /* used by save_report to avoid duplicating */
static char  *_log_capture     = NULL;   /* NULL = don't capture */
static u32    _log_capture_pos = 0;
static u32    _log_capture_cap = 0;

/* Translate \n to \r\n on the way to UART so terminals don't stair-step
 * the output. The gfx and report-buffer sides keep bare \n.
 *
 * In the JC_PROBE build there's no DEBUG_UART_PORT, so hw_init never
 * configures UART_B and any uart_send would block forever polling
 * UART_LSR.THRE on an unclocked controller. Compile this whole helper
 * out and let _log_flush skip the UART path. */
#ifdef JC_PROBE
static inline void uart_send_crlf(u32 idx, const u8 *buf, u32 n)
{
    (void)idx; (void)buf; (void)n;
}
#else
static void uart_send_crlf(u32 idx, const u8 *buf, u32 n)
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
#endif  /* JC_PROBE */

static void _log_capture_append(const char *s, u32 n)
{
    if (!_log_capture || _log_capture_pos + n + 1 > _log_capture_cap)
        return;
    memcpy(_log_capture + _log_capture_pos, s, n);
    _log_capture_pos += n;
    _log_capture[_log_capture_pos] = 0;
}

/* Emit `_log_buf` to all sinks. Caller has already filled the buffer via
 * s_printf / s_vprintf. `color` only matters for the gfx sink. */
static void _log_emit(u32 color)
{
    u32 n = strlen(_log_buf);
    if (!_log_uart_only) {
        u32 prev = gfx_con.fgcol;
        gfx_con.fgcol = color;
        gfx_puts(_log_buf);
        gfx_con.fgcol = prev;
    }
    if (!_log_no_uart)
        uart_send_crlf(UART_B, (u8 *)_log_buf, n);
    _log_capture_append(_log_buf, n);
}

/* LCD geometry: the panel is 1280x720 in landscape; gfx_putc renders 16
 * pixels per char, so each row holds 80 chars and there's no auto-wrap.
 * A format string that's wider than this just spills off the right edge
 * of the screen with no warning. We catch obvious overflow at compile
 * time below.
 *
 * The check has two practical limitations the caller has to keep in mind:
 *   - It measures the format-string LITERAL only. A runtime substitution
 *     (`%d` / `%s` / etc.) can still push the rendered line past 80 cols
 *     -- the check catches static overflow, not dynamic.
 *   - It measures TOTAL length, treating each `\n` as one column too. So
 *     a multi-line format string whose individual lines all fit but
 *     whose total length exceeds the budget will be flagged. The fix is
 *     to split such cases into separate LOG calls (which is also
 *     friendlier to read). Two existing multi-line format strings were
 *     split when this check was added.
 *
 * The +2 budget covers a trailing `\n` plus the implicit nul. */
#define LCD_COLS 80

#define LOG(fmt, ...) do {                                \
    _Static_assert(sizeof(fmt) <= LCD_COLS + 2,           \
        "LOG format string wider than 80-col LCD");       \
    s_printf(_log_buf, fmt, ##__VA_ARGS__);               \
    _log_emit(COL_DEFAULT);                               \
} while (0)

static void _log_color_impl(u32 color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s_vprintf(_log_buf, fmt, ap);
    va_end(ap);
    _log_emit(color);
}

#define log_color(color, fmt, ...) do {                   \
    _Static_assert(sizeof(fmt) <= LCD_COLS + 2,           \
        "log_color format string wider than 80-col LCD"); \
    _log_color_impl(color, fmt, ##__VA_ARGS__);           \
} while (0)

#define HEADER(s)  log_color(COL_INFO, "%s\n", s)

/* ------------------------------------------------------------------------ */
/* Boot-time progress indicator                                             */
/*                                                                          */
/* During the boot UART dump (_log_uart_only = true), nothing is being      */
/* drawn to the LCD - it sits on the static splash screen. To distinguish   */
/* "still working" from "crashed", redraw a single status line below the   */
/* splash with a spinner glyph + current probe name. The spinner ticks     */
/* every call so even a single long-running probe (I2C census, ~3 s, scans */
/* 0x08-0x77 on three buses) shows visible motion.                         */
/*                                                                          */
/* The status row lives at fixed pixel coordinates so it never collides    */
/* with the scrolling splash text. Once the dump phase ends and the pager  */
/* takes over, the screen gets cleared and the status row is gone.        */

#define STATUS_ROW_PX  (16 * 8)   /* row 8: under the splash + init steps */
#define STATUS_BLANK   "                                                                              "

static u32 g_status_spin_idx = 0;

/* ------------------------------------------------------------------------ */
/* Diagnostic-finding registry                                              */
/*                                                                          */
/* (Replaces 16+ scattered globals that the verdict used to read directly.) */
/*                                                                          */
/* One source of truth for everything the verdict cross-checks. Each detail */
/* probe stores its observation under a stable string key (`dx_set`), the   */
/* verdict iterates per-macro key lists and rolls them up. Replacing the    */
/* per-finding globals with this registry keeps the global namespace small  */
/* and makes adding a new sub-check a two-line change: one dx_set in the    */
/* probe + one key in the macro's list.                                     */
/*                                                                          */
/* Severity ladder is FAIL > WARN > PASS; the verdict picks the worst.      */
/* The detail string carries the runtime reason (e.g. "4-bit", "0xD8") so   */
/* the verdict doesn't have to know how to format each value -- the probe   */
/* that captured the value does, when it knows the value.                   */

typedef enum { DX_PASS, DX_WARN, DX_FAIL } dx_sev_t;

typedef struct {
    const char *key;        /* string literal, immortal */
    dx_sev_t   sev;
    char       detail[48];  /* short reason, "" on a clean pass */
} dx_finding_t;

#define DX_CAP 48
static dx_finding_t _dx_tbl[DX_CAP];
static int          _dx_n;

/* Reset between full probe sweeps so refresh-all (`a`) can repopulate
 * cleanly without stale entries surviving when a probe path changes. */
static void dx_reset(void) { _dx_n = 0; }

/* Record/overwrite a finding. fmt may be NULL to clear the detail. */
static void dx_set(const char *key, dx_sev_t sev, const char *fmt, ...)
{
    dx_finding_t *f = NULL;
    for (int i = 0; i < _dx_n; i++)
        if (strcmp(_dx_tbl[i].key, key) == 0) { f = &_dx_tbl[i]; break; }
    if (!f) {
        if (_dx_n >= DX_CAP) return;
        f = &_dx_tbl[_dx_n++];
        f->key = key;
    }
    f->sev = sev;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        s_vprintf(f->detail, fmt, ap);
        va_end(ap);
    } else {
        f->detail[0] = 0;
    }
}

static const dx_finding_t *dx_get(const char *key)
{
    for (int i = 0; i < _dx_n; i++)
        if (strcmp(_dx_tbl[i].key, key) == 0) return &_dx_tbl[i];
    return NULL;
}

/* Explicit gate for the on-LCD progress spinner. The boot dump phase
 * sets this true; everything else (save_report, host-driven 'G'
 * group-refresh, plain 'r' single-page refresh) leaves it false so
 * probes don't scribble the spinner over the pager output. We can't
 * just check `_log_uart_only` because the 'G' handler also flips that
 * to suppress LCD writes, we need a distinct concept. */
static bool g_show_status = false;

static void status_set(const char *msg)
{
    if (!g_show_status)
        return;

    static const char spin[] = "|/-\\";
    u32 saved_x = gfx_con.x;
    u32 saved_y = gfx_con.y;
    u32 saved_fg = gfx_con.fgcol;

    /* Wipe the line, then redraw. STATUS_BLANK is exactly the LCD width
     * so nothing left over from a previous (longer) message remains. */
    gfx_con_setpos(0, STATUS_ROW_PX);
    gfx_con.fgcol = COL_BG;
    gfx_puts(STATUS_BLANK);
    gfx_con_setpos(0, STATUS_ROW_PX);
    gfx_con.fgcol = COL_INFO;
    gfx_printf("  [%c] %s", spin[g_status_spin_idx & 3], msg);
    g_status_spin_idx++;

    /* Restore the cursor + colour so any subsequent _log_emit (which
     * shouldn't fire while _log_uart_only is true, but defensive) lands
     * back where it expected. */
    gfx_con_setpos(saved_x, saved_y);
    gfx_con.fgcol = saved_fg;
}

/* Pick a colour for a value with two warning thresholds and two error
 * thresholds. Pass INT_MIN / INT_MAX to disable a side. */
#define HEALTH_NONE 0x7FFFFFFE
static u32 health_color(int v, int warn_lo, int warn_hi, int err_lo, int err_hi)
{
    if (err_lo != HEALTH_NONE && v <  err_lo) return COL_ERR;
    if (err_hi != HEALTH_NONE && v >  err_hi) return COL_ERR;
    if (warn_lo != HEALTH_NONE && v < warn_lo) return COL_WARN;
    if (warn_hi != HEALTH_NONE && v > warn_hi) return COL_WARN;
    return COL_OK;
}

/* ------------------------------------------------------------------------ */
/* Probes                                                                   */

static void probe_soc(void)
{
    HEADER("[SoC]");
    u32 hidrev = APB_MISC(APB_MISC_GP_HIDREV);
    u32 chip_major = (hidrev >> 4) & 0xF;
    u32 chip_minor = (hidrev >> 16) & 0xF;
    u32 chip_id    = (hidrev >> 8)  & 0xFF;
    LOG("  HIDREV       : 0x%08X\n", hidrev);
    LOG("  Chip ID      : 0x%02X (%s)\n", chip_id,
        chip_major == 2 ? "T210B01 Mariko" : "T210 Erista");
    LOG("  Major.Minor  : %d.%02d\n", chip_major, chip_minor);
}

static void probe_fuses(void)
{
    HEADER("[Fuses - identity]");
    LOG("  PRODUCTION   : 0x%08X\n", FUSE(FUSE_PRODUCTION_MODE));
    LOG("  SKU_INFO     : 0x%08X\n", FUSE(FUSE_SKU_INFO));
    LOG("  ODM4         : 0x%08X (DRAM ID %d)\n",
        fuse_read_odm(4), fuse_read_dramid(true));
    LOG("  HW state     : %s\n",
        fuse_read_hw_state() == FUSE_NX_HW_STATE_PROD ? "Prod" : "Dev");
    LOG("  HW type      : %d\n", fuse_read_hw_type());
    LOG("  Keygen rev   : %d\n", fuse_read_odm_keygen_rev());

    bool patched = fuse_check_patched_rcm();
    /* SoC-aware colouring. On Mariko (T210B01) every unit ships with the
     * BootROM RCM bug patched, it's the expected state, not an anomaly,
     * so we keep it neutral. On Erista (T210), patched=true means the
     * RCM exploit is gone (factory ipatch burnt later in the lifecycle)
     * which is interesting but not a hardware fault either; flag yellow.
     * Erista with patched=false is the exploitable state we expect when
     * the user is running this payload in the first place, green. */
    u32 chip_major = (APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF;
    bool is_mariko = (chip_major == 2);
    u32 patched_color = !patched   ? COL_OK
                      : is_mariko  ? COL_DEFAULT
                                   : COL_WARN;
    log_color(patched_color,
        "  Patched RCM  : %s\n",
        patched ? (is_mariko ? "yes (Mariko - expected)" : "yes (ipatch burnt)")
                : "no (exploitable)");

    /* Burnt anti-downgrade fuses (ODM7). 255 means over-burnt. */
    u32 odm7 = fuse_read_odm(7);
    u32 odm6 = fuse_read_odm(6);
    int burnt = 0;
    for (u32 b = odm7; b; b &= b - 1) burnt++;
    log_color(odm7 == 0xFFFFFFFF ? COL_ERR : COL_OK,
        "  Burnt fuses  : %d | %d (ODM7 | ODM6)\n", burnt,
        __builtin_popcount(odm6));

    HEADER("[Fuses - speedo / IDDQ]");
    LOG("  CPU SPEEDO 0 : %d\n",   FUSE(FUSE_CPU_SPEEDO_0_CALIB));
    LOG("  CPU SPEEDO 1 : %d\n",   FUSE(FUSE_CPU_SPEEDO_1_CALIB));
    LOG("  CPU SPEEDO 2 : %d\n",   FUSE(FUSE_CPU_SPEEDO_2_CALIB));
    LOG("  SOC SPEEDO 0 : %d\n",   FUSE(FUSE_SOC_SPEEDO_0_CALIB));
    LOG("  SOC SPEEDO 2 : %d\n",   FUSE(FUSE_SOC_SPEEDO_2_CALIB));
    LOG("  CPU IDDQ     : %d\n",   FUSE(FUSE_CPU_IDDQ_CALIB) * 4);
    LOG("  GPU IDDQ     : %d\n",   FUSE(FUSE_GPU_IDDQ_CALIB) * 5);
    LOG("  SOC IDDQ     : %d\n",   FUSE(FUSE_SOC_IDDQ_CALIB) * 4);
    LOG("  BROM rev     : 0x%02X\n", FUSE(FUSE_SOC_SPEEDO_1_CALIB));
    LOG("  FT rev       : %d.%02d (0x%X)\n",
        (FUSE(FUSE_OPT_FT_REV) >> 5) & 0x3F,
         FUSE(FUSE_OPT_FT_REV) & 0x1F, FUSE(FUSE_OPT_FT_REV));
    LOG("  CP rev       : %d.%02d (0x%X)\n",
        (FUSE(FUSE_OPT_CP_REV) >> 5) & 0x3F,
         FUSE(FUSE_OPT_CP_REV) & 0x1F, FUSE(FUSE_OPT_CP_REV));
    LOG("  USB ctrl     : %s\n",
        (FUSE(FUSE_RESERVED_SW) & 0x80) ? "XUSB" : "USB2");

    HEADER("[Fuses - lot / wafer]");
    static const char base36[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    u32 lot_enc = FUSE(FUSE_OPT_LOT_CODE_0);
    char lot_bcd[6] = {0};
    for (int i = 0; i < 5; i++) {
        u32 d = (lot_enc & 0x3F000000) >> 24;
        lot_bcd[i] = (d < 36) ? base36[d] : '?';
        lot_enc <<= 6;
    }
    u32 fab = FUSE(FUSE_OPT_FAB_CODE);
    int die_x = FUSE(FUSE_OPT_X_COORDINATE);
    int die_y = FUSE(FUSE_OPT_Y_COORDINATE);
    if (die_x & (1 << 8)) die_x -= 512;            /* 9-bit signed */
    LOG("  Vendor code  : %d\n", FUSE(FUSE_OPT_VENDOR_CODE));
    LOG("  FAB / LOT    : %c%s\n",
        (fab < 36) ? base36[fab] : '?', lot_bcd);
    LOG("  Wafer ID     : %d\n", FUSE(FUSE_OPT_WAFER_ID));
    LOG("  X / Y coord  : %d / %d\n", die_x, die_y);

    HEADER("[Fuses - Public Key SHA-256]");
    LOG("  PK0..3       : %08X%08X%08X%08X\n",
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY0)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY1)),
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY2)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY3)));
    LOG("  PK4..7       : %08X%08X%08X%08X\n",
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY4)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY5)),
        byte_swap_32(FUSE(FUSE_PUBLIC_KEY6)), byte_swap_32(FUSE(FUSE_PUBLIC_KEY7)));

    HEADER("[Fuses - SBK / DK]");
    /* If all five private-key fuses read 0xFFFFFFFF, the SBK/DK have been
     * locked out by the bootrom, that's normal post-pkg1. */
    bool locked =
        FUSE(FUSE_PRIVATE_KEY0) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY1) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY2) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY3) == 0xFFFFFFFF &&
        FUSE(FUSE_PRIVATE_KEY4) == 0xFFFFFFFF;
    if (locked) {
        log_color(COL_WARN, "  SBK / DK     : locked out (bootrom)\n");
    } else {
        LOG("  SBK          : %08X%08X%08X%08X\n",
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY0)),
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY1)),
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY2)),
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY3)));
        LOG("  DK           : %08X\n",
            byte_swap_32(FUSE(FUSE_PRIVATE_KEY4)));
    }
}

static void probe_pmic(void)
{
    HEADER("[MAX77620 main PMIC, I2C5 @ 0x3C]");
    u8 cid3 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CID3);
    u8 cid4 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CID4);
    u8 cid5 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CID5);
    LOG("  CID3 Si rev  : 0x%02X (max77620 v%d)\n", cid3, cid3 & 0xF);

    /* OTP 0x35 = Erista, 0x53 = Mariko. Compare against SoC HIDREV major:
     * a mismatch hints at a swapped PMIC or a wired-in module. */
    u32 chip_major = (APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF;
    bool otp_ok = (chip_major == 2 && cid4 == 0x53) ||
                  (chip_major == 1 && cid4 == 0x35);
    log_color(otp_ok ? COL_OK : COL_ERR,
        "  CID4 OTP     : 0x%02X (%s)%s\n", cid4,
        cid4 == 0x35 ? "Erista" : cid4 == 0x53 ? "Mariko" : "Unknown",
        otp_ok ? "" : " - mismatch with SoC!");
    dx_set("soc_pmic_otp", otp_ok ? DX_PASS : DX_FAIL,
        otp_ok ? "" : "chip M%d, otp 0x%02X", chip_major, cid4);
    LOG("  CID5 ES rev  : 0x%02X\n", cid5);

    /* MAX77621 is the Erista CPU/GPU buck regulator (two single-phase
     * chips on I2C5 at 0x1B/0x1C). Mariko / OLED / Lite replace it with
     * the multi-phase MAX77812 — handled by probe_max77812 below.
     * Trying to read MAX77621 on Mariko returns NACK -> 0xFF on most
     * lines and prints garbage. Gate by SoC chip-major. */
    if (chip_major == 1) {
        HEADER("[MAX77621 CPU/GPU regulators, I2C5 @ 0x1B/0x1C]");
        u8 cpu_id = i2c_recv_byte(I2C_5, MAX77621_CPU_I2C_ADDR, MAX77621_REG_CHIPID1);
        u8 gpu_id = i2c_recv_byte(I2C_5, MAX77621_GPU_I2C_ADDR, MAX77621_REG_CHIPID1);
        LOG("  CPU CHIPID1  : 0x%02X (max77621 v%d)\n", cpu_id, cpu_id & 0xF);
        LOG("  GPU CHIPID1  : 0x%02X (max77621 v%d)\n", gpu_id, gpu_id & 0xF);
        /* Output voltage from VOUT register (0x00) and VOUT_DVS register
         * (0x01). Format: bit7 = enable, bits6:0 = voltage = 606250 + N * 6250 uV.
         * VOUT is the active setting; VOUT_DVS is the pre-loaded DVS target
         * (selected via the GPIO_DVS pin, see Hekate's max7762x.c). HOS
         * uses both for fast CPU/GPU DVFS. We surface both per chip for the
         * tech to compare. */
        struct max77621_rail {
            u32 i2c_addr;
            const char *name;
            u32 vmin_uv;
            u32 vmax_uv;
        };
        static const struct max77621_rail bc_rails[] = {
            {MAX77621_CPU_I2C_ADDR, "CPU", 1000000, 1400000},
            {MAX77621_GPU_I2C_ADDR, "GPU", 1200000, 1400000},
        };
        for (size_t r = 0; r < sizeof(bc_rails)/sizeof(bc_rails[0]); r++) {
            u8 vout     = i2c_recv_byte(I2C_5, bc_rails[r].i2c_addr, MAX77621_REG_VOUT);
            u8 vout_dvs = i2c_recv_byte(I2C_5, bc_rails[r].i2c_addr, MAX77621_REG_VOUT_DVS);
            bool en     = (vout & MAX77621_VOUT_ENABLE_MASK) != 0;
            bool en_dvs = (vout_dvs & MAX77621_VOUT_ENABLE_MASK) != 0;
            u32 uv      = ((vout     & MAX77621_DVC_DVS_VOLT_MASK) * 6250) + 606250;
            u32 uv_dvs  = ((vout_dvs & MAX77621_DVC_DVS_VOLT_MASK) * 6250) + 606250;
            bool ok     = en && uv >= bc_rails[r].vmin_uv && uv <= bc_rails[r].vmax_uv;
            log_color(ok ? COL_OK : COL_WARN,
                "  %s VOUT     : %s %d.%03d V  (range %d.%03d - %d.%03d V)\n",
                bc_rails[r].name,
                en ? "ON " : "off",
                uv / 1000000, (uv / 1000) % 1000,
                bc_rails[r].vmin_uv / 1000000, (bc_rails[r].vmin_uv / 1000) % 1000,
                bc_rails[r].vmax_uv / 1000000, (bc_rails[r].vmax_uv / 1000) % 1000);
            log_color(COL_DEFAULT,
                "  %s VOUT_DVS : %s %d.%03d V  (DVFS pre-load)\n",
                bc_rails[r].name,
                en_dvs ? "ON " : "off",
                uv_dvs / 1000000, (uv_dvs / 1000) % 1000);
        }
    }
}

/* MAX77812 — Mariko / OLED / Lite multi-phase buck regulator that
 * replaces the dual MAX77621 setup found on Erista. Two retail
 * variants exist:
 *   PHASE211 @ I2C5 0x33 : 2-phase M1 (GPU) + 1-phase M3 (DRAM) +
 *                          1-phase M4 (CPU). This is what every
 *                          consumer Switch ships with.
 *   PHASE31  @ I2C5 0x31 : 3-phase M1 + 1-phase M4. Dev-kit / high-power
 *                          GPU variant; M3 is unused.
 * Both probe with no harm — only one ACKs on a real board. We try
 * 0x33 first (retail likelihood) and fall back to 0x31. If neither
 * answers we mark the probe FAIL and skip; on Erista we don't run
 * this probe at all.
 *
 * VOUT register encoding (M1..M4): vout_mv = 250 + N * 5, range
 * 250..1525 mV (LV variant — Switch uses LV). Values quoted in
 * the BDK's max77812.h match this. EN_CTRL holds per-phase enable
 * bits at 0/2/4/6. BUCK_STAT reflects per-rail power-good /
 * over-current latches; non-zero is a fault signal. */
static u8 _max77812_probe_addr(u8 addr)
{
    /* Read VERSION register; valid silicon returns ES2_VERSION (0x04)
     * or QS_VERSION (0x05) in the low 3 bits. A NACK on the bus
     * surfaces as 0xFF; an unpowered chip as 0x00. Either way the
     * masked-low-3-bits don't equal 4 or 5 so we reject. */
    u8 ver = i2c_recv_byte(I2C_5, addr, MAX77812_REG_VERSION) & MAX77812_VERSION_MASK;
    return (ver == MAX77812_ES2_VERSION || ver == MAX77812_QS_VERSION) ? ver : 0xFF;
}

static void probe_max77812(void)
{
    u32 chip_major = (APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF;
    if (chip_major != 2) {
        /* Erista doesn't have MAX77812; skip silently to keep the
         * page list clean rather than emitting a "N/A" stub. */
        return;
    }
    HEADER("[MAX77812 CPU/GPU/DRAM buck (Mariko), I2C5 @ 0x33/0x31]");

    u8 addr = MAX77812_PHASE211_CPU_I2C_ADDR;
    u8 ver  = _max77812_probe_addr(addr);
    bool phase211 = true;
    if (ver == 0xFF) {
        addr = MAX77812_PHASE31_CPU_I2C_ADDR;
        ver  = _max77812_probe_addr(addr);
        phase211 = false;
    }
    if (ver == 0xFF) {
        log_color(COL_ERR, "  No MAX77812 at either 0x33 or 0x31\n");
        dx_set("max77812", DX_FAIL, "no MAX77812 ack");
        return;
    }
    static const char *ver_str[] = {
        [MAX77812_ES2_VERSION] = "ES2", [MAX77812_QS_VERSION] = "QS"
    };
    log_color(COL_OK,
        "  Variant      : %s @ 0x%02X (%s silicon)\n",
        phase211 ? "PHASE211 (retail)" : "PHASE31 (dev kit)",
        addr, ver_str[ver]);

    u8 en_ctrl   = i2c_recv_byte(I2C_5, addr, MAX77812_REG_EN_CTRL);
    u8 buck_stat = i2c_recv_byte(I2C_5, addr, MAX77812_REG_BUCK_STAT);
    u8 topsys    = i2c_recv_byte(I2C_5, addr, MAX77812_REG_TOPSYS_STAT);

    /* Per-phase config: name + register offset + sane voltage range.
     * On PHASE211 only M1 / M3 / M4 are populated; M2 reads back as
     * the M1 master because of internal phase tying. We only enumerate
     * the populated rails per variant. */
    struct rail {
        u8 vout_reg;
        u8 en_mask;
        const char *name;
        u32 vmin_mv;
        u32 vmax_mv;
    };
    /* PHASE211 retail layout: M1 = GPU, M3 = LPDDR4 VDD2 (1.1 V),
     * M4 = CPU. Sane operating bands chosen to span the DVFS range
     * documented for HOS plus a safety margin. */
    static const struct rail rails_phase211[] = {
        {MAX77812_REG_M1_VOUT, MAX77812_EN_CTRL_EN_M1_MASK, "M1 (GPU) ",  500, 1300},
        {MAX77812_REG_M3_VOUT, MAX77812_EN_CTRL_EN_M3_MASK, "M3 (DRAM)",  900, 1200},
        {MAX77812_REG_M4_VOUT, MAX77812_EN_CTRL_EN_M4_MASK, "M4 (CPU) ",  600, 1300},
    };
    static const struct rail rails_phase31[] = {
        {MAX77812_REG_M1_VOUT, MAX77812_EN_CTRL_EN_M1_MASK, "M1 (GPU) ",  500, 1300},
        {MAX77812_REG_M4_VOUT, MAX77812_EN_CTRL_EN_M4_MASK, "M4 (CPU) ",  600, 1300},
    };
    const struct rail *rails  = phase211 ? rails_phase211 : rails_phase31;
    size_t       n_rails = phase211 ? sizeof(rails_phase211)/sizeof(rails_phase211[0])
                                    : sizeof(rails_phase31)/sizeof(rails_phase31[0]);

    int n_oob = 0, n_on = 0;
    for (size_t i = 0; i < n_rails; i++) {
        u8 vout = i2c_recv_byte(I2C_5, addr, rails[i].vout_reg);
        u32 mv  = 250 + (vout & MAX77812_BUCK_VOLT_MASK) * 5;
        bool en = (en_ctrl & rails[i].en_mask) != 0;
        bool oob = en && (mv < rails[i].vmin_mv || mv > rails[i].vmax_mv);
        if (en) n_on++;
        if (oob) n_oob++;
        u32 col = oob ? COL_ERR : (en ? COL_OK : COL_DEFAULT);
        log_color(col,
            "  %s    : %s  %d.%03d V  (range %d.%03d - %d.%03d V)%s\n",
            rails[i].name,
            en ? "ON " : "off",
            mv / 1000, mv % 1000,
            rails[i].vmin_mv / 1000, rails[i].vmin_mv % 1000,
            rails[i].vmax_mv / 1000, rails[i].vmax_mv % 1000,
            oob ? " WRONG" : "");
    }

    /* In RCM the BPMP runs hwtest with the A57 CPU and GPU offline and
     * DRAM clocked through MAX77620 SD1 directly. MAX77812 is therefore
     * expected to be present-but-dormant: chip ACKs, all rails off,
     * EN_CTRL=0, no fault latches. Once HOS boots it brings the chip up.
     * Surface this as a friendly "dormant" line instead of three "off"
     * rails the tech might misread as faults. */
    if (n_on == 0)
        log_color(COL_DEFAULT,
            "                 (all rails dormant - normal in RCM, HOS brings them up)\n");

    LOG("  EN_CTRL      : 0x%02X\n", en_ctrl);
    log_color(buck_stat ? COL_WARN : COL_OK,
        "  BUCK_STAT    : 0x%02X%s\n", buck_stat,
        buck_stat ? " (per-rail fault latch set - check rails above)" : "");
    log_color(topsys ? COL_WARN : COL_OK,
        "  TOPSYS_STAT  : 0x%02X%s\n", topsys,
        topsys ? " (system-level fault: thermal / OV / UV)" : "");

    /* Verdict: only fault on real signals. Rail-off in RCM is expected,
     * not a fault. We FAIL when a rail is ON but VOUT is out of band
     * (mis-programmed regulator) or when BUCK/TOPSYS latches are set. */
    dx_set("max77812",
        (n_oob || buck_stat || topsys) ? DX_FAIL : DX_PASS,
        n_oob     ? "%d rails out of band" :
        buck_stat ? "BUCK_STAT 0x%02X"     :
        topsys    ? "TOPSYS_STAT 0x%02X"   : "",
        n_oob ? n_oob : (buck_stat ? buck_stat : topsys));
}

static void probe_battery(void)
{
    int v = 0;
    HEADER("[MAX17050 fuel gauge, I2C1 @ 0x36]");

    if (max17050_get_property(MAX17050_RepSOC, &v) == 0) {
        int soc_int = (v >> 8) & 0xFF;
        log_color(health_color(soc_int, 30, HEALTH_NONE, 10, HEALTH_NONE),
            "  SOC          : %d.%d %%\n", soc_int, ((v & 0xFF) * 10) / 256);
    }
    if (max17050_get_property(MAX17050_VCELL, &v) == 0) {
        log_color(health_color(v, 3700, 4250, 3500, 4350),
            "  VCELL        : %d mV\n", v);
    }
    if (max17050_get_property(MAX17050_OCVInternal, &v) == 0)
        LOG("  OCV          : %d mV\n", v);
    if (max17050_get_property(MAX17050_TEMP, &v) == 0) {
        int t10 = v;
        log_color(health_color(t10 / 10, 5, 40, 0, 50),
            "  Temp         : %d.%d C\n",
            t10 / 10, (t10 >= 0 ? t10 : ~t10) % 10);
    }
    if (max17050_get_property(MAX17050_AvgCurrent, &v) == 0)
        LOG("  AvgCurrent   : %d mA\n", v / 1000);
    if (max17050_get_property(MAX17050_Current, &v) == 0)
        LOG("  Current      : %d mA\n", v / 1000);

    int rep = 0, full = 0, design = 0;
    max17050_get_property(MAX17050_RepCap,    &rep);
    max17050_get_property(MAX17050_FullCAP,   &full);
    max17050_get_property(MAX17050_DesignCap, &design);
    LOG("  RepCap       : %d mAh\n", rep);
    /* Battery health = full / design. Below 80 % means cells are aging. */
    int health_pct = design ? (full * 100 / design) : 0;
    log_color(health_color(health_pct, 80, HEALTH_NONE, 60, HEALTH_NONE),
        "  FullCap      : %d mAh (%d%% of design)\n", full, health_pct);
    LOG("  DesignCap    : %d mAh\n", design);
    dx_set("batt_health",
        health_pct < 60 ? DX_FAIL :
        health_pct < 80 ? DX_WARN : DX_PASS,
        health_pct < 80 ? "%d%% of design" : "", health_pct);

    /* Verdict signal from the temp read above (`v` was last assigned
     * MAX17050_TEMP). Negative = NTC disconnected, > 45 C = heat stress. */
    int t10 = 0;
    max17050_get_property(MAX17050_TEMP, &t10);
    dx_set("batt_ntc",
        t10 < 0      ? DX_FAIL :
        t10 / 10 >= 45 ? DX_WARN : DX_PASS,
        t10 < 0      ? "%d.%d C (NTC disconnected?)" :
        t10 / 10 >= 45 ? "%d.%d C hot" : "",
        t10 / 10, t10 < 0 ? -t10 % 10 : t10 % 10);

    if (max17050_get_property(MAX17050_MinVolt, &v) == 0)
        LOG("  Min volt     : %d mV\n", v);
    if (max17050_get_property(MAX17050_MaxVolt, &v) == 0)
        LOG("  Max volt     : %d mV\n", v);
    if (max17050_get_property(MAX17050_V_empty, &v) == 0)
        LOG("  V_empty      : %d mV\n", v);
    if (max17050_get_property(MAX17050_Age, &v) == 0)
        LOG("  Age          : %d %%\n", v);
    if (max17050_get_property(MAX17050_Cycles, &v) == 0) {
        log_color(health_color(v, HEALTH_NONE, 200, HEALTH_NONE, 500),
            "  Cycles       : %d\n", v);
    }

    /* Predictive / learning fields not exposed by max17050_get_property:
     *   TTE  (REG11) : time-to-empty, LSB = 5.625 s. 0xFFFF = invalid /
     *                  charging. A live, settled gauge under load reports
     *                  a real number here; persistent 0 / 0xFFFF on a
     *                  discharging unit means the gauge can't predict.
     *   QH   (REG4D) : coulomb counter high word.
     *   QL   (REG4E) : coulomb counter low word.
     *   dQacc (REG45)/dPacc (REG46) : learning slew accumulators - non-
     *                  zero means the gauge is still relearning capacity.
     * The full QH:QL read is the absolute coulomb count since gauge POR;
     * we expose both halves so a tech can spot a stuck counter (both
     * always-zero) without doing the per-LSB unit math at this stage. */
    u16 tte = 0, qh = 0, ql = 0, dqacc = 0, dpacc = 0;
    i2c_recv_buf_small((u8 *)&tte,   2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_TTE);
    i2c_recv_buf_small((u8 *)&qh,    2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_QH);
    i2c_recv_buf_small((u8 *)&ql,    2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_QL);
    i2c_recv_buf_small((u8 *)&dqacc, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_dQacc);
    i2c_recv_buf_small((u8 *)&dpacc, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_dPacc);
    if (tte == 0xFFFF) {
        LOG("  TTE          : invalid / charging\n");
    } else {
        /* tte * 5.625 sec -> minutes = tte * 5625 / (60 * 1000) = tte * 9 / 96. */
        u32 min = ((u32)tte * 9) / 96;
        LOG("  TTE          : 0x%04X (%d min)\n", tte, min);
    }
    /* Slash-separated pair name in the key column: the host parser
     * splits each row on the first colon, so "QH:QL" would put "QL ..."
     * into the value column and "QH" into the key. Slash is safe. */
    LOG("  QH/QL        : 0x%04X / 0x%04X (coulomb counter, raw)\n", qh, ql);
    log_color((dqacc | dpacc) ? COL_DEFAULT : COL_WARN,
        "  dQacc/dPacc  : 0x%04X / 0x%04X%s\n", dqacc, dpacc,
        (dqacc | dpacc) ? " (gauge learning)" : " (no learning activity)");

    /* Identity + STATUS + FSTAT come from raw I2C reads, Hekate's
     * max17050_get_property only handles the curated charging fields
     * above. STATUS bit definitions (per the datasheet REG00):
     *   bit 15 BR  : battery-remove since last clear
     *   bit 14 SMX : SOC max alert
     *   bit 13 TMX : temp max alert
     *   bit 12 VMX : volt max alert
     *   bit 11 BI  : battery-insert since last clear
     *   bit 10 SMN : SOC min alert
     *   bit  9 TMN : temp min alert
     *   bit  8 VMN : volt min alert
     *   bit  3 BST : battery status (1 = absent)
     *   bit  1 POR : power-on reset latch
     *
     * FSTAT (REG3D) reflects the fuel-gauge internal state machine
     * (per Maxim MAX17050 datasheet + Linux max17042_battery driver):
     *   bit 0 DNR    : data not ready (still booting up)
     *   bit 6 RelDt2 : long-relax mode achieved (current near 0 for
     *                   extended time, gauge re-calibrated)
     *   bit 7 FQ     : full-charge detected during last cycle
     *   bit 8 EDet   : empty detection
     *   bit 9 RelDt  : relax mode active (current near 0 right now)
     * Bits 10-15 are reserved internal state, may read non-zero.
     *
     * DevName (REG21) returns 0x00AC for genuine MAX17050. */
    u16 status = 0, fstat = 0, devname = 0, manname = 0;
    i2c_recv_buf_small((u8 *)&status,  2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_STATUS);
    i2c_recv_buf_small((u8 *)&fstat,   2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_FSTAT);
    i2c_recv_buf_small((u8 *)&devname, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_DevName);
    i2c_recv_buf_small((u8 *)&manname, 2, I2C_1, MAXIM17050_I2C_ADDR, MAX17050_ManName);

    log_color(devname == 0x00AC ? COL_OK : COL_WARN,
        "  DevName      : 0x%04X%s\n", devname,
        devname == 0x00AC ? " (MAX17050 verified)" : " (UNEXPECTED, not MAX17050?)");
    dx_set("fuel_devname", devname == 0x00AC ? DX_PASS : DX_FAIL,
        devname == 0x00AC ? "" : "DevName 0x%04X != MAX17050", devname);
    LOG("  ManName      : 0x%04X\n", manname);

    /* STATUS decode, POR alone is normal at first boot after pack
     * reset/battery swap. BR/BI alone are normal at insertion. Persistent
     * VMN/TMN/SMN/VMX/TMX/SMX would be the actually-interesting alarms. */
    char sbuf[96] = {0};
    if (status == 0) {
        log_color(COL_OK, "  STATUS       : 0x0000 (no alerts)\n");
    } else {
        if (status & 0x8000) strcat(sbuf, "BR,");
        if (status & 0x4000) strcat(sbuf, "SMX,");
        if (status & 0x2000) strcat(sbuf, "TMX,");
        if (status & 0x1000) strcat(sbuf, "VMX,");
        if (status & 0x0800) strcat(sbuf, "BI,");
        if (status & 0x0400) strcat(sbuf, "SMN,");
        if (status & 0x0200) strcat(sbuf, "TMN,");
        if (status & 0x0100) strcat(sbuf, "VMN,");
        if (status & 0x0008) strcat(sbuf, "BST(absent),");
        if (status & 0x0002) strcat(sbuf, "POR,");
        u32 n = strlen(sbuf);
        if (n) sbuf[n - 1] = 0;
        /* POR-only or BI-only is benign; anything else flagged warn. */
        bool benign = !(status & ~(0x0002 | 0x0800));
        log_color(benign ? COL_DEFAULT : COL_WARN,
            "  STATUS       : 0x%04X (%s)\n", status, sbuf);
    }
    /* Verdict signal: POR latch indicates a fresh power-cycle of the
     * gauge IC (typical: battery swap), readings above are advisory
     * until a full charge-discharge cycle relearns the cell. */
    dx_set("fuel_por", (status & 0x0002) ? DX_WARN : DX_PASS,
        (status & 0x0002) ? "POR latched (relearn pending)" : "");

    /* FSTAT: RelDt set means current is near zero and the fuel-gauge
     * values you're reading are fully trustworthy. DNR set means the
     * gauge is still warming up. We mask off the reserved high bits
     * (10-15) before scanning so noise in those bits doesn't get
     * misclassified. */
    char fbuf[80] = {0};
    if (fstat & 0x0001) strcat(fbuf, "DNR,");
    if (fstat & 0x0040) strcat(fbuf, "RelDt2(long-relax),");
    if (fstat & 0x0080) strcat(fbuf, "FQ(full-charge),");
    if (fstat & 0x0100) strcat(fbuf, "EDet(empty),");
    if (fstat & 0x0200) strcat(fbuf, "RelDt(relaxed),");
    u32 fn = strlen(fbuf);
    if (fn) fbuf[fn - 1] = 0;
    log_color((fstat & 0x0001) ? COL_WARN : COL_DEFAULT,
        "  FSTAT        : 0x%04X (%s)\n", fstat,
        fn ? fbuf : "no documented flags set");
}

static void probe_charger(void)
{
    int v = 0;
    HEADER("[BQ24193 charger, I2C1 @ 0x6B]");

    u8 stat   = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, 0x08); /* status */
    /* BQ24193 FAULT_REG (0x09) is double-read latched: per the datasheet
     * "the first read reports any latched faults since the last read;
     *  subsequent reads report the current fault state". So we read twice
     *, first read shows what HAPPENED (any sticky events from prior PD
     * negotiation, last shutdown, etc.), second read shows what's HAPPENING
     * RIGHT NOW. A latched-but-cleared event (first != 0, second == 0) is
     * benign and tech-normal, it usually just means VBUS dipped during
     * a PD handshake and recovered. A persistent fault (second != 0)
     * means something is actively wrong. */
    u8 fault1 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, 0x09);
    u8 fault2 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, 0x09);
    static const char *vbus[]  = {"none", "USB-SDP", "Adapter", "OTG"};
    static const char *charg[] = {"not charging", "pre", "fast", "done"};
    u8 vb = (stat >> 6) & 3;
    u8 ch = (stat >> 4) & 3;
    u8 pg = (stat >> 2) & 1;

    LOG("  VBUS_STAT    : %d (%s)\n", vb, vbus[vb]);
    LOG("  CHRG_STAT    : %d (%s)\n", ch, charg[ch]);
    /* Power-good must follow VBUS: VBUS present but PG=0 means a charge
     * IC fault, likely BQ or surrounding circuitry. */
    log_color((vb != 0 && pg == 0) ? COL_ERR : COL_OK,
        "  PG_STAT      : %d\n", pg);
    /* Verdict signals: VBUS-vs-Power-Good consistency, and any sticky
     * fault bit on the now-stable read. Bits 7..3 of FAULT_REG are
     * WDOG, BOOST, CHRG[1:0], BAT, NTC[2:0]. */
    dx_set("charger_pg",
        (vb != 0 && pg == 0) ? DX_FAIL : DX_PASS,
        (vb != 0 && pg == 0) ? "VBUS present but PG=0" : "");
    dx_set("charger_fault",
        (fault2 & 0xF8) ? DX_FAIL : DX_PASS,
        (fault2 & 0xF8) ? "FAULT 0x%02X latched" : "", fault2);

    /* Decode all FAULT_REG bits. Bit map (per Hekate's bq24193.h, which
     * matches the BQ24193 datasheet REG09 layout):
     *   bit 7    : WATCHDOG_FAULT
     *   bit 6    : BOOST_FAULT (5V boost / OTG over-current)
     *   bits 5:4 : CHRG_FAULT  (00=normal, 01=input, 10=thermal, 11=safety-timer)
     *   bit 3    : BATT_OVP    (battery over-voltage)
     *   bits 2:0 : NTC_FAULT   (000=normal, 010=warm, 011=cool, 101=cold, 110=hot) */
    static const char *chrg_fault_str[4] = {
        "", "input", "thermal", "safety-timer"};
    static const char *ntc_fault_str[8] = {
        "", "?", "warm", "cool", "?", "cold", "hot", "?"};
    for (int pass = 0; pass < 2; pass++) {
        u8 f = pass ? fault2 : fault1;
        const char *label = pass ? "now      " : "since last";
        char buf[96];
        buf[0] = 0;
        if (f) {
            char tmp[24];
            if (f & 0x80) { strcat(buf, "watchdog,"); }
            if (f & 0x40) { strcat(buf, "boost,"); }
            u8 cf = (f >> 4) & 0x3;
            if (cf)       { s_printf(tmp, "%s,", chrg_fault_str[cf]);
                            strcat(buf, tmp); }
            if (f & 0x08) { strcat(buf, "batt-OVP,"); }
            u8 nf = f & 0x7;
            if (nf)       { s_printf(tmp, "NTC-%s,", ntc_fault_str[nf]);
                            strcat(buf, tmp); }
            u32 n = strlen(buf);
            if (n) buf[n - 1] = 0;            /* strip trailing comma */
        }
        u32 col = (f == 0) ? COL_OK
                : (pass    ? COL_ERR
                           : COL_WARN);       /* latched-only is yellow */
        log_color(col, "  FAULT (%s) : 0x%02X (%s)\n",
            label, f, f == 0 ? "none" : buf);
    }

    if (bq24193_get_property(BQ24193_InputCurrentLimit, &v) == 0)
        LOG("  IN current   : %d mA\n", v);
    if (bq24193_get_property(BQ24193_InputVoltageLimit, &v) == 0)
        LOG("  IN voltage   : %d mV\n", v);
    if (bq24193_get_property(BQ24193_SystemMinimumVoltage, &v) == 0)
        LOG("  System min   : %d mV\n", v);
    if (bq24193_get_property(BQ24193_FastChargeCurrentLimit, &v) == 0)
        LOG("  FastCharge   : %d mA\n", v);
    if (bq24193_get_property(BQ24193_ChargeVoltageLimit, &v) == 0)
        LOG("  CV target    : %d mV\n", v);
    if (bq24193_get_property(BQ24193_ThermalRegulation, &v) == 0)
        LOG("  Therm thresh : %d C (charge-throttle cutoff, IC default 120)\n", v);

    /* BQ24193_TempStatus decodes the NTC fault register: 0 normal,
     * 2 warm, 3 cool, 5 cold, 6 hot. THIS is the real-time battery
     * temperature class. Anything other than 0 means the IC is
     * actively limiting charge based on the battery's NTC sensor. */
    if (bq24193_get_property(BQ24193_TempStatus, &v) == 0) {
        const char *ts = "?";
        u32 tcol = COL_OK;
        switch (v) {
        case 0: ts = "normal"; break;
        case 2: ts = "warm";   tcol = COL_WARN; break;
        case 3: ts = "cool";   tcol = COL_WARN; break;
        case 5: ts = "cold";   tcol = COL_ERR;  break;
        case 6: ts = "hot";    tcol = COL_ERR;  break;
        }
        log_color(tcol, "  Batt NTC     : %d (%s)\n", v, ts);
    }

    if (bq24193_get_property(BQ24193_DevID, &v) == 0)
        LOG("  DevID        : 0x%02X\n", v);

    /* Configuration fields not exposed by bq24193_get_property. These
     * matter when "charging looks fine but never reaches 100 %" or
     * "charger is silently offline":
     *   REG03 IPRECHG  : pre-charge current (high nibble). 128 + N*128 mA
     *                    range 128-2048 mA. 0 means dead-battery
     *                    revival is impossible.
     *   REG03 ITERM    : termination current (low nibble). Same encoding.
     *                    If ITERM is below the cell's natural taper-end
     *                    current, "Done" is never reached.
     *   REG05 ENTIMER  : safety timer enable. Disabled = no upper bound
     *                    on charge time (datasheet warns against this).
     *   REG05 CHGTIMER : safety timer duration (5h/8h/12h/20h).
     *   REG05 WATCHDOG : I2C watchdog (off/40s/80s/160s). HOS resets
     *                    this periodically; if expired the charger
     *                    falls back to default config.
     *   REG07 BATFET_DI: BATFET disable latch. If 1 the battery is
     *                    electrically disconnected from the system rail
     *                    and no amount of VBUS will help. */
    u8 reg03 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_PreChrgTerm);
    u8 reg05 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_ChrgTermTimer);
    u8 reg07 = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_Misc);
    u32 iprechg = ((reg03 >> 4) & 0xF) * 128 + 128;
    u32 iterm   = ((reg03     ) & 0xF) * 128 + 128;
    log_color(iprechg < 256 ? COL_WARN : COL_OK,
        "  IPRECHG      : %d mA (REG03 high nibble)\n", iprechg);
    log_color(iterm < 128 ? COL_WARN : COL_OK,
        "  ITERM        : %d mA (REG03 low nibble)\n", iterm);
    bool entimer = (reg05 & BQ24193_CHRGTERM_ENTIMER_MASK) != 0;
    static const char *chgtimer_str[4] = {"5h", "8h", "12h", "20h"};
    static const char *watchdog_str[4] = {"disabled", "40s", "80s", "160s"};
    u8  chgtimer_idx = (reg05 & BQ24193_CHRGTERM_CHGTIMER_MASK) >> 1;
    u8  wdog_idx     = (reg05 & BQ24193_CHRGTERM_WATCHDOG_MASK) >> 4;
    log_color(entimer ? COL_OK : COL_WARN,
        "  Safety timer : %s (CHGTIMER=%s)\n",
        entimer ? "ON" : "DISABLED", chgtimer_str[chgtimer_idx]);
    LOG("  I2C watchdog : %s\n", watchdog_str[wdog_idx]);
    bool batfet_off = (reg07 & BQ24193_MISC_BATFET_DI_MASK) != 0;
    log_color(batfet_off ? COL_ERR : COL_OK,
        "  BATFET       : %s%s\n",
        batfet_off ? "DISABLED" : "enabled",
        batfet_off ? " (battery isolated from system!)" : "");
    dx_set("charger_batfet", batfet_off ? DX_FAIL : DX_PASS,
        batfet_off ? "BATFET latched off" : "");

    /* Cross-check via Hekate's bq24193_get_version helper, it reads
     * VendorPart (reg 0x0A) and confirms it equals 0x2F for the genuine
     * BQ24193 part. A mismatch here on a unit that ACKs at 0x6B means
     * a non-genuine replacement chip has been swapped in. */
    u32 bqver = 0;
    if (bq24193_get_version(&bqver) == 0) {
        log_color(COL_OK,
            "  Vendor part  : 0x%02X (BQ24193 verified)\n", bqver);
    } else {
        log_color(COL_WARN,
            "  Vendor part  : 0x%02X (UNEXPECTED, not BQ24193?)\n", bqver);
    }
}

static void probe_usbpd(void)
{
    HEADER("[BM92T36 USB-PD, I2C1 @ 0x18]");
    u32 ver = 0;
    int ver_res = bm92t36_get_version(&ver);
    if (ver_res == 0)
        LOG("  Version      : 0x%08X\n", ver);
    else
        log_color(COL_ERR, "  Version read FAILED\n");
    dx_set("usb_pd", (ver_res == 0 && ver != 0) ? DX_PASS : DX_FAIL,
        (ver_res == 0 && ver != 0) ? "" : "BM92T36 not responding");

    bool inserted = false;
    usb_pd_objects_t pd = {0};
    bm92t36_get_source_info(&inserted, &pd);
    LOG("  Cable        : %s\n", inserted ? "inserted" : "not inserted");
    LOG("  PDO count    : %d\n", pd.pdo_no);
    for (u32 i = 0; i < pd.pdo_no && i < 7; i++) {
        LOG("    PDO[%d]    : %d V  %d mA\n", i,
            pd.pdos[i].voltage, pd.pdos[i].amperage);
    }
    if (inserted) {
        LOG("  Selected     : %d V  %d mA\n",
            pd.selected_pdo.voltage, pd.selected_pdo.amperage);
    }

    /* STATUS2 register (0x04) carries the live cable / orientation
     * state. The BM92T36 datasheet exposes:
     *   bit 0..3 : VBUS state, current PD power role, etc.
     *   bit 4    : data-role (UFP=0 / DFP=1)
     *   bit 5    : CC orientation (0 = CC1 normal, 1 = CC2 flipped)
     *   bit 6..7 : SOP'/SOP'' presence (cable-marker chip)
     * A single bad CC pin is one of the most common dock-fault complaints
     * (the user's USB-C port works one way but not the other), so this
     * single bit is worth surfacing alongside the inserted/PDO state. */
    u8 status2[2] = {0};
    if (i2c_recv_buf_big(status2, 2, I2C_1, BM92T36_I2C_ADDR, 0x04) == 0) {
        u8 s2 = status2[0];
        LOG("  STATUS2      : 0x%02X 0x%02X\n", status2[0], status2[1]);
        log_color(COL_OK,
            "  Data role    : %s\n", (s2 & (1u << 4)) ? "DFP (host)" : "UFP (device)");
        log_color(COL_OK,
            "  CC orient.   : %s\n", (s2 & (1u << 5)) ? "CC2 (cable flipped)"
                                                      : "CC1 (cable normal)");
    } else {
        log_color(COL_WARN, "  STATUS2 read FAILED (cable disconnected?)\n");
    }
}

static void probe_5v(void)
{
    HEADER("[5V regulator]");
    /* The 5V regulator drives three downstream loads: the cooling FAN
     * (T210B01 only), and the Joy-Con left / right rails. Each is
     * independently enabled by the bdk on demand. */
    LOG("  FAN          : %s\n",
        regulator_5v_get_dev_enabled(REGULATOR_5V_FAN)  ? "enabled" : "off");
    LOG("  Joy-Con L    : %s\n",
        regulator_5v_get_dev_enabled(REGULATOR_5V_JC_L) ? "enabled" : "off");
    LOG("  Joy-Con R    : %s\n",
        regulator_5v_get_dev_enabled(REGULATOR_5V_JC_R) ? "enabled" : "off");
}

/* MAX77620 internal GPIO state. The PMIC has 8 GPIO pins (GPIO0..GPIO7)
 * exposed at config registers 0x36..0x3D. Each register encodes:
 *   bit 0 : drive type   (0=open-drain, 1=push-pull)
 *   bit 1 : direction    (0=output, 1=input)
 *   bit 2 : input value  (live readback when direction=input)
 *   bit 3 : output value (latch when direction=output)
 *
 * On the Switch, GPIO3/5/6/7 participate in the MAX77620 Flexible
 * Power Sequencer (FPS), the PMIC's autonomous rail-up/rail-down
 * state machine. They are NOT simple "rail enable" outputs in the
 * GPIO-driven sense; they're FPS source/sink signals coupling the
 * PMIC to external enables and to the SoC. The rails themselves
 * (CPU/GPU bucks, DSI 1.2 V, 3.3 V general) come up via FPS timer
 * slots configured in OTP, with these GPIO pins acting as
 * coordinating signals.
 *
 * Hekate's bdk/power/max7762x.h has a brief comment associating
 * these pins with the rails they coordinate with, annotations
 * below mirror that, but treat them as informational rather than
 * "if it's low, the rail is off". The actual rail status is in the
 * Regulators page (probe_regulators). */
static void probe_pmic_gpios(void)
{
    HEADER("[MAX77620 GPIOs]");
    /* GPIO5/GPIO6 are FPS (Flexible Power Sequencer) source pins to the
     * external buck regulator. On Erista that's the dual MAX77621
     * (separate CPU and GPU chips); on Mariko/OLED/Lite it's the
     * multi-phase MAX77812 (M1=GPU, M4=CPU). Pick the label per SoC. */
    bool mariko = (((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2);
    const char *cpu_role = mariko ? "FPS - MAX77812 M4 (CPU)"
                                  : "FPS - CPU MAX77621";
    const char *gpu_role = mariko ? "FPS - MAX77812 M1 (GPU)"
                                  : "FPS - GPU MAX77621";
    const struct {
        u8 idx;
        const char *role;
    } pins[] = {
        {0, "generic"},
        {1, "generic"},
        {2, "generic"},
        {3, "FPS - 3.3V rail"},
        {4, "generic"},
        {5, cpu_role},
        {6, gpu_role},
        {7, "FPS - LDO0 (DSI 1.2V)"},
    };
    for (size_t i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) {
        u8 reg = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR,
                               MAX77620_REG_GPIO0 + pins[i].idx);
        const char *dir = (reg & 0x02) ? "input " : "output";
        const char *drv = (reg & 0x01) ? "push-pull " : "open-drain";
        int in_val  = (reg & 0x04) ? 1 : 0;
        int out_val = (reg & 0x08) ? 1 : 0;
        LOG("  GPIO%d        : 0x%02X  %s %s  IN=%d OUT=%d  (%s)\n",
            pins[i].idx, reg, dir, drv, in_val, out_val, pins[i].role);
    }

    /* FPS (Flexible Power Sequencer) configuration. The MAX77620 has
     * three FPS masters (FPS0, FPS1, FPS2) and each rail / GPIO can be
     * assigned to one of them with a per-rail slot in the sequence.
     * Reading FPS_CFGx surfaces the master config:
     *   bit 0   ENFPS_SW   - SW-controlled enable
     *   bits 2:1 EN_SRC    - which event triggers this FPS master
     *   bits 5:3 TIME_PERIOD - per-slot dwell time, encoded as
     *               40 us << N (so 0=40us .. 6=2560us, 7=reserved).
     * Switch HOS programs FPS0 to a 1280-2560 us slot period; a
     * mis-configured (very short) period can race rail ramp times
     * and explain otherwise-mysterious boot stalls. */
    static const u8 fps_regs[3] = {
        MAX77620_REG_FPS_CFG0, MAX77620_REG_FPS_CFG1, MAX77620_REG_FPS_CFG2
    };
    for (int f = 0; f < 3; f++) {
        u8 cfg = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, fps_regs[f]);
        u8 tp_code = (cfg & MAX77620_FPS_TIME_PERIOD_MASK)
                     >> MAX77620_FPS_TIME_PERIOD_SHIFT;
        u8 en_src  = (cfg & MAX77620_FPS_EN_SRC_MASK)
                     >> MAX77620_FPS_EN_SRC_SHIFT;
        u32 tp_us  = tp_code < 7 ? (40u << tp_code) : 0;
        LOG("  FPS%d cfg     : 0x%02X (slot=%d us, src=%d, sw_en=%d)\n",
            f, cfg, tp_us, en_src, cfg & MAX77620_FPS_ENFPS_SW_MASK);
    }
}

/* Cooling fan probe, Mariko only.
 *
 * Mariko (T210B01) Switch and Mariko Switch Lite both have an active
 * cooling fan driven by Tegra PWM channel 1 with the tach feedback
 * wired to GPIO_PORT_S pin 7. Erista (T210) has passive cooling
 * (fanless) so this probe early-outs there.
 *
 * Two readings:
 *   - PWM duty: instantaneous, derived from PWM_CSR_1 bits 16:23.
 *     0 = fan off, ~236 = max speed (Hekate inverts the polarity
 *     so the register value 0xEC = 0% to the fan).
 *   - Tach RPM: requires counting rising edges on the tach line over
 *     a sampling window. We use 500 ms (vs Hekate's 2 s) to keep the
 *     boot dump fast; this gives ~2x lower precision but still
 *     distinguishes fan-running from fan-stuck.
 *
 * Repair-tech use case: a Mariko unit running hot in HOS with the fan
 * showing 0 RPM here while duty > 0 is a clear "fan dead, replace it"
 * verdict. Fan running at ~2000-4000 RPM with non-zero duty = healthy. */
static void probe_fan(void)
{
    HEADER("[Cooling fan]");

    u32 chip_major = (APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF;
    if (chip_major != 2) {
        log_color(COL_DEFAULT,
            "  Status       : Erista (T210), passive cooling (no fan)\n");
        return;
    }

    /* PWM_CSR_1 layout for the fan channel:
     *   bit 31    : PWM_CSR_EN , channel enable
     *   bit 24    : PWM_CSR_M  , absolute 0% override (Hekate uses
     *                             this to disable the fan without
     *                             touching the duty bits)
     *   bits 23:16: duty (inverted polarity per Hekate fan_set_duty:
     *               register 0 = 100%, register 236 = ~0%)
     *
     * Hekate's own fan_get_speed has a bug where it doesn't check the
     * enable / absolute-0 bits before back-calculating duty, so it
     * reports "duty 236 = max" when the fan is in fact disabled. We
     * decode all three properly. */
    u32 csr      = PWM(PWM_CONTROLLER_PWM_CSR_1);
    bool ch_en   = (csr & (1u << 31)) != 0;
    bool abs_off = (csr & (1u << 24)) != 0;
    u32 inv_duty = (csr >> 16) & 0xFF;
    u32 duty     = (!ch_en || abs_off) ? 0
                 : (inv_duty >= 236)   ? 0
                                       : (236 - inv_duty);
    int duty_pct = (duty * 100) / 236;
    log_color(duty == 0 ? COL_DEFAULT : COL_OK,
        "  PWM duty     : %d/236 (%d%%)%s\n",
        duty, duty_pct,
        !ch_en  ? "  [PWM channel disabled]"
        : abs_off ? "  [absolute 0%% override]"
                  : "");

    /* Tach: count rising edges on PORT_S pin 7 over 500 ms. We don't
     * pre-init the GPIO/PINMUX, if the fan was set up by Hekate's
     * fan_set_duty before launching us (some launch paths do this),
     * the pin is already in input-tristate-with-pullup. If not, we'd
     * read 0 RPM and the operator can compare against duty to spot a
     * driver/fan mismatch. */
    int high = 0, low = 0, edges = 0, last = -1;
    u32 deadline = get_tmr_us() + 500000;     /* 500 ms */
    while ((s32)(deadline - get_tmr_us()) > 0) {
        int v = gpio_read(GPIO_PORT_S, GPIO_PIN_7);
        if (v) high++;
        else low++;
        if (last != -1 && v != last) edges++;
        last = v;
    }
    /* Each fan revolution produces 2 tach pulses (Hekate /2 in
     * fan_get_speed). Edges count both rising AND falling, so divide
     * by 4 to get revolutions during 500 ms, then x120 for RPM. */
    u32 rpm = edges / 4 * 120;
    /* Verdict signal: only flag a stalled fan when we actually
     * commanded one to spin (channel enabled + duty > 0). With duty=0
     * a zero rpm reading is the correct, expected state. */
    if (ch_en && duty > 0) {
        dx_set("fan_stalled", rpm > 0 ? DX_PASS : DX_FAIL,
            rpm > 0 ? "" : "duty %d but rpm 0", duty);
    } else {
        dx_set("fan_stalled", DX_PASS, "");
    }

    if (!ch_en) {
        log_color(COL_DEFAULT,
            "  Tach RPM     : not measured (PWM channel disabled in RCM)\n");
    } else if (duty == 0 && rpm == 0) {
        log_color(COL_DEFAULT,
            "  Tach RPM     : 0 (fan idle - duty=0, expected)\n");
    } else if (duty > 0 && rpm == 0) {
        log_color(COL_ERR,
            "  Tach RPM     : 0 - fan should be spinning (duty %d) but isn't!\n",
            duty);
    } else {
        log_color(COL_OK,
            "  Tach RPM     : ~%d (high=%d low=%d edges=%d in 500 ms)\n",
            rpm, high, low, edges);
    }
}

static void probe_thermal(void)
{
    HEADER("[TMP451 thermal sensor, I2C1 @ 0x4C]");
    u16 soc = tmp451_get_soc_temp(false);
    u16 pcb = tmp451_get_pcb_temp(false);
    int soc_int = soc >> 8;
    int pcb_int = pcb >> 8;
    log_color(health_color(soc_int, HEALTH_NONE, 50, 0, 70),
        "  SoC die temp : %02d.%d C\n", soc_int, (soc & 0xFF) / 10);
    log_color(health_color(pcb_int, HEALTH_NONE, 40, HEALTH_NONE, 55),
        "  PCB temp     : %02d.%d C\n", pcb_int, (pcb & 0xFF) / 10);

    /* Status register (0x02) and conversion rate. TMP451 status:
     *   bit 7 BUSY, bit 6 LHIGH, bit 5 LLOW, bit 4 RHIGH, bit 3 RLOW,
     *   bit 2 OPEN (remote diode open circuit - SoC sensor unhooked),
     *   bit 1 RTHRM, bit 0 LTHRM (THERM2 trip flags).
     * OPEN is the failure mode that matters most for repair: a mechanically
     * loose or unsoldered diode line on the SoC under-fill.
     *
     * Note on register addresses: TMP451 uses split read / write
     * addresses for the configuration and limit registers (the BDK's
     * TMP451_CNV_RATE_REG = 0x0A is the *write* address). To read
     * them back we use the corresponding read addresses:
     *   0x03 = Config (read)        | 0x09 (write)
     *   0x04 = Cnv rate (read)      | 0x0A (write)
     *   0x05 = Local high limit     | 0x0B (write)
     *   0x06 = Local low limit      | 0x0C (write)
     *   0x07 = Remote high limit MSB| 0x0D (write)
     *   0x08 = Remote low limit MSB | 0x0E (write) */
    u8 status   = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x02);
    u8 cnv      = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x04);
    u8 lhigh    = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x05);
    u8 llow     = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x06);
    u8 rhigh    = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x07);
    u8 rlow     = i2c_recv_byte(I2C_1, TMP451_I2C_ADDR, 0x08);
    bool open = (status & 0x04) != 0;
    log_color(open ? COL_ERR : COL_OK,
        "  Status       : 0x%02X%s%s%s%s%s%s\n", status,
        open            ? " OPEN(remote-diode)" : "",
        (status & 0x80) ? " BUSY"   : "",
        (status & 0x40) ? " LHIGH"  : "",
        (status & 0x20) ? " LLOW"   : "",
        (status & 0x10) ? " RHIGH"  : "",
        (status & 0x08) ? " RLOW"   : "");
    /* Conversion rate: 2^cnv / 16 Hz (0x06 = 4 Hz, default after init).
     *   cnv >= 4: rate = 1 << (cnv - 4) Hz (1, 2, 4, 8, 16, 32, 64)
     *   cnv <  4: rate = 1 / (1 << (4 - cnv)) Hz (1/16, 1/8, 1/4, 1/2) */
    static const char *cnv_str[] = {
        "1/16 Hz", "1/8 Hz", "1/4 Hz", "1/2 Hz",
        "1 Hz", "2 Hz", "4 Hz", "8 Hz",
        "16 Hz", "32 Hz", "64 Hz"
    };
    LOG("  Cnv rate     : 0x%02X (%s)\n", cnv,
        cnv < (sizeof(cnv_str)/sizeof(cnv_str[0])) ? cnv_str[cnv] : "?");

    /* Alert thresholds. The IC raises THERM and ALERT lines when
     * the corresponding limit is crossed. Limits are signed degrees
     * Celsius in the 8-bit MSB (the LSB sub-degree limits are not
     * used for thresholding on Switch). HOS / Hekate program these
     * to bracket the operating range; a unit with WRONG limits (or
     * default 0/85 from POR) is interesting because it means
     * software hasn't configured the sensor — we'd see a missing
     * thermal-management init somewhere. */
    LOG("  PCB limits   : low %d C / high %d C\n", (s8)llow, (s8)lhigh);
    LOG("  SoC limits   : low %d C / high %d C\n", (s8)rlow, (s8)rhigh);

    /* Cross-check against MAX17050's battery internal temperature.
     * The three sensors (TMP451 SoC die, TMP451 PCB, MAX17050 battery)
     * sample physically different things but in a console at thermal
     * equilibrium they should agree within ~10-15 C. A bigger spread
     * means one sensor is wrong (loose diode, mis-calibrated gauge,
     * dead thermistor) — surface the worst-pair gap as a diagnostic. */
    int batt_temp = 0;
    bool batt_ok = max17050_get_property(MAX17050_TEMP, &batt_temp) == 0;
    if (batt_ok) {
        /* MAX17050 returns deg C * 10 (centi-degrees). */
        int batt_int = batt_temp / 10;
        int spread = soc_int > pcb_int ? soc_int - pcb_int : pcb_int - soc_int;
        int batt_spread_a = soc_int > batt_int ? soc_int - batt_int : batt_int - soc_int;
        int batt_spread_b = pcb_int > batt_int ? pcb_int - batt_int : batt_int - pcb_int;
        int max_spread = spread;
        if (batt_spread_a > max_spread) max_spread = batt_spread_a;
        if (batt_spread_b > max_spread) max_spread = batt_spread_b;
        log_color(max_spread > 20 ? COL_ERR :
                  max_spread > 12 ? COL_WARN : COL_OK,
            "  Sensor agree : SoC %d C / PCB %d C / Batt %d C  (max gap %d C)\n",
            soc_int, pcb_int, batt_int, max_spread);
        dx_set("temp_agree",
            max_spread > 20 ? DX_FAIL :
            max_spread > 12 ? DX_WARN : DX_PASS,
            max_spread > 12 ? "%d C spread" : "", max_spread);
    }

    /* Verdict signals. Tegra X1 throttles at 85 C; >70 C in handheld
     * idle is suspicious. PCB skin shouldn't exceed ~45 C even under
     * sustained load, so the threshold is tighter on channel 0. An OPEN
     * remote diode invalidates the SoC reading regardless of value. */
    dx_set("soc_die_temp",
        open          ? DX_FAIL :
        soc_int >= 85 ? DX_FAIL :
        soc_int >= 70 ? DX_WARN : DX_PASS,
        open          ? "remote diode OPEN" :
        soc_int >= 85 ? "%d C >= throttle" :
        soc_int >= 70 ? "%d C warm" : "", soc_int);
    dx_set("pcb_temp",
        pcb_int >= 60 ? DX_FAIL :
        pcb_int >= 45 ? DX_WARN : DX_PASS,
        pcb_int >= 60 ? "%d C (thermal pad?)" :
        pcb_int >= 45 ? "%d C warm" : "", pcb_int);
}

/* ------------------------------------------------------------------------ */
/* DRAM identity (LPDDR4 / LPDDR4X via EMC mode-register reads)             */
/*                                                                          */
/* The two LPDDR chips on the SoM identify themselves via JEDEC mode        */
/* registers. Hekate's gui_info reads:                                      */
/*   MR5 : manufacturer ID (1 = Samsung, 6 = Hynix, 255 = Micron, ...)      */
/*   MR6 : revision ID 1                                                    */
/*   MR7 : revision ID 2                                                    */
/*   MR8 : density encoding (bits 5:2)                                      */
/* sdram_read_mrx() drives the EMC bus to issue a mode-register read on     */
/* both chips and both ranks; we surface chip0/chip1 separately so a board  */
/* with one bad LPDDR shows a clear asymmetry. Erista is LPDDR4, Mariko is  */
/* LPDDR4X - the only practical difference here is naming.                  */
static const char *_dram_vendor_name(u8 v)
{
    switch (v) {
    case 1:   return "Samsung";
    case 5:   return "Nanya";
    case 6:   return "Hynix";
    case 8:   return "Winbond";
    case 19:  return "CXMT";
    case 255: return "Micron";
    default:  return NULL;  /* caller prints numeric */
    }
}

static const char *_dram_density_str(u8 mr8)
{
    switch ((mr8 & 0x3C) >> 2) {
    case 2: return "512MB";
    case 3: return "768MB";
    case 4: return "1GB";
    case 5: return "1.5GB";
    case 6: return "2GB";
    default: return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Physical input census - buttons + Joy-Con rail attach + AC adapter       */
/*                                                                          */
/* Surface every human-input pin we can read in one place so a repair tech  */
/* can verify each switch / contact in a single page. All checks are pure   */
/* GPIO / I2C reads, no debounce / no wait.                                 */
/*                                                                          */
/*   POWER       : MAX77620 ONOFFSTAT.EN0 over I2C5 (active high when       */
/*                 button is pressed). Erista routes the same line to a     */
/*                 GPIO too but the resistor is missing on retail boards.   */
/*   VOL+        : PX6 active-low (pressed = 0)                             */
/*   VOL-        : PX7 active-low                                           */
/*   HOME (Sio)  : PY1 active-low; only HOAG (Switch Lite) has it physical  */
/*   JC-R rail   : PH6 active-low (Joy-Con or chassis adapter pulls down)   */
/*   JC-L rail   : PE6 active-low                                           */
/*   AC adapter  : MAX77620 ONOFFSTAT.ACOK = bit 1                          */
/*                                                                          */
/* On a healthy console with no buttons pressed and no Joy-Cons attached    */
/* every line reads "released / empty". With a chassis adapter on JC-R the  */
/* corresponding line reads "ATTACHED" - useful confirmation that the rail  */
/* connector spring contacts are seating properly. */
static void probe_inputs(void)
{
    HEADER("[Input census]");

    /* Power & AC-OK from MAX77620 ONOFFSTAT (reg 0x15). */
    u8 onoffstat = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFSTAT);
    bool power_pressed = (onoffstat & MAX77620_ONOFFSTAT_EN0) != 0;
    bool ac_ok         = (onoffstat & (1u << 1)) != 0;
    log_color(power_pressed ? COL_WARN : COL_OK,
        "  POWER btn  : %s   (ONOFFSTAT.EN0=%d)\n",
        power_pressed ? "PRESSED" : "released", power_pressed);
    log_color(ac_ok ? COL_OK : COL_DEFAULT,
        "  AC adapter : %s    (ONOFFSTAT.ACOK=%d)\n",
        ac_ok ? "present" : "absent ", ac_ok);
    LOG("  ONOFFSTAT  : 0x%02X\n", onoffstat);

    /* VOL +/- = PX6 / PX7, active low. We can't use btn_read() because it
     * also returns POWER and we want each pin individually. */
    int vol_up   = gpio_read(GPIO_PORT_X, GPIO_PIN_6);
    int vol_down = gpio_read(GPIO_PORT_X, GPIO_PIN_7);
    log_color(vol_up   ? COL_OK : COL_WARN, "  VOL+ btn   : %s   (PX6=%d)\n",
        vol_up   ? "released" : "PRESSED ", vol_up);
    log_color(vol_down ? COL_OK : COL_WARN, "  VOL- btn   : %s   (PX7=%d)\n",
        vol_down ? "released" : "PRESSED ", vol_down);

    /* HOME = PY1, active low. Only HOAG (Switch Lite) physically routes
     * this; on Icosa/Iowa/AULA the trace exists but no switch is fitted,
     * so it idles HIGH (or floating, depending on internal pull). */
    bool is_hoag = (fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG);
    int home = gpio_read(GPIO_PORT_Y, GPIO_PIN_1);
    if (is_hoag) {
        log_color(home ? COL_OK : COL_WARN,
            "  HOME btn   : %s   (PY1=%d, Lite-only)\n",
            home ? "released" : "PRESSED ", home);
    } else {
        log_color(COL_DEFAULT,
            "  HOME btn   : N/A (no physical switch on this SKU)   (PY1=%d)\n",
            home);
    }

    /* Joy-Con rail attach: PH6 (R) and PE6 (L), active low. Hekate's
     * _jc_rail_detect briefly enables UART2_TX/UART3_TX as inputs and
     * unlatches via a dummy gpio_read first; for a passive census we
     * skip the unlatch dance and just sample the GPIOs directly.
     * Active-low because a connected Joy-Con (or chassis adapter that
     * shorts the detect line) pulls PH6/PE6 to GND. */
    int jc_r = gpio_read(GPIO_PORT_H, GPIO_PIN_6);
    int jc_l = gpio_read(GPIO_PORT_E, GPIO_PIN_6);
    log_color(!jc_r ? COL_OK : COL_DEFAULT,
        "  JC-R rail  : %s    (PH6=%d, IsAttached active-low)\n",
        !jc_r ? "ATTACHED" : "empty   ", jc_r);
    log_color(!jc_l ? COL_OK : COL_DEFAULT,
        "  JC-L rail  : %s    (PE6=%d, IsAttached active-low)\n",
        !jc_l ? "ATTACHED" : "empty   ", jc_l);

    /* MAX77620 power-button + wake-source configuration. Two registers:
     *   ONOFFCNFG1 (0x41):
     *     bit 7    SFT_RST   - 1=SFT shutdown on long-press / 0=hard reset
     *     bits 5:3 MRT       - manual reset hold time (encoded 2..16 s)
     *     bit 2    SLPEN     - SLEEP enable
     *     bit 1    PWR_OFF   - power-off command latch
     *   ONOFFCNFG2 (0x42):
     *     bit 7 SFT_RST_WK   - wake from sleep on soft-reset event
     *     bit 6 WD_RST_WK    - wake on watchdog reset
     *     bit 4 WK_ACOK      - wake on AC-adapter insert
     *     bit 3 WK_MBATT     - wake on main-battery event
     *     bit 2 WK_ALARM1    - wake on RTC alarm 1
     *     bit 1 WK_ALARM2    - wake on RTC alarm 2
     *     bit 0 WK_EN0       - wake on POWER button press
     * For repair: a tech seeing "POWER btn does nothing" can verify
     * WK_EN0 isn't masked here. A console that wakes spontaneously
     * usually has an unintended wake source set (WK_MBATT on a
     * deteriorating battery is the classic). */
    u8 cnfg1 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG1);
    u8 cnfg2 = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG2);
    u8 mrt   = (cnfg1 & MAX77620_ONOFFCNFG1_MRT_MASK) >> MAX77620_ONOFFCNFG1_MRT_SHIFT;
    /* MRT encoding: each tick = 2 s, range 2..16 s. Switch HOS programs
     * 8 s (mrt code 011) — a held POWER triggers a hard reset after 8 s,
     * matching the documented "press and hold POWER for 8 seconds to
     * force-reset" behavior. */
    LOG("  ONOFFCNFG1 : 0x%02X (MRT=%d -> %d s, %s)\n",
        cnfg1, mrt, (mrt + 1) * 2,
        (cnfg1 & MAX77620_ONOFFCNFG1_SFT_RST) ? "long-press = SFT shutdown"
                                              : "long-press = hard reset");
    LOG("  ONOFFCNFG2 : 0x%02X (wake:%s%s%s%s%s%s%s)\n", cnfg2,
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_EN0)    ? " POWER"   : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_ACOK)   ? " ACOK"    : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_MBATT)  ? " MBATT"   : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_ALARM1) ? " ALARM1"  : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WK_ALARM2) ? " ALARM2"  : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_WD_RST_WK) ? " WDOG"    : "",
        (cnfg2 & MAX77620_ONOFFCNFG2_SFT_RST_WK)? " SFT-RST" : "");
}

/* ------------------------------------------------------------------------ */
/* Joy-Con detection + version (UART_B / UART_C HID)                        */
/*                                                                          */
/* The Joy-Con MCUs talk to the Switch over UART_B (right) and UART_C       */
/* (left) at 1 Mbaud. Detecting them physically is just two GPIO reads      */
/* (PH6 / PE6) and we already do that in the input census; this probe       */
/* adds the *interactive* layer - bringing up the rails, polling the        */
/* Joy-Cons via Hekate's BDK, and reporting battery + BT MAC + connection   */
/* type.                                                                    */
/*                                                                          */
/* It only does the polling in the JC_PROBE Makefile build because Hekate's */
/* joycon.c body is gated on `#if !defined(DEBUG_UART_PORT) ||              */
/* !(DEBUG_UART_PORT)`, in the default build UART_B carries our debug    */
/* log, which is electrically the same line a real Joy-Con-R would speak    */
/* on, so the two uses are mutually exclusive at compile time.              */
/*                                                                          */
/* Build with `make JC_PROBE=1` to drop UART debug and pick up Joy-Con      */
/* polling for both rails. The default build still surfaces the IsAttached  */
/* bits so you know whether a Joy-Con / chassis adapter is physically       */
/* present, just without protocol-level info. */

#ifdef JC_PROBE
/* ------------------------------------------------------------------------ */
/* Joy-Con HID device-info subcommand (0x02) - firmware version fetcher    */
/*                                                                          */
/* Hekate's joycon.c retrieves MAC + type at init via the wired             */
/* JC_WIRED_CMD_GET_INFO (subcmd 0x01) but never sends the *HID*-level     */
/* REQUEST_DEVICE_INFO (subcmd 0x02) which is what carries the firmware     */
/* version major/minor. The HID transport over UART works after the         */
/* Joy-Con has handshaked (jc->state >= HID_CONN). We rebuild the wire      */
/* packet manually here so we don't have to fork joycon.c just to add this  */
/* one command.                                                             */
/*                                                                          */
/* Wire format (from Hekate's jc_wired_hdr_t / jc_hid_out_rpt_t):           */
/*                                                                          */
/*   send (host -> Joy-Con):                                                */
/*     19 01 03                  jc_uart_hdr_t magic                        */
/*     12 00                     total_size LE = sizeof(wired_hdr) - 5 + 11 */
/*     92                        cmd = JC_WIRED_HID                         */
/*     00                        subcmd (header-level)                      */
/*     0B 00                     payload_size LE = 11                       */
/*     00                        status                                     */
/*     00                        crc_payload (not enforced)                 */
/*     XX                        crc_hdr = CRC8(cmd..crc_payload, 6 bytes) */
/*    , HID output report payload (11 bytes),                           */
/*     01                        cmd = JC_HID_OUTPUT_RPT                    */
/*     00                        pkt_id (rolling, 0..F)                     */
/*     00 01 40 40 00 01 40 40   rumble (mute pattern)                      */
/*     02                        subcmd = REQUEST_DEVICE_INFO               */
/*                                                                          */
/*   receive (Joy-Con -> host):                                             */
/*     19 81 03                  RX magic                                   */
/*     ...wired header...        cmd 0x92                                   */
/*    , HID input subcmd reply payload,                                 */
/*     21                        cmd = JC_HID_SUBMCD_RPT                    */
/*     pkt_id, conn+batt, btn x3, lstick x3, rstick x3, vib  (12 bytes)    */
/*     82                        submcd_ack (= 0x80 | subcmd)               */
/*     02                        subcmd                                     */
/*     fw_major fw_minor jc_type 02 mac[6:reversed] 01 spi_color  (subcmd_data) */
/*                                                                          */
/* CRC8 polynomial 0x8D, MSB-first - copied from Hekate's _jc_crc.         */
static u8 _jc_crc8(const u8 *data, u16 len)
{
    u8 crc = 0;
    for (u16 i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (u8)((crc << 1) ^ 0x8D) : (u8)(crc << 1);
    }
    return crc;
}

/* Send REQUEST_DEVICE_INFO subcmd to one Joy-Con UART, read reply, extract
 * firmware version. Returns 0 on success, 1 on no-reply / parse failure.
 * Caller is responsible for ensuring the Joy-Con on this UART is already
 * in HID-connected state (joycon_poll has been driven to completion). */
static int probe_jc_fw_version(u32 uart, u8 *fw_major, u8 *fw_minor)
{
    /* Build request (23 bytes total) */
    u8 pkt[23] = {0};
    /* jc_uart_hdr_t */
    pkt[0]  = 0x19; pkt[1] = 0x01; pkt[2] = 0x03;
    pkt[3]  = 18;   pkt[4] = 0;          /* total_size LE = 12 - 5 + 11 = 18 */
    /* jc_wired_hdr_t (cmd, subcmd, payload_size, status, crc_payload, crc_hdr) */
    pkt[5]  = 0x92;                       /* JC_WIRED_HID */
    pkt[6]  = 0;                          /* subcmd (header-level) */
    pkt[7]  = 11;   pkt[8] = 0;          /* payload_size LE = 11 */
    pkt[9]  = 0;                          /* status */
    pkt[10] = 0;                          /* crc_payload */
    pkt[11] = _jc_crc8(&pkt[5], 6);       /* crc_hdr over cmd..crc_payload */
    /* HID output report */
    pkt[12] = 0x01;                       /* JC_HID_OUTPUT_RPT */
    pkt[13] = 0;                          /* pkt_id */
    /* rumble[2] = mute pattern (8 bytes) */
    pkt[14] = 0x00; pkt[15] = 0x01; pkt[16] = 0x40; pkt[17] = 0x40;
    pkt[18] = 0x00; pkt[19] = 0x01; pkt[20] = 0x40; pkt[21] = 0x40;
    pkt[22] = 0x02;                       /* subcmd = REQUEST_DEVICE_INFO */

    uart_send(uart, pkt, sizeof(pkt));
    uart_wait_xfer(uart, UART_TX_IDLE);

    /* Drain UART for ~150 ms collecting bytes into a scratch buffer */
    static u8 rx[256];
    u32 rx_pos = 0;
    u32 deadline = get_tmr_us() + 150000;
    while ((s32)(deadline - get_tmr_us()) > 0 && rx_pos < sizeof(rx)) {
        u8 b;
        if (uart_recv(uart, &b, 1) == 1)
            rx[rx_pos++] = b;
    }

    /* Scan for the wired RX magic followed by a SUBMCD_RPT for 0x02 */
    /* Reply layout: rx[i..i+11] = jc_wired_hdr_t (12 bytes), then the HID
     * input report. The HID report's submcd_ack is at +13 from payload
     * start (after cmd, pkt_id, conn/batt, 3 stick groups, vib_decider).
     * We're looking for: cmd=0x21, submcd_ack=0x82, subcmd=0x02. */
    for (u32 i = 0; i + 30 < rx_pos; i++) {
        if (rx[i] != 0x19 || rx[i+1] != 0x81 || rx[i+2] != 0x03)
            continue;
        if (rx[i+5] != 0x92)               /* not JC_WIRED_HID */
            continue;
        const u8 *hid = &rx[i + 12];        /* HID input report starts here */
        if (hid[0]  != 0x21)                /* not JC_HID_SUBMCD_RPT */
            continue;
        if (hid[13] != 0x82 || hid[14] != 0x02)  /* not the 0x02 reply */
            continue;
        /* subcmd_data starts at hid + 15 */
        *fw_major = hid[15];
        *fw_minor = hid[16];
        return 0;
    }
    return 1;
}
#endif  /* JC_PROBE */

static void probe_joycon(void)
{
    HEADER("[Joy-Con detection + version]");

#ifdef JC_PROBE
    /* In JC_PROBE builds jc_init_hw + initial polling already ran in
     * ipl_main, so by the time this probe runs:
     *   - The PH6/PE6 GPIO buffer was unlatched by _jc_rail_detect
     *     (Hekate's "HW BUG" workaround in joycon.c) and now reads true.
     *   - The Joy-Con MCUs have completed handshake (rail detect ->
     *     wake -> baud-rate-up -> HID input mode) and joycon_poll()
     *     returns valid conn_l / conn_r and bt_conn_* fields.
     * We run a handful more polls here to refresh the report - cheap. */
    for (u32 i = 0; i < 10; i++) {
        joycon_poll();
        msleep(5);
    }
#endif

    /* IsAttached pins (PH6 = R, PE6 = L), active low. In default builds
     * these reads can be stale until something runs the unlatch dance,
     * which we don't do (it would interfere with UART_B debug). The
     * status here is best-effort in default mode; for accurate Joy-Con
     * detection switch to the JC_PROBE build. */
    int jc_r_attached = !gpio_read(GPIO_PORT_H, GPIO_PIN_6);
    int jc_l_attached = !gpio_read(GPIO_PORT_E, GPIO_PIN_6);
    log_color(jc_r_attached ? COL_OK : COL_DEFAULT,
        "  JC-R rail  : %s   (PH6 IsAttached)\n",
        jc_r_attached ? "ATTACHED" : "empty   ");
    log_color(jc_l_attached ? COL_OK : COL_DEFAULT,
        "  JC-L rail  : %s   (PE6 IsAttached)\n",
        jc_l_attached ? "ATTACHED" : "empty   ");

#ifdef JC_PROBE
    LOG("  Joy-Con poll : enabled (JC_PROBE=1 build)\n");
    jc_gamepad_rpt_t *rpt = joycon_poll();
    if (rpt) {
        if (rpt->conn_l) {
            log_color(COL_OK,
                "  JC-L state  : connected, batt=0x%02X chrg=0x%02X type=0x%02X\n",
                rpt->batt_info_l, rpt->batt_chrg_l, rpt->bt_conn_l.type);
        } else if (jc_l_attached) {
            log_color(COL_WARN,
                "  JC-L state  : attached but did not respond over UART (timeout)\n");
        }
        if (rpt->conn_r) {
            log_color(COL_OK,
                "  JC-R state  : connected, batt=0x%02X chrg=0x%02X type=0x%02X\n",
                rpt->batt_info_r, rpt->batt_chrg_r, rpt->bt_conn_r.type);
        } else if (jc_r_attached) {
            log_color(COL_WARN,
                "  JC-R state  : attached but did not respond over UART (timeout)\n");
        }
        /* Pull pairing info (BT MAC of each Joy-Con + the host MAC it
         * was paired to). Two flags come back: is_l_hos / is_r_hos
         * indicating whether each Joy-Con's pairing slot points at this
         * console (HOS = current Switch). Only meaningful when at least
         * one Joy-Con responded. */
        if (rpt->conn_l || rpt->conn_r) {
            bool is_l_hos = false, is_r_hos = false;
            jc_gamepad_rpt_t *pair = jc_get_bt_pairing_info(&is_l_hos, &is_r_hos);
            if (pair) {
                if (rpt->conn_l) {
                    LOG("  JC-L MAC    : %02X:%02X:%02X:%02X:%02X:%02X (paired to this Switch: %s)\n",
                        pair->bt_conn_l.mac[5], pair->bt_conn_l.mac[4],
                        pair->bt_conn_l.mac[3], pair->bt_conn_l.mac[2],
                        pair->bt_conn_l.mac[1], pair->bt_conn_l.mac[0],
                        is_l_hos ? "yes" : "no");
                }
                if (rpt->conn_r) {
                    LOG("  JC-R MAC    : %02X:%02X:%02X:%02X:%02X:%02X (paired to this Switch: %s)\n",
                        pair->bt_conn_r.mac[5], pair->bt_conn_r.mac[4],
                        pair->bt_conn_r.mac[3], pair->bt_conn_r.mac[2],
                        pair->bt_conn_r.mac[1], pair->bt_conn_r.mac[0],
                        is_r_hos ? "yes" : "no");
                }
            }
        }
        /* Firmware version via HID subcmd 0x02 (REQUEST_DEVICE_INFO).
         * Hekate's joycon stack doesn't fetch this - we send the wired
         * HID packet manually. JC-L lives on UART_C, JC-R on UART_B. */
        if (rpt->conn_l) {
            u8 fw_maj = 0, fw_min = 0;
            if (probe_jc_fw_version(UART_C, &fw_maj, &fw_min) == 0)
                log_color(COL_OK,
                    "  JC-L firmware: %d.%02d\n", fw_maj, fw_min);
            else
                log_color(COL_WARN,
                    "  JC-L firmware: read FAILED (no reply within 150 ms)\n");
        }
        if (rpt->conn_r) {
            u8 fw_maj = 0, fw_min = 0;
            if (probe_jc_fw_version(UART_B, &fw_maj, &fw_min) == 0)
                log_color(COL_OK,
                    "  JC-R firmware: %d.%02d\n", fw_maj, fw_min);
            else
                log_color(COL_WARN,
                    "  JC-R firmware: read FAILED (no reply within 150 ms)\n");
        }
    } else {
        log_color(COL_WARN, "  joycon_poll() returned NULL (jc_init_hw failed?)\n");
    }
#else
    log_color(COL_DEFAULT,
        "  Joy-Con poll : disabled (UART_B owned by debug log)\n");
#endif
}

static void probe_dram(void)
{
    HEADER("[DRAM identity]");

    /* HIDREV major: 1 = Erista (LPDDR4), 2 = Mariko (LPDDR4X). The shift
     * returns the major nibble, not a bool, so we have to compare. */
    bool mariko = ((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) >= 2;
    LOG("  Type         : %s\n", mariko ? "LPDDR4X (Mariko)" : "LPDDR4 (Erista)");

    u8 dram_id = fuse_read_dramid(true);
    LOG("  Fuse DRAM ID : %d\n", dram_id);

    emc_mr_data_t v   = sdram_read_mrx(MR5_MAN_ID);
    emc_mr_data_t r0  = sdram_read_mrx(MR6_REV_ID1);
    emc_mr_data_t r1  = sdram_read_mrx(MR7_REV_ID2);
    emc_mr_data_t den = sdram_read_mrx(MR8_DENSITY);

    /* Per-chip readout. chip0 = first LPDDR, chip1 = second LPDDR. */
    for (int c = 0; c < 2; c++) {
        u8 vid = c ? v.chip1.rank0_ch0   : v.chip0.rank0_ch0;
        u8 rev0 = c ? r0.chip1.rank0_ch0 : r0.chip0.rank0_ch0;
        u8 rev1 = c ? r1.chip1.rank0_ch0 : r1.chip0.rank0_ch0;
        u8 d8  = c ? den.chip1.rank0_ch0 : den.chip0.rank0_ch0;

        const char *vname = _dram_vendor_name(vid);
        const char *dname = _dram_density_str(d8);
        u32 col = (vname && dname) ? COL_OK : COL_WARN;

        if (vname)
            log_color(col, "  Chip %d vend  : %s\n", c, vname);
        else
            log_color(col, "  Chip %d vend  : Unknown (id %d)\n", c, vid);
        log_color(col, "  Chip %d rev   : %X.%02X\n", c, rev0, rev1);
        if (dname)
            log_color(col, "  Chip %d dens  : %s\n", c, dname);
        else
            log_color(col, "  Chip %d dens  : Unknown (MR8 0x%02X)\n", c, d8);
    }

    /* Cross-rank symmetry check. If a stacked chip has both ranks, MR5 of
     * rank 0 and rank 1 should match; otherwise we're seeing only one
     * working die. Channels also should match - mismatch = bad LPDDR. */
    bool sym0 = (v.chip0.rank0_ch0 == v.chip0.rank0_ch1) &&
                (r0.chip0.rank0_ch0 == r0.chip0.rank0_ch1);
    bool sym1 = (v.chip1.rank0_ch0 == v.chip1.rank0_ch1) &&
                (r0.chip1.rank0_ch0 == r0.chip1.rank0_ch1);
    log_color((sym0 && sym1) ? COL_OK : COL_WARN,
        "  Channel sym  : chip0 %s, chip1 %s\n",
        sym0 ? "OK" : "MISMATCH",
        sym1 ? "OK" : "MISMATCH");
    dx_set("dram_sym", (sym0 && sym1) ? DX_PASS : DX_FAIL,
        (sym0 && sym1) ? "" : "channel asymmetry");

    /* MR4 Refresh Rate / Temperature class (per JEDEC LPDDR4):
     *   bits 2:0 RR encode:
     *     000 below operating limit (very cold)
     *     001 4x refresh (<=45 C)
     *     010 2x refresh (<=65 C)
     *     011 1x refresh, normal (65..85 C)
     *     100 0.5x refresh (85..95 C, derating required)
     *     101 0.25x refresh (95..105 C, derating)
     *     110 high-temp + DRT (105..125 C, vendor-specific)
     *     111 above operating limit (>125 C, shutdown imminent)
     *   bit 7   TUF (temperature update flag)
     * MR4 reading "high temp" while TMP451 reads cool means the LPDDR
     * has a hotspot or torn die - useful triage signal. */
    emc_mr_data_t mr4 = sdram_read_mrx(MR4_TEMP);
    static const char *rr_str[8] = {
        "below limit",       "4x refresh",          "2x refresh",
        "1x normal",         "0.5x refresh",        "0.25x refresh",
        "high-temp + DRT",   "above limit (>125C)"
    };
    int n_dram_warm = 0;
    for (int c = 0; c < 2; c++) {
        u8 m = c ? mr4.chip1.rank0_ch0 : mr4.chip0.rank0_ch0;
        u8 rr = m & 0x7;
        bool tuf = (m & 0x80) != 0;
        u32 col = (rr >= 6) ? COL_ERR
                : (rr >= 4 || rr <= 1) ? COL_WARN
                : COL_OK;
        if (rr >= 4) n_dram_warm++;
        log_color(col,
            "  Chip %d MR4   : 0x%02X (%s%s)\n",
            c, m, rr_str[rr], tuf ? ", TUF" : "");
    }
    dx_set("dram_mr4", n_dram_warm ? DX_WARN : DX_PASS,
        n_dram_warm ? "%d die in derating mode" : "", n_dram_warm);
}

static void probe_display(void)
{
    HEADER("[DSI panel ID]");
    u32 raw = display_get_verbose_panel_id();
    u16 dec = display_get_decoded_panel_id();
    LOG("  Raw ID bytes : %02X %02X %02X\n",
        raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF);

    bool sentinel = (raw & 0xFFFFFF) == 0xCCCCCC;
    log_color(sentinel ? COL_ERR : COL_OK,
        "  Decoded ID   : 0x%04X%s\n", dec, sentinel ? " (read FAILED)" : "");
    dx_set("dsi_id", sentinel ? DX_FAIL : DX_PASS,
        sentinel ? "DSI ID read failed (cable?)" : "");

    const char *name = "Unknown";
    bool known = true;
    switch (dec) {
    case PANEL_JDI_XXX062M:     name = "JDI 062M (generic)"; break;
    case PANEL_JDI_LAM062M109A: name = "JDI LAM062M109A";    break;
    case PANEL_JDI_LPM062M326A: name = "JDI LPM062M326A";    break;
    case PANEL_INL_P062CCA_AZ1: name = "InnoLux P062CCA";    break;
    case PANEL_AUO_A062TAN01:   name = "AUO A062TAN";        break;
    case PANEL_INL_2J055IA_27A: name = "InnoLux 2J055IA";    break;
    case PANEL_AUO_A055TAN01:   name = "AUO A055TAN";        break;
    case PANEL_SHP_LQ055T1SW10: name = "Sharp LQ055T1SW10";  break;
    case PANEL_SAM_AMS699VC01:  name = "Samsung AMS699VC01"; break;
    default: known = false; break;
    }
    log_color(known ? COL_OK : (sentinel ? COL_ERR : COL_WARN),
        "  Model        : %s\n", name);
}

/* ------------------------------------------------------------------------ */
/* Display backlight & PWM controller                                       */
/*                                                                          */
/* On Switch the backlight is driven by two paths depending on the panel:   */
/*  - Older LCD panels (JDI, InnoLux, Sharp): Tegra PWM controller channel  */
/*    0 (PWM_CSR_0 @ 0x7000A000) outputs a square wave on LCD_BL_PWM (pin   */
/*    PV0/PV1 muxed to PWM0). Hekate's display_init() ramps it to duty 150  */
/*    out of 255 (~58 %) so the screen is visibly lit when our payload      */
/*    runs.                                                                 */
/*  - Newer Mariko OLED panels (Samsung): the brightness command rides over */
/*    DSI itself, and Hekate stashes the level in a DC scratch register at  */
/*    DC_DCS_BACKLIGHT_LEVEL (display_a + 0x68C) for state recovery.        */
/*                                                                          */
/* In both cases the rail enable is GPIO PV0; the muxed function on the     */
/* PWM pin (LCD_BL_PWM) tells us which control path is in use. A duty of 0  */
/* with the rail on, or the rail off with a non-zero duty, is a backlight   */
/* fault that's worth flagging in yellow.                                   */
static void probe_backlight(void)
{
    HEADER("[Display backlight & PWM]");

    u32 pwm_csr0  = PWM(PWM_CONTROLLER_PWM_CSR_0);
    bool pwm_en   = (pwm_csr0 & PWM_CSR_EN) != 0;
    u32  pwm_duty = (pwm_csr0 >> 16) & 0xFF;
    LOG("  PWM_CSR_0    : 0x%08X\n", pwm_csr0);
    log_color(pwm_en ? COL_OK : COL_WARN,
        "  PWM enabled  : %s\n", pwm_en ? "yes" : "no");
    if (pwm_en) {
        u32 pct = (pwm_duty * 100) / 255;
        u32 col = pwm_duty == 0 ? COL_WARN
                : pwm_duty < 16 ? COL_WARN
                : COL_OK;
        log_color(col, "  PWM duty     : %d / 255 (%d%%)\n", pwm_duty, pct);
    }

    /* DC scratch reg holding the DSI-internal backlight level. Hekate
     * stores duty 0..255 here when the panel uses MIPI DCS for brightness
     * (Samsung OLED on Mariko OLED model). On LCD panels this stays at
     * its boot value and is meaningless. */
    u32 dcs_lvl = DISPLAY_A(DC_DCS_BACKLIGHT_LEVEL);
    LOG("  DCS BL level : 0x%08X (DSI-internal path, OLED panels)\n", dcs_lvl);

    /* Pinmux state for the two backlight pins. PWM mux function 1 = PWM0,
     * function 0 = RSVD0/GPIO. */
    u32 pmx_pwm = PINMUX_AUX(PINMUX_AUX_LCD_BL_PWM);
    u32 pmx_en  = PINMUX_AUX(PINMUX_AUX_LCD_BL_EN);
    u32 pmx_func = pmx_pwm & PINMUX_FUNC_MASK;
    const char *func_name =
        pmx_func == 1 ? "PWM0"  :
        pmx_func == 0 ? "RSVD0" :
                        "other";
    LOG("  BL_PWM mux   : 0x%08X (func %d - %s, %s, %s)\n",
        pmx_pwm, pmx_func, func_name,
        (pmx_pwm & PINMUX_TRISTATE) ? "tristate" : "driven",
        (pmx_pwm & PINMUX_PULL_UP) ? "pull-up" :
        (pmx_pwm & PINMUX_PULL_DOWN) ? "pull-down" : "no pull");
    LOG("  BL_EN  mux   : 0x%08X\n", pmx_en);

    /* GPIO V port pin states. PV0 carries LCD_BL_PWM, PV1 carries
     * LCD_BL_EN. Important: gpio_read() returns GPIO_IN (the actual pin
     * level), not the latched output register. So when the BL_PWM pin
     * is muxed to PWM0, reading PV0 just samples the PWM waveform at an
     * arbitrary moment - high or low purely depends on timing within
     * the ~25 kHz cycle, and tells us nothing about whether the pin is
     * being driven. We surface that caveat instead of judging it. PV1
     * stays in GPIO mode and is meaningful as-is, though some Mariko
     * variants leave it pull-down LOW because the WLED driver IC
     * doesn't need a separate enable - PWM > 0 already lights it. */
    int v0 = gpio_read(GPIO_PORT_V, GPIO_PIN_0);
    int v1 = gpio_read(GPIO_PORT_V, GPIO_PIN_1);
    bool pwm_mode = (pmx_func == 1);
    /* PV0 only carries a meaningful EN signal when the pinmux has the
     * pad in GPIO mode; in PWM mode it's a waveform snapshot, not a
     * static level, so we skip the row entirely rather than print a
     * misleading reading. */
    if (!pwm_mode) {
        log_color(v0 ? COL_OK : COL_WARN,
            "  GPIO PV0 EN  : %s (BL_PWM in GPIO mode)\n",
            v0 ? "HIGH" : "LOW (off)");
    }
    log_color(COL_DEFAULT,
        "  GPIO PV1 SIG : %s (LCD_BL_EN, low on some Mariko panels)\n",
        v1 ? "HIGH" : "LOW");

    /* Real backlight-on test: PWM enabled at non-zero duty with the
     * pinmux routed to PWM0 means the WLED driver is being clocked.
     * This is the strongest signal we can get without poking the
     * driver IC over its own bus (which the Switch doesn't expose). */
    if (pwm_en && pwm_mode && pwm_duty > 0) {
        log_color(COL_OK,
            "  Verdict      : PWM0 driving BL_PWM @ duty %d - backlight lit\n",
            pwm_duty);
        dx_set("backlight", DX_PASS, "");
    } else if (pwm_en && pwm_duty == 0) {
        log_color(COL_WARN,
            "  Verdict      : PWM enabled but duty = 0 (screen dark)\n");
        dx_set("backlight", DX_WARN, "duty 0");
    } else if (!pwm_mode) {
        log_color(COL_WARN,
            "  Verdict      : BL_PWM not muxed to PWM0 - DSI-internal panel?\n");
        dx_set("backlight", DX_WARN, "DSI-internal");
    } else {
        log_color(COL_WARN,
            "  Verdict      : PWM disabled - backlight is off\n");
        dx_set("backlight", DX_WARN, "PWM disabled");
    }
}

static void probe_touch(void)
{
    HEADER("[Touch panel (FTS4, I2C3 @ 0x49)]");

    /* The touch IC is unpowered at boot and I2C3's clock is gated. If we
     * called touch_get_*() now, the i2c_xfer_packet poll loop would spin
     * forever and the watchdog would reset the SoC after ~5 s. Hekate's
     * `touch_power_on()` configures the GPIO reset pin, enables LDO6 (the
     * AVDD/DVDD supply for the panel), pinmux/clock-init's I2C3, releases
     * the reset, waits for the controller-ready event, and runs the
     * sense-enable handshake. Returns 0 on full success, 1 if init
     * retries gave up - we report either way and keep probing for the
     * static info populated during the attempt. */
    int power_rc = touch_power_on();
    log_color(power_rc ? COL_ERR : COL_OK,
        "  power-on     : %s\n", power_rc ? "FAILED (panel cable / IC fault?)" : "OK");

    touch_info_t *info = touch_get_chip_info();
    log_color(info->chip_id == 0x3670 ? COL_OK : COL_ERR,
        "  chip_id      : 0x%04X\n", info->chip_id);
    dx_set("touch_id", info->chip_id == 0x3670 ? DX_PASS : DX_FAIL,
        info->chip_id == 0x3670 ? "" : "chip_id 0x%04X != FTS4", info->chip_id);
    LOG("  fw_ver       : 0x%04X\n", info->fw_ver);
    LOG("  config_id    : 0x%02X\n", info->config_id);
    LOG("  config_ver   : 0x%02X\n", info->config_ver);
    log_color(info->clone ? COL_WARN : COL_OK,
        "  clone        : %s\n", info->clone ? "yes" : "no");

    touch_panel_info_t *panel = touch_get_panel_vendor();
    int panel_idx = panel ? (s8)panel->idx : -2;
    if (panel)
        LOG("  vendor       : %s (idx %d)\n", panel->vendor, panel_idx);
    else
        log_color(COL_ERR, "  vendor       : <error>\n");

    touch_fw_info_t fw = {0};
    if (touch_get_fw_info(&fw) == 0) {
        /* Pairing table mirrors Hekate gui_info.c:1264. */
        struct { u32 fw_id; int idx; } pairs[] = {
            {0x00100100, -1}, {0x00100200, 0}, {0x00120100, 0},
            {0x32000001, 0},  {0x001A0300, 1}, {0x32000102, 1},
            {0x00290100, 2},  {0x32000302, 2}, {0x31051820, 3},
            {0x32000402, 3},  {0x32000501, 4}, {0x33000502, 4},
            {0x33000503, 4},  {0x33000510, 4}, {0xFFFFFFFF, -1},
        };
        int expected = -3;
        for (u32 i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
            if (pairs[i].fw_id == fw.fw_id) { expected = pairs[i].idx; break; }
        bool paired = (expected != -3 && expected == panel_idx);

        LOG("  fw_id        : %02X.%02X.%02X.%02X\n",
            (fw.fw_id >> 24) & 0xFF, (fw.fw_id >> 16) & 0xFF,
            (fw.fw_id >>  8) & 0xFF,  fw.fw_id        & 0xFF);
        LOG("  ftb_ver      : 0x%04X\n", fw.ftb_ver);
        LOG("  fw_rev       : 0x%04X (display %02X%02X)\n",
            fw.fw_rev, fw.fw_rev & 0xFF, (fw.fw_rev >> 8) & 0xFF);
        log_color(paired ? COL_OK : COL_ERR,
            "  pairing      : %s\n", paired ? "OK" : "MISMATCH");
    } else {
        log_color(COL_ERR, "  fw read FAILED\n");
    }
}

/* Rohm BH1730 ambient-light sensor on I2C2 @ 0x29. The ALS sits on the
 * front bezel near the speaker grille and drives auto-brightness on
 * stock HOS. A failing ALS makes the screen stuck at min or max
 * brightness regardless of light, which a tech easily mistakes for a
 * backlight or panel fault. Probe surfaces:
 *   - chip ID byte (BH1730 returns 0x71: part 0x7, rev 0x1)
 *   - configured gain / cycle (driver default = 64x, 38)
 *   - raw visible / IR ADC counts after one integration window
 *   - decoded lux (over-limit flagged when any channel saturates).
 * als_power_on() enables LDO6 (already enabled by the regulator probe
 * but idempotent), pinmuxes I2C2, and triggers a continuous-conversion
 * mode. Default integration time is ~103 ms (cycle * 2.7 ms). */
static void probe_als(void)
{
    HEADER("[BH1730 ambient light sensor, I2C2 @ 0x29]");
    als_ctxt_t ctxt = {0};
    u8 id = als_power_on(&ctxt);
    bool id_ok = (id & 0xF0) == 0x70;
    log_color(id_ok ? COL_OK : COL_ERR,
        "  ID           : 0x%02X%s\n", id,
        id_ok ? " (BH1730 verified)" : " (UNEXPECTED, no chip / I2C fault?)");
    dx_set("als_id", id_ok ? DX_PASS : DX_FAIL,
        id_ok ? "" : "ID 0x%02X != BH1730", id);
    if (!id_ok)
        return;

    LOG("  Gain / cycle : %dx / %d\n",
        (int[]){1,2,64,128}[ctxt.gain & 0x3], ctxt.cycle);

    /* Wait one integration window (~103 ms at default cycle=38) so the
     * first ADC sample is meaningful. The driver's continuous-conversion
     * mode will keep updating after this. */
    msleep(110);
    get_als_lux(&ctxt);
    log_color(ctxt.over_limit ? COL_WARN : COL_OK,
        "  Visible      : %d counts%s\n", ctxt.vi_light,
        ctxt.over_limit ? " (saturated)" : "");
    LOG("  IR           : %d counts\n", ctxt.ir_light);
    LOG("  Lux          : %d\n", ctxt.lux);
}

/* SD/eMMC global init state, reused by probe_storage and save_report. */
static bool g_sd_ok   = false;
static bool g_emmc_ok = false;


/* Translate Hekate's SDHCI_TIMING_* enum into the human-readable speed name
 * Hekate's info screen uses ("HS25 / SDR104 / DDR50 / HS400 ..."). The
 * raw card_clock is also useful so we print both. */
static const char *_sdhci_timing_name(u32 t)
{
    switch (t) {
    case SDHCI_TIMING_MMC_ID:    return "MMC ID";
    case SDHCI_TIMING_MMC_LS26:  return "MMC LS26";
    case SDHCI_TIMING_MMC_HS52:  return "MMC HS52";
    case SDHCI_TIMING_MMC_HS200: return "MMC HS200";
    case SDHCI_TIMING_MMC_HS400: return "MMC HS400";
    case SDHCI_TIMING_SD_ID:     return "SD ID";
    case SDHCI_TIMING_SD_DS12:   return "SD DS12";
    case SDHCI_TIMING_SD_HS25:   return "SD HS25";
    case SDHCI_TIMING_UHS_SDR12: return "UHS SDR12";
    case SDHCI_TIMING_UHS_SDR25: return "UHS SDR25";
    case SDHCI_TIMING_UHS_SDR50: return "UHS SDR50";
    case SDHCI_TIMING_UHS_SDR104:return "UHS SDR104";
    case SDHCI_TIMING_UHS_DDR50: return "UHS DDR50";
    case SDHCI_TIMING_UHS_SDR82: return "UHS SDR82";
    case SDHCI_TIMING_MMC_HS100: return "MMC HS100";
    case SDHCI_TIMING_UHS_DDR200:return "UHS DDR200";
    default:                     return "?";
    }
}

/* Walk Hekate's sd_mode / emmc_mode enums backwards to figure out which
 * fall-through level the controller actually settled on. The init helpers
 * decrement the global mode on each retry, so the post-init value tells us
 * whether the bus came up at full speed or had to drop down. */
static void _print_sd_init_mode(void)
{
    int m = sd_get_mode();
    static const struct { int code; const char *name; u32 col; } ks[] = {
        {SD_UHS_SDR104, "UHS SDR104 (4-bit, max)",   COL_OK},
        {SD_UHS_SDR82,  "UHS SDR82  (4-bit)",        COL_OK},
        {SD_4BIT_HS25,  "HS25       (4-bit)",        COL_WARN},
        {SD_1BIT_HS25,  "HS25       (1-bit, fallback - signal-integrity issue?)", COL_ERR},
        {SD_INIT_FAIL,  "INIT FAILED",               COL_ERR},
    };
    const char *name = "(unknown)";
    u32 col = COL_DEFAULT;
    for (size_t i = 0; i < sizeof(ks)/sizeof(ks[0]); i++)
        if (ks[i].code == m) { name = ks[i].name; col = ks[i].col; break; }
    log_color(col, "  init mode    : %s\n", name);
}

static void _print_emmc_init_mode(void)
{
    int m = emmc_get_mode();
    static const struct { int code; const char *name; u32 col; } km[] = {
        {EMMC_MMC_HS400, "HS400 (8-bit, DDR, max for Switch eMMC)", COL_OK},
        {EMMC_MMC_HS200, "HS200 (8-bit)",  COL_OK},
        {EMMC_8BIT_HS52, "HS52  (8-bit)",  COL_WARN},
        {EMMC_1BIT_HS52, "HS52  (1-bit, fallback)", COL_ERR},
        {EMMC_INIT_FAIL, "INIT FAILED",    COL_ERR},
    };
    const char *name = "(unknown)";
    u32 col = COL_DEFAULT;
    for (size_t i = 0; i < sizeof(km)/sizeof(km[0]); i++)
        if (km[i].code == m) { name = km[i].name; col = km[i].col; break; }
    log_color(col, "  init mode    : %s\n", name);
}

static void probe_storage(void)
{
    HEADER("[SD card (SDMMC1)]");
    /* Skip re-init if ipl_main already initialized SD/eMMC (JC_PROBE
     * build pre-inits to keep SD UHS handshake timing clean). A
     * second sd_initialize() would power-cycle the card and re-attempt
     * UHS negotiation, which can fail the second time and lose the
     * good UHS connection from the first attempt. */
    int sd_res = sd_storage.initialized ? 0 : sd_initialize(false);
    g_sd_ok = (sd_res == 0);
    if (g_sd_ok) {
        LOG("  manfid       : 0x%02X\n", sd_storage.cid.manfid);
        LOG("  oemid        : 0x%04X\n", sd_storage.cid.oemid);
        LOG("  prod_name    : %.5s\n",   sd_storage.cid.prod_name);
        LOG("  hwrev/fwrev  : %d / %d\n", sd_storage.cid.hwrev, sd_storage.cid.fwrev);
        LOG("  serial       : 0x%08X\n", sd_storage.cid.serial);
        LOG("  date         : %02d/%04d\n", sd_storage.cid.month, sd_storage.cid.year);
        LOG("  size         : %d MiB\n", sd_storage.sec_cnt >> 11);

        /* Bus-width + card clock (Hekate's info screen prints these too).
         * 1-bit mode means a signal-integrity fallback; on a healthy
         * card slot we always negotiate to 4-bit. The card_clock field
         * is the actual configured rate after handshake; not the same
         * as the SD spec mode name (which we decode separately). */
        u32 bw = (sd_storage.sdmmc) ? sdmmc_get_bus_width(sd_storage.sdmmc)
                                    : SDMMC_BUS_WIDTH_1;
        int bw_n = bw == SDMMC_BUS_WIDTH_8 ? 8 :
                   bw == SDMMC_BUS_WIDTH_4 ? 4 :
                   bw == SDMMC_BUS_WIDTH_1 ? 1 : 0;
        const char *bw_name =
            bw == SDMMC_BUS_WIDTH_4 ? "4-bit"  :
            bw == SDMMC_BUS_WIDTH_8 ? "8-bit"  :
            bw == SDMMC_BUS_WIDTH_1 ? "1-bit"  :
                                      "?";
        log_color(bw == SDMMC_BUS_WIDTH_4 ? COL_OK : COL_WARN,
            "  bus_width    : %s\n", bw_name);
        dx_set("sd_bus", bw_n >= 4 ? DX_PASS : DX_WARN,
            bw_n >= 4 ? "" : "%d-bit (dirty slot?)", bw_n);
        if (sd_storage.sdmmc) {
            /* sdmmc->card_clock is in kHz (set by clock_sdmmc_get_card_clock_div
             * which uses kHz tables: SDR104 / HS400 = 200000). Format as MHz
             * with two decimals so the natural fractional clocks (~199.68 MHz
             * after divisor rounding) read cleanly. */
            u32 khz = sd_storage.sdmmc->card_clock;
            LOG("  card_clock   : %d.%02d MHz (raw %d kHz)\n",
                khz / 1000, (khz / 10) % 100, khz);
        }
        _print_sd_init_mode();

        /* SD performance classes from SD_STATUS (ACMD13). Already cached
         * in sd_storage.ssr by sd_initialize() so this is just a print.
         * Speed/UHS/Video/App class meanings:
         *   speed_class : 0/2/4/6/10 = no spec / C2 / C4 / C6 / C10
         *                 (sustained sequential write floor in MB/s)
         *   uhs_grade   : 0=none, 1=U1 (10 MB/s), 3=U3 (30 MB/s)
         *   video_class : 0/6/10/30/60/90 = no spec / V6 / V10 / V30 ...
         *                 (sustained video-recording write floor)
         *   app_class   : 0/1/2 = no spec / A1 / A2
         *                 (random IOPS floor: A1=1500R/500W, A2=4000R/2000W)
         * For Switch use, U3 / V30 / A2 is the modern target. Anything
         * lower is functional but explains "loads slowly" complaints
         * for game-card replacement scenarios. */
        const char *uhs_str =
            sd_storage.ssr.uhs_grade == 1 ? "U1" :
            sd_storage.ssr.uhs_grade == 3 ? "U3" : "none";
        char video_buf[8] = "none";
        if (sd_storage.ssr.video_class)
            s_printf(video_buf, "V%d", sd_storage.ssr.video_class);
        const char *app_str =
            sd_storage.ssr.app_class == 1 ? "A1" :
            sd_storage.ssr.app_class == 2 ? "A2" : "none";
        log_color(sd_storage.ssr.speed_class >= 10 ? COL_OK : COL_WARN,
            "  speed class  : C%d\n", sd_storage.ssr.speed_class);
        log_color(sd_storage.ssr.uhs_grade == 3 ? COL_OK :
                  sd_storage.ssr.uhs_grade ? COL_WARN : COL_DEFAULT,
            "  uhs grade    : %s\n", uhs_str);
        log_color(sd_storage.ssr.video_class >= 30 ? COL_OK :
                  sd_storage.ssr.video_class ? COL_WARN : COL_DEFAULT,
            "  video class  : %s\n", video_buf);
        log_color(sd_storage.ssr.app_class == 2 ? COL_OK :
                  sd_storage.ssr.app_class ? COL_WARN : COL_DEFAULT,
            "  app class    : %s\n", app_str);
        /* AU (Allocation Unit) sizes: relevant for filesystem alignment
         * and large-file write performance. uhs_au_size takes precedence
         * if set (UHS-mode AU); falls back to legacy au_size. Encoded as
         * 4-bit tier where N -> 16 KiB << N. */
        u8 au = sd_storage.ssr.uhs_au_size ? sd_storage.ssr.uhs_au_size
                                           : sd_storage.ssr.au_size;
        if (au && au <= 14)
            LOG("  AU size      : %d KiB%s\n", 16 << (au - 1),
                sd_storage.ssr.uhs_au_size ? " (UHS)" : "");
    } else {
        log_color(COL_WARN, "  sd_initialize FAILED (ejected? unsupported?)\n");
    }

    HEADER("[eMMC (SDMMC4)]");
    int emmc_res = emmc_storage.initialized ? 0 : emmc_initialize(false);
    g_emmc_ok = (emmc_res == 0);
    if (g_emmc_ok) {
        LOG("  manfid       : 0x%02X\n", emmc_storage.cid.manfid);
        LOG("  oemid        : 0x%02X\n", (u8)emmc_storage.cid.oemid);
        LOG("  prod_name    : %.6s\n",   emmc_storage.cid.prod_name);
        LOG("  prv          : 0x%02X\n", emmc_storage.cid.prv);
        LOG("  serial       : 0x%08X\n", emmc_storage.cid.serial);
        LOG("  date         : %02d/%04d\n", emmc_storage.cid.month, emmc_storage.cid.year);

        /* Same bus_width / card_clock surface as SD. Switch eMMC always
         * comes up at 8-bit HS400 when healthy; anything narrower or
         * slower is a sign that the negotiation fell back. */
        u32 bw = (emmc_storage.sdmmc) ? sdmmc_get_bus_width(emmc_storage.sdmmc)
                                      : SDMMC_BUS_WIDTH_1;
        int bw_n = bw == SDMMC_BUS_WIDTH_8 ? 8 :
                   bw == SDMMC_BUS_WIDTH_4 ? 4 :
                   bw == SDMMC_BUS_WIDTH_1 ? 1 : 0;
        const char *bw_name =
            bw == SDMMC_BUS_WIDTH_8 ? "8-bit"  :
            bw == SDMMC_BUS_WIDTH_4 ? "4-bit"  :
            bw == SDMMC_BUS_WIDTH_1 ? "1-bit"  :
                                      "?";
        log_color(bw == SDMMC_BUS_WIDTH_8 ? COL_OK : COL_WARN,
            "  bus_width    : %s\n", bw_name);
        dx_set("emmc_bus", bw_n == 8 ? DX_PASS : DX_FAIL,
            bw_n == 8 ? "" : "%d-bit (trace damage?)", bw_n);
        if (emmc_storage.sdmmc) {
            u32 khz = emmc_storage.sdmmc->card_clock;
            LOG("  card_clock   : %d.%02d MHz (raw %d kHz)\n",
                khz / 1000, (khz / 10) % 100, khz);
        }
        _print_emmc_init_mode();
    } else {
        log_color(COL_ERR, "  emmc_initialize FAILED\n");
    }
    /* Suppress unused-fn warnings if either decoder isn't reached. */
    (void)_sdhci_timing_name;
}

static void probe_partitions(void)
{
    HEADER("[eMMC partitions]");
    if (!g_emmc_ok) {
        log_color(COL_ERR, "  eMMC not initialised - re-run probe_storage first\n");
        return;
    }
    /* Sizes come straight from the cached EXT_CSD that sdmmc init populated.
     * BOOT0 / BOOT1 size = BOOT_SIZE_MULT (EXT_CSD[226]) * 128 KiB.
     * RPMB size          = RPMB_SIZE_MULT (EXT_CSD[168]) * 128 KiB.
     * USER sec_cnt is in 512-byte sectors. No partition switch required --
     * the values are sitting in emmc_storage.ext_csd already. */
    u32 boot_kb = emmc_storage.ext_csd.boot_mult * 128;
    u32 rpmb_kb = emmc_storage.ext_csd.rpmb_mult * 128;
    LOG("  USER (GPP)   : %d sectors (%d MiB)\n",
        emmc_storage.sec_cnt, emmc_storage.sec_cnt >> 11);
    LOG("  BOOT0        : %d KiB\n", boot_kb);
    LOG("  BOOT1        : %d KiB\n", boot_kb);
    LOG("  RPMB         : %d KiB\n", rpmb_kb);
}

/* Switch serial number from PRODINFO partition.
 *
 * The PRODINFO partition (GPT entry 0) holds factory calibration data.
 * Per switchbrew.org/wiki/Calibration, the plaintext layout is:
 *   offset 0x000 : "CAL0" magic
 *   offset 0x004 : version (4 bytes)
 *   offset 0x008 : body size (4 bytes)
 *   ...
 *   offset 0x250 : SerialNumber (24 bytes ASCII, null-padded)
 *
 * On the wire PRODINFO is AES-XTS encrypted with BIS key 0 (ks_crypt=0,
 * ks_tweak=1 in Hekate's nx_emmc_bis layer), so a raw sdmmc_storage_read
 * returns ciphertext. To recover plaintext we route through the BIS
 * driver which does the AES-XTS DECRYPT via the SE engine. The keyslots
 * have to be pre-loaded:
 *   - When the user has run Lockpick_RCM previously, prod.keys lives at
 *     sd:/switch/prod.keys. We parse `bis_key_00` from there and load
 *     the two halves into SE slots 0 (crypt) and 1 (tweak) ourselves.
 *   - In the RCM-Emulator with --prod-keys, slots 0/1 are stashed with
 *     bis_key_00 from the keyfile at startup, so the load-from-SD step
 *     is harmless (it just re-loads the same value the emulator already
 *     installed, or no-ops if the keyfile is absent on the emulated SD).
 *   - On a stock console with no prior Lockpick run, we have no keys.
 *     The CAL0 check will fail and the probe reports "encrypted - run
 *     Lockpick to populate sd:/switch/prod.keys".
 *
 * The 24-byte field comfortably holds the standard 14-character Switch
 * serial: XAW10000000000 (Erista launch), XAJ40000000000 (Erista Mariko
 * HAC-001(-01)/V2), XKJ50000000000 (Mariko OLED), XJW70000000000 (Lite),
 * etc. */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Try to populate SE slots 0/1 with bis_key_00 from sd:/switch/prod.keys.
 * Returns 0 on success, non-zero on any failure (file missing, key not
 * present, parse error). The slot writes go directly through the SE
 * engine -- the AES key registers are write-only, so we can't read them
 * back to verify, but a successful CAL0 magic decode in probe_serial
 * confirms the load worked. */
static int load_bis_keys_from_sd(const char **err)
{
    *err = NULL;
    if (!sd_storage.initialized) {
        *err = "SD not ready";
        return 7;
    }

    /* Mount the SD if no other probe has done it yet. probe_serial runs
     * early in the storage page so the SD-content scan and save_report
     * mounts haven't fired yet. f_mount with a fresh FATFS replaces any
     * prior binding cleanly. */
    static FATFS s_bis_fs;
    static bool  s_bis_mounted = false;
    if (!s_bis_mounted) {
        FRESULT mfr = f_mount(&s_bis_fs, "0:", 1);
        if (mfr != FR_OK) {
            *err = "SD f_mount failed";
            return 8;
        }
        s_bis_mounted = true;
    }

    FIL fp;
    FRESULT fr = f_open(&fp, "0:/switch/prod.keys", FA_READ);
    if (fr != FR_OK) {
        *err = "no sd:/switch/prod.keys (run Lockpick first)";
        return 1;
    }

    /* Lockpick's prod.keys is ~14 KiB. Allocate 32 KiB to be safe. */
    u32 cap = 32 * 1024;
    char *body = (char *)malloc(cap + 1);
    if (!body) {
        f_close(&fp);
        *err = "out of memory";
        return 2;
    }
    UINT br = 0;
    fr = f_read(&fp, body, cap, &br);
    f_close(&fp);
    if (fr != FR_OK) {
        free(body);
        *err = "f_read failed";
        return 3;
    }
    body[br] = 0;

    /* Find the line starting with `bis_key_00` followed by `=` or whitespace
     * (so `bis_key_00_alt` or similar can't match by prefix). prod.keys
     * lines are LF-terminated; CR is tolerated. */
    const char *needle = "bis_key_00";
    char *p = body;
    char *line = NULL;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char *start = p;
        while (start < eol && (*start == ' ' || *start == '\t')) start++;
        if ((size_t)(eol - start) >= 11 && memcmp(start, needle, 10) == 0) {
            char after = start[10];
            if (after == ' ' || after == '\t' || after == '=') {
                line = start;
                break;
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    if (!line) {
        free(body);
        *err = "bis_key_00 not in keyfile";
        return 4;
    }

    /* Skip past `bis_key_00`, optional whitespace, `=`, optional ws. */
    char *q = line + 10;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '=') { free(body); *err = "malformed line (no =)"; return 5; }
    q++;
    while (*q == ' ' || *q == '\t') q++;

    /* Parse 64 hex chars into 32 bytes. */
    u8 key[32] __attribute__((aligned(4)));
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(q[i*2]);
        int lo = hex_nibble(q[i*2 + 1]);
        if (hi < 0 || lo < 0) {
            free(body);
            *err = "bad hex in bis_key_00";
            return 6;
        }
        key[i] = (u8)((hi << 4) | lo);
    }
    free(body);

    /* Load into SE: slot 0 = crypt half (low 16), slot 1 = tweak half
     * (high 16). Matches what nx_emmc_bis_init picks for PRODINFO. */
    se_aes_key_set(0, key,      16);
    se_aes_key_set(1, key + 16, 16);
    return 0;
}

static void probe_serial(void)
{
    HEADER("[Switch serial number (PRODINFO)]");
    if (!g_emmc_ok) {
        log_color(COL_ERR, "  eMMC not initialised\n");
        return;
    }

    /* Load BIS keys from SD. We hold off on printing the status until
     * after the CAL0 check so we can colour by outcome: green when keys
     * actually decode, yellow when they're missing OR present-but-wrong
     * (the file loaded but doesn't match this console). A missing or
     * mismatched keyfile is a config issue, not a hardware fault, so
     * neither case warrants red. */
    const char *key_err = NULL;
    bool keys_loaded = (load_bis_keys_from_sd(&key_err) == 0);

    /* Parse the GPT to locate PRODINFO. The LBA range is fixed in stock
     * Switch layouts (34..8191) but we read it from the GPT so we adapt
     * cleanly to non-stock partitionings. */
    LIST_INIT(gpt);
    emmc_gpt_parse(&gpt);
    emmc_part_t *part = emmc_part_find(&gpt, "PRODINFO");
    if (!part) {
        log_color(COL_WARN,
            "  BIS keys     : %s\n",
            keys_loaded ? "loaded from sd:/switch/prod.keys"
                        : (key_err ? key_err : "not loaded"));
        log_color(COL_ERR, "  PRODINFO partition not in GPT\n");
        emmc_gpt_free(&gpt);
        return;
    }

    /* AES-XTS DECRYPT through the SE engine. Cache disabled, two sectors
     * with no reuse, so the 256 MiB cluster cache is wasted overhead.
     * emummc_offset = 0 means "talk to real eMMC", not an SD-backed
     * emuMMC. */
    nx_emmc_bis_init(part, false, 0);

    static u8 hdr[512] __attribute__((aligned(8)));
    static u8 buf[512] __attribute__((aligned(8)));
    int hdr_res = nx_emmc_bis_read(0, 1, hdr);
    int srl_res = nx_emmc_bis_read(1, 1, buf);
    nx_emmc_bis_end();

    bool cal0_ok = !hdr_res && !srl_res &&
                   hdr[0] == 'C' && hdr[1] == 'A' &&
                   hdr[2] == 'L' && hdr[3] == '0';
    /* Two-state finding: pass when keys decoded; warn (not fail) when
     * either keys are absent or wrong-console -- keyfile config is
     * not a hardware fault. */
    dx_set("prodinfo", (keys_loaded && cal0_ok) ? DX_PASS : DX_WARN,
        (keys_loaded && cal0_ok) ? "" :
        keys_loaded ? "decode failed (wrong keys?)" :
                      "no keys");

    /* Status line: green only when keys loaded AND CAL0 actually decoded.
     * Yellow when the keyfile is missing OR loaded but the wrong console
     * (CAL0 came back as gibberish). Phrasing carries an explicit WARN
     * keyword ("invalid", "skipped") so the host viewer's text-driven
     * classifier picks the right severity without relying on ANSI codes
     * (UART_B is plain text). */
    if (keys_loaded && cal0_ok) {
        log_color(COL_OK,
            "  BIS keys     : loaded from sd:/switch/prod.keys\n");
    } else if (keys_loaded && !cal0_ok) {
        log_color(COL_WARN,
            "  BIS keys     : loaded but invalid (wrong unit's prod.keys?)\n");
    } else {
        log_color(COL_WARN,
            "  BIS keys     : skipped (%s)\n",
            key_err ? key_err : "not loaded");
    }

    LOG("  Partition    : LBA %d..%d (%d KiB)\n",
        part->lba_start, part->lba_end,
        ((part->lba_end - part->lba_start + 1) * 512) / 1024);
    emmc_gpt_free(&gpt);

    if (hdr_res || srl_res) {
        log_color(COL_ERR, "  BIS read failed (%d / %d)\n", hdr_res, srl_res);
        return;
    }

    /* Yellow on a CAL0 mismatch, not red: the unit is fine, it's the
     * keyfile that's the problem. The trailing tag carries a WARN
     * keyword for the host viewer; bdk's s_printf doesn't grok the
     * %.Ns precision modifier, so print the 4 magic bytes individually. */
    log_color(cal0_ok ? COL_OK : COL_WARN,
        "  CAL0 magic   : %c%c%c%c%s\n",
        cal0_ok ? hdr[0] : '?', cal0_ok ? hdr[1] : '?',
        cal0_ok ? hdr[2] : '?', cal0_ok ? hdr[3] : '?',
        cal0_ok ? "" : " (invalid - decrypt produced gibberish)");

    if (!cal0_ok) {
        log_color(COL_WARN, "  Serial       : %s\n",
            keys_loaded
                ? "skipped (gibberish, prod.keys not for this device)"
                : "skipped (encrypted, run Lockpick to populate"
                  " sd:/switch/prod.keys)");
        return;
    }

    /* Extract serial. The field is null-padded ASCII; we copy at most
     * 24 bytes and stop at the first non-printable / null. */
    char serial[25] = {0};
    for (int i = 0; i < 24; i++) {
        u8 c = buf[0x50 + i];
        if (c < 0x20 || c >= 0x7F) break;
        serial[i] = (char)c;
    }
    if (serial[0] == 0) {
        log_color(COL_WARN, "  Serial       : (empty / wiped)\n");
    } else {
        log_color(COL_OK, "  Serial       : %s\n", serial);
    }
}

static void probe_emmc_health(void)
{
    HEADER("[eMMC health & EXT_CSD]");
    if (!g_emmc_ok) {
        log_color(COL_ERR, "  eMMC not initialised\n");
        return;
    }
    /* JEDEC PRE_EOL_INFO byte: 0x01 = normal, 0x02 = warning (>=80% used),
     * 0x03 = urgent. DEVICE_LIFE_TIME_EST_TYP_A/B are in 10% buckets:
     * 0x01 = 0-10%, 0x02 = 10-20%, ... 0x0A = 90-100%, 0x0B = exceeded. */
    u8 eol = emmc_storage.ext_csd.pre_eol_info;
    u8 a   = emmc_storage.ext_csd.dev_life_est_a;
    u8 b   = emmc_storage.ext_csd.dev_life_est_b;
    static const char *eol_str[] = {"unknown", "normal", "warning", "urgent"};
    log_color(eol == 1 ? COL_OK : eol == 2 ? COL_WARN : eol == 3 ? COL_ERR : COL_DEFAULT,
        "  PRE_EOL_INFO : 0x%02X (%s)\n", eol, eol < 4 ? eol_str[eol] : "?");
    log_color(a >= 0x09 ? COL_ERR : a >= 0x07 ? COL_WARN : COL_OK,
        "  Life used A  : 0x%02X (%d-%d%%)\n", a,
        a == 0 ? 0 : (a - 1) * 10, a == 0 ? 10 : a == 0x0B ? 100 : a * 10);
    log_color(b >= 0x09 ? COL_ERR : b >= 0x07 ? COL_WARN : COL_OK,
        "  Life used B  : 0x%02X (%d-%d%%)\n", b,
        b == 0 ? 0 : (b - 1) * 10, b == 0 ? 10 : b == 0x0B ? 100 : b * 10);
    LOG("  EXT_CSD rev  : %d\n", emmc_storage.ext_csd.rev);
    LOG("  Card type    : 0x%02X\n", emmc_storage.ext_csd.card_type);
    LOG("  Cache size   : %d KiB\n", emmc_storage.ext_csd.cache_size / 1024);
    LOG("  bkops_en     : 0x%02X\n", emmc_storage.ext_csd.bkops_en);
    /* Verdict signal: PRE_EOL_INFO + worst of life-used A/B. */
    dx_set("emmc_health",
        (eol == 3 || a >= 0x09 || b >= 0x09) ? DX_FAIL :
        (eol == 2 || a >= 0x07 || b >= 0x07) ? DX_WARN : DX_PASS,
        (eol == 3 || a >= 0x09 || b >= 0x09) ? "worn (eol %d, A=%d B=%d)" :
        (eol == 2 || a >= 0x07 || b >= 0x07) ? "aging (eol %d, A=%d B=%d)" : "",
        eol, a, b);
}

static void probe_kfuse(void)
{
    HEADER("[KFUSE (HDCP keys)]");
    /* Hekate's kfuse_wait_ready() is an UNBOUNDED `while (!STATE_DONE);`
     * loop. If the KFUSE block isn't clocked - or the silicon is in a
     * bad state - the payload hangs forever, the BPMP watchdog fires,
     * and modchip-injected setups (picofly) re-inject hwtest in a
     * reset-loop. We replicate kfuse_read()'s clock-enable dance and
     * bound the wait ourselves. */
    clock_enable_kfuse();

    bool done = false, crc_pass = false;
    u32 timeout_us = get_tmr_us() + 100000;   /* 100 ms cap */
    while ((s32)(timeout_us - get_tmr_us()) > 0) {
        u32 state = KFUSE(KFUSE_STATE);
        if (state & KFUSE_STATE_DONE) {
            done     = true;
            crc_pass = (state & KFUSE_STATE_CRCPASS) != 0;
            break;
        }
    }
    LOG("  KFUSE_STATE  : 0x%08X\n", KFUSE(KFUSE_STATE));
    clock_disable_kfuse();

    if (!done) {
        log_color(COL_ERR, "  status       : TIMEOUT (block did not assert DONE)\n");
        dx_set("kfuse", DX_WARN, "timeout");
    } else {
        log_color(crc_pass ? COL_OK : COL_ERR,
            "  status       : %s\n", crc_pass ? "OK (CRC pass)" : "FAILED (CRC bad)");
        dx_set("kfuse", crc_pass ? DX_PASS : DX_FAIL,
            crc_pass ? "" : "CRC bad");
    }
}

/* ------------------------------------------------------------------------ */
/* PMC reset reason + MAX77620 fault latches + RTC                          */
/*                                                                          */
/* Boot forensics. Three sources, all surface why the previous power cycle  */
/* ended:                                                                   */
/*   1. PMC RST_STATUS (reg 0x1B4, low 3 bits) - Tegra-side reason: power-  */
/*      on, watchdog, thermal sensor, software, LP0 wake, AOTAG.            */
/*   2. MAX77620 NVERC (reg 0x0C, latched in non-volatile bits) - PMIC-     */
/*      side reason: forced shutdown, watchdog, hard reset, thermal         */
/*      overload, low-battery shutdown, MBO/MBU power-supply faults.        */
/*   3. MAX77620 INTLBT / IRQSD - thermal-alarm and per-rail power-fault    */
/*      latches captured since the last clear.                              */
/* On a normally-shut-down console all four read 0x00. Any non-zero value   */
/* is a real triage signal - the box rebooted because of *something*. We    */
/* also dump the current RTC time so a tech can correlate "console says     */
/* boot was at 14:23 on 2024-08-12" against their workshop log.             */
/* PMC scratch register decode. The Power Management Controller has
 * 256 32-bit "scratch" registers that survive across SoC resets and
 * deep-sleep cycles, they're how Hekate, HOS package1, fuses ipatch,
 * and Atmosphere stash boot-handover state. Most slots are opaque
 * (used by HOS internals and not externally documented), but a few
 * have well-known meanings that are useful for repair-tech diagnosis:
 *
 *   SCRATCH0  : reboot-mode flags written by Hekate / TZ to choose the
 *               next-boot path. Bits we know:
 *                 BIT(0)  WARMBOOT       (resume from sleep)
 *                 BIT(1)  RCM            (next reboot enters BootROM RCM)
 *                 BIT(29) PAYLOAD        (chainload a payload after pkg1)
 *                 BIT(30) BOOTLOADER     (chainload a CFW bootloader)
 *                 BIT(31) RECOVERY       (force fusee/recovery path)
 *
 *   SCRATCH20 : ODM customer config, bit 18 (0x40000) = "debug console"
 *               flag that Hekate explicitly clears in hw_init.
 *
 *   SCRATCH200: temporarily holds reset cause across the BootROM->bldr
 *               handover; Hekate captures it then zeroes it.
 *
 * Read-only here. Just dump the values + decode the documented bits. */
static void probe_pmc_scratch(void)
{
    HEADER("[PMC scratch]");

    u32 s0   = PMC(APBDEV_PMC_SCRATCH0);
    u32 s20  = PMC(APBDEV_PMC_SCRATCH20);
    u32 s200 = PMC(APBDEV_PMC_SCRATCH200);

    log_color(COL_DEFAULT, "  SCRATCH0     : 0x%08X\n", s0);
    if (s0 & PMC_SCRATCH0_MODE_WARMBOOT)
        LOG("                 - WARMBOOT bit (resume-from-sleep flag set)\n");
    if (s0 & PMC_SCRATCH0_MODE_RCM)
        LOG("                 - RCM bit (next reboot would re-enter BootROM RCM)\n");
    if (s0 & PMC_SCRATCH0_MODE_PAYLOAD)
        LOG("                 - PAYLOAD bit (CFW payload chainload requested)\n");
    if (s0 & PMC_SCRATCH0_MODE_BOOTLOADER)
        LOG("                 - BOOTLOADER bit (CFW bootloader chainload requested)\n");
    if (s0 & PMC_SCRATCH0_MODE_RECOVERY)
        LOG("                 - RECOVERY bit (force fusee/recovery path)\n");

    log_color(COL_DEFAULT, "  SCRATCH20    : 0x%08X\n", s20);
    if (s20 & 0x00040000)
        log_color(COL_INFO,
            "                 - bit 18 set: ODM debug-console flag (HOS would enable kgdb)\n");

    log_color(COL_DEFAULT, "  SCRATCH200   : 0x%08X\n", s200);

    /* Hekate "next-boot intent". Despite living in the MAX77620's RTC
     * scratch slots (2 slots, 6 bits each), this has nothing to do with
     * time -- it's just the only battery-backed scratch storage on the
     * board, so Hekate piggybacks on it to communicate with itself
     * across SoC resets ("next reboot, force the menu" / "next reboot,
     * do UMS"). The reason field decodes to:
     *   0 NOP  , normal boot, no special handling
     *   1 SELF , autoboot_idx into autoboot_list
     *   2 MENU , force Hekate menu
     *   3 UMS  , force USB Mass Storage of partition ums_idx
     *   4 REC  , set SCRATCH0 RECOVERY and reboot to self
     *   5 PANIC, bootloader-side panic (T210B01 only)
     *
     * If the unit unexpectedly enters Hekate menu instead of HOS at
     * every boot, REASON=2 stuck here would explain it. We surface it
     * as "Hekate boot intent" so the line doesn't read like a sibling
     * of the RTC time entries above. */
    rtc_reboot_reason_t rr = {0};
    bool valid = max77620_rtc_get_reboot_reason(&rr);
    static const char *kReasons[6] = {
        "NOP (normal boot)", "SELF (autoboot)", "MENU (force Hekate menu)",
        "UMS (force USB mass storage)", "REC (recovery)", "PANIC"};
    if (!valid) {
        log_color(COL_OK,
            "  Hekate intent: none (normal boot, no override pending)\n");
    } else {
        u32 reason = rr.dec.reason & 0xF;
        log_color(reason == 0 ? COL_OK : COL_INFO,
            "  Hekate intent: %d (%s)\n",
            reason, reason < 6 ? kReasons[reason] : "?");
        if (reason == 1)
            LOG("                 autoboot_idx=%d list=%d\n",
                rr.dec.autoboot_idx, rr.dec.autoboot_list);
        if (reason == 3)
            LOG("                 ums_idx=%d\n", rr.dec.ums_idx);
    }
}

static void probe_reset(void)
{
    HEADER("[Reset + PMIC latches + RTC + Hekate intent]");

    /* 1. Tegra PMC reset status */
    u32 rst = PMC(APBDEV_PMC_RST_STATUS) & PMC_RST_STATUS_MASK;
    static const char *kRstNames[] = {
        "POR (cold boot - power-on reset)",
        "WATCHDOG (BPMP timeout - hung firmware?)",
        "SENSOR (thermal trip - SoC overheat)",
        "SW_MAIN (software reboot)",
        "LP0 (wake from deep sleep)",
        "AOTAG (always-on tag-based reset)",
        "(reserved 6)",
        "(reserved 7)",
    };
    /* POR is the only "expected" reason on a cold boot. Anything else
     * means the previous session ended unexpectedly. */
    u32 col = (rst == PMC_RST_STATUS_POR) ? COL_OK : COL_WARN;
    if (rst == 1 || rst == 2) col = COL_ERR;
    log_color(col, "  PMC RST_STATUS: %d (%s)\n", rst, kRstNames[rst & 7]);

    /* 2. MAX77620 NVERC (non-volatile reset cause) */
    u8 nverc = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_NVERC);
    /* Severe latches = real fault evidence. Soft latches (SHDN/HDRST/
     * RSTIN) are user-triggered and only warn. */
    u8 nverc_severe = nverc & (MAX77620_NVERC_WTCHDG | MAX77620_NVERC_TOVLD |
                               MAX77620_NVERC_MBLSD  | MAX77620_NVERC_MBO   |
                               MAX77620_NVERC_MBU);
    dx_set("pmic_nverc",
        nverc_severe ? DX_FAIL : nverc ? DX_WARN : DX_PASS,
        nverc_severe ? "severe latch 0x%02X" :
        nverc        ? "soft latch 0x%02X" : "", nverc);
    log_color(nverc ? COL_WARN : COL_OK,
        "  PMIC NVERC   : 0x%02X%s\n", nverc, nverc ? "" : " (clean)");
    if (nverc & MAX77620_NVERC_SHDN)    log_color(COL_WARN, "                 - SHDN  : forced shutdown via EN0\n");
    if (nverc & MAX77620_NVERC_WTCHDG)  log_color(COL_ERR,  "                 - WTCHDG: PMIC watchdog tripped\n");
    if (nverc & MAX77620_NVERC_HDRST)   log_color(COL_WARN, "                 - HDRST : hard reset (long power-button)\n");
    if (nverc & MAX77620_NVERC_TOVLD)   log_color(COL_ERR,  "                 - TOVLD : thermal overload trip\n");
    if (nverc & MAX77620_NVERC_MBLSD)   log_color(COL_ERR,  "                 - MBLSD : main-batt low-voltage shutdown\n");
    if (nverc & MAX77620_NVERC_MBO)     log_color(COL_ERR,  "                 - MBO   : main-batt over-voltage\n");
    if (nverc & MAX77620_NVERC_MBU)     log_color(COL_ERR,  "                 - MBU   : main-batt under-voltage\n");
    if (nverc & MAX77620_NVERC_RSTIN)   log_color(COL_WARN, "                 - RSTIN : RST input asserted\n");

    /* 3. INTLBT and IRQSD: latched IRQ events */
    u8 intlbt = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_INTLBT);
    u8 irqsd  = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_IRQSD);
    dx_set("pmic_intlbt", intlbt ? DX_WARN : DX_PASS,
        intlbt ? "thermal/low-batt latch 0x%02X" : "", intlbt);
    /* IRQSD bits latch on every SD-rail buck transition, including the
     * normal cold-boot ramp from off to in-regulation -- the per-bit
     * decode below is informational. Only escalate the verdict when
     * NVERC already shows a severe latch (i.e. the box rebooted because
     * of something), in which case IRQSD pinpoints which rail tripped.
     * Otherwise: PASS, with the bits visible on the detail page for a
     * tech who wants to look. */
    dx_set("pmic_irqsd",
        (irqsd && nverc_severe) ? DX_FAIL : DX_PASS,
        (irqsd && nverc_severe)
            ? "rail-fault latch 0x%02X (NVERC severe)" : "",
        irqsd);
    log_color(intlbt ? COL_WARN : COL_OK,
        "  PMIC INTLBT  : 0x%02X%s\n", intlbt, intlbt ? "" : " (no thermal/low-batt events)");
    if (intlbt & MAX77620_IRQ_TJALRM2_MASK) log_color(COL_ERR,  "                 - TJALRM2: junction-temp alarm 2\n");
    if (intlbt & MAX77620_IRQ_TJALRM1_MASK) log_color(COL_WARN, "                 - TJALRM1: junction-temp alarm 1\n");
    if (intlbt & MAX77620_IRQ_LBM_MASK)     log_color(COL_WARN, "                 - LBM    : low-battery monitor\n");
    /* IRQSD note: the four PFI_SDx bits latch power-fault edges, they
     * fire whenever a SD-rail buck regulator transitions through the
     * fault threshold. The PMIC sets them during:
     *   - chip POR / HDRST as each buck ramps from off to in-regulation
     *   - a controlled shutdown via ONOFFCNFG1.PWR_OFF (the 'q' key on
     *     this payload uses that path), where rails ramp DOWN through
     *     the threshold one at a time, you typically see SD0/SD1/SD3
     *     set, SD2 (PLL) often stays clear because PLL was the first
     *     rail brought down
     *   - real in-band power faults during operation
     *
     * We can't tell the three apart from IRQSD alone, the bit pattern
     * is the same. The NVERC line above carries the actual reset cause
     * (clean = nothing the PMIC noticed; SHDN/HDRST/etc = a recorded
     * event). So we surface IRQSD as info-coloured per-bit lines and
     * let the tech cross-reference NVERC + RST_STATUS instead of
     * raising a yellow flag on every partial pattern. */
    if (irqsd == 0) {
        log_color(COL_OK, "  PMIC IRQSD   : 0x%02X (no SD-rail power faults)\n", irqsd);
    } else {
        log_color(COL_DEFAULT,
            "  PMIC IRQSD   : 0x%02X (SD-rail thresholds latched - cross-check NVERC)\n",
            irqsd);
    }
    if (irqsd & MAX77620_IRQSD_PFI_SD0) log_color(COL_DEFAULT, "                 - PFI_SD0 : CPU rail threshold latched\n");
    if (irqsd & MAX77620_IRQSD_PFI_SD1) log_color(COL_DEFAULT, "                 - PFI_SD1 : DDR rail threshold latched\n");
    if (irqsd & MAX77620_IRQSD_PFI_SD2) log_color(COL_DEFAULT, "                 - PFI_SD2 : PLL rail threshold latched\n");
    if (irqsd & MAX77620_IRQSD_PFI_SD3) log_color(COL_DEFAULT, "                 - PFI_SD3 : 1V8 rail threshold latched\n");

    /* 4. RTC current time. Two things to know about Switch RTC:
     *
     *    (a) Hekate's BDK already 1-indexes time->month internally
     *        (1=January, 12=December), so do NOT add 1 again on display
     *        -- doing so was the off-by-one that printed January as 02.
     *
     *    (b) The MAX77620 calendar registers are not the same time the
     *        user sees in HOS. HOS keeps its own epoch offset (in system
     *        save data and PMC scratch) and adds it to whatever the
     *        MAX77620 calendar reads. After a coin-cell flat-discharge
     *        or a board reflow that briefly drops VBAT, the calendar
     *        resets to 2000-01-01 00:00:00 while HOS quietly keeps the
     *        offset and still shows the right wall time. So a year
     *        reading at the cold-boot default is informational, not a
     *        fault -- we flag it as such instead of pretending the read
     *        is broken.
     *
     *    Hekate's own GUI uses an epoch offset stored in
     *    /bootloader/nyx.ini ([config] timeoffset=<hex>, optionally
     *    timedst=0|1) so its time display matches HOS. We parse the
     *    same fields to surface the same wall time in the dump when the
     *    user has set up the offset in Hekate. The MAX77620 raw remains
     *    on the line below for techs who want to see the underlying
     *    register state.
     */
    rtc_time_t raw = {0};
    max77620_rtc_get_time(&raw);
    bool unset = (raw.year <= 2010);
    log_color(unset ? COL_DEFAULT : COL_OK,
        "  RTC raw      : %04d-%02d-%02d %02d:%02d:%02d%s\n",
        raw.year, raw.month, raw.day, raw.hour, raw.min, raw.sec,
        unset ? "  (calendar unset, HOS keeps its own offset)" : "");

    int  nyx_offset    = 0;
    bool nyx_offset_ok = false;
    bool nyx_dst       = false;
    {
        FIL fp;
        FRESULT fr = f_open(&fp, "0:/bootloader/nyx.ini", FA_READ);
        if (fr == FR_OK) {
            /* nyx.ini is small (under 1 KiB in practice). Read into a
             * stack buffer and scan line-by-line for the two keys we
             * care about. We don't need a full INI parser -- just
             * find `timeoffset=` and `timedst=` outside any other
             * section, accepting the same hex/decimal encodings
             * Hekate writes. */
            static char ini[2048];
            UINT br = 0;
            f_read(&fp, ini, sizeof(ini) - 1, &br);
            f_close(&fp);
            ini[br] = 0;

            char *p = ini;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                char *eol = p;
                while (*eol && *eol != '\n' && *eol != '\r') eol++;
                /* timeoffset=<hex>, possibly negative ("-e10"). */
                if (!nyx_offset_ok && (size_t)(eol - p) > 11 &&
                    memcmp(p, "timeoffset=", 11) == 0) {
                    char *v = p + 11;
                    int sign = 1;
                    if (*v == '-') { sign = -1; v++; }
                    int n = 0;
                    bool any = false;
                    while (v < eol) {
                        int hi = (*v >= '0' && *v <= '9') ? *v - '0'
                              : (*v >= 'a' && *v <= 'f') ? *v - 'a' + 10
                              : (*v >= 'A' && *v <= 'F') ? *v - 'A' + 10
                              : -1;
                        if (hi < 0) break;
                        n = (n << 4) | hi;
                        any = true;
                        v++;
                    }
                    if (any) {
                        nyx_offset    = sign * n;
                        nyx_offset_ok = true;
                    }
                } else if ((size_t)(eol - p) > 8 &&
                           memcmp(p, "timedst=", 8) == 0) {
                    nyx_dst = (p[8] == '1');
                }
                p = (*eol) ? eol + 1 : eol;
            }
        }
    }

    /* Hekate writes timeoffset=1 as a sentinel meaning "user opted out
     * of the offset" -- treat it as absent. Anything else is honoured. */
    if (nyx_offset_ok && nyx_offset != 0 && nyx_offset != 1) {
        max77620_rtc_set_epoch_offset(nyx_offset);
        max77620_rtc_set_auto_dst(nyx_dst);
        rtc_time_t adj = {0};
        max77620_rtc_get_time_adjusted(&adj);
        /* bdk's s_printf doesn't grok the %+d sign-forcing flag, so
         * print the offset's sign byte by hand. */
        log_color(COL_OK,
            "  RTC adjusted : %04d-%02d-%02d %02d:%02d:%02d"
            "  (nyx.ini offset %s%d s%s)\n",
            adj.year, adj.month, adj.day, adj.hour, adj.min, adj.sec,
            nyx_offset >= 0 ? "+" : "", nyx_offset,
            nyx_dst ? ", auto-DST" : "");
    } else if (unset) {
        LOG("  RTC adjusted : not available"
            " (no /bootloader/nyx.ini timeoffset)\n");
    }
}

static void probe_regulators(void)
{
    HEADER("[MAX77620 regulators]");
    int n_wrong = 0;
    /* For each rail surface:
     *   - Power-OK bit (rail crossed ~80% of target during ramp)
     *   - Configured voltage from the per-rail VOLT register
     *   - Sane operating range (low / high), picked per-rail to allow for
     *     DVFS where Hekate / HOS legitimately retunes the rail at runtime
     *
     * Voltage registers per Hekate's _pmic_regulators table:
     *   SD0..SD3   = 0x16..0x19 (12.5 mV steps from 600 mV, mask varies)
     *   LDO0..LDO8 = 0x23/0x25/0x27/.../0x33 (LDOx_CFG, low 6 bits = volt)
     *               step 25 mV (LDO0/1/4) or 50 mV (LDO2/3/5/6/7/8)
     * Decoded uV = (reg_val & mask) * uv_step + uv_min.
     *
     * Sane ranges below are intentionally wide on the rails that DVFS:
     *   SD0  (CPU)  - 0.6 V idle / BPMP, up to 1.4 V under CPU load
     *   LDO2 (SD3V) - 1.8 V on UHS (SDR104), 3.3 V on legacy SD
     * For fixed-target rails the band is +/-10 % of the configured value.
     * Anything outside the band gets a WRONG flag; rails that are off get
     * no flag (the configured voltage is meaningless when the rail is
     * disabled). */
    struct rail {
        int  id;
        const char *name;
        u8   volt_reg;
        u8   mask;
        u32  step_uv;
        u32  base_uv;
        u32  ok_min_uv;
        u32  ok_max_uv;
    };
    /* Rail labels and ranges cross-referenced against Hekate's
     * bdk/power/max7762x.h "Switch Power domains (max77620)" table.
     * The earlier labels (LDO3=USB, LDO5=USB1, LDO6=TS, LDO8=HDMI)
     * were guesses and wrong: LDO3/LDO5 are the GAME-CARD slot rails,
     * LDO6 powers Touch + ALS, LDO8 is a multi-purpose XUSB/DP/MCU
     * rail. Voltage tolerances copied from Hekate's per-LDO defaults. */
    static const struct rail rails[] = {
        /* id  name              volt_reg  mask  step    base    ok_min   ok_max  */
        {0,  "SD0  (SoC CPU)",   0x16, 0x7F, 12500, 600000,  600000, 1400000 }, /* DVFS */
        {1,  "SD1  (DRAM)",      0x17, 0x7F, 12500, 600000, 1000000, 1200000 },
        {2,  "SD2  (LDO src)",   0x18, 0xFF, 12500, 600000, 1262500, 1387500 },
        {3,  "SD3  (1V8 gen)",   0x19, 0xFF, 12500, 600000, 1700000, 1900000 },
        {4,  "LDO0 (Display)",   0x23, 0x3F, 25000, 800000, 1100000, 1300000 },
        {5,  "LDO1 (XUSB+PCIE)", 0x25, 0x3F, 25000, 800000,  950000, 1150000 },
        {6,  "LDO2 (SDMMC1)",    0x27, 0x3F, 50000, 800000, 1700000, 3400000 }, /* UHS<->legacy */
        {7,  "LDO3 (GC ASIC)",   0x29, 0x3F, 50000, 800000, 2900000, 3300000 },
        {8,  "LDO4 (RTC)",       0x2B, 0x3F, 12500, 800000,  800000,  900000 },
        /* LDO5 OK band spans both documented modes: GC Card 1.8V (the
         * default after Hekate per-LDO config) and 3.1V GC ASIC alt
         * (per Hekate max7762x.h line 65 OTP comment). */
        {9,  "LDO5 (GC Card)",   0x2D, 0x3F, 50000, 800000, 1700000, 3200000 },
        {10, "LDO6 (Touch+ALS)", 0x2F, 0x3F, 50000, 800000, 2700000, 3000000 },
        {11, "LDO7 (XUSB)",      0x31, 0x3F, 50000, 800000,  950000, 1150000 },
        {12, "LDO8 (XUSB/DP)",   0x33, 0x3F, 50000, 800000,  950000, 2900000 },
    };
    for (size_t i = 0; i < sizeof(rails)/sizeof(rails[0]); i++) {
        int ok = max77620_regulator_get_status(rails[i].id);
        u8  reg_val = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].volt_reg);
        u32 uv = ((u32)(reg_val & rails[i].mask)) * rails[i].step_uv + rails[i].base_uv;
        u32 col = COL_DEFAULT;
        const char *flag = "";
        if (ok) {
            if (uv < rails[i].ok_min_uv || uv > rails[i].ok_max_uv) {
                col = COL_ERR;
                flag = " WRONG";
                n_wrong++;
            } else {
                col = COL_OK;
            }
        }
        log_color(col,
            "  %s : %s  %d.%03d V  (range %d.%03d - %d.%03d V)%s\n",
            rails[i].name,
            ok ? "ON " : "off",
            uv / 1000000, (uv / 1000) % 1000,
            rails[i].ok_min_uv / 1000000, (rails[i].ok_min_uv / 1000) % 1000,
            rails[i].ok_max_uv / 1000000, (rails[i].ok_max_uv / 1000) % 1000,
            flag);
    }
    dx_set("pmic_rails", n_wrong ? DX_FAIL : DX_PASS,
        n_wrong ? "%d ON but out of band" : "", n_wrong);
}

static void probe_clocks(void)
{
    HEADER("[Clocks]");
    /* Raw register dumps - hard to derive exact MHz without replaying the
     * full PLL math, so we just show the register state. The values are
     * meaningful when comparing two consoles or before/after a config
     * change. */
    LOG("  PLLP_BASE    : 0x%08X\n", CLOCK(0xA0));
    LOG("  PLLP_OUTA    : 0x%08X\n", CLOCK(0xA4));
    LOG("  PLLP_OUTB    : 0x%08X\n", CLOCK(0x68));
    LOG("  SCLK_BURST   : 0x%08X\n", CLOCK(0x28));
    LOG("  SUPER_SCLK   : 0x%08X\n", CLOCK(0x2C));
    LOG("  CLK_SYSTEM   : 0x%08X\n", CLOCK(0x30));
    LOG("  CLK_OUT_ENB_L: 0x%08X\n", CLOCK(0x10));
    LOG("  CLK_OUT_ENB_H: 0x%08X\n", CLOCK(0x14));
    LOG("  CLK_OUT_ENB_U: 0x%08X\n", CLOCK(0x18));
    LOG("  CLK_OUT_ENB_X: 0x%08X\n", CLOCK(0x280));

    /* PLL ENABLE / LOCK status. Per the Tegra X1 TRM each PLLx_BASE
     * register has bit 27 = LOCK (read-only, set when the loop is
     * locked) and bit 30 = ENABLE. A PLL that is enabled but not locked
     * means the loop is hunting - "console boots, then hangs after a
     * few seconds" symptoms map onto this. n_unlocked is the verdict
     * signal: an enabled PLL that hasn't locked is always wrong. */
    log_color(COL_INFO, "[Clocks - PLL lock status]\n");
    struct pll_entry { u32 off; const char *name; };
    static const struct pll_entry plls[] = {
        {0x80,  "PLLC "},
        {0x90,  "PLLM "},
        {0xA0,  "PLLP "},
        {0xB0,  "PLLA "},
        {0xC0,  "PLLU "},
        {0xD0,  "PLLD "},
        {0xE0,  "PLLX "},
        {0x4B8, "PLLD2"},
        {0x590, "PLLDP"},
        {0x4C4, "PLLRE"},
    };
    int n_unlocked = 0;
    for (size_t i = 0; i < sizeof(plls)/sizeof(plls[0]); i++) {
        u32 base   = CLOCK(plls[i].off);
        bool en    = (base & (1u << 30)) != 0;
        bool lock  = (base & (1u << 27)) != 0;
        bool fail  = en && !lock;
        if (fail) n_unlocked++;
        log_color(fail ? COL_ERR : (en ? COL_OK : COL_DEFAULT),
            "  %s        : %s%s\n", plls[i].name,
            en ? (lock ? "ENABLED, LOCKED" : "ENABLED, NO-LOCK") : "disabled",
            fail ? " (loop is hunting)" : "");
    }
    dx_set("plls", n_unlocked ? DX_FAIL : DX_PASS,
        n_unlocked ? "%d enabled but not locked" : "", n_unlocked);

    /* Decoded frequencies via the PTO (Pulse Tag Observer) cell. The
     * BPMP couples the named clock to a 32.768 kHz reference for one
     * window and counts ticks; clock_get_dev_freq() returns the result
     * in kHz. We reach for the freqs a tech actually cares about: CPU,
     * SCLK (system / BPMP bus), EMC (DRAM), PLLP_OBS, SDMMC4 (eMMC).
     *
     * Note: hwtest runs on the BPMP, so CCLK_G reads as 0 unless the
     * payload separately fires the A57 cluster - we still print it for
     * completeness and call out the "(BPMP idle)" caveat in the line
     * itself. */
    /* New section header so the host viewer renders the decoded clock
     * rates as their own collapsible table beside the raw register dump,
     * instead of sticking the separator in as a free-form note line. */
    log_color(COL_INFO, "[Clocks - decoded rates]\n");
    u32 osc_khz = clock_get_osc_freq();
    LOG("  OSC          : %d.%03d MHz\n", osc_khz / 1000, osc_khz % 1000);
    struct { clock_pto_id_t id; const char *name; } kClocks[] = {
        {CLK_PTO_PLLP_OBS, "PLLP_OBS    "},
        {CLK_PTO_SCLK,     "SCLK / BPMP "},
        {CLK_PTO_EMC,      "EMC (DRAM)  "},
        {CLK_PTO_SDMMC4,   "SDMMC4(eMMC)"},
        {CLK_PTO_SDMMC1,   "SDMMC1(SD)  "},
        {CLK_PTO_CCLK_G,   "CCLK_G(A57) "},
    };
    for (size_t i = 0; i < sizeof(kClocks)/sizeof(kClocks[0]); i++) {
        u32 khz = clock_get_dev_freq(kClocks[i].id);
        if (khz == 0) {
            LOG("  %s : (idle / not clocked)\n", kClocks[i].name);
        } else {
            LOG("  %s : %d.%03d MHz\n",
                kClocks[i].name, khz / 1000, khz % 1000);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* GPIO pin census - configuration of every pin we touch                    */
/*                                                                          */
/* For repair triage: surface the full state (PINMUX + GPIO CNF/OE/OUT/IN)  */
/* of every pin involved in display / Joy-Con rails / debug UART, in one    */
/* place. A bent pin or solder fault shows up here as an unexpected         */
/* function selector or GPIO mode that doesn't match the role we expect     */
/* the pin to play.                                                         */
/*                                                                          */
/* Pinmux is the same APB_MISC register read in other probes. GPIO state is */
/* the four-byte block per port within its bank (CNF=+0x00, OE=+0x10,       */
/* OUT=+0x20, IN=+0x30 within (port&3)<<2 inside (port>>2)<<8).             */
/*                                                                          */
/* For pads where the relevant function is an alt-function (UART, PWM,      */
/* etc.), GPIO CNF=0 (SPIO mode) is the *correct* state - the alt-function  */
/* controller drives the pin and the GPIO out/in bits become irrelevant.    */
/* We still print them for completeness; readers can ignore CNF/OE/OUT/IN   */
/* whenever the row reads "SPIO". */
static const char *_pmx_pull_str(u32 pmx)
{
    u32 p = pmx & PINMUX_PULL_MASK;
    return p == PINMUX_PULL_UP   ? "pull-up"
         : p == PINMUX_PULL_DOWN ? "pull-dn"
                                 : "no-pull";
}

/* LCD console geometry:
 *   1280 x 720 panel rendered in landscape, 16-pixel font.
 *   = 80 columns by 45 rows.
 *
 * Anything over 80 chars on a single LOG line gets clipped on the
 * right (gfx_putc has no soft-wrap). To keep pages readable on the
 * device, every per-row format string used by long-tabular pages
 * should fit in <=80 chars at worst-case substitution. The static
 * asserts below catch obvious overflows at compile time by sizing
 * a fixed-width *template* with the widest possible substitution
 * pre-baked, and asserting against LCD_COLS_MAX. */
#define LCD_COLS_MAX 80

static void probe_gpio_census(void)
{
    HEADER("[GPIO pin census]");

    /* {label, pinmux_offset (0 = none), port, pin, role}
     * Roles must fit in <=18 chars so the rendered row stays under
     * LCD_COLS_MAX. The verbose form is in source comments above
     * each block. */
    struct pin_row {
        const char *label;     /* 4 chars padded ("PA5 " or "PCC3") */
        u32         pmx_off;
        u32         port;
        u32         pin;
        const char *role;      /* <= 18 chars; extra detail goes in comments */
    };
    static const struct pin_row rows[] = {
        /* 5V regulator master enable */
        { "PA5 ", PINMUX_AUX_SATA_LED_ACTIVE, GPIO_PORT_A, GPIO_PIN_5,
          "5V regulator EN"          /* FAN5333 enable */ },

        /* LCD pipeline */
        { "PI0 ", 0,                          GPIO_PORT_I, GPIO_PIN_0,
          "LCD AVDD CH2 EN"          /* +5.4 V */ },
        { "PI1 ", 0,                          GPIO_PORT_I, GPIO_PIN_1,
          "LCD AVDD CH1 EN"          /* -5.4 V */ },
        { "PV0 ", PINMUX_AUX_LCD_BL_PWM,      GPIO_PORT_V, GPIO_PIN_0,
          "LCD BL PWM"               /* PWM0 channel */ },
        { "PV1 ", PINMUX_AUX_LCD_BL_EN,       GPIO_PORT_V, GPIO_PIN_1,
          "LCD BL EN" },
        { "PV2 ", PINMUX_AUX_LCD_RST,         GPIO_PORT_V, GPIO_PIN_2,
          "LCD reset (act-low)" },

        /* Joy-Con R rail (UART_B path, our debug port) */
        { "PK3 ", PINMUX_AUX_GPIO_PK3,        GPIO_PORT_K, GPIO_PIN_3,
          "JC-R charge EN"           /* gates Tegra<->rail buffer */ },
        { "PG0 ", PINMUX_AUX_UART2_TX,        GPIO_PORT_G, GPIO_PIN_0,
          "UART2_TX (JC-R)" },
        { "PG1 ", PINMUX_AUX_UARTX_RX(UART_B),GPIO_PORT_G, GPIO_PIN_1,
          "UART2_RX (JC-R)" },
        { "PH6 ", PINMUX_AUX_GPIO_PH6,        GPIO_PORT_H, GPIO_PIN_6,
          "JC-R attach detect" },

        /* Joy-Con L rail (UART_C path) */
        { "PCC3", 0,                          GPIO_PORT_CC, GPIO_PIN_3,
          "JC-L charge EN" },
        { "PD1 ", PINMUX_AUX_UART3_TX,        GPIO_PORT_D, GPIO_PIN_1,
          "UART3_TX (JC-L)" },
        { "PD0 ", PINMUX_AUX_UARTX_RX(UART_C),GPIO_PORT_D, GPIO_PIN_0,
          "UART3_RX (JC-L)" },
        { "PE6 ", PINMUX_AUX_GPIO_PE6,        GPIO_PORT_E, GPIO_PIN_6,
          "JC-L attach detect" },

        /* Front-panel buttons */
        { "PX6 ", 0,                          GPIO_PORT_X, GPIO_PIN_6,
          "VOL+ btn (act-low)" },
        { "PX7 ", 0,                          GPIO_PORT_X, GPIO_PIN_7,
          "VOL- btn (act-low)" },
        { "PY1 ", 0,                          GPIO_PORT_Y, GPIO_PIN_1,
          "HOME btn (Lite SKU)" },
    };

    /* Compile-time check: a worst-case rendered row must fit in 80
     * chars. The "WIDEST_GPIO_ROW" template below mirrors the format
     * string with each %-substitution replaced by the widest value it
     * can produce. sizeof()-1 strips the trailing NUL. If a future
     * format change blows past LCD_COLS_MAX, the build fails here
     * with a clear message instead of silently clipping pixels. */
#define WIDEST_GPIO_ROW \
    "  PCC3 : fn=X pull-up tri ie=X SPIO OE=X OUT=X IN=X  (XXXXXXXXXXXXXXXXXX)"
    _Static_assert(sizeof(WIDEST_GPIO_ROW) - 1 <= LCD_COLS_MAX,
                   "GPIO census row format overflows the LCD width");
#undef WIDEST_GPIO_ROW

    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        const struct pin_row *r = &rows[i];
        u32 base = ((r->port >> 2) << 8) + ((r->port & 3) << 2);
        u8  cnf = GPIO(base + 0x00);
        u8  oe  = GPIO(base + 0x10);
        u8  out = GPIO(base + 0x20);
        u8  in  = GPIO(base + 0x30);
        bool gpio_mode = (cnf & r->pin) != 0;

        if (r->pmx_off) {
            u32 pmx  = PINMUX_AUX(r->pmx_off);
            u32 fn   = pmx & PINMUX_FUNC_MASK;
            bool tri = pmx & PINMUX_TRISTATE;
            bool ie  = pmx & PINMUX_INPUT_ENABLE;
            /* Pre-pad pull string to 7 chars; bdk's s_printf doesn't
             * support the %-Ns left-justified width flag. */
            char pull_pad[8];
            const char *p = _pmx_pull_str(pmx);
            u32 plen = strlen(p);
            if (plen > 7) plen = 7;
            memcpy(pull_pad, p, plen);
            for (u32 k = plen; k < 7; k++) pull_pad[k] = ' ';
            pull_pad[7] = 0;
            /* Format kept compact to fit LCD_COLS_MAX. The pmx hex
             * value is no longer printed - all four decoded fields
             * (fn / pull / tri / ie) carry the same info more
             * readably. SD-card-saved report still has full detail.
             *
             * Pin name is the key, everything else is the value, with
             * a colon between, this makes the host viewer render the
             * page as a proper table (key column = pin label, value
             * column = decoded state) instead of free-form notes. */
            LOG("  %s : fn=%d %s %s ie=%d %s OE=%d OUT=%d IN=%d  (%s)\n",
                r->label, fn, pull_pad,
                tri ? "tri" : "drv", !!ie,
                gpio_mode ? "GPIO" : "SPIO",
                !!(oe & r->pin), !!(out & r->pin), !!(in & r->pin),
                r->role);
        } else {
            /* No dedicated PINMUX entry for this pin (front-panel
             * buttons, charge-EN gating that uses GPIO directly). */
            LOG("  %s : pmx=(n/a)              %s OE=%d OUT=%d IN=%d  (%s)\n",
                r->label,
                gpio_mode ? "GPIO" : "SPIO",
                !!(oe & r->pin), !!(out & r->pin), !!(in & r->pin),
                r->role);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* UART debug port (UART_B / Joy-Con R rail)                                */
/*                                                                          */
/* The Joy-Con-R chassis-side debug header carries UART_B (115200 8N1, 1.8  */
/* V logic). hwtest writes log lines out of it; this probe reports the      */
/* full RX-side configuration so a chassis-adapter wiring issue can be      */
/* localised without scope or multimeter:                                   */
/*                                                                          */
/*   host USB-UART -> chassis cable -> chassis-side level shifter           */
/*                  (powered from the JC-R 5V rail enabled via PA5)         */
/*       -> chassis connector -> Tegra side buffer (gated by PK3 high)      */
/*       -> Tegra pad PG1 (UART2_RX) -> UART2 RX FIFO -> UART_LSR.RDR=1     */
/*                                                                          */
/* If ANY of those links breaks, RX stops working but TX still does because */
/* the return path is asymmetric. We dump pinmux, the relevant GPIO         */
/* enables driving the level-shifter rail, the UART controller registers,   */
/* and a 10 ms sample of the line via GPIO so:                              */
/*                                                                          */
/*   line stuck LOW   = level shifter unpowered or shorted to GND           */
/*   line idle HIGH    = shifter alive, host quiet                          */
/*   line transitions  = host adapter is sending; if no bytes land in FIFO  */
/*                       check baud / framing                               */
/*                                                                          */
/* Purely passive and read-only. After sampling PG1 we restore SPIO so the  */
/* UART regains control of the pin for the rest of the session. */
#ifndef JC_PROBE
static void probe_uart_b(void)
{
    HEADER("[UART_B debug port]");

    /* 1. Pinmux for PG1 (UART2_RX). Must be function 0 (UART) with input
     *    enable on and SOMETHING anchoring the idle state HIGH - either
     *    an internal pull-up (switch-coreboot's choice, idles HIGH on
     *    its own) or pure tristate (Hekate's default, relies on the
     *    external driver to hold the line HIGH between bytes - works
     *    for Joy-Cons but not for passive USB-UART chassis adapters).
     *    PINMUX_AUX_UARTX_RX(idx) for UART_B resolves to offset 0xF8;
     *    the BDK only exposes the indexed helper, no UART2-specific
     *    macro. */
    u32 pmx = PINMUX_AUX(PINMUX_AUX_UARTX_RX(UART_B));
    u32 fn  = pmx & PINMUX_FUNC_MASK;
    bool tri = pmx & PINMUX_TRISTATE;
    bool ie  = pmx & PINMUX_INPUT_ENABLE;
    u32 pull = pmx & PINMUX_PULL_MASK;
    const char *pull_name = pull == PINMUX_PULL_UP   ? "pull-up"
                          : pull == PINMUX_PULL_DOWN ? "pull-down"
                          : pull == PINMUX_PULL_NONE ? "no-pull"
                          : "?";
    LOG("  UART2_RX pmx : 0x%08X\n", pmx);
    log_color((fn == 0 && ie) ? COL_OK : COL_ERR,
        "  pad config   : fn=%d %s, %s, %s, %s\n", fn,
        fn == 0 ? "(UART)" : "(WRONG)",
        tri ? "tristate" : "drive-allowed",
        pull_name,
        ie ? "input-enable" : "INPUT-DISABLED");
    if (pull == PINMUX_PULL_UP)
        log_color(COL_OK,
            "  idle anchor  : internal PULL_UP (line idles HIGH on its own)\n");
    else if (tri && pull == PINMUX_PULL_NONE) {
        log_color(COL_WARN,
            "  idle anchor  : tristate only (line floats unless driven)\n");
        log_color(COL_WARN,
            "                 OK with Joy-Cons, fails on passive USB-UART\n");
    }
    else if (pull == PINMUX_PULL_DOWN)
        log_color(COL_ERR,
            "  idle anchor  : PULL_DOWN (line idles LOW - UART will see permanent BREAK)\n");

    /* 2. PA5 = master 5V regulator EN, and 3. PK3 = Joy-Con-R buffer /
     *    charge enable. Both are driven as GPIO outputs by Hekate but
     *    the corresponding PINMUX_AUX entries leave INPUT_ENABLE off
     *    (PINMUX_AUX_SATA_LED_ACTIVE = 1, PINMUX_AUX_GPIO_PK3 = drive
     *    + pull-down + fn2). With the input buffer disabled, gpio_read()
     *   , which samples the IN register, always returns 0 even when
     *    we're actively driving HIGH. So we read OUT (what we wrote)
     *    plus CNF (GPIO vs SPIO mode) and OE (output enabled) directly,
     *    bypassing the BDK helper. Tegra X1 GPIO bank layout:
     *      bank base    = (port >> 2) << 8
     *      port slot    = (port & 3) << 2
     *      CNF=+0x00, OE=+0x10, OUT=+0x20, IN=+0x30 */
    {
        u32 base_a = ((GPIO_PORT_A >> 2) << 8) + ((GPIO_PORT_A & 3) << 2);
        u8  cnf_a = GPIO(base_a + 0x00);
        u8  oe_a  = GPIO(base_a + 0x10);
        u8  out_a = GPIO(base_a + 0x20);
        u8  in_a  = GPIO(base_a + 0x30);
        bool driven_high_a = (cnf_a & GPIO_PIN_5) && (oe_a & GPIO_PIN_5)
                             && (out_a & GPIO_PIN_5);
        log_color(driven_high_a ? COL_OK : COL_ERR,
            "  PA5 (5V_EN)  : %s   (CNF=%d OE=%d OUT=%d IN=%d)\n",
            driven_high_a ? "HIGH (driven)" : "NOT driven HIGH",
            !!(cnf_a & GPIO_PIN_5), !!(oe_a & GPIO_PIN_5),
            !!(out_a & GPIO_PIN_5), !!(in_a & GPIO_PIN_5));
    }
    {
        u32 base_k = ((GPIO_PORT_K >> 2) << 8) + ((GPIO_PORT_K & 3) << 2);
        u8  cnf_k = GPIO(base_k + 0x00);
        u8  oe_k  = GPIO(base_k + 0x10);
        u8  out_k = GPIO(base_k + 0x20);
        u8  in_k  = GPIO(base_k + 0x30);
        bool driven_high_k = (cnf_k & GPIO_PIN_3) && (oe_k & GPIO_PIN_3)
                             && (out_k & GPIO_PIN_3);
        log_color(driven_high_k ? COL_OK : COL_ERR,
            "  PK3 (JC-R EN): %s   (CNF=%d OE=%d OUT=%d IN=%d)\n",
            driven_high_k ? "HIGH (driven)" : "NOT driven HIGH",
            !!(cnf_k & GPIO_PIN_3), !!(oe_k & GPIO_PIN_3),
            !!(out_k & GPIO_PIN_3), !!(in_k & GPIO_PIN_3));
    }
    /* Also report the software-side regulator state (what reg_5v_dev
     * tracks). On Mariko PA5 is gated by both this and the FAN5333 IC's
     * own enable detection; the [5V regulator] page already shows
     * regulator_5v_get_dev_enabled() per device. */

    /* 4. UART_B controller registers. UART_B base = 0x70006040. */
    volatile u32 *uart = (volatile u32 *)0x70006040;
    u8  lcr  = uart[3] & 0xFF;   /* offset 0x0C */
    u8  mcr  = uart[4] & 0xFF;   /* offset 0x10 */
    u8  lsr  = uart[5] & 0xFF;   /* offset 0x14 */
    u32 vstat = uart[11];        /* offset 0x2C VENDOR_STATUS */
    u32 rx_count = (vstat >> 8) & 0x3F;
    LOG("  UART_LCR     : 0x%02X (DLAB=%d, word=%d)\n",
        lcr, (lcr >> 7) & 1, (lcr & 3) + 5);
    LOG("  UART_MCR     : 0x%02X\n", mcr);

    /* Baud-rate divisor (DLL / DLM) — shadowed at offsets 0x00 / 0x04
     * when LCR.DLAB=1. We flip DLAB, snapshot the divisor latches,
     * then restore the original LCR. The whole sequence completes in
     * microseconds — no in-flight TX bytes are lost (THR continues to
     * empty since the shift register is independent of the latch
     * mux). A divisor of 0 means the UART was never initialised.
     *
     * The UART input clock is *not* OSC. Tegra X1 routes it through
     * CLK_SOURCE_UARTB (CLOCK 0x17C):
     *   bits 31:30 = source: 00=PLLP_OUT0 (408 MHz fixed),
     *                01=PLLC2_OUT0, 10=PLLC_OUT0, 11=CLK_M (= OSC).
     *   bit   24   = UART_SRC_CLK_DIV_EN — when 0 the source-side
     *                divisor is BYPASSED (clock passes straight
     *                through); when 1 it's applied. Hekate only sets
     *                bit 24 for the 1 M / 3 M baud paths; for 115 200
     *                the divisor field still reads 2 but the divider
     *                is bypassed, so UART_B input = 408 MHz directly.
     *   bits  7:0  = N (encodes 1+N/2 fractional division when bit 24
     *                is set; ignored when not).
     * Canonical 115 200 baud: src=0, bit 24=0, divisor latch=221 →
     * effective baud = 408_000_000 / (16 * 221) = 115_345 (0.13 %
     * fast — well within UART tolerance). */
    uart[3] = (lcr | 0x80);          /* set DLAB */
    u8 dll = uart[0] & 0xFF;
    u8 dlm = uart[1] & 0xFF;
    uart[3] = lcr;                   /* restore */
    u32 divisor = ((u32)dlm << 8) | dll;

    u32 clk_src_reg = CLOCK(0x17C);
    u32 clk_src     = (clk_src_reg >> 30) & 0x3;
    bool src_div_en = (clk_src_reg & (1u << 24)) != 0;
    u32 clk_div_n   = clk_src_reg & 0xFF;
    u32 src_khz;
    const char *src_name;
    switch (clk_src) {
    case 0: src_khz = 408000;                src_name = "PLLP_OUT0"; break;
    case 3: src_khz = clock_get_osc_freq();  src_name = "CLK_M";     break;
    /* PLLC / PLLC2 paths exist but hekate doesn't use them for UART. */
    default: src_khz = 0;                    src_name = "PLLC?";     break;
    }
    /* When bit 24 is clear the source-side divider is bypassed - input
     * == source. When set, input = src * 2 / (N + 2). */
    u32 uart_in_khz = !src_khz ? 0
                      : src_div_en ? (src_khz * 2) / (clk_div_n + 2)
                                   : src_khz;
    u32 baud = (divisor && uart_in_khz)
               ? (uart_in_khz * 1000) / (16 * divisor) : 0;
    LOG("  CLK_SOURCE   : 0x%08X (src=%s, src-div=%s, %d kHz in)\n",
        clk_src_reg, src_name,
        src_div_en ? "ON" : "bypassed", uart_in_khz);
    log_color((divisor && baud >= DEBUG_UART_BAUDRATE * 95 / 100
                       && baud <= DEBUG_UART_BAUDRATE * 105 / 100)
              ? COL_OK : COL_WARN,
        "  Baud divisor : %d (DLL=0x%02X DLM=0x%02X) -> %d baud (target %d)\n",
        divisor, dll, dlm, baud, DEBUG_UART_BAUDRATE);
    LOG("  UART_LSR     : 0x%02X (%s%s%s%s%s%s%s%s)\n", lsr,
        (lsr & 0x80) ? "FIFOE "  : "",
        (lsr & 0x40) ? "TMTY "   : "",
        (lsr & 0x20) ? "THRE "   : "",
        (lsr & 0x10) ? "BRK "    : "",
        (lsr & 0x08) ? "FERR "   : "",
        (lsr & 0x04) ? "PERR "   : "",
        (lsr & 0x02) ? "OVRF "   : "",
        (lsr & 0x01) ? "RDR"     : "");
    /* FERR / PERR / BRK suggest baud-rate mismatch or wrong framing. */
    if (lsr & 0x1C)
        log_color(COL_ERR,
            "                 ^- framing/parity/break errors latched - check baud\n");
    LOG("  VENDOR_STAT  : 0x%08X (RX_FIFO=%d byte(s))\n", vstat, rx_count);

    /* 5. Sample PG1 as a raw GPIO for ~10 ms. UART idle state is HIGH
     *    (mark = 1). If the line is dead, we'll see it stuck LOW or
     *    floating. If the host adapter is currently transmitting, we
     *    should see edges. We restore SPIO before returning. */
    /* New section header so the line sample shows up as its own
     * collapsible table in the host viewer instead of a stray note. */
    log_color(COL_INFO, "[raw PG1 sample (10 ms via GPIO)]\n");
    gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_GPIO);
    int high = 0, low = 0, edges = 0, last = -1;
    u32 end = get_tmr_us() + 10000;
    while ((s32)(end - get_tmr_us()) > 0) {
        int v = gpio_read(GPIO_PORT_G, GPIO_PIN_1);
        if (v) high++; else low++;
        if (last != -1 && v != last) edges++;
        last = v;
    }
    gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_SPIO);  /* hand back to UART */
    LOG("    samples      : %d high, %d low, %d edges\n", high, low, edges);
    if (edges > 0) {
        log_color(COL_OK,
            "    Verdict      : line ACTIVE (%d transitions) - host IS sending\n", edges);
    } else if (high > 0 && low == 0) {
        log_color(COL_OK,
            "    Verdict      : line idle HIGH - shifter alive, host quiet\n");
    } else if (low > 0 && high == 0) {
        log_color(COL_ERR,
            "    Verdict      : line stuck LOW - shifter unpowered or shorted to GND\n");
    } else {
        log_color(COL_WARN,
            "    Verdict      : mixed (%d high / %d low / no edges) - line floating\n",
            high, low);
    }

}
#endif  /* !JC_PROBE */

/* ------------------------------------------------------------------------ */
/* Boot ROM + pkg1 identification (BOOT0)                                   */

static void probe_boot0_pkg1(void)
{
    HEADER("[BOOT0 / pkg1 fingerprint]");
    if (!g_emmc_ok) {
        log_color(COL_ERR, "  eMMC not initialised\n");
        return;
    }
    /* Switch to BOOT0 partition for the read, then snap back to USER.
     * sdmmc_storage_set_mmc_partition returns 0 on success, non-zero on
     * failure - the same trap as sd_initialize() / emmc_initialize(). */
    if (sdmmc_storage_set_mmc_partition(&emmc_storage, 1) != 0) {
        log_color(COL_ERR, "  partition switch failed\n");
        return;
    }
    /* pkg1 lives at byte offset 0x100000 in BOOT0 (sector 0x800), per
     * Hekate's PKG1_BOOTLOADER_MAIN_OFFSET. On Erista the pk1_hdr_t starts
     * at offset 0 of that sector. On Mariko there's a signed OEM
     * `bl_hdr_t210b01_t` header (0x170 bytes) BEFORE pk1_hdr_t, so the
     * timestamp / keygen / version lives at sector-offset 0x170. We read
     * one sector and pick the right offset. pk1_hdr_t layout:
     *   +0x00..0x0F  4x SHA-256 magic words
     *   +0x10..0x1D  14-byte ASCII build tag (e.g. "20240805_BLAB")
     *   +0x1E        keygen
     *   +0x1F        version
     */
    static u8 buf[0x200] __attribute__((aligned(8)));
    if (sdmmc_storage_read(&emmc_storage, 0x800, 1, buf) != 0) {
        log_color(COL_ERR, "  BOOT0 read failed\n");
        sdmmc_storage_set_mmc_partition(&emmc_storage, 0);
        return;
    }
    /* Mariko BOOT0 has a 0x170-byte OEM header (`bl_hdr_t210b01_t`)
     * before pk1_hdr_t; Erista has the pk1_hdr_t at sector start. The
     * SoC chip-major nibble usually tells us which one we're on, but
     * that's not reliable when:
     *   - the emulator's --oem flag doesn't match the dumped BOOT0
     *   - someone copies a BOOT0 image between SoC families to inspect it
     * Try both offsets and pick the one whose 14-byte ASCII timestamp
     * field decodes to printable characters. The real Mariko bl_hdr
     * has zero-bytes scattered through the first 0x170, so its 0x10
     * window is junk; pkg1's timestamp window is always ASCII digits
     * + underscores. The match is unambiguous. */
    bool mariko_chip = ((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2;
    int p_erista = 0, p_mariko = 0;
    for (int i = 0; i < 14; i++) {
        u8 ce = buf[0       + 0x10 + i];
        u8 cm = buf[0x170   + 0x10 + i];
        if (ce >= 0x20 && ce < 0x7F) p_erista++;
        if (cm >= 0x20 && cm < 0x7F) p_mariko++;
    }
    u32 pk1_off  = (p_mariko > p_erista) ? 0x170 : 0;
    bool mariko_layout = (pk1_off == 0x170);
    if (mariko_layout != mariko_chip) {
        log_color(COL_WARN,
            "  Note         : SoC=%s, BOOT0=%s, using BOOT0 layout\n",
            mariko_chip  ? "Mariko" : "Erista",
            mariko_layout ? "Mariko" : "Erista");
    }
    const u8 *pk1 = buf + pk1_off;

    if (mariko_layout) {
        /* Dump a few useful Mariko OEM-header fields too. */
        u32 version    = *(u32 *)(buf + 0x150);
        u32 size       = *(u32 *)(buf + 0x154);
        u32 load_addr  = *(u32 *)(buf + 0x158);
        u32 entrypoint = *(u32 *)(buf + 0x15C);
        LOG("  bl_hdr ver   : 0x%08X\n", version);
        LOG("  bl_hdr size  : 0x%08X\n", size);
        LOG("  bl_hdr load  : 0x%08X\n", load_addr);
        LOG("  bl_hdr entry : 0x%08X\n", entrypoint);
    }

    char build[15] = {0};
    int  printable = 0;
    for (int i = 0; i < 14; i++) {
        u8 c = pk1[0x10 + i];
        if (c >= 0x20 && c < 0x7F) {
            build[i] = c;
            printable++;
        } else {
            build[i] = '.';
        }
    }
    /* If the entire timestamp is non-printable, the BOOT0 sector came
     * back empty (0x00s) - the read either succeeded against an
     * unprovisioned eMMC (emulator default, brand-new board) or got
     * silently muxed away. Either way there's no pkg1 here to validate
     * against. Stay verbose but mark `not present` so the verdict won't
     * count this as a soft warn. */
    if (printable == 0) {
        log_color(COL_WARN,
            "  pkg1 build   : (BOOT0 sector 0x800 read all-zero - pkg1 absent)\n");
        sdmmc_storage_set_mmc_partition(&emmc_storage, 0);
        return;
    }
    log_color(COL_OK, "  pkg1 build   : %s\n", build);
    LOG("  pkg1 keygen  : 0x%02X\n", pk1[0x1E]);
    LOG("  pkg1 version : 0x%02X\n", pk1[0x1F]);

    /* Map the YYYYMMDD prefix of the pkg1 timestamp to its HOS firmware
     * range. Table mirrors Hekate's _pkg1_ids[] in bootloader/hos/pkg1.c.
     * pk1_hos_idx is local: the anti-downgrade dx_set below is the only
     * downstream consumer, both in this function. */
    int pk1_hos_idx = -1;
    static const struct { const char *ts; const char *hos; } kHosTable[] = {
        {"20161121", "1.0.0"},
        {"20170210", "2.0.0 - 2.3.0"},
        {"20170519", "3.0.0"},
        {"20170710", "3.0.1 - 3.0.2"},
        {"20170921", "4.0.0 - 4.1.0"},
        {"20180220", "5.0.0 - 5.1.0"},
        {"20180802", "6.0.0 - 6.1.0"},
        {"20181107", "6.2.0"},
        {"20181218", "7.0.0"},
        {"20190208", "7.0.1"},
        {"20190314", "8.0.0 - 8.0.1"},
        {"20190531", "8.1.0 - 8.1.1"},
        {"20190809", "9.0.0 - 9.0.1"},
        {"20191021", "9.1.0 - 9.2.0"},
        {"20200303", "10.0.0 - 10.2.0"},
        {"20201030", "11.0.0 - 11.0.1"},
        {"20210129", "12.0.0 - 12.0.1"},
        {"20210422", "12.0.2 - 12.0.3"},
        {"20210607", "12.1.0"},
        {"20210805", "13.0.0 - 13.2.0"},
        {"20220105", "13.2.1"},
        {"20220209", "14.0.0 - 14.1.2"},
        {"20220801", "15.0.0 - 15.0.1"},
        {"20230111", "16.0.0 - 16.1.0"},
        {"20230906", "17.0.0 - 17.0.1"},
        {"20240207", "18.0.0 - 18.1.0"},
        {"20240808", "19.0.0 - 19.0.1"},
        {"20250206", "20.0.0 - 20.5.0"},
        {"20251009", "21.0.0 - 21.2.0"},
        {"20260123", "22.0.0+"},
    };
    const char *hos = "Unknown (newer than this hwtest knows about?)";
    u32  best_idx = 0;
    bool exact = false;
    for (u32 i = 0; i < sizeof(kHosTable)/sizeof(kHosTable[0]); i++) {
        if (memcmp(build, kHosTable[i].ts, 8) == 0) {
            hos = kHosTable[i].hos;
            exact = true;
            break;
        }
        /* If no exact match, keep the latest entry whose timestamp is
         * <= ours so we can at least report "newer than HOS X". */
        if (memcmp(build, kHosTable[i].ts, 8) >= 0)
            best_idx = i;
    }
    if (!exact) {
        char buf[80];
        s_printf(buf, "newer than HOS %s", kHosTable[best_idx].hos);
        log_color(COL_WARN, "  HOS version  : %s\n", buf);
        pk1_hos_idx = -1;
    } else {
        log_color(COL_OK, "  HOS version  : %s\n", hos);
        /* Find table index for the verdict cross-check. */
        for (u32 i = 0; i < sizeof(kHosTable)/sizeof(kHosTable[0]); i++) {
            if (memcmp(build, kHosTable[i].ts, 8) == 0) {
                pk1_hos_idx = (int)i;
                break;
            }
        }
    }

    /* Anti-downgrade cross-check: burnt fuses must be >= the minimum
     * fuse count the detected HOS version requires, otherwise the eMMC
     * was restored from a console with newer firmware (illegal
     * downgrade). Mapping mirrors Hekate's _pkg1_ids[].fuses field. */
    if (pk1_hos_idx >= 0) {
        int burnt = __builtin_popcount(fuse_read_odm(7));
        static const u8 kFusesByIdx[] = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 9,
            10, 11, 12, 13, 14, 14, 15, 15, 15,
            16, 16, 17, 18, 19, 19, 20, 21, 22, 23,
        };
        int min_f = kFusesByIdx[pk1_hos_idx];
        dx_set("fuses_pkg1",
            burnt < min_f         ? DX_FAIL :
            burnt > min_f + 2     ? DX_WARN : DX_PASS,
            burnt < min_f     ? "%d burnt < %d expected" :
            burnt > min_f + 2 ? "%d burnt > %d expected" : "",
            burnt, min_f);
    }

    sdmmc_storage_set_mmc_partition(&emmc_storage, 0);
}

static void probe_autorcm(void)
{
    HEADER("[AutoRCM detection]");
    if (!g_emmc_ok) {
        log_color(COL_ERR, "  eMMC not initialised\n");
        return;
    }

    /* Mariko silicon has RCM patched in BootROM and AutoRCM has no effect.
     * Hekate refuses to even toggle it on Mariko. We still report the BCT
     * state for completeness. */
    bool patched = fuse_check_patched_rcm();

    if (sdmmc_storage_set_mmc_partition(&emmc_storage, 1) != 0) {
        log_color(COL_ERR, "  partition switch to BOOT0 failed\n");
        return;
    }

    /* BCT[0] starts at byte 0x200 = sector 1 of BOOT0. The RSA modulus
     * sits at offset 0x110..0x20F within the BCT block; what Hekate
     * touches is byte 0x10 of the modulus, which lands at byte 0x10 of
     * sector 1's content. (The AutoRCM patch zeroes that byte; the
     * legitimate modulus has it as 0xF7 for prod silicon, 0x37 for dev.) */
    static u8 buf[0x200] __attribute__((aligned(8)));
    int rd = sdmmc_storage_read(&emmc_storage, 1, 1, buf);
    sdmmc_storage_set_mmc_partition(&emmc_storage, 0);
    if (rd != 0) {
        log_color(COL_ERR, "  sector 1 read failed\n");
        return;
    }

    u8 corr_mod0, mod1;
    nx_emmc_get_autorcm_masks(&corr_mod0, &mod1);
    LOG("  modulus[0x10]: 0x%02X (expected 0x%02X for legit BCT)\n",
        buf[0x10], corr_mod0);
    LOG("  modulus[0x11]: 0x%02X (expected 0x%02X)\n", buf[0x11], mod1);

    if (patched) {
        /* Mariko (or RCM-patched Erista): AutoRCM is a no-op even if the
         * BCT byte is zeroed, so the comparison is purely informational.
         * Mariko's BCT is also signed differently (the modulus check
         * Hekate uses for AutoRCM detection only applies to Erista's
         * BCT layout); a mismatch here usually means "Mariko BCT, not
         * directly comparable", not corruption. Don't raise an alarm. */
        log_color(COL_OK,
            "  AutoRCM       : N/A (BootROM RCM patched)\n");
    } else if (buf[0x11] != mod1) {
        log_color(COL_ERR,
            "  AutoRCM       : invalid (BCT[0] signature byte mismatch)\n");
    } else if (buf[0x10] == corr_mod0) {
        log_color(COL_OK,
            "  AutoRCM       : DISABLED (clean BCT)\n");
    } else {
        log_color(COL_WARN,
            "  AutoRCM       : ENABLED (BCT modulus byte zeroed)\n");
    }
}

/* ------------------------------------------------------------------------ */
/* eMMC GPT listing (USER partition, sector 1)                              */

static void probe_gpt(void)
{
    HEADER("[eMMC GPT]");
    if (!g_emmc_ok) {
        log_color(COL_ERR, "  eMMC not initialised\n");
        return;
    }
    /* USER partition is the active partition by default (set by
     * sdmmc_storage_init_mmc); no switch needed. Read sector 1 (the GPT
     * header) plus enough subsequent sectors to cover the partition
     * entry array. Standard Switch eMMC has 14 partitions, each entry
     * 128 bytes, so 14*128 = 1792 bytes = 4 sectors after the header. */
    static u8 buf[5 * 0x200] __attribute__((aligned(8)));
    if (sdmmc_storage_read(&emmc_storage, 1, 5, buf) != 0) {
        log_color(COL_ERR, "  GPT read failed\n");
        return;
    }
    /* Header signature: "EFI PART" at offset 0 of the GPT header. */
    if (memcmp(buf, "EFI PART", 8) != 0) {
        log_color(COL_ERR, "  signature missing - not a GPT-partitioned eMMC\n");
        LOG("  sector 1 [0..7]:");
        for (int i = 0; i < 8; i++) LOG(" %02X", buf[i]);
        LOG("\n");
        dx_set("gpt", DX_FAIL, "signature missing");
        return;
    }
    u32 num_parts = *(u32 *)(buf + 0x50);
    u32 entry_sz  = *(u32 *)(buf + 0x54);
    /* part_ent_lba is the LBA on disk where the partition entry array
     * starts. Switch eMMC USER GPT typically uses 2, but read the field
     * to be safe. We read sectors 1..5 (5 sectors) into buf, so the
     * in-buffer offset is (part_ent_lba - 1) * 512. */
    u64 part_ent_lba = *(u64 *)(buf + 0x48);
    log_color(COL_OK, "  signature    : OK (\"EFI PART\")\n");
    LOG("  partitions   : %d entries, %d bytes each (LBA %d)\n",
        num_parts, entry_sz, (u32)part_ent_lba);

    /* GPT integrity: header has its own CRC32 stored at +0x10 (with the
     * crc field zeroed during calc), and the entry-array CRC32 at +0x58.
     * A bad CRC = the eMMC was either corrupted or has a non-stock
     * layout. crc32_calc() is the standard JEDEC poly which is also
     * what the GPT spec requires. Header size lives at +0x0C. */
    {
        u32 hdr_size = *(u32 *)(buf + 0x0C);
        u32 hdr_crc  = *(u32 *)(buf + 0x10);
        u32 ent_crc  = *(u32 *)(buf + 0x58);
        if (hdr_size < 0x5C || hdr_size > 0x100) {
            log_color(COL_WARN, "  hdr_size     : %d (suspicious - skipping CRC)\n", hdr_size);
        } else {
            /* Compute header CRC with the stored CRC field zeroed. */
            static u8 hdr_copy[0x100];
            memcpy(hdr_copy, buf, hdr_size);
            *(u32 *)(hdr_copy + 0x10) = 0;
            u32 calc_hdr = crc32_calc(0, hdr_copy, hdr_size);
            log_color(calc_hdr == hdr_crc ? COL_OK : COL_ERR,
                "  hdr CRC32    : 0x%08X (stored 0x%08X) %s\n",
                calc_hdr, hdr_crc,
                calc_hdr == hdr_crc ? "OK" : "MISMATCH");
            bool hdr_ok = (calc_hdr == hdr_crc);

            /* Entry array CRC: spans num_parts * entry_sz bytes starting
             * at sector part_ent_lba (= our buf + (part_ent_lba-1)*512).
             * Cap at what fits in the 5-sector read window. */
            u32 ent_total = num_parts * entry_sz;
            u32 ent_off   = (u32)((part_ent_lba - 1) * 512);
            u32 buf_left  = (5 * 512) - ent_off;
            bool ent_ok = true;  /* assume pass if we can't compute */
            if (ent_total <= buf_left) {
                u32 calc_ent = crc32_calc(0, buf + ent_off, ent_total);
                log_color(calc_ent == ent_crc ? COL_OK : COL_ERR,
                    "  ent CRC32    : 0x%08X (stored 0x%08X) %s\n",
                    calc_ent, ent_crc,
                    calc_ent == ent_crc ? "OK" : "MISMATCH");
                ent_ok = (calc_ent == ent_crc);
            } else {
                log_color(COL_DEFAULT,
                    "  ent CRC32    : skipped (entries span %d bytes, only %d in read window)\n",
                    ent_total, buf_left);
            }
            dx_set("gpt", (hdr_ok && ent_ok) ? DX_PASS : DX_FAIL,
                hdr_ok && ent_ok ? "" :
                !hdr_ok          ? "header CRC mismatch" :
                                   "entry CRC mismatch");
        }
    }

    if (entry_sz != 128 || num_parts > 32) {
        log_color(COL_WARN, "  unusual entry size / count - aborting decode\n");
        return;
    }
    if (part_ent_lba < 1 || part_ent_lba > 4) {
        log_color(COL_WARN, "  part_ent_lba %d outside read window - aborting decode\n",
            (u32)part_ent_lba);
        return;
    }
    /* Walk entries. bdk's s_printf doesn't grok the `-` flag for
     * left-justified strings, so we pad manually into a 22-char field.
     * Clamp the loop to whatever fits in the 5-sector read window so
     * we never run off the buffer. */
    const u8 *e = buf + (part_ent_lba - 1) * 512;
    u32 max_in_buf = (5 * 512 - (part_ent_lba - 1) * 512) / 128;
    if (num_parts > max_in_buf) num_parts = max_in_buf;
    for (u32 i = 0; i < num_parts; i++, e += 128) {
        /* Skip empty (all-zero type GUID). */
        bool empty = true;
        for (int b = 0; b < 16; b++) if (e[b]) { empty = false; break; }
        if (empty) continue;

        u64 first = *(u64 *)(e + 0x20);
        u64 last  = *(u64 *)(e + 0x28);
        u32 size_kib = (u32)((last - first + 1) / 2);  /* sectors of 512 -> KiB */

        /* Name is UTF-16LE, 36 chars max. ASCII-folded for display. */
        char name[37] = {0};
        u32 name_len = 0;
        for (int j = 0; j < 36; j++) {
            u16 c = *(u16 *)(e + 0x38 + j * 2);
            if (!c) break;
            name[j] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            name_len++;
        }
        char padded[24];
        memcpy(padded, name, name_len);
        for (u32 j = name_len; j < 22; j++) padded[j] = ' ';
        padded[22] = 0;

        /* "<index>. <name>" as the row key, "<size> + LBA range" as the
         * value. The host viewer treats "  KEY : VALUE" with a colon
         * separator as a table row, so this lays the partition list out
         * as one row per partition with the name as the leftmost column. */
        if (size_kib >= 1024)
            LOG("  %2d. %s : %4d MiB  [LBA %d..%d]\n",
                i, padded, size_kib >> 10, (u32)first, (u32)last);
        else
            LOG("  %2d. %s : %4d KiB  [LBA %d..%d]\n",
                i, padded, size_kib, (u32)first, (u32)last);
    }
}

/* ------------------------------------------------------------------------ */
/* SD card content scan                                                     */

static void probe_sd_content(void)
{
    HEADER("[SD card content]");
    if (!g_sd_ok) {
        log_color(COL_ERR, "  SD not ready\n");
        return;
    }
    /* The save_report path already mounts the SD when called - mount here
     * too in case the save was skipped (eMMC absent or report disabled). */
    static FATFS s_scan_fs;
    static bool  s_scan_mounted = false;
    if (!s_scan_mounted) {
        FRESULT fr = f_mount(&s_scan_fs, "0:", 1);
        if (fr != FR_OK) {
            log_color(COL_ERR, "  f_mount failed (%d)\n", fr);
            return;
        }
        s_scan_mounted = true;
    }

    /* Check whether each well-known CFW directory exists, list a few
     * useful version files inline. f_stat tests existence cheaply. */
    static const char *paths[] = {
        "0:/atmosphere",
        "0:/atmosphere/contents",
        "0:/bootloader",
        "0:/bootloader/payloads",
        "0:/switch",
        "0:/emuMMC",
        "0:/Nintendo",
    };
    /* bdk's s_printf doesn't grok `%-30s`; pad manually to 30 chars. */
    FILINFO fi;
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        char padded[40];
        u32 plen = strlen(paths[i]);
        if (plen >= sizeof(padded)) plen = sizeof(padded) - 1;
        memcpy(padded, paths[i], plen);
        for (u32 j = plen; j < 30; j++) padded[j] = ' ';
        padded[30] = 0;
        FRESULT fr = f_stat(paths[i], &fi);
        log_color(fr == FR_OK ? COL_OK : COL_DEFAULT,
            "  %s : %s\n", padded,
            fr == FR_OK ? (fi.fattrib & AM_DIR ? "DIR" : "FILE") : "absent");
    }

    /* Atmosphère version sources, in order of preference:
     *   1. /atmosphere/release.txt   (older releases)
     *   2. /atmosphere/version       (newer releases use this name)
     * Either is plain ASCII like "1.7.1". If both are absent we still
     * report whether /atmosphere/contents has any sysmodule directories
     * as a fallback presence indicator. */
    static const char *ams_paths[] = {
        "0:/atmosphere/release.txt",
        "0:/atmosphere/version",
    };
    FIL  fp;
    bool ams_found = false;
    for (size_t i = 0; i < sizeof(ams_paths)/sizeof(ams_paths[0]); i++) {
        FRESULT fr = f_open(&fp, ams_paths[i], FA_READ);
        if (fr != FR_OK) continue;
        char buf[64] = {0};
        UINT br = 0;
        f_read(&fp, buf, sizeof(buf) - 1, &br);
        f_close(&fp);
        for (UINT j = 0; j < br; j++)
            if (buf[j] == '\r' || buf[j] == '\n') { buf[j] = 0; break; }
        log_color(COL_OK, "  atmosphere   : %s (%s)\n",
            buf[0] ? buf : "(empty)", ams_paths[i] + 2);  /* skip "0:" */
        ams_found = true;
        break;
    }
    if (!ams_found) {
        FILINFO ams_fi;
        FRESULT fr = f_stat("0:/atmosphere/contents", &ams_fi);
        log_color(fr == FR_OK ? COL_WARN : COL_DEFAULT,
            "  atmosphere   : %s\n",
            fr == FR_OK ? "version file absent, contents/ exists"
                        : "absent");
    }

    /* Hekate version is embedded in /bootloader/update.bin: the
     * `ipl_ver_meta_t` lives right after start.S code (PATCHED_RELOC_SZ
     * = 0x94) AND boot_cfg_t (sizeof = 0x84), so magic is at offset
     * 0x94 + 0x84 = 0x118 and version at 0x11C. Magic is "ICTC"
     * (0x43544349). Version field packs each ASCII digit into a byte:
     * (mj<<0) | (mn<<8) | (hf<<16) | (rl<<24). */
    FRESULT hk_fr = f_open(&fp, "0:/bootloader/update.bin", FA_READ);
    if (hk_fr == FR_OK) {
        u8 hdr[0x130] = {0};
        UINT br = 0;
        f_read(&fp, hdr, sizeof(hdr), &br);
        f_close(&fp);
        u32 magic = *(u32 *)(hdr + 0x118);
        u32 ver   = *(u32 *)(hdr + 0x11C);
        if (magic == 0x43544349) {
            int mj = (ver        & 0xFF) - '0';
            int mn = ((ver >> 8) & 0xFF) - '0';
            int hf = ((ver >> 16) & 0xFF) - '0';
            log_color(COL_OK, "  hekate       : v%d.%d.%d (update.bin)\n", mj, mn, hf);
        } else {
            log_color(COL_WARN, "  hekate       : update.bin magic mismatch (0x%08X)\n", magic);
        }
    } else {
        log_color(COL_DEFAULT, "  hekate       : update.bin absent\n");
    }

    /* emuMMC config, plain ini, cleartext. Just show its presence. */
    FRESULT em_fr = f_stat("0:/emuMMC/emummc.ini", &fi);
    log_color(em_fr == FR_OK ? COL_OK : COL_DEFAULT,
        "  emummc.ini   : %s\n", em_fr == FR_OK ? "present" : "absent");
}

/* ------------------------------------------------------------------------ */
/* SD-card report                                                           */

static FATFS s_fs;
static FIL   s_fp;
static bool  s_mounted = false;
static char  s_report_path[128];

/* Report buffer that's filled DURING the boot dump pass and reused by
 * save_report. The boot dump runs every probe once and emits to UART;
 * setting _log_capture before the dump captures the same text into
 * this buffer at zero extra cost. save_report then just writes the
 * buffer, no second probe sweep, no I2C re-scans, no eMMC reads.
 *
 * This was the cause of the "rendering pager" pause: the old save_report
 * called run_all_probes() a SECOND time to fill its buffer, which on
 * real hardware is 5-10 s of redundant I2C / SDMMC activity.
 *
 * 32 KB holds the full report comfortably (~28 KB observed in practice
 * with all probes enabled). Allocated once via heap_alloc in ipl_main. */
static char *g_report_body = NULL;

static int save_report(void)
{
    if (!g_sd_ok) {
        log_color(COL_ERR, "[save] SD not ready - cannot write report\n");
        return 1;
    }
    if (!g_emmc_ok) {
        log_color(COL_ERR, "[save] eMMC not ready - no serial for path\n");
        return 1;
    }
    if (!g_report_body) {
        log_color(COL_ERR, "[save] report buffer not allocated\n");
        return 3;
    }
    if (!s_mounted) {
        if (f_mount(&s_fs, "0:", 1) != FR_OK) {
            log_color(COL_ERR, "[save] f_mount failed\n");
            return 2;
        }
        s_mounted = true;
    }

    /* Build path = backup/<emmc_serial>/hwtest.txt */
    emmcsn_path_impl(s_report_path, "", "hwtest.txt", &emmc_storage);

    FRESULT fr = f_open(&s_fp, s_report_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        log_color(COL_ERR, "[save] f_open failed (%d): %s\n", fr, s_report_path);
        return 4;
    }
    UINT bw = 0;
    f_write(&s_fp, g_report_body, strlen(g_report_body), &bw);
    f_close(&s_fp);
    log_color(COL_OK, "[save] %s (%d bytes)\n", s_report_path, bw);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Verdict, cross-validation of fields gathered earlier                    */

/* The verdict is a pure aggregator over the dx registry. Each macro
 * below names the keys it cares about; we look each up, take the worst
 * severity, and emit one summary line. Adding a new sub-check is a
 * single dx_set() in the relevant probe + the key here. */
struct dx_macro {
    const char *name;          /* pre-padded to 15 cols for column-align */
    const char *const *keys;   /* NULL-terminated */
};

static const char *_k_boot[]    = { "soc_pmic_otp", "fuses_pkg1", NULL };
static const char *_k_pmic[]    = { "pmic_rails", "pmic_nverc",
                                    "pmic_irqsd", "pmic_intlbt",
                                    "max77812", NULL };
static const char *_k_charger[] = { "charger_pg", "charger_fault",
                                    "charger_batfet", "usb_pd", NULL };
static const char *_k_battery[] = { "batt_health", "batt_ntc",
                                    "fuel_devname", "fuel_por", NULL };
static const char *_k_thermal[] = { "soc_die_temp", "pcb_temp",
                                    "temp_agree", "fan_stalled", NULL };
static const char *_k_storage[] = { "emmc_health", "emmc_bus", "sd_bus",
                                    "gpt", "kfuse", "prodinfo", NULL };
static const char *_k_display[] = { "dsi_id", "backlight", NULL };
static const char *_k_inputs[]  = { "touch_id", "als_id", NULL };
static const char *_k_memory[]  = { "dram_sym", "dram_mr4", "plls", NULL };

static const struct dx_macro _macros[] = {
    { "Boot integrity", _k_boot    },
    { "PMIC          ", _k_pmic    },
    { "Charger       ", _k_charger },
    { "Battery       ", _k_battery },
    { "Thermal       ", _k_thermal },
    { "Storage       ", _k_storage },
    { "Display       ", _k_display },
    { "Inputs        ", _k_inputs  },
    { "Memory        ", _k_memory  },
};

static void probe_verdict(void)
{
    HEADER("[Verdict]");
    int pass = 0, warn = 0, fail = 0;

    for (size_t m = 0; m < sizeof(_macros)/sizeof(_macros[0]); m++) {
        const struct dx_macro *mc = &_macros[m];
        int p = 0, w = 0, f = 0;
        const char *why = NULL;
        for (const char *const *k = mc->keys; *k; k++) {
            const dx_finding_t *fnd = dx_get(*k);
            if (!fnd) continue;     /* probe didn't run / didn't set */
            switch (fnd->sev) {
            case DX_PASS: p++; break;
            case DX_WARN:
                w++;
                if (!why) why = fnd->detail[0] ? fnd->detail : *k;
                break;
            case DX_FAIL:
                f++;
                if (!why) why = fnd->detail[0] ? fnd->detail : *k;
                break;
            }
        }
        u32 col = f ? COL_ERR : w ? COL_WARN : COL_OK;
        const char *state = f ? "FAIL" : w ? "warn" : "pass";
        if (why)
            log_color(col, "  %s : %s (%s)\n", mc->name, state, why);
        else
            log_color(col, "  %s : %s\n", mc->name, state);
        pass += p; warn += w; fail += f;
    }

    LOG("\n");
    u32 col = fail ? COL_ERR : warn ? COL_WARN : COL_OK;
    log_color(col, "  Verdict      : %d pass, %d warn, %d fail\n",
        pass, warn, fail);
}

/* ------------------------------------------------------------------------ */
/* Pager + UART command loop                                                */

typedef void (*probe_fn_t)(void);

/* Each entry is ONE LCD page (one probe) with a display name. Multiple
 * consecutive entries can share a display name; from the LCD pager's
 * point of view they're separate pages (so each fits on screen), but
 * over UART they emit the same `--- name ---` boundary. The host
 * viewer's parser treats same-named boundaries as the same logical
 * page and accumulates [Section] headers into it, which is what we
 * want, since each probe emits its own [Section] header for its data.
 *
 * The previous design (one entry holding multiple probe pointers) made
 * the LCD overflow once a domain held more than ~45 rows of output.
 * Splitting back to one-probe-per-entry caps each LCD page at the
 * single probe's worth of text while keeping the host viewer's grouped
 * presentation. */
struct page_entry {
    probe_fn_t  fn;
    const char *name;
};

static const struct page_entry g_pages[] = {
    /* Index 0: top-level verdict. Depends on globals populated by every
     * other probe, so run_all_probes runs it LAST. The pager shows it
     * as the first navigable page. */
    {probe_verdict,    "Verdict"},

    /* Identity. */
    {probe_soc,        "SoC"},
    {probe_fuses,      "Fuses"},
    {probe_kfuse,      "Fuses"},

    /* Power & charging. Each probe gets its own LCD page; they all
     * share the "Power & charging" name so the host viewer groups them. */
    {probe_pmic,       "Power & charging"},
    {probe_max77812,   "Power & charging"},
    {probe_pmic_gpios, "Power & charging"},
    {probe_regulators, "Power & charging"},
    {probe_5v,         "Power & charging"},
    {probe_battery,    "Power & charging"},
    {probe_charger,    "Power & charging"},
    {probe_usbpd,      "Power & charging"},
    {probe_thermal,    "Power & charging"},
    {probe_fan,        "Power & charging"},

    /* Memory & clocks. */
    {probe_dram,       "Memory & clocks"},
    {probe_clocks,     "Memory & clocks"},

    /* All storage probes share one display name, this is the request
     * "why are partitions/GPT/health in different panes when Storage
     * exists". Now they all live under "Storage" on the host viewer. */
    {probe_storage,    "Storage"},
    {probe_serial,     "Storage"},
    {probe_partitions, "Storage"},
    {probe_emmc_health,"Storage"},
    {probe_gpt,        "Storage"},
    {probe_boot0_pkg1, "Storage"},
    {probe_autorcm,    "Storage"},
    {probe_sd_content, "Storage"},

    /* Display + backlight. */
    {probe_display,    "Display"},
    {probe_backlight,  "Display"},

    /* Touch + Joy-Con + buttons + ambient light. */
    {probe_touch,      "Inputs"},
    {probe_als,        "Inputs"},
    {probe_joycon,     "Inputs"},
    {probe_inputs,     "Inputs"},

    /* Low-level dumps. probe_uart_b is excluded in the JC_PROBE build
     * where the Joy-Con stack owns UART_B. */
    {probe_gpio_census,"Raw state"},
#ifndef JC_PROBE
    {probe_uart_b,     "Raw state"},
#endif
    {probe_reset,      "Raw state"},
    {probe_pmc_scratch,"Raw state"},
};
#define N_PAGES (sizeof(g_pages) / sizeof(g_pages[0]))

void run_all_probes(void)
{
    /* Data probes populate the dx registry; the verdict aggregates it.
     * Reset between sweeps so a refresh-all (`a`) doesn't see stale
     * findings from the previous run. The verdict runs LAST because it
     * reads what the data probes deposited; the pager still shows it
     * first via index 0. */
    dx_reset();
    for (u32 i = 1; i < N_PAGES; i++) {
        status_set(g_pages[i].name);
        log_color(COL_HEADER, "\n--- %s ---\n", g_pages[i].name);
        g_pages[i].fn();
    }
    status_set(g_pages[0].name);
    log_color(COL_HEADER, "\n--- %s ---\n", g_pages[0].name);
    g_pages[0].fn();
    status_set("done - rendering pager");
}

/* Compute the position of LCD page idx within its display group, e.g.
 * for a Storage entry where idx = 17 returns (group_idx=4, group_total=7)
 * meaning "this is the 4th of 7 pages with name 'Storage'". The LCD
 * header surfaces this so the user knows where they are inside a
 * multi-page domain. */
static void _page_group_pos(int idx, int *out_group_idx, int *out_group_total)
{
    const char *name = g_pages[idx].name;
    int total = 0, pos = 0;
    for (u32 i = 0; i < N_PAGES; i++) {
        if (strcmp(g_pages[i].name, name) == 0) {
            total++;
            if ((int)i <= idx) pos = total;
        }
    }
    *out_group_idx = pos;
    *out_group_total = total;
}

static void render_page(int idx)
{
    gfx_clear_grey(0x1B);
    gfx_con_setpos(0, 0);
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);

    /* The page-number header + help banner + separator are LCD chrome;
     * the host viewer doesn't need them and showing them just confuses
     * its parser (they don't match any of the three line shapes it
     * recognises). Suppress them from UART, then re-enable so the
     * actual probe body still ships over the wire. */
    int gidx, gtot;
    _page_group_pos(idx, &gidx, &gtot);
    bool prev = _log_no_uart;
    _log_no_uart = true;
    if (gtot > 1) {
        log_color(COL_HEADER, "hwtest - %s [%d/%d]  -  page %d/%d\n",
                  g_pages[idx].name, gidx, gtot, idx + 1, (int)N_PAGES);
    } else {
        log_color(COL_HEADER, "hwtest - %s  -  page %d/%d\n",
                  g_pages[idx].name, idx + 1, (int)N_PAGES);
    }
    LOG("n/VOL+ next | p/VOL- prev | r refresh | a all | s save | q/POWER off\n");
    LOG("=============================================================\n\n");
    _log_no_uart = prev;

    /* Page body goes to BOTH sinks. We emit the same `--- Name ---`
     * boundary the boot dump uses so the host viewer parses each
     * pager-driven re-render as a fresh page (or refresh of an
     * existing one). The host viewer treats consecutive identical
     * boundary names as the same logical page (Storage is 7 LCD
     * pages but one host pane). */
    log_color(COL_HEADER, "\n--- %s ---\n", g_pages[idx].name);
    g_pages[idx].fn();
}

static int uart_getc(void)
{
#ifdef JC_PROBE
    /* No DEBUG_UART_PORT in this build -> UART_B isn't initialised, so
     * uart_recv would block on UART_LSR.RDR forever. The pager loop
     * still works, just without keyboard shortcuts over serial. */
    return -1;
#else
    u8 c;
    if (uart_recv(UART_B, &c, 1) != 1)
        return -1;
    return c;
#endif
}

void ipl_main(void)
{
    /* Bring up clocks, regulators, I2C, etc. */
    hw_init();

    /* Move the stack into IPL load region and wire up a heap. */
    pivot_stack(IPL_LOAD_ADDR);
    heap_init((void *)IPL_HEAP_START);

    /* Bring up everything UART_B RX needs on the right Joy-Con rail.
     * hw_init() only configures TX (PG0) for the debug log; RX requires
     * four additional steps that are otherwise only done from
     * bdk/input/joycon.c::_jc_power_supply when a real Joy-Con is being
     * polled:
     *
     *   1. Route PG1 (UART2_RX) to SPIO mode so the Tegra UART2
     *      controller actually owns the pin instead of the GPIO block.
     *   2. Override hw_init's PINMUX_AUX_UART2_RX (= INPUT_ENABLE | TRISTATE)
     *      with INPUT_ENABLE | PULL_UP. The Tegra UART idles at logic HIGH
     *      and Hekate's TRISTATE setup leaves the pin high-impedance with
     *      no internal pull. Joy-Cons happen to drive the line continuously
     *      so they get away with it, but a passive USB-UART chassis adapter
     *      that only drives during transmission lets the line float between
     *      bytes and the receiver sees noise / framing errors.
     *      switch-coreboot's _config_uart_b enables PULL_UP for exactly
     *      this reason
     *      (https://github.com/lulle2007200/switch-coreboot/blob/593ce32688de8600c6e61e7ccdd0e785f0e3267c/src/soc/nvidia/tegra210/lp0/sc7_exit/tegra_sc7_exit.c#L72).
     *   3. Enable the 5 V supply to the Joy-Con-R rail. The RX line on
     *      the chassis goes through a level-shifter that's powered from
     *      this rail; without it the line stays floating regardless of
     *      what the host adapter sends.
     *   4. Drive PK3 high (Joy-Con-R charge-enable). This gates the
     *      MOSFET that bridges the Tegra-side signals to the rail-side
     *      pads.
     *
     * TX appears to "just work" without (2)/(3)/(4) because the Tegra side
     * drives the line directly, but the return path is asymmetric. */
#ifndef JC_PROBE
    gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_SPIO);
    PINMUX_AUX(PINMUX_AUX_UARTX_RX(UART_B)) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP;
    regulator_5v_enable(REGULATOR_5V_JC_R);
    PINMUX_AUX(PINMUX_AUX_GPIO_PK3) = PINMUX_DRIVE_4X | PINMUX_PULL_DOWN | 2;
    gpio_direction_output(GPIO_PORT_K, GPIO_PIN_3, GPIO_HIGH);

    /* Drain any phantom byte the receiver caught while the line was
     * transitioning from "stuck LOW" (no pull) to "idle HIGH" (PULL_UP
     * applied above). The LOW->HIGH edge briefly looks like a START bit
     * + 8 zero data bits + STOP bit, decoding to a spurious 0x00 in the
     * RX FIFO. uart_init() already cleared the FIFO during hw_init(),
     * but our PINMUX override happens *after* uart_init, so the
     * transition byte arrives later. Settle the line first then flush. */
    usleep(2000);
    {
        volatile u32 *uart_b_lsr = (volatile u32 *)(0x70006040 + 0x14);
        volatile u32 *uart_b_rbr = (volatile u32 *)(0x70006040 + 0x00);
        while (*uart_b_lsr & 0x01)        /* LSR.RDR */
            (void)*uart_b_rbr;            /* read and discard */
    }
#endif  /* !JC_PROBE , JC_PROBE-specific init runs after display setup so we can animate it */

    static const char banner[] =
        "\n=== hwtest === read-only,"
        " single write: backup/<emmc_serial>/hwtest.txt\n";
    uart_send_crlf(UART_B, (u8 *)banner, strlen(banner));

    /* Bring up the LCD + console. The LCD is physically 720 x 1280
     * portrait, but our gfx_putc renders in landscape: cursor coords go
     * 0..1279 along the long axis and 0..719 along the short axis. The
     * pixel-write helper inside gfx.c rotates each pixel 90 deg CW.
     * Hold the console with the right Joy-Con rail (UART side) at the
     * top to read the screen naturally. */
    display_init();
    u32 *fb = display_init_window_a_pitch();
    gfx_init_ctxt(fb, 1280, 720, 720);
    gfx_con_init();

    /* Render the "initialising" splash IMMEDIATELY (before backlight
     * ramp + before any other init) so when the panel becomes visible
     * the user sees content rather than a few seconds of dark screen.
     * The pager loop's first render_page() will overwrite this. */
    gfx_clear_grey(0x1B);
    gfx_con_setpos(0, 0);
    gfx_con_setcol(COL_HEADER, 1, COL_BG);
    gfx_printf("hwtest - initialising\n\n");
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);

    /* Backlight on (1 s ramp). Drawing happens in parallel with the
     * ramp - by the time the panel is bright enough to read, the
     * splash text below will already be on screen. */
    display_backlight_pwm_init();
    display_backlight_brightness(100, 1000);

    /* SD card + eMMC init done unconditionally before the boot dump.
     *
     * This brings up LDO2 (SDMMC1 rail), enables SDMMC1/SDMMC4 clocks,
     * and writes the SD/eMMC pinmux. Without doing it here, probe_storage
     * is the lazy entry point, but that runs AFTER probe_regulators
     * and probe_clocks in page order, so the boot dump captured those
     * regulators / clocks in their *pre-init* state while a subsequent
     * 'a' refresh-all sees them post-init. Doing the init up front
     * makes both captures byte-identical for the storage-derived
     * fields, which removes a class of "why did the values change?"
     * confusion when comparing snapshots.
     *
     * Failure handling: sd_initialize and emmc_initialize each return
     * non-zero when the card / chip can't be brought up. Both have
     * bounded timeouts (~1-2 s) inside Hekate, so an empty SD slot or
     * a faulty SD reader produces a fail-then-continue, NOT a hang.
     * probe_storage reads sd_storage.initialized + emmc_storage.initialized
     * to detect this state and prints "sd_initialize FAILED" cleanly
     * instead of crashing. save_report sees g_sd_ok=false and logs
     * "[save] SD not ready" without retrying. This means the payload
     * boots and runs end-to-end whether the user RCM-injected via SD,
     * via TegraRcmGUI, or via a unit with a dead SD reader, there's
     * no failure mode that strands them at a black screen.
     *
     * UHS-I timing note: on the JC_PROBE build, sd_initialize must run
     * BEFORE jc_init_hw, the long Joy-Con poll between rail-up and
     * SD init would otherwise cause the CMD11 1.8 V switch to silently
     * fall back to HS25 at 3.3 V. */
    gfx_printf("  SD card / eMMC init     ...");
    sd_initialize(false);                /* return value intentionally
                                            ignored; probe_storage will
                                            re-check sd_storage.initialized
                                            and skip / report cleanly */
    emmc_initialize(false);              /* same, eMMC is soldered, so
                                            failure here is genuinely a
                                            board-fault diagnostic, not
                                            a missing-card situation */
    gfx_printf(" done\n");

#ifdef JC_PROBE
    /* Joy-Con stack init: powers both rails, runs Hekate's PH6/PE6
     * unlatch dance, then poll-spins the controllers for 5 s so the
     * MCU has time to wake up + answer the Discover request. */
    gfx_printf("  Joy-Con stack init      ...");
    jc_init_hw();
    gfx_printf(" done\n");

    /* 250 polls × 20 ms = 5 s. Print one dot every ~250 ms (every 12
     * iterations) so the operator can see polling is making progress. */
    gfx_printf("  Polling Joy-Cons (~5 s) ...");
    for (u32 i = 0; i < 250; i++) {
        joycon_poll();
        if (i && (i % 12 == 0))
            gfx_printf(".");
        msleep(20);
    }
    gfx_printf(" done\n");
#endif

    /* Common second phase: thermal sensor + clock boost, then probes.
     * These steps are quick (<100 ms) so they get a single combined
     * line rather than per-step animation. */
    gfx_printf("  Hardware setup          ...");
    /* TMP451 needs an explicit conversion-rate write to leave its
     * power-on 0.0625 Hz mode. */
    tmp451_init();
    /* Match Hekate: boost BPMP/SCLK before SDMMC init. */
    bpmp_clk_rate_set(BPMP_CLK_NORMAL);
    gfx_printf(" done\n\n");
    gfx_printf("Running probes, building report...\n");

    /* Allocate the SD-report buffer BEFORE the boot dump and arm the
     * capture sink. Every line emitted by the dump pass below also
     * goes into this buffer via _log_capture_append, so save_report
     * can later just dump the buffer to SD without re-running every
     * probe (which used to cost an extra 5-10 s on real hardware --
     * the pause the user noticed at "rendering pager"). */
    const u32 report_cap = 32 * 1024;
    g_report_body = (char *)malloc(report_cap);
    if (g_report_body) {
        g_report_body[0] = 0;
        _log_capture     = g_report_body;
        _log_capture_pos = 0;
        _log_capture_cap = report_cap;
    }

    /* Dump every probe to UART once at boot - the serial log is the
     * primary diagnostic capture. The status spinner is only enabled
     * here, subsequent on-demand probe runs (host 'G' refresh, plain
     * 'r' refresh, save_report) leave g_show_status false so probes
     * don't draw over the pager. */
    g_show_status  = true;
    _log_uart_only = true;
    log_color(COL_HEADER, "\n=== hwtest full UART dump ===\n");
    run_all_probes();
    log_color(COL_HEADER, "\n=== end of dump - pager active on LCD ===\n\n");
    _log_uart_only = false;
    g_show_status  = false;

    /* Detach the capture sink. The buffer remains for save_report. */
    _log_capture = NULL;

    /* Auto-save the report to SD if we managed to reach the eMMC for
     * the directory name. Re-saving via the 's' UART key just writes
     * the same buffer again, no probes re-run. */
    if (g_sd_ok && g_emmc_ok)
        save_report();

    int page = 0;
    render_page(page);

    for (;;) {
        int c   = uart_getc();
        u8  btn = btn_read();

        bool advance = (c == 'n' || c == ' ') || (btn & BTN_VOL_UP);
        bool back    = (c == 'p' || c == 'b') || (btn & BTN_VOL_DOWN);
        bool refresh = (c == 'r');
        bool refresh_all = (c == 'a' || c == 'A');
        bool save    = (c == 's' || c == 'S');
        bool quit    = (c == 'q' || c == 'Q') || (btn & BTN_POWER);

        if (advance || back) {
            /* Batch consecutive nav chars from the UART RX FIFO before
             * rendering. Without this, sending "nnnnn" from the host
             * would re-run probes 5 times and dump 5 page bodies over
             * UART, slow and wasteful when the user only cares about
             * the final destination. We accumulate a delta then render
             * once for the destination page. Anything other than n/p/
             * space/b breaks the batch. */
            int delta = advance ? 1 : -1;
            for (;;) {
                int next = uart_getc();
                if (next == 'n' || next == ' ') delta++;
                else if (next == 'p' || next == 'b') delta--;
                else if (next < 0) break;       /* FIFO empty, render now */
                else {
                    /* Different command queued. We can't push it back to
                     * the FIFO, so process the navigation now and let
                     * the next loop iteration re-read this byte... except
                     * uart_recv has already consumed it. Stash + handle.
                     * For simplicity we just drop it here, rare in
                     * practice (user wouldn't follow nav with another
                     * command in a single burst). */
                    break;
                }
            }
            int new_page = ((int)page + delta) % (int)N_PAGES;
            if (new_page < 0) new_page += (int)N_PAGES;
            page = new_page;
            render_page(page);
        } else if (refresh) {
            render_page(page);
        } else if (c == 'G') {
            /* Group refresh: 'G' followed by an ASCII group name and a
             * '\n'. Re-runs every probe whose g_pages[].name matches the
             * supplied string and re-emits each over UART, WITHOUT
             * touching the LCD. The host viewer uses this to refresh a
             * single pane (e.g. "Storage") without dragging the on-
             * console display away from whatever the operator is
             * looking at, the user explicitly asked for refresh that
             * doesn't switch panes.
             *
             * 200 ms timeout on the name read; any byte other than \n
             * / \r terminates the name normally. Empty name = no-op. */
            char gname[64];
            u32 glen = 0;
            u32 deadline = get_tmr_us() + 200000;
            while (glen < sizeof(gname) - 1) {
                int b = uart_getc();
                if (b < 0) {
                    if ((s32)(get_tmr_us() - deadline) > 0) break;
                    msleep(2);
                    continue;
                }
                if (b == '\n' || b == '\r') break;
                gname[glen++] = (char)b;
            }
            gname[glen] = 0;
            if (glen > 0) {
                bool prev = _log_uart_only;
                _log_uart_only = true;   /* suppress LCD writes */
                for (u32 i = 0; i < N_PAGES; i++) {
                    if (strcmp(g_pages[i].name, gname) == 0) {
                        log_color(COL_HEADER, "\n--- %s ---\n",
                                  g_pages[i].name);
                        g_pages[i].fn();
                    }
                }
                _log_uart_only = prev;
            }
        } else if (refresh_all) {
            /* 'a' = re-run every probe and re-emit the full UART dump,
             * just like the boot-time sweep. The host viewer uses this to
             * pull a fresh snapshot of every domain in one shot.
             * The pager's currently-shown LCD page is then re-rendered
             * so the on-console display stays in sync. */
            _log_uart_only = true;
            log_color(COL_HEADER, "\n=== hwtest full UART dump ===\n");
            run_all_probes();
            log_color(COL_HEADER, "\n=== end of dump - pager active on LCD ===\n\n");
            _log_uart_only = false;
            render_page(page);
        } else if (save) {
            save_report();
        } else if (quit) {
            while (btn_read() & BTN_POWER)
                ;
            /* Power the console off cleanly. Asserting MAX77620_REG_ONOFFCNFG1
             * bit PWR_OFF tells the main PMIC to drop every regulator rail in
             * the proper sequence, the SoC powers down for real.
             *
             * Why not reboot to RCM? `power_set_state(REBOOT_RCM)` runs
             * hw_deinit which hangs partway through (display_end works,
             * but the BPMP/MMU teardown stalls on this payload). The bare
             * watchdog PMC reset DOES work but only re-enters RCM, it
             * doesn't reload Hekate, so a picofly-less console ends up
             * in RCM with no payload waiting. A clean power-off lets the
             * user press POWER themselves to wake the modchip / RCM jig
             * for the next payload. */
            log_color(COL_HEADER, "Powering off...\n");
            i2c_send_byte(I2C_5, MAX77620_I2C_ADDR,
                          MAX77620_REG_ONOFFCNFG1, MAX77620_ONOFFCNFG1_PWR_OFF);
            while (1)
                ;
        } else {
            msleep(20);
        }

        /* Debounce: if a button is still held, wait for release before
         * accepting the next press. UART chars don't need debouncing. */
        if (btn) {
            while (btn_read() == btn)
                msleep(20);
        }
    }
}
