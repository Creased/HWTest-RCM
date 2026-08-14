/*
 * hwtest - one-shot Switch hardware probe / repair report.
 * Split from main.c: display probes
 */
#include "hwtest.h"




/* Decoded panel ID -> marketing name. Shared with the PRODINFO probe, which
 * decodes the same 0xVVTT form out of CAL0's lcd_vendor field so the two can
 * be compared. NULL means "not one we know". */
const char *panel_model_name(u16 dec)
{
    switch (dec) {
    case PANEL_JDI_XXX062M:     return "JDI 062M (generic)";
    case PANEL_JDI_LAM062M109A: return "JDI LAM062M109A";
    case PANEL_JDI_LPM062M326A: return "JDI LPM062M326A";
    case PANEL_INL_P062CCA_AZ1: return "InnoLux P062CCA";
    case PANEL_AUO_A062TAN01:   return "AUO A062TAN";
    case PANEL_INL_2J055IA_27A: return "InnoLux 2J055IA";
    case PANEL_AUO_A055TAN01:   return "AUO A055TAN";
    case PANEL_SHP_LQ055T1SW10: return "Sharp LQ055T1SW10";
    case PANEL_SAM_AMS699VC01:  return "Samsung AMS699VC01";
    default: return NULL;
    }
}

void probe_display(void)
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

    const char *name = panel_model_name(dec);
    log_color(name ? COL_OK : (sentinel ? COL_ERR : COL_WARN),
        "  Model        : %s\n", name ? name : "Unknown");
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
void probe_backlight(void)
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
    /* Mariko OLED (AULA, Samsung AMS699VC01) has NO PWM backlight: the
     * panel is self-emissive and brightness is a MIPI-DCS command carried
     * over DSI, with Hekate stashing the level in DC_DCS_BACKLIGHT_LEVEL.
     * On AULA the PWM path sitting idle is the *expected* state, so judging
     * it against PWM0 (the LCD path below) wrongly flags every OLED unit.
     * Branch on the decoded panel ID and validate the DCS level instead. */
    bool is_oled = (display_get_decoded_panel_id() == PANEL_SAM_AMS699VC01);
    if (is_oled) {
        bool lit = (dcs_lvl != 0);
        log_color(lit ? COL_OK : COL_WARN,
            "  Verdict      : OLED (AULA) DCS backlight 0x%08X%s\n",
            dcs_lvl, lit ? "" : " (level 0 - screen dark)");
        dx_set("backlight", lit ? DX_PASS : DX_WARN,
            lit ? "" : "OLED DCS level 0");
    } else if (pwm_en && pwm_mode && pwm_duty > 0) {
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

