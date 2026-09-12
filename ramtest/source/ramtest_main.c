/*
 * hwtest dedicated DRAM diagnostic.
 *
 * This payload is intentionally separate from the normal hwtest sweep: a
 * full 4-8 GiB write/verify pass is slow and rewrites the tested RAM,
 * and needs real EMC training at 1600 MHz first.
 */
#include <stdarg.h>
#include <string.h>

#include <display/di.h>
#include <mem/mc_t210.h>
#include <mem/smmu.h>
#include <power/max77620.h>
#include <soc/i2c.h>
#include <soc/bpmp.h>
#include <soc/hw_init.h>
#include <soc/t210.h>
#include <soc/timer.h>
#include <soc/uart.h>
#include <utils/btn.h>
#include <utils/sprintf.h>
#include <utils/util.h>
#include <gfx.h>

#include "ramtest_mtc.h"
#include "ui_theme.h"

extern void pivot_stack(u32 stack_top);

#define RAM_WINDOW_BYTES       SZ_4M
#define RAMTEST_WINDOW_IOVA  0xC0000000u
#define RAMTEST_MAX_ERRORS   16u
#define MTC_PERIODIC_MS     100u
#define RAMTEST_RETENTION_MS 2000u

#define COL_TEXT COL_DEFAULT
#define COL_DIM  COL_DEFAULT
#define COL_FAIL COL_ERR

typedef enum {
    PAT_ADDR = 0,
    PAT_ADDR_INV,
    PAT_ZERO,
    PAT_ONES,
    PAT_AA,
    PAT_55,
    PAT_PRBS,
    PAT_COUNT,
} pattern_t;

#define PAT_BIT(p) (1u << (u32)(p))
#define PAT_ALL_MASK ((1u << PAT_COUNT) - 1u)

typedef enum {
    RAM_MODE_QUICK = 0,
    RAM_MODE_ADVANCED,
    RAM_MODE_CUSTOM,
} ram_mode_t;

typedef struct {
    u64 addr;
    u32 expected;
    u32 actual;
    u8  pattern;
} ram_error_t;

static pde_t *g_ptb;
static u32 g_ram_mb;
static u32 g_regions_total;
static u32 g_regions_testable;
static u32 g_errors;
static ram_error_t g_error_log[RAMTEST_MAX_ERRORS];
static u32 g_error_log_n;
static u32 g_next_periodic;
static bool g_abort;
static bool g_abort_user;
static bool g_run_failed;
static ram_mode_t g_mode;
static u32 g_pattern_mask;
static u32 g_custom_mask = PAT_ALL_MASK;
static u32 g_platform;
static u32 g_pass_index;
static u32 g_pass_total;
static u32 g_passes_completed;
static u32 g_run_sequence;
static pattern_t g_active_pattern;

typedef enum {
    PHASE_NONE = 0,
    PHASE_WRITE,
    PHASE_RETENTION,
    PHASE_VERIFY,
} test_phase_t;

static test_phase_t g_phase;
static u32 g_phase_done;
static u32 g_phase_total;

static void record_error(u64 addr, u32 expected, u32 actual, pattern_t p);

static void emit_out(const char *buf, bool align_to_title)
{
    if (align_to_title) {
        u32 x, y;
        gfx_con_getpos(&x, &y);
        gfx_con_setpos(UI_TITLE_X, y);
    }
    gfx_puts(buf);
#ifdef DEBUG_UART_PORT
    uart_send(DEBUG_UART_PORT, (const u8 *)buf, strlen(buf));
#endif
}

static void out(const char *fmt, ...)
{
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    s_vprintf(buf, fmt, ap);
    va_end(ap);
    emit_out(buf, false);
}

/* gfx_puts() resets x to 0 after a newline. Result screens are laid out on
 * the same UI_TITLE_X guide as the [hwtest] header/caption, so restore that
 * margin before every logical result row without adding spaces to UART. */
static void out_aligned(const char *fmt, ...)
{
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    s_vprintf(buf, fmt, ap);
    va_end(ap);
    emit_out(buf, true);
}

static void set_color(u32 c)
{
    gfx_con_setcol(c, 0, 0);
}

static void screen_title(const char *phase)
{
    gfx_clear_grey(0x1B);
    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_HEADER, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_HEADER_Y);
    gfx_printf("[hwtest]");
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_CAPTION_Y);
    gfx_printf("%s", phase);
    gfx_con_setpos(UI_TITLE_X, 120);
    set_color(COL_TEXT);
}

