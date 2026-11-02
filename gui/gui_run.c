/*
 * hwtest GUI - the full check.
 *
 * Runs the same sweep as the text payload (run_probe_mask) and the same
 * report path (the capture buffer, saved to the SD card), with a progress
 * screen in front. The probes block; the screen is redrawn from
 * status_set(), which the sweep calls before every page and from inside
 * the long ones (status_step).
 *
 * Every line the probes print also lands here, split by report page and
 * with its colour kept as LVGL recolour markup, for the "Technical
 * details" views.
 */
#include "gui.h"
#include "gui_text.h"

/* ---- per-page capture of the report, with colours -------------------- */

#define PAGE_MAX 16
#define PAGE_CAP (64 * 1024)

static struct {
    char name[32];
    char *text;
    u32 len;
} s_pages[PAGE_MAX];
static u32 s_page_n;
static int s_cur = -1;
static bool s_capture;

static void _put(char c)
{
    if (s_cur < 0)
        return;
    if (s_pages[s_cur].len + 2 < PAGE_CAP) {
        s_pages[s_cur].text[s_pages[s_cur].len++] = c;
        s_pages[s_cur].text[s_pages[s_cur].len] = 0;
    }
}

static void _puts(const char *s)
{
    while (*s)
        _put(*s++);
}

static const char *_hex_of(u32 col)
{
    switch (col) {
    case COL_OK:     return GUI_HEX_OK;
    case COL_WARN:   return GUI_HEX_WARN;
    case COL_ERR:    return GUI_HEX_FAIL;
    case COL_INFO:   return GUI_HEX_ACCENT;
    case COL_HEADER: return "FF9F43";
    default:         return NULL;
    }
}

/* A page boundary is the "--- Name ---" header run_probe_mask prints. */
static bool _page_header(const char *s, u32 n)
{
    if (n < 10 || memcmp(s, "\n--- ", 5) || memcmp(s + n - 5, " ---\n", 5))
        return false;
    u32 len = n - 10;
    if (len >= sizeof(s_pages[0].name))
        len = sizeof(s_pages[0].name) - 1;
    for (u32 i = 0; i < s_page_n; i++)
        if (strlen(s_pages[i].name) == len && !memcmp(s_pages[i].name, s + 5, len)) {
            s_cur = i;
            return true;
        }
    if (s_page_n == PAGE_MAX) {
        s_cur = -1;
        return true;
    }
    s_cur = s_page_n++;
    memcpy(s_pages[s_cur].name, s + 5, len);
    s_pages[s_cur].name[len] = 0;
    if (!s_pages[s_cur].text)
        s_pages[s_cur].text = malloc(PAGE_CAP);
    s_pages[s_cur].len = 0;
    s_pages[s_cur].text[0] = 0;
    return true;
}

/* Called from log.c for every line the probes print. A coloured line
 * becomes "#RRGGBB text#"; a '#' in the text is escaped as "##" outside a
 * coloured span and dropped inside one, where LVGL would read it as the
 * end of the colour. */
void gui_log_capture(u32 color, const char *s, u32 n)
{
    if (!s_capture || _page_header(s, n))
        return;
    const char *hex = _hex_of(color);
    bool bol = s_cur >= 0 && (!s_pages[s_cur].len ||
                              s_pages[s_cur].text[s_pages[s_cur].len - 1] == '\n');
    bool open = false;
    for (u32 i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n') {
            if (open)
                _put('#');
            open = false;
            _put('\n');
            bol = true;
            continue;
        }
        if (bol && hex) {
            _put('#');
            _puts(hex);
            _put(' ');
            open = true;
        }
        bol = false;
        if (c == '#') {
            if (open)
                continue;
            _put('#');
        }
        _put(c);
    }
    if (open)
        _put('#');
}

const char *gui_report_page(const char *name)
{
    for (u32 i = 0; i < s_page_n; i++)
        if (!strcmp(s_pages[i].name, name))
            return s_pages[i].text;
    return NULL;
}

u32 gui_report_page_count(void)
{
    return s_page_n;
}

const char *gui_report_page_name(u32 idx)
{
    return idx < s_page_n ? s_pages[idx].name : NULL;
}

/* ---- progress screen ------------------------------------------------------ */

