/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: storage probes
 */
#include "hwtest.h"


/* SD/eMMC global init state, reused by probe_storage and save_report. */
bool g_sd_ok   = false;
bool g_emmc_ok = false;


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

void probe_storage(void)
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
}

void probe_partitions(void)
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
 * engine: the AES key registers are write-only, so we can't read them
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

/* CAL0 text fields are fixed-length and null-padded. Copy up to max bytes,
 * stopping at the first byte that isn't printable ASCII; dst needs max+1. */
static void cal0_str(char *dst, const char *src, u32 max)
{
    u32 i;
    for (i = 0; i < max; i++) {
        u8 c = (u8)src[i];
        if (c < 0x20 || c >= 0x7F) break;
        dst[i] = (char)c;
    }
    dst[i] = 0;
}

void probe_serial(void)
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

    /* Pull the rest of CAL0 while BIS is still up. The body hash covers
     * body_size bytes starting at 0x40, and most of the factory calibration
     * we decode further down sits well past the two sectors above. body_size
     * comes off the (still unverified) header, so bound it before it turns
     * into a malloc size. */
    u32 body_size = *(u32 *)&hdr[0x08];
    u8 *cal0 = NULL;
    if (!hdr_res && body_size >= sizeof(nx_emmc_cal0_t) - 0x40 &&
        body_size <= SZ_64K) {
        u32 total = ((0x40 + body_size) + 511) & ~511u;
        cal0 = malloc(total);
        if (cal0 && nx_emmc_bis_read(0, total / 512, cal0)) {
            free(cal0);
            cal0 = NULL;
        }
    }
    nx_emmc_bis_end();

    bool cal0_ok = !hdr_res && !srl_res &&
                   hdr[0] == 'C' && hdr[1] == 'A' &&
                   hdr[2] == 'L' && hdr[3] == '0';
    /* Two-state finding: pass when keys decoded; warn (not fail) when
     * either keys are absent or wrong-console: keyfile config is
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
        free(cal0);
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
        free(cal0);
        return;
    }

    /* Extract serial: CAL0 0x250, i.e. sector 1 + 0x50. */
    char serial[25];
    cal0_str(serial, (const char *)&buf[0x50], 24);
    if (serial[0] == 0) {
        log_color(COL_WARN, "  Serial       : (empty / wiped)\n");
    } else {
        log_color(COL_OK, "  Serial       : %s\n", serial);
    }

    /* Radio factory calibration, from the same two CAL0 sectors we already
     * decrypted (offsets per switchbrew's Calibration page):
     *
     *   0x080  WlanCountryCodeNum       (sector 0 + 0x80)
     *   0x210  WlanMacAddress           (sector 1 + 0x10)
     *   0x220  BdAddress                (sector 1 + 0x20)
     *
     * Why this is on the repair path: HOS refuses to bring Wi-Fi up when the
     * WLAN calibration is missing or wiped, and the symptom - "can't connect
     * to Wi-Fi" - looks identical to a dead radio. An all-00 / all-FF MAC or
     * a zero country-code count means the DATA is gone, so no amount of
     * chip-level work will fix it; a valid MAC here points the finger back at
     * the hardware (or at HOS config). Same for the BD address and Bluetooth.
     *
     * All-FF is what an erased flash region reads as; all-00 is what a
     * zero-filled / re-created PRODINFO looks like. */
    const u8 *wmac = &buf[0x10];
    const u8 *bmac = &buf[0x20];
    u32 cc_num = (u32)hdr[0x80] | ((u32)hdr[0x81] << 8) |
                 ((u32)hdr[0x82] << 16) | ((u32)hdr[0x83] << 24);

    bool w_zero = true, w_ff = true, b_zero = true, b_ff = true;
    for (int i = 0; i < 6; i++) {
        if (wmac[i] != 0x00) w_zero = false;
        if (wmac[i] != 0xFF) w_ff   = false;
        if (bmac[i] != 0x00) b_zero = false;
        if (bmac[i] != 0xFF) b_ff   = false;
    }
    bool w_bad = w_zero || w_ff;
    bool b_bad = b_zero || b_ff;

    log_color(w_bad ? COL_ERR : COL_OK,
        "  WLAN MAC     : %02X:%02X:%02X:%02X:%02X:%02X%s\n",
        wmac[0], wmac[1], wmac[2], wmac[3], wmac[4], wmac[5],
        w_zero ? "  (all zero - calib wiped!)" :
        w_ff   ? "  (all FF - calib erased!)" : "");
    log_color(b_bad ? COL_ERR : COL_OK,
        "  BD MAC       : %02X:%02X:%02X:%02X:%02X:%02X%s\n",
        bmac[0], bmac[1], bmac[2], bmac[3], bmac[4], bmac[5],
        b_zero ? "  (all zero)" : b_ff ? "  (all FF)" : "");
    /* A stock console lists a couple of hundred country codes; 0 means the
     * region table never got written, which alone stops Wi-Fi from coming up. */
    log_color(cc_num == 0 || cc_num > 128 ? COL_ERR : COL_OK,
        "  WLAN cc num  : %d%s\n", cc_num,
        cc_num == 0     ? "  (no country codes - Wi-Fi cannot init)" :
        cc_num > 128    ? "  (out of range - corrupt)" : "");

    dx_set("wlan_cal",
        (w_bad || cc_num == 0 || cc_num > 128) ? DX_FAIL : DX_PASS,
        w_zero        ? "WLAN MAC all-zero" :
        w_ff          ? "WLAN MAC all-FF"   :
        cc_num == 0   ? "no WLAN country codes" :
        cc_num > 128  ? "WLAN cc num %d corrupt" : "", cc_num);

    /* --- Rest of CAL0 ---------------------------------------------------
     * Everything past the radio MACs lives beyond the two sectors above, so
     * walk the full copy as the real struct instead of hand-counted offsets.
     * Two things make this worth carrying on a repair bench:
     *
     *  - body_sha256 is a factory hash over the whole body. It catches a
     *    PRODINFO that decrypts cleanly (right keys, right magic) but has
     *    been corrupted or hand-edited since: the state that makes HOS
     *    refuse to boot without saying why.
     *  - the rest is an inventory of what the factory fitted. Held against
     *    what the hardware reports now, a swapped panel or a different
     *    battery shows up straight away, and it explains odd behaviour:
     *    HOS keeps driving the replacement with the original's calibration.
     */
    if (!cal0) {
        log_color(COL_WARN,
            "  CAL0 body    : not read (body_size 0x%X out of range)\n",
            body_size);
        return;
    }
    const nx_emmc_cal0_t *c = (const nx_emmc_cal0_t *)cal0;
    char str[0x20];

    u8 hash[0x20];
    se_sha_hash_256_oneshot(hash, &c->cfg_id1, c->body_size);
    bool hash_ok = !memcmp(hash, c->body_sha256, sizeof(hash));
    log_color(hash_ok ? COL_OK : COL_ERR,
        "  CAL0 body    : SHA-256 %s\n",
        hash_ok ? "OK" : "MISMATCH - body corrupt");
    LOG("  CAL0 header  : v%d, %d B body, %d update(s)\n",
        c->version, c->body_size, c->update_cnt);
    dx_set("cal0_hash", hash_ok ? DX_PASS : DX_FAIL,
        hash_ok ? "" : "CAL0 body SHA-256 mismatch");

    cal0_str(str, c->cfg_id1, sizeof(c->cfg_id1));
    LOG("  Config ID    : %s\n", str[0] ? str : "(empty)");

    /* nn::settings::system::ProductModel. Worth cross-checking against the
     * chip we actually booted on: PRODINFO travels with the eMMC, so a board
     * that reports Iowa while the SoC reads T210 means the eMMC came off a
     * different console, which is exactly what a donor-board repair looks
     * like from software, and it explains a unit that boots but is refused
     * online. */
    static const char *prod_models[] = {
        "invalid", "Nx (Erista)", "Copper (Erista dev)", "Iowa (Mariko)",
        "Hoag (Lite)", "Calcio (Mariko dev)", "Aula (OLED)"
    };
    u32 pm = c->product_model;
    LOG("  Product model: %d - %s\n", pm,
        pm < ARRAY_SIZE(prod_models) ? prod_models[pm] : "unknown");

    bool soc_mariko = (((APB_MISC(APB_MISC_GP_HIDREV) >> 4) & 0xF) == 2);
    /* 1/2 are Erista silicon, 3..6 are Mariko silicon. Anything outside that
     * we simply do not judge. */
    bool pm_mariko = (pm >= 3 && pm <= 6);
    bool pm_erista = (pm == 1 || pm == 2);
    if (pm_mariko || pm_erista) {
        bool agree = (pm_mariko == soc_mariko);
        log_color(agree ? COL_OK : COL_WARN,
            "  Model vs SoC : %s\n",
            agree ? "agree"
                  : "DISAGREE - PRODINFO is from another console family");
        dx_set("cal0_model", agree ? DX_PASS : DX_WARN,
            agree ? "" : "CAL0 says model %d, SoC is %s", pm,
            soc_mariko ? "Mariko" : "Erista");
    }

    /* nyx's decode: CAL0 packs the vendor in byte 0 and the panel type in
     * byte 2, which recombine into the same 0xVVTT the DSI read returns.
     * An all-zero field is not a panel ID, it means the factory never wrote
     * one, which is common on Mariko. Comparing against it would report every
     * console as having a replaced screen, so don't. */
    u32 lcd_vendor = c->lcd_vendor & 0xFFFFFF;
    u16 cal_panel = (u16)(((lcd_vendor & 0xFF) << 8) | (lcd_vendor >> 16));
    if (!lcd_vendor) {
        LOG("  LCD vendor   : not recorded in CAL0\n");
    } else {
        const char *cal_name = panel_model_name(cal_panel);
        LOG("  LCD vendor   : %06X -> 0x%04X %s\n", lcd_vendor, cal_panel,
            cal_name ? cal_name : "(unknown)");

        /* Only compare when the DSI ID actually read back; a failed read
         * returns the 0xCCCCCC sentinel and would look like a mismatch. A
         * real mismatch is a warning, not a fault: a replaced panel is
         * ordinary repair work, it just means the factory calibration no
         * longer matches what is fitted. */
        if ((display_get_verbose_panel_id() & 0xFFFFFF) != 0xCCCCCC) {
            u16 dsi_panel = display_get_decoded_panel_id();
            bool same = (dsi_panel == cal_panel);
            log_color(same ? COL_OK : COL_WARN,
                "  Panel fitted : 0x%04X %s\n", dsi_panel,
                same ? "(matches CAL0)" : "(differs from CAL0 - replaced?)");
            dx_set("cal0_panel", same ? DX_PASS : DX_WARN,
                same ? "" : "fitted panel 0x%04X, CAL0 says 0x%04X",
                dsi_panel, cal_panel);
        }
    }

    /* Entries in the country-code table hold a regulatory-domain tag, not an
     * ISO country code: retail units carry a single "R1". Printed next to
     * the cc count above because a blank entry under a non-zero count means
     * the table itself is damaged, not just empty. */
    cal0_str(str, c->wlan_cc[0], sizeof(c->wlan_cc[0]));
    LOG("  WLAN cc tbl  : \"%s\" (last idx %d)\n", str, c->wlan_cc_last);

    cal0_str(str, c->battery_lot, sizeof(c->battery_lot));
    LOG("  Battery lot  : %s (rev %d)\n", str[0] ? str : "(empty)",
        c->battery_ver);
    LOG("  USB-C PD rev : %d\n", c->usbc_pwr_src_circuit_ver);
    LOG("  Touch IC     : vendor %d\n", c->touch_ic_vendor_id);
    LOG("  6-axis IMU   : type %d, mount %d\n",
        c->console_6axis_sensor_type, c->console_6axis_sensor_mount_type);
    LOG("  Stick L/R    : type 0x%02X / 0x%02X\n",
        c->analog_stick_type_l, c->analog_stick_type_r);
    LOG("  Region code  : %d\n", c->region_code);

    /* Speaker EQ/DRC tuning. An all-zero block means it was never written,
     * which HOS reads as "no calibration": quiet, flat-sounding audio on a
     * unit whose speakers are otherwise fine. */
    bool spk_blank = true;
    for (u32 i = 0; i < sizeof(c->spk_cal); i++) {
        if (((const u8 *)&c->spk_cal)[i]) { spk_blank = false; break; }
    }
    log_color(spk_blank ? COL_WARN : COL_OK, "  Speaker cal  : %s\n",
        spk_blank ? "blank (never calibrated)" : "present");

    free(cal0);
}