static bool should_abort(void)
{
    if (btn_read() & BTN_VOL_DOWN) {
        g_abort = true;
        g_abort_user = true;
    }
    return g_abort;
}

static bool training_tick(void)
{
    u32 now = get_tmr_ms();
    if ((s32)(now - g_next_periodic) >= 0) {
        ram_mtc_periodic();
        g_next_periodic = now + MTC_PERIODIC_MS;
        if (ram_mtc_errors()) {
            g_run_failed = true;
            return false;
        }
    }
    return !should_abort();
}

static bool overlaps(u64 a, u64 asz, u64 b, u64 bsz)
{
    return a < b + bsz && a + asz > b;
}

static bool protected_region(u64 phys)
{
    /* Keep the first 4 MiB clear of boot-time low-DRAM/carveout state.
     * The RAM diagnostic itself runs from IRAM, so the old 16 MiB guard was
     * unnecessarily conservative and hid three otherwise testable regions. */
    if (overlaps(phys, RAM_WINDOW_BYTES, DRAM_START, SZ_4M))
        return true;
    if (overlaps(phys, RAM_WINDOW_BYTES, SMMU_HEAP_ADDR, SZ_4M))
        return true;
    if (overlaps(phys, RAM_WINDOW_BYTES, IPL_FB_ADDRESS, IPL_FB_SZ))
        return true;
    return false;
}

static bool smmu_setup(void)
{
    smmu_init();
    g_ptb = (pde_t *)smmu_domain_init(MC_SMMU_AVPC_ASID, 1);
    if (!g_ptb)
        return false;

    /* The BPMP itself lives below 4 GiB. Preserve identity mappings for its
     * normal DRAM accesses, then reuse one 4 MiB IOVA to reach every physical
     * region, including RAM above 4 GiB. */
    smmu_map_huge(g_ptb, DRAM_START, DRAM_START,
        (u32)((0x100000000ULL - DRAM_START) / RAM_WINDOW_BYTES), SMMU_ATTR_ALL);
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLEAN_WAY, true);
    smmu_flush_all();
    smmu_enable();
    return true;
}

static void smmu_teardown(void)
{
    if (!g_ptb)
        return;
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, true);
    smmu_disable();
    smmu_domain_deinit(MC_SMMU_AVPC_ASID, 1);
    smmu_reset_heap();
    g_ptb = NULL;
}

static volatile u32 *map_region(u64 phys)
{
    /* The same VA is reused for different physical memory. Clean dirty lines
     * before changing the translation and invalidate after it, otherwise the
     * cache can make two different DRAM regions appear identical. */
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, true);
    smmu_map_huge(g_ptb, RAMTEST_WINDOW_IOVA, phys, 1, SMMU_ATTR_ALL);
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, true);
    smmu_flush_all();
    return (volatile u32 *)RAMTEST_WINDOW_IOVA;
}

static u32 pattern_seed(pattern_t p, u64 phys, u32 salt)
{
    u32 lo = (u32)phys;
    u32 hi = (u32)(phys >> 32);
    u32 x = lo ^ (hi * 0x9E3779B9u) ^ salt ^ (0x6D2B79F5u * ((u32)p + 1u));
    return x ? x : 0xA5366B4Du;
}

