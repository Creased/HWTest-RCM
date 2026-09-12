/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: verdict probes
 */
#include "hwtest.h"


/* ------------------------------------------------------------------------ */
/* Cross-checks: validate that fields from independent chips agree          */
/*                                                                          */
/* Per-probe verdicts catch "this chip reports a fault". Cross-checks catch */
/* "two chips disagree about the same physical state" - faults that no     */
/* single chip can see on its own. Canonical example: a broken trace        */
/* between the BQ24193 charger output and the MAX77620 ACOK pin. Each chip  */
/* self-reports as healthy but they disagree about whether VBUS is present. */
/*                                                                          */
/* Emitted as the [Cross-checks] section of the Verdict pane (called from   */
/* probe_verdict before its own aggregation). Sharing the page header keeps */
/* both sections - cross-checks plus verdict aggregate - in one navigable   */
/* LCD page. The xc_* keys are dx_set here and consumed by the aggregator   */
/* immediately below. */
static void _emit_xchecks(void)
{
    HEADER("[Cross-checks]");

    /* === A. VBUS / ACOK / USB-PD consistency ===
     * Three independent chips observe the USB-C input:
     *   - BM92T36 (USB-PD controller) sees the cable via CC and gates VBUS
     *   - BQ24193 (charger) sees VBUS at its input (VBUS_STAT, bits 7:6
     *     of STATUS 0x08: 0=none, 1=USB-SDP, 2=adapter, 3=OTG)
     *   - MAX77620 (PMIC) sees the adapter via its ACOK sense pin
     * On a healthy unit all three agree. A pair that disagrees localises
     * a broken trace; all three saying "absent" while a known-good
     * charger is plugged in points upstream of all of them (the port,
     * the cable, or the PD controller itself). */
    bool pd_inserted = false;
    usb_pd_objects_t pdinfo = {0};
    bm92t36_get_source_info(&pd_inserted, &pdinfo);
    u8 bq_status = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, 0x08);
    u8 vbus      = (bq_status >> 6) & 3;
    u8 chrg      = (bq_status >> 4) & 3;
    bool vbus_present = (vbus != 0);
    u8 onoffstat = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFSTAT);
    bool acok    = (onoffstat & MAX77620_ONOFFSTAT_ACOK) != 0;
    bool all_present = pd_inserted && vbus_present && acok;
    bool all_absent  = !pd_inserted && !vbus_present && !acok;
    bool acok_match  = all_present || all_absent;
    log_color(acok_match ? COL_OK : COL_ERR,
        "  VBUS<->ACOK   : PD=%s BQ=%s ACOK=%d  %s\n",
        pd_inserted ? "ins" : "no ", vbus_present ? "present" : "absent ", acok,
        acok_match ? (all_absent ? "(all say no source)"
                                 : "(consistent)")
                   : "(MISMATCH - localise the break)");
    dx_set("xc_vbus_acok", acok_match ? DX_PASS : DX_FAIL,
        acok_match ? "" : "VBUS observers disagree");

    /* === B. Charge state vs current direction ===
     * BQ24193 CHRG_STAT (bits 5:4 of STATUS): 0=not-charging, 1=pre-,
     * 2=fast-charging, 3=done. MAX17050 Current is signed in mA: + means
     * charging-in, - means discharging-out, ~0 means idle/done. If the
     * charger says "fast charging" but the gauge reads negative current,
     * the sense resistor or its trace is broken (charger pumps but gauge
     * doesn't see it) — repair-relevant because the unit will appear to
     * "not charge" in HOS even though the charger is working. */
    /* max17050_get_property(MAX17050_Current) returns microamps (raw
     * register * 156.25 µA per LSB, with integer math giving µA), not
     * milliamps. probe_battery divides by 1000 before printing as mA;
     * we do the same for the cross-check. */
    int current_ua = 0;
    bool curr_ok = max17050_get_property(MAX17050_Current, &current_ua) == 0;
    if (curr_ok) {
        int current_ma = current_ua / 1000;
        bool charging  = (chrg == 1 || chrg == 2);
        /* Allow a small dead-band around 0 to avoid noise tripping the
         * check when CHRG="fast" right at the start of a charge cycle
         * before the gauge's averaging catches up. */
        bool curr_pos  = (current_ma > 30);
        bool curr_neg  = (current_ma < -30);

        /* "Charging but the gauge sinks" is only evidence of a broken sense
         * path when the supply could actually cover the system draw. On a
         * current-limited source - a USB-SDP capped at 500 mA is the common
         * case on a repair bench - the console legitimately runs off the pack
         * while the BQ still reports CHRG_STAT=fast, so the gauge reads
         * negative on a perfectly healthy unit: a real Erista on USB-SDP
         * with a 500 mA input limit measures -224 mA. Only judge when
         * the input is a proper adapter with headroom. */
        int in_lim_ma = 0;
        bool lim_ok = bq24193_get_property(BQ24193_InputCurrentLimit,
                                           &in_lim_ma) == 0;
        bool input_limited = (vbus == 1) ||          /* USB-SDP */
                             (!lim_ok) || (in_lim_ma <= 500);
        bool suspicious = charging && curr_neg && !input_limited;
        bool ok = !suspicious;
        log_color(ok ? COL_OK : COL_WARN,
            "  CHRG<->Curr   : BQ chrg=%d, gauge=%d mA  %s\n",
            chrg, current_ma,
            /* Keep these short: the fixed prefix already eats 44 of the 80
             * LCD columns, so anything past ~36 chars runs off the panel. */
            suspicious ? "(NEGATIVE - sense resistor?)"
            : (charging && curr_neg) ? "(input-limited, pack helps)"
            : (charging && curr_pos) ? "(charging, current +)"
            : chrg == 0              ? "(idle/discharging)"
                                     : "(consistent)");
        /* The +/-30 mA deadband and source-headroom test are empirical
         * localization heuristics. A mismatch is useful evidence, but not
         * enough by itself to declare charger/gauge hardware failed. */
        dx_set("xc_charge_dir", ok ? DX_PASS : DX_WARN,
            ok ? "" : "BQ charging but gauge sees %d mA", current_ma);
    }

    /* === C. eMMC HS400 mode requires 8-bit bus ===
     * HS400 is the highest-speed eMMC mode and is only valid on 8-bit.
     * If init_mode landed at HS400 but bus_width came back narrower
     * than 8, something silently fell back. (sd_def.h speed names: HS400
     * is the fastest, then HS200, then DDR50/HS, then default.) */
    if (g_emmc_ok && emmc_storage.sdmmc) {
        u32 bw = sdmmc_get_bus_width(emmc_storage.sdmmc);
        bool is_8bit = (bw == SDMMC_BUS_WIDTH_8);
        /* CARD_TYPE says what the eMMC supports, not what this session is
         * currently using. Hekate tracks the negotiated mode explicitly in
         * emmc_get_mode(); only an actual HS400 session requires 8-bit. */
        u32 mode = emmc_get_mode();
        bool hs400 = (mode == EMMC_MMC_HS400);
        bool ok = !hs400 || is_8bit;
        log_color(ok ? COL_OK : COL_ERR,
            "  eMMC HS400    : mode=%d, bus=%d-bit  %s\n", mode,
            is_8bit ? 8 : (bw == SDMMC_BUS_WIDTH_4 ? 4 : 1),
            ok ? "(consistent)" : "(HS400 active but bus narrow!)");
        dx_set("xc_emmc_mode", ok ? DX_PASS : DX_WARN,
            ok ? "" : "HS400 active but bus %d-bit",
            is_8bit ? 8 : (bw == SDMMC_BUS_WIDTH_4 ? 4 : 1));
    }
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
    u32 group_mask;            /* launcher domain(s) that make this meaningful */
    bool require_all_groups;   /* true when keys span independent domains */
};

