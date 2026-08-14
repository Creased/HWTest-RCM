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
 * 32 KB holds the full report comfortably (~28 KB observed in practice
 * with all probes enabled). Allocated once via heap_alloc in ipl_main. */
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
    UINT bw = 0, bw2 = 0;
    const char *tail = strstr(g_report_body, "\n--- Diagnostics ---\n");
    if (tail) {
        f_write(&s_fp, tail + 1, strlen(tail + 1), &bw);
        f_write(&s_fp, g_report_body, (UINT)(tail - g_report_body), &bw2);
        bw += bw2;
    } else {
        f_write(&s_fp, g_report_body, strlen(g_report_body), &bw);
    }
    f_close(&s_fp);
    log_color(COL_OK, "[save] %s (%d bytes)\n", s_report_path, bw);
    return 0;
}