static inline u32 prbs_step(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void fill_region(volatile u32 *win, u64 phys, pattern_t p, u32 salt)
{
    const u32 words = RAM_WINDOW_BYTES / sizeof(u32);
    const u32 base = (u32)phys;
    const u32 hi = (u32)(phys >> 32);
    const u32 high_mix = (hi << 16) | (hi >> 16);
    u32 fixed = 0;

    switch (p) {
        case PAT_ZERO: fixed = 0x00000000u; goto fill_fixed;
        case PAT_ONES: fixed = 0xFFFFFFFFu; goto fill_fixed;
        case PAT_AA:   fixed = 0xAAAAAAAAu; goto fill_fixed;
        case PAT_55:   fixed = 0x55555555u; goto fill_fixed;
        case PAT_ADDR:
            for (u32 i = 0; i < words; ++i)
                win[i] = (base + (i << 2)) ^ high_mix ^ salt;
            return;
        case PAT_ADDR_INV:
            for (u32 i = 0; i < words; ++i)
                win[i] = ~((base + (i << 2)) ^ high_mix ^ salt);
            return;
        case PAT_PRBS:
        default: {
            u32 state = pattern_seed(p, phys, salt);
            for (u32 i = 0; i < words; ++i) {
                state = prbs_step(state);
                win[i] = state;
            }
            return;
        }
    }

fill_fixed:
    for (u32 i = 0; i < words; ++i)
        win[i] = fixed;
}

static void verify_region(volatile u32 *win, u64 phys, pattern_t p, u32 salt)
{
    const u32 words = RAM_WINDOW_BYTES / sizeof(u32);
    const u32 base = (u32)phys;
    const u32 hi = (u32)(phys >> 32);
    const u32 high_mix = (hi << 16) | (hi >> 16);
    u32 fixed = 0;

    switch (p) {
        case PAT_ZERO: fixed = 0x00000000u; goto verify_fixed;
        case PAT_ONES: fixed = 0xFFFFFFFFu; goto verify_fixed;
        case PAT_AA:   fixed = 0xAAAAAAAAu; goto verify_fixed;
        case PAT_55:   fixed = 0x55555555u; goto verify_fixed;
        case PAT_ADDR:
            for (u32 i = 0; i < words; ++i) {
                u32 exp = (base + (i << 2)) ^ high_mix ^ salt;
                u32 got = win[i];
                if (got != exp)
                    record_error(phys + ((u64)i << 2), exp, got, p);
            }
            return;
        case PAT_ADDR_INV:
            for (u32 i = 0; i < words; ++i) {
                u32 exp = ~((base + (i << 2)) ^ high_mix ^ salt);
                u32 got = win[i];
                if (got != exp)
                    record_error(phys + ((u64)i << 2), exp, got, p);
            }
            return;
        case PAT_PRBS:
        default: {
            u32 state = pattern_seed(p, phys, salt);
            for (u32 i = 0; i < words; ++i) {
                state = prbs_step(state);
                u32 got = win[i];
                if (got != state)
                    record_error(phys + ((u64)i << 2), state, got, p);
            }
            return;
        }
    }

verify_fixed:
    for (u32 i = 0; i < words; ++i) {
        u32 got = win[i];
        if (got != fixed)
            record_error(phys + ((u64)i << 2), fixed, got, p);
    }
}

static const char *pattern_name(pattern_t p)
{
    switch (p) {
        case PAT_ADDR:     return "address pattern";
        case PAT_ADDR_INV: return "inverse address";
        case PAT_ZERO:     return "all-zero";
        case PAT_ONES:     return "all-one";
        case PAT_AA:       return "AA pattern";
        case PAT_55:       return "55 pattern";
        case PAT_PRBS:     return "PRBS + retention";
        default:           return "unknown";
    }
}

static const char *pattern_menu_name(pattern_t p)
{
    switch (p) {
        case PAT_ADDR:     return "Address pattern";
        case PAT_ADDR_INV: return "Inverse address";
        case PAT_ZERO:     return "All-zero";
        case PAT_ONES:     return "All-one";
        case PAT_AA:       return "AA pattern";
        case PAT_55:       return "55 pattern";
        case PAT_PRBS:     return "PRBS + retention";
        default:           return "Unknown";
    }
}

static void record_error(u64 addr, u32 expected, u32 actual, pattern_t p)
{
    if (g_errors != 0xFFFFFFFFu)
        g_errors++;
    if (g_error_log_n < RAMTEST_MAX_ERRORS) {
        ram_error_t *e = &g_error_log[g_error_log_n++];
        e->addr = addr;
        e->expected = expected;
        e->actual = actual;
        e->pattern = (u8)p;
    }
}

static void draw_progress_bar(u32 pct)
{
    enum { BAR_WIDTH = 48 };
    char bar[BAR_WIDTH + 1];
    u32 filled = (pct * BAR_WIDTH) / 100u;
    if (filled > BAR_WIDTH)
        filled = BAR_WIDTH;

    for (u32 i = 0; i < BAR_WIDTH; ++i)
        bar[i] = i < filled ? '#' : '-';
    bar[BAR_WIDTH] = 0;

    gfx_con_setpos(UI_TITLE_X, 150);
    gfx_con_setcol(COL_OK, 1, COL_BG);
    gfx_printf("[%s] %3d%%   ", bar, pct);
    set_color(COL_TEXT);
}

static void progress(const char *verb, pattern_t p, u32 done, u32 total, u32 base_pct)
{
    static u32 last_pct = 0xFFFFFFFFu;
    static u32 last_pass = 0xFFFFFFFFu;
    static u32 last_errors = 0xFFFFFFFFu;
    static u32 last_run = 0xFFFFFFFFu;
    static pattern_t last_pattern = PAT_COUNT;
    static char last_verb;
    u32 half = total ? (done * 50u) / total : 50u;
    u32 pct = base_pct + half;
    char verb_key = verb[0];
    bool pct_changed = pct != last_pct;

    if (!pct_changed && g_run_sequence == last_run &&
        g_pass_index == last_pass && g_errors == last_errors &&
        p == last_pattern && verb_key == last_verb)
        return;

    last_pct = pct;
    last_pass = g_pass_index;
    last_errors = g_errors;
    last_run = g_run_sequence;
    last_pattern = p;
    last_verb = verb_key;

    gfx_con_setpos(UI_TITLE_X, 120);
    gfx_con_setcol(COL_TEXT, 1, COL_BG);
    gfx_printf("Pass %d/%d   %s %s   errors %d                              ",
        g_pass_index, g_pass_total, verb, pattern_name(p), g_errors);
    draw_progress_bar(pct);

#ifdef DEBUG_UART_PORT
    if (pct_changed && (pct % 10u) == 0)
        uart_printf("[ram] %s %s %d%% errors=%d\n", verb, pattern_name(p), pct, g_errors);
#endif
}

static bool write_sweep(pattern_t p, u32 salt)
{
    u64 dram_end = DRAM_START + ((u64)g_ram_mb << 20);
    u32 done = 0;
    g_phase = PHASE_WRITE;
    g_phase_done = 0;
    g_phase_total = g_regions_testable;

    for (u64 phys = DRAM_START; phys < dram_end; phys += RAM_WINDOW_BYTES) {
        if (protected_region(phys))
            continue;

        volatile u32 *win = map_region(phys);
        fill_region(win, phys, p, salt);

        /* Commit the region before remapping the same virtual window. */
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, true);
        ++done;
        g_phase_done = done;
        progress("write", p, done, g_regions_testable, 0);
        if (!training_tick())
            return false;
    }
    return true;
}