void probe_emmc_health(void)
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

    /* Additional JEDEC capability fields decoded from the raw EXT_CSD
     * blob. These aren't parsed into the BDK's mmc_ext_csd_t struct
     * but the full 512-byte response is cached in `raw_ext_csd[]`.
     *
     *   231 SEC_FEATURE_SUPPORT  - secure-erase, sanitize, trim caps
     *   232 TRIM_MULT            - 0 = TRIM not supported (perf flag)
     *   229 SEC_TRIM_MULT        - secure-trim timeout (0 = unsupported)
     *    61 DATA_SECTOR_SIZE     - 0=512B, 1=4KiB native sector mode
     *   503 HPI_FEATURES         - high-priority interrupt support
     *   162 RST_N_FUNCTION       - hardware reset behaviour
     *
     * These are diagnostically useful for two repair scenarios:
     *   - "eMMC works but HOS shutdown is glacially slow" -> often
     *     missing HPI / trim / sanitize support (chip is replacement
     *     part with reduced firmware features).
     *   - "Game saves take forever to write after a few months" ->
     *     trim disabled, wear leveling can't reclaim space efficiently. */
    u8 sec_feat = emmc_storage.raw_ext_csd[231];
    u8 trim_mlt = emmc_storage.raw_ext_csd[232];
    u8 strim_mlt= emmc_storage.raw_ext_csd[229];
    u8 sec_size = emmc_storage.raw_ext_csd[61];
    u8 hpi_feat = emmc_storage.raw_ext_csd[503];
    u8 rst_func = emmc_storage.raw_ext_csd[162];
    log_color(COL_DEFAULT,
        "  SEC_FEATURE  : 0x%02X (%s%s%s%s)\n", sec_feat,
        (sec_feat & 0x01) ? "secure-erase " : "",
        (sec_feat & 0x10) ? "auto-erase "   : "",
        (sec_feat & 0x40) ? "sanitize "     : "",
        sec_feat ? "" : "none");
    log_color(trim_mlt ? COL_OK : COL_WARN,
        "  TRIM_MULT    : 0x%02X (%s)\n", trim_mlt,
        trim_mlt ? "TRIM supported" : "TRIM NOT supported");
    LOG("  SEC_TRIM_MULT: 0x%02X\n", strim_mlt);
    LOG("  Sector size  : %s native (DATA_SECTOR_SIZE=0x%02X)\n",
        sec_size ? "4 KiB" : "512 B", sec_size);
    log_color(hpi_feat & 0x01 ? COL_OK : COL_WARN,
        "  HPI_FEATURES : 0x%02X (%s)\n", hpi_feat,
        (hpi_feat & 0x01) ? ((hpi_feat & 0x02) ? "supported, CMD12"
                                                : "supported, CMD13")
                          : "NOT supported");
    LOG("  RST_N_FUNC   : 0x%02X (%s)\n", rst_func,
        (rst_func & 0x03) == 0x01 ? "permanently enabled" :
        (rst_func & 0x03) == 0x02 ? "permanently disabled" :
                                    "temporarily disabled");

    /* Verdict signal: PRE_EOL_INFO + worst of life-used A/B. */
    dx_set("emmc_health",
        (eol == 3 || a >= 0x09 || b >= 0x09) ? DX_FAIL :
        (eol == 2 || a >= 0x07 || b >= 0x07) ? DX_WARN : DX_PASS,
        (eol == 3 || a >= 0x09 || b >= 0x09) ? "worn (eol %d, A=%d B=%d)" :
        (eol == 2 || a >= 0x07 || b >= 0x07) ? "aging (eol %d, A=%d B=%d)" : "",
        eol, a, b);
}