static const struct {
    const char *page;
    const char *label;
    const char *icon;
} s_steps[] = {
    { "SoC",              "Identifying the console",      ICON_ID },
    { "Fuses",            NULL,                           NULL },
    { "Power & charging", "Battery, charging and power",  ICON_BATTERY },
    { "Memory & clocks",  "Memory and clocks",            ICON_CHIP },
    { "Storage",          "Storage and game card reader", ICON_STORAGE },
    { "Wireless",         "Wi-Fi and Bluetooth",          ICON_WIFI },
    { "Audio",            "Sound",                        ICON_SPEAKER },
    { "Display",          "Screen",                       ICON_SCREEN },
    { "Inputs",           "Touch, sensors and buttons",   ICON_TOUCH },
    { "Raw state",        "System details",               ICON_LIST },
    { "Diagnostics",      "Putting the results together", ICON_HEALTH },
};
#define N_STEPS ARRAY_SIZE(s_steps)

static lv_obj_t *s_bar, *s_pct, *s_now, *s_sub;
static lv_obj_t *s_row_icon[N_STEPS], *s_row_text[N_STEPS];
static int s_step = -1;
static u32 s_started, s_total;
static u32 s_last_draw;
static bool s_running, s_hw_ready, s_saved;
bool gui_report_saved(void)
{
    return s_saved;
}

bool gui_report_ready(void)
{
    return s_page_n && !s_running;
}

/* The step a page belongs to: a page with no label of its own (Fuses)
 * belongs to the one above it. */
static int _step_of(const char *page)
{
    int step = -1;
    for (u32 i = 0; i < N_STEPS; i++) {
        if (s_steps[i].label)
            step = i;
        if (!strcmp(s_steps[i].page, page))
            return step;
    }
    return -1;
}

static void _row_state(int i, int state)   /* 0 waiting, 1 running, 2 done */
{
    if (!s_row_text[i])
        return;
    char buf[64];
    if (state == 2) {
        lv_label_set_text(s_row_icon[i], "#" GUI_HEX_OK " " ICON_CHECK "#");
        s_printf(buf, "%s", s_steps[i].label);
    } else if (state == 1) {
        lv_label_set_text(s_row_icon[i], "#" GUI_HEX_ACCENT " " ICON_PLAY "#");
        s_printf(buf, "#" GUI_HEX_ACCENT " %s...#", s_steps[i].label);
    } else {
        lv_label_set_text(s_row_icon[i], "#" GUI_HEX_HINT " " ICON_CIRCLE "#");
        s_printf(buf, "#" GUI_HEX_HINT " %s#", s_steps[i].label);
    }
    lv_label_set_text(s_row_text[i], buf);
}

static void _draw(bool force)
{
    u32 now = get_tmr_ms();
    if (!force && now - s_last_draw < 120)
        return;
    s_last_draw = now;
    u32 pct = s_total ? (s_started * 100) / s_total : 0;
    if (pct > 100)
        pct = 100;
    char buf[8];
    s_printf(buf, "%d%%", pct);
    lv_label_set_text(s_pct, buf);
    lv_bar_set_value(s_bar, pct);
    gui_refresh_now();
}

/* The sweep's progress hook (core.c calls it before every page, and the
 * long probes call it through status_step for the steps inside). */
void status_set(const char *msg)
{
    uart_poll_reboot();
    if (!s_running)
        return;

    for (u32 i = 0; i < N_PAGES; i++) {
        if (msg != g_pages[i].name)
            continue;
        s_started++;
        int step = _step_of(msg);
        if (step >= 0 && step != s_step) {
            if (s_step >= 0)
                _row_state(s_step, 2);
            s_step = step;
            _row_state(step, 1);
            lv_label_set_text(s_now, s_steps[step].label);
        }
        lv_label_set_text(s_sub, "");
        _draw(true);
        return;
    }
    /* Once every page has started, what is left is the sweep's own
     * closing breadcrumb, meant for the text pager. */
    if (s_total && s_started >= s_total)
        return;
    /* A step inside a page ("Wireless: PCIe lanes"): the page is already
     * the line above, so only the part after it. */
    const char *colon = strstr(msg, ": ");
    lv_label_set_text(s_sub, colon ? colon + 2 : msg);
    _draw(false);
}

/* The left column is wider here than the results': the bar and its
 * percentage. */