static bool verify_sweep(pattern_t p, u32 salt)
{
    u64 dram_end = DRAM_START + ((u64)g_ram_mb << 20);
    u32 done = 0;
    g_phase = PHASE_VERIFY;
    g_phase_done = 0;
    g_phase_total = g_regions_testable;

    for (u64 phys = DRAM_START; phys < dram_end; phys += RAM_WINDOW_BYTES) {
        if (protected_region(phys))
            continue;

        volatile u32 *win = map_region(phys);
        verify_region(win, phys, p, salt);

        ++done;
        g_phase_done = done;
        progress("verify", p, done, g_regions_testable, 50);
        if (!training_tick())
            return false;
    }
    return true;
}

static bool retention_wait(void)
{
    g_phase = PHASE_RETENTION;
    g_phase_done = 0;
    g_phase_total = RAMTEST_RETENTION_MS;
    u32 start = get_tmr_ms();
    u32 end = start + RAMTEST_RETENTION_MS;
    while ((s32)(get_tmr_ms() - end) < 0) {
        if (!training_tick())
            return false;
        g_phase_done = get_tmr_ms() - start;
        if (g_phase_done > g_phase_total)
            g_phase_done = g_phase_total;
        msleep(10);
    }
    return true;
}

static bool run_pass(pattern_t p, u32 salt)
{
    ++g_pass_index;
    g_active_pattern = p;
    screen_title(pattern_name(p));
    progress("write", p, 0, g_regions_testable, 0);
    u32 before = g_errors;
    u32 start = get_tmr_ms();

    if (!write_sweep(p, salt))
        return false;
    if (p == PAT_PRBS && !retention_wait())
        return false;
    if (!verify_sweep(p, salt))
        return false;

    u32 secs = (get_tmr_ms() - start) / 1000u;
    gfx_con_setpos(UI_TITLE_X, 190);
    set_color(g_errors == before ? COL_OK : COL_FAIL);
    gfx_printf("pass %s: %s, +%d errors, %ds                              ",
        pattern_name(p), g_errors == before ? "PASS" : "FAIL",
        g_errors - before, secs);
#ifdef DEBUG_UART_PORT
    uart_printf("[ram] pass=%s result=%s new_errors=%d seconds=%d\n",
        pattern_name(p), g_errors == before ? "PASS" : "FAIL",
        g_errors - before, secs);
#endif
    ++g_passes_completed;
    g_phase = PHASE_NONE;
    g_phase_done = 0;
    g_phase_total = 0;
    return true;
}

