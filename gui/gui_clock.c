/*
 * hwtest GUI - clocks.
 *
 * hwtest.bin runs the probes with the BPMP at 408 MHz and the DRAM at its
 * 204 MHz boot clock. That suits the probes, but drawing is memory-bound and
 * scrolling crawls at it, so the GUI runs where hekate's menus do:
 *  - BPMP at hekate's boost (576 MHz on Erista, 589 MHz on Mariko);
 *  - DRAM trained and switched by hekate's Minerva module, as Nyx does: 1600
 *    MHz on Erista; Mariko has no table to train from and stops at 408 MHz.
 *
 * Minerva comes from hwtest/libsys_minerva.bso, built from the same hekate
 * tree as the rest, or failing that from hekate's own copy under
 * bootloader/sys. The module's config struct follows hekate's version and
 * hekate changes the handshake magic with it, so a copy that does not
 * answer the handshake is passed over. Without any, the DRAM stays at
 * 204 MHz and only the BPMP is raised.
 *
 * gui_clock_fast(false) puts both back to the payload's clocks: the sweep
 * runs there, so the report reads as the text payload's does, and the RAM
 * test starts from the clock it trains from.
 */
#include "gui.h"
#include <ianos/ianos.h>
#include <mem/minerva.h>

/* Ours first, then hekate's. */
static const char *const s_minerva_paths[] = {
    "hwtest/libsys_minerva.bso",
    "bootloader/sys/libsys_minerva.bso",
};

static minerva_str_t *s_mtc;
static void (*s_mtc_call)(mtc_config_t *cfg, void *unused);
static const char *s_minerva_path;   /* the copy in use, NULL for none */
static u32 s_dram_fast = FREQ_204;
static bool s_fast;

static void _mtc(u32 mode, u32 to)
{
    mtc_config_t *cfg = &s_mtc->mtc_cfg;
    cfg->train_mode = mode;
    cfg->rate_to = to;
    s_mtc_call(cfg, NULL);
}

static void _dram_switch(u32 freq)
{
    /* Minerva hangs when asked to switch to the clock it is already at. */
    if (s_mtc_call && s_mtc->mtc_cfg.rate_from != freq)
        _mtc(OP_SWITCH, freq);
}

/* bdk's minerva_init(), reading our copy of the module. */
static void _minerva_init(void)
{
    if (!g_sd_ok || sd_mount())
        return;
    s_mtc = zalloc(sizeof(minerva_str_t));
    mtc_config_t *cfg = &s_mtc->mtc_cfg;
    bool no_table = hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01;
    cfg->mtc_table = s_mtc->mtc_table;
    cfg->sdram_id  = fuse_read_dramid(false);
    /* The first copy that loads and answers the handshake is the one. */
    u32 ep = 0;
    for (u32 i = 0; i < ARRAY_SIZE(s_minerva_paths) && !ep; i++) {
        cfg->init_done = no_table ? MTC_IRB_MAGIC : MTC_NEW_MAGIC;
        ep = ianos_static_module((char *)s_minerva_paths[i], cfg);
        if (ep && cfg->init_done != MTC_INIT_MAGIC)
            ep = 0;
        if (ep)
            s_minerva_path = s_minerva_paths[i];
    }
    sd_unmount();    /* the card stays up for the probes */
    if (!ep) {
        free(s_mtc);
        s_mtc = NULL;
        return;
    }
    s_mtc_call = (void *)ep;

    cfg->rate_from = FREQ_204;
    if (no_table) {
        _mtc(OP_SWITCH, FREQ_408);
        s_dram_fast = FREQ_408;
        return;
    }
    _mtc(OP_TRAIN, FREQ_204);
    _mtc(OP_TRAIN, FREQ_800);
    _mtc(OP_TRAIN, FREQ_1600);
    _mtc(OP_SWITCH, FREQ_800);     /* bdk's FSP workaround */
    _mtc(OP_SWITCH, FREQ_1600);
    s_dram_fast = FREQ_1600;
}

/* Erista's LPDDR4 drifts at the high clocks and needs a periodic
 * re-training; Nyx runs it every EMC_PERIODIC_TRAIN_MS too. */
static void _dram_periodic(void *unused)
{
    (void)unused;
    if (s_mtc_call && s_mtc->mtc_cfg.rate_from >= FREQ_1066)
        _mtc(OP_PERIODIC_TRAIN, s_mtc->mtc_cfg.rate_from);
}

void gui_clock_fast(bool fast)
{
    if (fast == s_fast)
        return;
    s_fast = fast;
    if (fast) {
        bool mariko = hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01;
        bpmp_clk_rate_set(mariko ? BPMP_CLK_DEFAULT_BOOST : BPMP_CLK_LOWER_BOOST);
        _dram_switch(s_dram_fast);
    } else {
        _dram_switch(FREQ_204);
        bpmp_clk_rate_set(BPMP_CLK_NORMAL);
    }
}

u32 gui_clock_dram_mhz(void)
{
    return (s_fast ? s_dram_fast : FREQ_204) / 1000;
}

/* Each step is logged as it completes, so a console that stops here says
 * on the UART whether the DRAM switch or the BPMP boost stopped it. */
void gui_clock_init(void)
{
    _minerva_init();
    if (s_mtc_call)
        lv_task_create(_dram_periodic, EMC_PERIODIC_TRAIN_MS,
                       LV_TASK_PRIO_HIGHEST, NULL);

    char buf[96];
    s_printf(buf, "gui: DRAM %d MHz (%s)\n", s_dram_fast / 1000,
             s_minerva_path ? s_minerva_path : "no libsys_minerva.bso found");
    uart_send_crlf(UART_B, (u8 *)buf, strlen(buf));

    s_fast = false;
    gui_clock_fast(true);
    static const char boosted[] = "gui: BPMP boosted\n";
    uart_send_crlf(UART_B, (u8 *)boosted, sizeof(boosted) - 1);

    /* What the clocks actually are now, counted by the PTO, not what was
     * asked for. */
    s_printf(buf, "gui: measured BPMP %d MHz, DRAM %d MHz\n",
             clock_get_dev_freq(CLK_PTO_SCLK) / 1000,
             clock_get_dev_freq(CLK_PTO_EMC) / 1000);
    uart_send_crlf(UART_B, (u8 *)buf, strlen(buf));
}