static const char *_k_boot[]    = { "soc_pmic_otp", "fuses_pkg1", NULL };
/* KFUSE CRC is set by probe_kfuse(), which is an Identity-group page, so it
 * cannot sit under the Storage-gated macro: a custom run that selects
 * Identity without Storage would drop a CRC failure before the verdict saw
 * it, and a Storage run would claim to have covered a check it never ran. */
static const char *_k_identity[] = { "kfuse", NULL };
static const char *_k_pmic[]    = { "pmic_rails", "pmic_nverc",
                                    "pmic_irqsd", "pmic_intlbt",
                                    "max77812", NULL };
static const char *_k_charger[] = { "charger_pg", "charger_fault",
                                    "usb_pd", "xc_vbus_acok", NULL };
static const char *_k_battery[] = { "batt_health", "batt_ntc",
                                    "fuel_devname",
                                    "xc_charge_dir", NULL };
static const char *_k_thermal[] = { "soc_die_temp", "pcb_temp",
                                    "fan_stalled", NULL };
static const char *_k_storage[] = { "emmc_health", "emmc_bus", "sd_bus",
                                    "wlan_cal", "cal0_hash", "cal0_panel",
                                    "cal0_model",
                                    "gpt", "prodinfo",
                                    "xc_emmc_mode", "emmc_errors",
                                    "sd_errors", "gc_asic", NULL };