static bool data_bus_test(void)
{
    u64 dram_end = DRAM_START + ((u64)g_ram_mb << 20);
    u64 phys = DRAM_START;
    while (phys < dram_end && protected_region(phys))
        phys += RAM_WINDOW_BYTES;
    if (phys >= dram_end)
        return false;

    volatile u32 *win = map_region(phys);
    volatile u32 *cell = &win[0x1000 / 4];
    for (u32 bit = 0; bit < 32; ++bit) {
        u32 v = 1u << bit;
        *cell = v;
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, true);
        if (*cell != v) {
            record_error(phys + 0x1000, v, *cell, PAT_ADDR);
            return false;
        }
        v = ~v;
        *cell = v;
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, true);
        if (*cell != v) {
            record_error(phys + 0x1000, v, *cell, PAT_ADDR);
            return false;
        }
    }
    return true;
}

static void menu_pixel(int x, int y, u32 color)
{
    if ((u32)x >= 1280u || (u32)y >= 720u)
        return;
    gfx_ctxt.fb[(u32)y + (1279u - (u32)x) * 720u] = color;
}

static void menu_fill_rect(int x, int y, int w, int h, u32 color)
{
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            menu_pixel(xx, yy, color);
}

static void menu_row(int x, int y, int w, int h, bool selected, const char *text)
{
    menu_fill_rect(x, y, w, h, selected ? COL_DEFAULT : COL_BG);
    gfx_con_setcol(selected ? COL_BG : COL_DEFAULT, 1,
                   selected ? COL_DEFAULT : COL_BG);
    gfx_con.fntsz = 16;
    gfx_con_setpos(x + 16, y + 8);
    gfx_printf("%s", text);
    set_color(COL_TEXT);
}

static void menu_footer(const char *action)
{
    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_DEFAULT, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_FOOTER_Y);
    gfx_printf("VOL+: Up   VOL-: Down   PWR: %s", action);
    set_color(COL_TEXT);
}

static void draw_main_menu(int selected)
{
    static const char *items[] = {
        "Quick test",
        "Advanced test",
        "Custom test",
        "Power off",
    };
    static const char *help[] = {
        "One full Address pattern write/verify pass.",
        "Run all seven RAM patterns.",
        "Choose individual RAM patterns to run.",
        "Shut down the console cleanly.",
    };

    screen_title("RAM test menu");
    for (int i = 0; i < 4; ++i)
        menu_row(UI_MAIN_ROW_X, UI_MAIN_ROW_Y + i * UI_MAIN_ROW_STEP,
                 UI_MAIN_ROW_W, UI_MAIN_ROW_H, selected == i, items[i]);

    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_INFO, 1, COL_BG);
    gfx_con_setpos(UI_TITLE_X, UI_MAIN_HELP_Y);
    gfx_printf("%s                                              ", help[selected]);
    menu_footer("Select option");
}

static void draw_custom_menu(int selected, u32 mask)
{
    screen_title("Custom RAM test");

    for (int i = 0; i < PAT_COUNT; ++i) {
        char line[64];
        s_printf(line, "[%c] %s", (mask & PAT_BIT(i)) ? 'x' : ' ',
                 pattern_menu_name((pattern_t)i));
        menu_row(UI_CUSTOM_ROW_X, UI_CUSTOM_ROW_Y + i * UI_CUSTOM_ROW_STEP,
                 UI_CUSTOM_ROW_W, UI_CUSTOM_ROW_H, selected == i, line);
    }

    menu_row(UI_CUSTOM_ROW_X, UI_CUSTOM_ROW_Y + PAT_COUNT * UI_CUSTOM_ROW_STEP + 12,
             UI_CUSTOM_ROW_W, UI_CUSTOM_ROW_H, selected == PAT_COUNT,
             "Run selected checks");
    menu_row(UI_CUSTOM_ROW_X, UI_CUSTOM_ROW_Y + (PAT_COUNT + 1) * UI_CUSTOM_ROW_STEP + 12,
             UI_CUSTOM_ROW_W, UI_CUSTOM_ROW_H, selected == PAT_COUNT + 1, "Back");

    gfx_con.fntsz = 16;
    gfx_con_setcol(COL_INFO, 1, COL_BG);
    gfx_con_setpos(UI_CUSTOM_SIDE_X, 128);
    gfx_printf("POWER toggles");
    gfx_con_setpos(UI_CUSTOM_SIDE_X, 152);
    gfx_printf("pattern checks");
    gfx_con_setpos(UI_CUSTOM_SIDE_X, 200);
    gfx_printf("Training + data bus");
    gfx_con_setpos(UI_CUSTOM_SIDE_X, 224);
    gfx_printf("always run first");
    menu_footer("Toggle / select");
}

