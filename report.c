/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: report probes
 */
#include "hwtest.h"


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
 * buffer, no second probe sweep, no I2C re-scans, no eMMC reads. A second
 * sweep would cost 5-10 s of redundant I2C / SDMMC activity on real
 * hardware.
 *
 * REPORT_CAP (128 KB) holds the full report with room to grow (~28 KB was
 * observed with every probe enabled before the Wi-Fi pages were added, and
 * the quiet bring-up pass is captured a second time). Allocated once in
 * ipl_main; log.c plants a marker if it ever fills. */
char *g_report_body = NULL;

int save_report(void)
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
    /* Lead the file with the verdict. It is computed from every other probe
     * so it can only be produced last, and on the LCD the pager solves that
     * by opening on page 0 - but a saved report is read top to bottom, and
     * a technician wants the summary before the 900 lines of detail behind
     * it. The verdict is the final section of the buffer, so writing that
     * tail first and the head after costs nothing and re-runs nothing. */
    UINT bw = 0, bw2 = 0, want;
    FRESULT wr;
    const char *tail = strstr(g_report_body, "\n--- Diagnostics ---\n");
    if (tail) {
        UINT n1 = strlen(tail + 1), n2 = (UINT)(tail - g_report_body);
        want = n1 + n2;
        wr = f_write(&s_fp, tail + 1, n1, &bw);
        if (wr == FR_OK)
            wr = f_write(&s_fp, g_report_body, n2, &bw2);
        bw += bw2;
    } else {
        want = strlen(g_report_body);
        wr = f_write(&s_fp, g_report_body, want, &bw);
    }
    FRESULT cr = f_close(&s_fp);
    /* A full card or a failing write used to be reported as a success. */
    if (wr != FR_OK || cr != FR_OK || bw != want) {
        log_color(COL_ERR, "[save] write failed (fr %d/%d)\n", wr, cr);
        return 5;
    }
    log_color(COL_OK, "[save] %s (%d bytes)\n", s_report_path, bw);
    return 0;
}