static const char *_k_wireless[] = { "bt_hci", "wifi_pcie", NULL };
static const char *_k_audio[]   = { "audio_codec", "audio_clocks", "audio_beep", NULL };
static const char *_k_display[] = { "dsi_id", "backlight", NULL };
static const char *_k_inputs[]  = { "touch_id", "touch_ito", "als_id", NULL };
static const char *_k_memory[]  = { "dram_sym", "plls", NULL };

static const struct dx_macro _macros[] = {
    /* Boot integrity spans two independent probes: PMIC OTP lives in Power,
     * while burnt-fuse/pkg1 compatibility lives in Storage. Do not call the
     * aggregate complete unless both domains ran. */
    { "Boot integrity", _k_boot,     PROBE_G_POWER | PROBE_G_STORAGE, true  },
    { "Identity      ", _k_identity, PROBE_G_IDENTITY,                  false },
    /* The rails and MAX77812 come from Power, but the NVERC/IRQSD/INTLBT
     * latches are read by probe_reset() over in Raw state. Gate on either:
     * dx_get() returns NULL for a key whose probe did not run, so a
     * Power-only run still prints the rails and a Raw-only run still
     * prints the latches. */
    { "PMIC          ", _k_pmic,     PROBE_G_POWER | PROBE_G_RAW,       false },
    { "Charger       ", _k_charger,  PROBE_G_POWER,                     false },
    { "Battery       ", _k_battery,  PROBE_G_POWER,                     false },
    { "Thermal       ", _k_thermal,  PROBE_G_POWER,                     false },
    { "Storage       ", _k_storage,  PROBE_G_STORAGE,                   false },
    { "Wireless      ", _k_wireless, PROBE_G_WIRELESS,                  false },
    { "Audio         ", _k_audio,    PROBE_G_AUDIO | PROBE_G_SOUND,     false },
    { "Display       ", _k_display,  PROBE_G_DISPLAY,                   false },
    { "Inputs        ", _k_inputs,   PROBE_G_INPUTS,                    false },
    { "Memory        ", _k_memory,   PROBE_G_MEMORY,                    false },
};

void probe_verdict(void)
{
    /* Cross-checks first - they dx_set() the xc_* keys that the verdict
     * aggregator below picks up. Both sections live on the same Verdict
     * pane (single page header `--- Verdict ---` from run_all_probes). */
    if ((g_run_mask & (PROBE_G_POWER | PROBE_G_STORAGE)) ==
        (PROBE_G_POWER | PROBE_G_STORAGE)) {
        _emit_xchecks();
    } else {
        HEADER("[Cross-checks]");
        log_color(COL_WARN,
            "  custom run: cross-checks skipped unless Power + Storage are selected\n");
    }

    HEADER("[Verdict]");
    int pass = 0, warn = 0, fail = 0;

    for (size_t m = 0; m < sizeof(_macros)/sizeof(_macros[0]); m++) {
        const struct dx_macro *mc = &_macros[m];
        u32 selected_groups = mc->group_mask & g_run_mask;
        if (mc->require_all_groups ? selected_groups != mc->group_mask
                                   : selected_groups == 0)
            continue;
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
        u32 col = f ? COL_ERR : w ? COL_WARN : p ? COL_OK : COL_WARN;
        const char *state = f ? "FAIL" : w ? "warn" : p ? "pass" : "not tested";
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