static bool custom_menu(void)
{
    int selected = 0;
    draw_custom_menu(selected, g_custom_mask);

    for (;;) {
        u8 btn = btn_read();
        int activated = -1;

        if (btn & BTN_VOL_UP)
            selected = (selected + PAT_COUNT + 1) % (PAT_COUNT + 2);
        else if (btn & BTN_VOL_DOWN)
            selected = (selected + 1) % (PAT_COUNT + 2);
        else if (btn & BTN_POWER)
            activated = selected;

        if (activated >= 0) {
            if (activated < PAT_COUNT) {
                g_custom_mask ^= PAT_BIT(activated);
            } else if (activated == PAT_COUNT) {
                if (g_custom_mask) {
                    g_mode = RAM_MODE_CUSTOM;
                    g_pattern_mask = g_custom_mask;
                    return true;
                }
            } else {
                return false;
            }
        }

        if (btn || activated >= 0) {
            draw_custom_menu(selected, g_custom_mask);
            if (btn)
                while (btn_read() == btn)
                    msleep(15);
        } else {
            msleep(15);
        }
    }
}

static u32 count_selected_patterns(u32 mask)
{
    u32 n = 0;
    for (u32 i = 0; i < PAT_COUNT; ++i)
        if (mask & PAT_BIT(i))
            ++n;
    return n;
}

static void choose_mode(void)
{
    int selected = 0;

    for (;;) {
        draw_main_menu(selected);
        for (;;) {
            u8 btn = btn_read();
            int activated = -1;

            if (btn & BTN_VOL_UP)
                selected = (selected + 3) % 4;
            else if (btn & BTN_VOL_DOWN)
                selected = (selected + 1) % 4;
            else if (btn & BTN_POWER)
                activated = selected;

            if (activated == 0) {
                g_mode = RAM_MODE_QUICK;
                g_pattern_mask = PAT_BIT(PAT_ADDR);
                goto selected_mode;
            }
            if (activated == 1) {
                g_mode = RAM_MODE_ADVANCED;
                g_pattern_mask = PAT_ALL_MASK;
                goto selected_mode;
            }
            if (activated == 2) {
                if (custom_menu())
                    goto selected_mode;
                draw_main_menu(selected);
            }
            if (activated == 3) {
                while (btn_read() & BTN_POWER)
                    msleep(20);
                screen_title("Power off");
                out("Powering off...\n");
                i2c_send_byte(I2C_5, MAX77620_I2C_ADDR,
                              MAX77620_REG_ONOFFCNFG1, MAX77620_ONOFFCNFG1_PWR_OFF);
                for (;;)
                    ;
            }

            if (btn) {
                draw_main_menu(selected);
                while (btn_read() == btn)
                    msleep(15);
            } else {
                msleep(15);
            }
        }
    }

selected_mode:
    g_pass_index = 0;
    g_pass_total = count_selected_patterns(g_pattern_mask);
#ifdef DEBUG_UART_PORT
    uart_printf("[ram] mode=%s patterns=%d mask=%02X\n",
        g_mode == RAM_MODE_QUICK ? "quick" :
        g_mode == RAM_MODE_ADVANCED ? "advanced" : "custom",
        g_pass_total, g_pattern_mask);
#endif
}

static void report_mtc_failure(void)
{
    u32 e = ram_mtc_errors();
    u32 m = ram_mtc_failed_training_mask();
    screen_title("DRAM training failed");
    set_color(COL_FAIL);
    out("1600 MHz training: FAIL\n");
    set_color(COL_TEXT);
    out("DRAM ID       : %02X\n", ram_mtc_dram_id());
    out("error flags   : %08X\n", e);
    out("training mask : %08X\n", m);
    if (e & RAM_MTC_ERR_UNSUPPORTED)    out(" - unsupported DRAM ID/table\n");
    if (e & RAM_MTC_ERR_TABLE)          out(" - invalid/corrupt timing table\n");
    if (e & RAM_MTC_ERR_ENTRY_CLOCK)    out(" - unexpected EMC entry clock; no training attempted\n");
    if (e & RAM_MTC_ERR_PLL_TIMEOUT)    out(" - memory PLL did not lock\n");
    if (e & RAM_MTC_ERR_STATUS_TIMEOUT) out(" - EMC/DLL status transition timed out\n");
    out("\nPower-cycle before trusting memory state after a HW timeout.\n");
}

