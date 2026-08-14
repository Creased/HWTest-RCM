/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: dx probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* Diagnostic-finding registry                                              */
/*                                                                          */
/* One source of truth for everything the verdict cross-checks. Each detail */
/* probe stores its observation under a stable string key (`dx_set`), the   */
/* verdict iterates per-macro key lists and rolls them up. The registry     */
/* keeps the global namespace small and makes adding a new sub-check a      */
/* two-line change: one dx_set in the probe + one key in the macro's list.  */
/*                                                                          */
/* Severity ladder is FAIL > WARN > PASS; the verdict picks the worst.
 * The detail string carries the runtime reason (e.g. "4-bit", "0xD8") so
 * the verdict doesn't have to know how to format each value; the probe
 * that captured the value does, when it knows the value. */

static dx_finding_t _dx_tbl[DX_CAP];
static int          _dx_n;

/* Reset between full probe sweeps so refresh-all (`a`) can repopulate
 * cleanly without stale entries surviving when a probe path changes. */
void dx_reset(void) { _dx_n = 0; }

/* Record/overwrite a finding. fmt may be NULL to clear the detail. */
void dx_set(const char *key, dx_sev_t sev, const char *fmt, ...)
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

const dx_finding_t *dx_get(const char *key)
{
    for (int i = 0; i < _dx_n; i++)
        if (strcmp(_dx_tbl[i].key, key) == 0) return &_dx_tbl[i];
    return NULL;
}

/* Pick a colour for a value with two warning thresholds and two error
 * thresholds. Pass HEALTH_NONE to disable a side. */
u32 health_color(int v, int warn_lo, int warn_hi, int err_lo, int err_hi)
{
    if (err_lo != HEALTH_NONE && v <  err_lo) return COL_ERR;
    if (err_hi != HEALTH_NONE && v >  err_hi) return COL_ERR;
    if (warn_lo != HEALTH_NONE && v < warn_lo) return COL_WARN;
    if (warn_hi != HEALTH_NONE && v > warn_hi) return COL_WARN;
    return COL_OK;
}