/* ------------------------------------------------------------------------ */
/* Boot ROM + pkg1 identification (BOOT0)                                   */

void probe_boot0_pkg1(void)
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
        log_color(COL_WARN, "  HOS version  : newer than HOS %s\n",
                  kHosTable[best_idx].hos);
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

    /* Anti-downgrade cross-check: HOS at boot compares the burnt fuse
     * count against the minimum its pkg1 requires. Two failure modes:
     *
     *   burnt < required  -> eMMC was restored from a console with
     *                        older HOS that hadn't yet burnt the fuses
     *                        the current pkg1 needs. HOS refuses boot
     *                        because it thinks the bootrom is too old.
     *                        FAIL — the only fix is to downgrade pkg1
     *                        to match, or burn fuses manually.
     *
     *   burnt > required  -> the inverse: this console was once on a
     *                        newer HOS that burnt the next anti-
     *                        downgrade fuse, then someone downgraded
     *                        pkg1 to an older version. OFW black-screens
     *                        at boot (the kernel reaches secure-monitor
     *                        init, sees the fuse mismatch, halts). CFW
     *                        like Atmosphère patches this check and
     *                        still boots — this is the classic
     *                        "OFW dead, CFW only" Switch state.
     *                        FAIL with diagnostic so the tech knows
     *                        the symptom comes from an over-burn.
     *
     * Mapping mirrors Hekate's _pkg1_ids[].fuses field. Note the
     * table is per-pkg1-timestamp; a console at HOS 20.4.0 with the
     * 20.0.0-20.5.0 pkg1 timestamp expects 21 burnt. 22 burnt means
     * the console was once on 20.6+ and downgraded. */
    if (pk1_hos_idx >= 0) {
        int burnt = __builtin_popcount(fuse_read_odm(7));
        static const u8 kFusesByIdx[] = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 9,
            10, 11, 12, 13, 14, 14, 15, 15, 15,
            16, 16, 17, 18, 19, 19, 20, 21, 22, 23,
        };
        int min_f = kFusesByIdx[pk1_hos_idx];
        dx_set("fuses_pkg1",
            burnt != min_f ? DX_FAIL : DX_PASS,
            burnt < min_f ? "%d burnt < %d (HOS pkg1 too NEW for this console)" :
            burnt > min_f ? "%d burnt > %d (OFW won't boot - CFW only)" : "",
            burnt, min_f);
    }

    sdmmc_storage_set_mmc_partition(&emmc_storage, 0);
}