static const char *phase_name(test_phase_t phase)
{
    switch (phase) {
        case PHASE_WRITE:     return "write";
        case PHASE_RETENTION: return "retention";
        case PHASE_VERIFY:    return "verify";
        default:              return "none";
    }
}

static void reset_run_stats(void)
{
    ++g_run_sequence;
    g_errors = 0;
    g_error_log_n = 0;
    g_abort = false;
    g_abort_user = false;
    g_run_failed = false;
    g_pass_index = 0;
    g_pass_total = 0;
    g_passes_completed = 0;
    g_active_pattern = PAT_ADDR;
    g_phase = PHASE_NONE;
    g_phase_done = 0;
    g_phase_total = 0;
}

static void report_final(u32 elapsed_ms)
{
    bool failed = g_run_failed || g_errors;
    const char *result = g_abort_user ? "ABORTED" : (failed ? "FAIL" : "PASS");

    screen_title(g_abort_user ? "ABORTED" : "Final result");
    set_color(g_abort_user ? COL_WARN : (failed ? COL_FAIL : COL_OK));
    out_aligned("DRAM result : %s\n", result);
    set_color(COL_TEXT);
    out_aligned("platform    : %s\n", g_platform == RAM_MTC_PLATFORM_MARIKO ? "Mariko/T210B01" : "Erista/T210");
    out_aligned("mode        : %s\n",
        g_mode == RAM_MODE_QUICK ? "quick" :
        g_mode == RAM_MODE_ADVANCED ? "advanced" : "custom");
    out_aligned("DRAM size   : %d MiB\n", g_ram_mb);
    out_aligned("scope/pass  : %d / %d regions (%d MiB)\n",
        g_regions_testable, g_regions_total, g_regions_testable * 4u);
    out_aligned("passes done : %d / %d\n", g_passes_completed, g_pass_total);

    if ((g_abort_user || g_run_failed) && g_pass_index > g_passes_completed) {
        out_aligned("stopped on  : pass %d/%d, %s, %s\n",
            g_pass_index, g_pass_total, pattern_name(g_active_pattern), phase_name(g_phase));
        if (g_phase == PHASE_RETENTION)
            out_aligned("phase prog  : %d / %d ms\n", g_phase_done, g_phase_total);
        else if (g_phase == PHASE_WRITE || g_phase == PHASE_VERIFY)
            out_aligned("phase prog  : %d / %d regions (%d MiB)\n",
                g_phase_done, g_phase_total, g_phase_done * 4u);
    }

    out_aligned("errors seen : %d\n", g_errors);
    out_aligned("elapsed     : %d s\n", elapsed_ms / 1000u);
    out_aligned("MTC flags   : %08X\n", ram_mtc_errors());

    for (u32 i = 0; i < g_error_log_n && i < 8; ++i) {
        const ram_error_t *e = &g_error_log[i];
        out_aligned("bad @ %08X%08X exp %08X got %08X (%s)\n",
            (u32)(e->addr >> 32), (u32)e->addr,
            e->expected, e->actual, pattern_name((pattern_t)e->pattern));
    }
    if (g_errors > g_error_log_n)
        out_aligned("+ %d additional failing words\n", g_errors - g_error_log_n);

    gfx_con_setpos(UI_TITLE_X, UI_FOOTER_Y);
    gfx_con_setcol(COL_DIM, 1, COL_BG);
    gfx_printf("POWER: run again    VOL-: power off");
    set_color(COL_TEXT);
}

static void wait_post_test_action(void)
{
    /* An abort is normally entered with VOL- still held; require a release so
     * it cannot immediately trigger the power-off action on this screen. */
    while (btn_read() & (BTN_POWER | BTN_VOL_DOWN | BTN_VOL_UP))
        msleep(20);

    for (;;) {
        u8 b = btn_read();
        if (b & BTN_POWER) {
            while (btn_read() & BTN_POWER)
                msleep(20);
            return;
        }
        if (b & BTN_VOL_DOWN) {
            gfx_con_setpos(UI_TITLE_X, UI_FOOTER_Y);
            gfx_con_setcol(COL_DIM, 1, COL_BG);
            gfx_printf("Powering off...                          ");
            i2c_send_byte(I2C_5, MAX77620_I2C_ADDR,
                          MAX77620_REG_ONOFFCNFG1, MAX77620_ONOFFCNFG1_PWR_OFF);
            for (;;)
                ;
        }
        msleep(20);
    }
}