#define RUN_COL1_W  584
#define RUN_COL2_X  (GUI_MARGIN + RUN_COL1_W + GUI_GAP)

static void _build(void)
{
    lv_obj_t *win = gui_win_new(ICON_HEALTH " Health Check");
    /* Nothing can be left until the sweep is over: no Close to press. */
    gui_win_close_hide(true);

    /* Left: where the check is. */
    s_now = gui_label(win, &gui_font_30, GUI_COL_TEXT, "Getting ready");
    lv_obj_set_pos(s_now, GUI_MARGIN, GUI_TOP);

    s_bar = lv_bar_create(win, NULL);
    lv_obj_set_size(s_bar, 480, 30);
    lv_obj_set_pos(s_bar, GUI_MARGIN, GUI_TOP + 66);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0);

    s_pct = gui_label(win, &gui_font_30, GUI_COL_ACCENT, "0%");
    lv_obj_align(s_pct, s_bar, LV_ALIGN_OUT_RIGHT_MID, 24, 0);

    s_sub = gui_hint(win, RUN_COL1_W, "");
    lv_obj_set_pos(s_sub, GUI_MARGIN, GUI_TOP + 116);

    lv_obj_t *note = gui_hint(win, RUN_COL1_W,
        "Keep the console on. Some checks make a short sound or run the fan.");
    lv_obj_set_pos(note, GUI_MARGIN, GUI_BOTTOM - 50);

    /* Right: every step, ticked off as it finishes. */
    gui_divider(win, RUN_COL2_X - GUI_GAP / 2 - 1, GUI_TOP, GUI_CONTENT_H);

    lv_coord_t y = GUI_TOP;
    memset(s_row_text, 0, sizeof(s_row_text));
    for (u32 i = 0; i < N_STEPS; i++) {
        if (!s_steps[i].label)
            continue;
        s_row_icon[i] = gui_label(win, &gui_font_20, GUI_COL_TEXT, "");
        lv_obj_set_pos(s_row_icon[i], RUN_COL2_X, y);
        s_row_text[i] = gui_label(win, &gui_font_20, GUI_COL_TEXT, "");
        lv_obj_set_pos(s_row_text[i], RUN_COL2_X + 40, y);
        _row_state(i, 0);
        y += 42;
    }
}

/* The storage and thermal set-up the text payload does before its sweep
 * (main.c), once per boot. The GUI does it at start-up: the header reads
 * the serial from the eMMC, and the clocks load Minerva from the SD. */
void gui_hw_prepare(void)
{
    if (s_hw_ready)
        return;
    g_sd_ok   = sd_initialize(false) == 0;
    g_emmc_ok = emmc_initialize(false) == 0;
    tmp451_init();
    s_hw_ready = true;
}

void gui_run(void)
{
    _build();
    s_step = -1;
    s_started = 0;
    s_total = 0;
    for (u32 i = 0; i < N_PAGES; i++) {
        g_run_mask = PROBE_G_ALL;
        if (page_enabled(i))
            s_total++;
    }
    _draw(true);

    gui_input_hold(true);
    s_running = true;
    gui_clock_fast(false);    /* the payload's clocks, for the same readings */

    gui_hw_prepare();

    if (!g_report_body)
        g_report_body = malloc(REPORT_CAP);
    if (g_report_body)
        log_capture_arm(g_report_body, REPORT_CAP);
    for (u32 i = 0; i < s_page_n; i++)
        s_pages[i].len = 0;
    s_page_n = 0;
    s_cur = -1;
    s_capture = true;

    log_color(COL_HEADER, "\n=== hwtest full UART dump ===\n");
    run_probe_mask(PROBE_G_ALL);
    log_color(COL_HEADER, "\n=== end of dump ===\n\n");

    s_running = false;
    s_capture = false;
    log_capture_arm(NULL, 0);
    if (s_step >= 0)
        _row_state(s_step, 2);
    s_started = s_total;
    lv_label_set_text(s_now, "Done");
    lv_label_set_text(s_sub, "Saving the report...");
    _draw(true);

    s_saved = g_sd_ok && g_emmc_ok && !save_report();
    gui_clock_fast(true);

    /* The touch and light-sensor probes power the panel's LDO6 down when
     * they finish; bring touch back for the screens that follow. */
    gui_input_resume();
    gui_input_hold(false);
    gui_show(gui_results);
}