void probe_autorcm(void)
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

void probe_gpt(void)
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

void probe_sd_content(void)
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
     *   1. /atmosphere/release.txt   (older official releases)
     *   2. /atmosphere/version       (newer official releases)
     *   3. /manifest.json            (HATS pack - structured JSON,
     *                                 most reliable source for HATS users)
     *   4. /HATS_VERSION.txt         (HATS pack - markdown fallback)
     * HATS doesn't ship the two official files; everything goes in
     * manifest.json + HATS_VERSION.txt at SD root. JSON is preferred
     * over markdown because the parsing is more deterministic.
     * If all four are absent we report whether /atmosphere/contents/
     * exists as a fallback presence indicator. */
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
    /* HATS pack fallback A - look for manifest.json at SD root.
     * Structured JSON, the most reliable HATS source. Layout:
     *   {
     *     ...
     *     "components": {
     *       "atmosphere": {
     *         "name": "Atmosphere",
     *         "version": "1.11.1",
     *         ...
     * Strategy: find the "atmosphere" key (this appears once near the
     * top), then find the next "version" key after it, then read the
     * quoted string value. The full file can be ~22 KB but the
     * atmosphere block is in the first ~500 bytes so a 4 KB read is
     * plenty. */
    if (!ams_found) {
        FRESULT fr = f_open(&fp, "0:/manifest.json", FA_READ);
        if (fr == FR_OK) {
            static char buf[4096];
            UINT br = 0;
            f_read(&fp, buf, sizeof(buf) - 1, &br);
            f_close(&fp);
            buf[br] = 0;
            const char *atmo_needle = "\"atmosphere\"";
            const u32   atmo_n = 12;
            const char *p = buf;
            const char *end = buf + br;
            for (; p + atmo_n < end; p++)
                if (memcmp(p, atmo_needle, atmo_n) == 0) break;
            if (p + atmo_n < end) {
                p += atmo_n;
                const char *ver_needle = "\"version\"";
                const u32   ver_n = 9;
                for (; p + ver_n < end; p++)
                    if (memcmp(p, ver_needle, ver_n) == 0) break;
                if (p + ver_n < end) {
                    p += ver_n;
                    /* Skip past `:` and the opening `"`. */
                    while (p < end && *p != '"') p++;
                    if (p < end && *p == '"') {
                        p++;
                        char ver[16] = {0};
                        int vlen = 0;
                        while (vlen < 15 && p < end && *p != '"')
                            ver[vlen++] = *p++;
                        log_color(COL_OK,
                            "  atmosphere   : %s (HATS manifest)\n",
                            ver[0] ? ver : "(unknown)");
                        ams_found = true;
                    }
                }
            }
        }
    }
    /* HATS pack fallback B - look for HATS_VERSION.txt at SD root and
     * grep a "**Atmosphere** (X.Y.Z)" markdown line from its content.
     * Used when manifest.json is absent (older HATS pack revisions
     * shipped only the text changelog). Format snippet:
     *   - **Atmosphere** (1.11.1) - Atmosphere-NX/Atmosphere
     * The bold-wrap "**Atmosphere**" appears exactly once at the
     * version-listing line; "Atmosphere" plain reappears later as
     * "Atmosphere-NX/Atmosphere" which we avoid by anchoring on the
     * markdown bold markers. */
    if (!ams_found) {
        FRESULT fr = f_open(&fp, "0:/HATS_VERSION.txt", FA_READ);
        if (fr == FR_OK) {
            static char buf[2048];
            UINT br = 0;
            f_read(&fp, buf, sizeof(buf) - 1, &br);
            f_close(&fp);
            buf[br] = 0;
            const char *needle = "**Atmosphere**";
            const u32   needle_n = 14;
            const char *p = buf;
            for (; p + needle_n < buf + br; p++) {
                if (memcmp(p, needle, needle_n) == 0) break;
            }
            if (p + needle_n < buf + br) {
                /* Skip past the bold markers and any whitespace, then
                 * find the opening '(' that wraps the version literal. */
                p += needle_n;
                while (p < buf + br && *p != '(' && *p != '\n') p++;
                if (p < buf + br && *p == '(') {
                    p++;
                    char ver[16] = {0};
                    int vlen = 0;
                    while (vlen < 15 && p < buf + br
                           && *p != ')' && *p != '\n')
                        ver[vlen++] = *p++;
                    log_color(COL_OK,
                        "  atmosphere   : %s (HATS pack)\n",
                        ver[0] ? ver : "(unknown)");
                    ams_found = true;
                }
            }
        }
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
/* SDMMC error counters                                                     */
/*                                                                          */
/* The BDK's sdmmc layer keeps three counters per device and bumps them from */
/* inside the transfer path (bdk/storage/sdmmc.c, _sdmmc_storage_readwrite   */
/* and _sdmmc_storage_handle_io_error):                                     */
/*                                                                          */
/*   RW_RETRY   a transfer failed and was retried (up to 5x, 50 ms apart)   */
/*   RW_FAIL    the retries ran out, forcing a re-init at a lower speed     */
/*   INIT_FAIL  that recovery re-init itself failed                        */
/*                                                                          */
/* Nothing else in this payload surfaces these, and they cost two pointer   */
/* reads. They matter because a card can initialise perfectly and still be  */
/* retrying on every transfer - which is exactly the "it works but          */
/* everything loads slowly" complaint that no identity or health probe      */
/* here can see.                                                            */
/*                                                                          */
/* This probe runs LAST in the Storage group on purpose, so the counts      */
/* cover every read the earlier probes performed: GPT, PRODINFO via BIS,    */
/* EXT_CSD, BOOT0/pkg1, AutoRCM and the SD content scan. Running it first   */
/* would report zeros no matter how sick the card is.                       */
/*                                                                          */
/* Caveat, verified in bdk/storage/sdmmc.c: the RW_RETRY increment sits in  */
/* the shared read/write path and unconditionally calls                     */
/* sd_error_count_increment(), even when the transfer was on eMMC. So the   */
/* SD retry figure covers BOTH devices, and EMMC_ERROR_RW_RETRY is never    */
/* incremented by anything (Nyx displays it anyway). We report what the     */
/* numbers actually mean instead of printing a zero that looks reassuring. */
void probe_storage_errors(void)
{
    HEADER("[SDMMC error counters]");

    u16 *sd_err   = sd_get_error_count();
    u16 *emmc_err = emmc_get_error_count();

    /* --- eMMC --- */
    u16 e_init = emmc_err[EMMC_ERROR_INIT_FAIL];
    u16 e_rw   = emmc_err[EMMC_ERROR_RW_FAIL];

    log_color(e_init ? COL_ERR : COL_OK,
        "  eMMC init failures : %d\n", e_init);
    log_color(e_rw ? COL_ERR : COL_OK,
        "  eMMC R/W failures  : %d\n", e_rw);
    log_color(COL_DEFAULT,
        "  eMMC R/W retries   : n/a (BDK counts these against SD)\n");

    dx_set("emmc_errors",
        (e_init || e_rw) ? DX_FAIL : DX_PASS,
        e_init ? "%d init fail" : e_rw ? "%d R/W fail" : "",
        e_init ? e_init : e_rw);

    /* --- SD --- */
    u16 s_init  = sd_err[SD_ERROR_INIT_FAIL];
    u16 s_rw    = sd_err[SD_ERROR_RW_FAIL];
    u16 s_retry = sd_err[SD_ERROR_RW_RETRY];

    log_color(s_init ? COL_ERR : COL_OK,
        "  SD init failures   : %d\n", s_init);
    log_color(s_rw ? COL_ERR : COL_OK,
        "  SD R/W failures    : %d\n", s_rw);
    /* Retries that eventually succeeded are not a failure, but they are the
     * earliest sign of a dirty slot, a worn card or a marginal trace. */
    log_color(s_retry ? COL_WARN : COL_OK,
        "  R/W retries        : %d  (SD + eMMC combined)\n", s_retry);

    dx_set("sd_errors",
        (s_init || s_rw) ? DX_FAIL :
        s_retry          ? DX_WARN : DX_PASS,
        s_init  ? "%d init fail"  :
        s_rw    ? "%d R/W fail"   :
        s_retry ? "%d retries"    : "",
        s_init ? s_init : s_rw ? s_rw : s_retry);

    if (!g_sd_ok)
        log_color(COL_DEFAULT,
            "  (SD never initialised - counts reflect that)\n");
}