void ipl_main(void)
{
    /* Platform detection is deliberately the first operation in the payload.
     * GP_HIDREV is readable directly from reset state, so choose the one MTC
     * implementation that is allowed to run before any hardware init. */
    const u32 hidrev = APB_MISC(APB_MISC_GP_HIDREV);
    const u32 chip_id = (hidrev >> 4) & 0xFu;
    g_platform = chip_id == GP_HIDREV_MAJOR_T210B01 ? RAM_MTC_PLATFORM_MARIKO :
                 chip_id == GP_HIDREV_MAJOR_T210    ? RAM_MTC_PLATFORM_ERISTA :
                                                       RAM_MTC_PLATFORM_UNKNOWN;
    ram_mtc_select_platform(g_platform);

    hw_init();
    pivot_stack(IPL_LOAD_ADDR);

    display_init();
    u32 *fb = display_init_window_a_pitch();
    gfx_init_ctxt(fb, 1280, 720, 720);
    gfx_con_init();
    display_backlight_pwm_init();
    display_backlight_brightness(100, 1000);

    if (g_platform == RAM_MTC_PLATFORM_UNKNOWN) {
        screen_title("Unsupported platform");
        set_color(COL_FAIL);
        out("Unsupported Tegra HIDREV %08X (major %X); no RAM training will run.\n",
            hidrev, chip_id);
        for (;;) msleep(1000);
    }

    g_ram_mb = MC(MC_EMEM_CFG) & 0x3FFFu;
    g_regions_total = g_ram_mb / 4u;
    g_regions_testable = 0;
    u64 dram_end = DRAM_START + ((u64)g_ram_mb << 20);
    for (u64 p = DRAM_START; p < dram_end; p += RAM_WINDOW_BYTES)
        if (!protected_region(p))
            ++g_regions_testable;

    for (;;) {
        reset_run_stats();
        choose_mode();

        screen_title("Training DRAM");
        out("Platform      : %s\n", g_platform == RAM_MTC_PLATFORM_MARIKO ? "Mariko/T210B01" : "Erista/T210");
        out("DRAM size     : %d MiB\n", g_ram_mb);
        out("DRAM ID       : %02X\n", (u32)0xFF);
        out("Target        : 1600 MHz\n");
        out("Training...\n");

        if (ram_mtc_train() != 0) {
            report_mtc_failure();
            for (;;) msleep(1000);
        }

        set_color(COL_OK);
        out("Training      : PASS\n");
        set_color(COL_TEXT);
        out("DRAM ID       : %02X\n", ram_mtc_dram_id());

        if (!smmu_setup()) {
            set_color(COL_FAIL);
            out("SMMU setup   : FAIL\n");
            ram_mtc_restore();
            for (;;) msleep(1000);
        }
        out("SMMU window   : PASS\n");

        g_next_periodic = get_tmr_ms() + MTC_PERIODIC_MS;
        u32 test_start = get_tmr_ms();

        screen_title("Data-bus preflight");
        bool bus_ok = data_bus_test();
        set_color(bus_ok ? COL_OK : COL_FAIL);
        out("walking data bits: %s\n", bus_ok ? "PASS" : "FAIL");
        if (!bus_ok)
            g_run_failed = true;

        static const pattern_t order[PAT_COUNT] = {
            PAT_ADDR, PAT_ADDR_INV, PAT_ZERO, PAT_ONES, PAT_AA, PAT_55, PAT_PRBS
        };
        static const u32 salts[PAT_COUNT] = {
            0x13579BDFu, 0x2468ACE0u, 0, 0, 0, 0, 0xC001D00Du
        };

        for (u32 i = 0; i < PAT_COUNT && !g_abort && !g_run_failed; ++i) {
            pattern_t p = order[i];
            if (!(g_pattern_mask & PAT_BIT(p)))
                continue;
            if (!run_pass(p, salts[i]))
                break;
        }

        u32 elapsed = get_tmr_ms() - test_start;
        smmu_teardown();
        ram_mtc_restore();
        report_final(elapsed);
        wait_post_test_action();
    }

}
