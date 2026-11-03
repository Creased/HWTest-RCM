/*
 * hwtest GUI - results.
 *
 * Three levels, each one tap deeper:
 *  1. the verdict and one row per part of the console (battery, charging,
 *     storage, ...) saying, in a few words, whether it is fine;
 *  2. a category: every check in it, and for anything not fine, what it
 *     means for the owner and what to try;
 *  3. the technical report pages behind it, as the probes printed them.
 *
 * Everything is read back from the dx registry the sweep filled, the same
 * findings the text payload's verdict aggregates.
 */
#include "gui.h"
#include "gui_text.h"

/* Worst last. ST_PARTIAL: what ran passed, but some of the category's
 * checks were not picked for this run (gui_custom). */
typedef enum { ST_UNTESTED, ST_OK, ST_SOFT, ST_PARTIAL, ST_WARN, ST_FAIL } st_t;

static u32 s_cat_sel;
static const char *s_tech_pages[PAGE_LIST_MAX];
static const char *s_tech_title;
static gui_screen_fn_t s_tech_back;

/* ---- evaluation ------------------------------------------------------- */

static const struct dx_macro *_row(const char *name)
{
    u32 n = strlen(name);
    for (u32 i = 0; i < g_dx_macro_n; i++) {
        const char *m = g_dx_macros[i].name;
        if (!strncmp(m, name, n) && (m[n] == ' ' || !m[n]))
            return &g_dx_macros[i];
    }
    return NULL;
}

/* The same rule probe_verdict() uses to decide a row was covered. */
static bool _row_ran(const struct dx_macro *mc)
{
    u32 sel = mc->group_mask & g_run_mask;
    return mc->require_all_groups ? sel == mc->group_mask : sel != 0;
}

static st_t _finding_state(const char *key, const dx_finding_t **out)
{
    const dx_finding_t *f = dx_get(key);
    if (out)
        *out = f;
    if (!f)
        return ST_UNTESTED;
    if (f->sev == DX_FAIL)
        return ST_FAIL;
    if (f->sev == DX_WARN) {
        const gui_check_text_t *t = gui_check_text(key);
        if (t && t->soft && !strncmp(f->detail, t->soft, strlen(t->soft)))
            return ST_SOFT;
        return ST_WARN;
    }
    return ST_OK;
}

/* Calls fn for every check behind a category; returns its worst state.
 * The checks of a row that was not picked for this run are passed to fn
 * as untested, so a category's page still lists them. */
static st_t _cat_walk(const gui_cat_t *c,
                      void (*fn)(const char *key, st_t st, void *ctx), void *ctx)
{
    st_t worst = ST_UNTESTED;
    bool skipped = false;
    for (u32 r = 0; r < ARRAY_SIZE(c->rows) && c->rows[r]; r++) {
        const struct dx_macro *mc = _row(c->rows[r]);
        if (!mc)
            continue;
        if (!_row_ran(mc)) {
            skipped = true;
            if (fn)
                for (const char *const *k = mc->keys; *k; k++)
                    fn(*k, ST_UNTESTED, ctx);
            continue;
        }
        for (const char *const *k = mc->keys; *k; k++) {
            st_t st = _finding_state(*k, NULL);
            if (st == ST_UNTESTED)
                continue;
            if (fn)
                fn(*k, st, ctx);
            if (st > worst)
                worst = st;
        }
    }
    /* Passed as far as it went is not the same as passed. */
    if (skipped && (worst == ST_OK || worst == ST_SOFT))
        return ST_PARTIAL;
    return worst;
}

/* ---- readings ------------------------------------------------------------ */

static bool _is_hex6(const char *s)
{
    for (u32 i = 0; i < 6; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

/* One captured line as the probe printed it: gui_log_capture's colour
 * markup off ("#RRGGBB " in front, '#' at the end, "##" for a '#'), and the
 * indent trimmed. */
static void _plain(char *dst, u32 cap, const char *s, u32 n)
{
    if (n >= 8 && s[0] == '#' && s[7] == ' ' && _is_hex6(s + 1)) {
        s += 8;
        n -= 8;
    }
    while (n && *s == ' ') {
        s++;
        n--;
    }
    u32 o = 0;
    for (u32 i = 0; i < n && o + 1 < cap; i++) {
        if (s[i] == '#') {
            if (i + 1 < n && s[i + 1] == '#')
                dst[o++] = s[++i];
            continue;
        }
        dst[o++] = s[i];
    }
    dst[o] = 0;
}

/* A value as the reading asks for it, with runs of spaces closed up. */
static void _fact_value(char *dst, u32 cap, const char *v, u8 how)
{
    const char *end = v + strlen(v);
    if (how == FACT_PAREN) {
        const char *open = strchr(v, '('), *close = open ? strchr(open, ')') : NULL;
        if (open && close) {
            v = open + 1;
            end = close;
        }
    } else if (how == FACT_CUT) {
        const char *cut = strstr(v, " (");
        if (cut)
            end = cut;
    }
    u32 o = 0;
    for (; v < end && o + 1 < cap; v++) {
        if (*v == ' ' && (!o || dst[o - 1] == ' '))
            continue;
        dst[o++] = *v;
    }
    while (o && dst[o - 1] == ' ')
        o--;
    dst[o] = 0;
}

/* Finds a reading in the last run's report; false when the run did not
 * print it. Section headers are the "[...]" lines, but not the sweep's
 * "[step]" breadcrumbs, which land in the middle of a section. */
static bool _fact_find(const gui_fact_t *f, char *out, u32 cap)
{
    const char *t = gui_report_page(f->page);
    if (!t)
        return false;
    bool in = !f->section;
    u32 ll = strlen(f->label);
    char line[160];
    while (*t) {
        u32 n = 0;
        while (t[n] && t[n] != '\n')
            n++;
        _plain(line, sizeof(line), t, n);
        t += n;
        if (*t)
            t++;
        if (line[0] == '[' && strncmp(line, "[step]", 6)) {
            in = f->section && !strncmp(line, f->section, strlen(f->section));
            continue;
        }
        if (!in || strncmp(line, f->label, ll))
            continue;
        const char *colon = strstr(line + ll, ": ");
        if (!colon)
            continue;
        _fact_value(out, cap, colon + 2, f->how);
        return out[0] != 0;
    }
    return false;
}

/* The readings, one under the other: the name small and grey, the value
 * under it. Returns where they end. */
static lv_coord_t _facts(lv_obj_t *parent, const gui_fact_t *facts,
                         lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t max_y)
{
    for (const gui_fact_t *f = facts; f && f->name && y + 50 <= max_y; f++) {
        char val[96];
        if (!_fact_find(f, val, sizeof(val)))
            continue;
        lv_obj_t *k = gui_hint(parent, 0, f->name);
        lv_obj_set_pos(k, x, y);
        /* A long reading goes on to a second line rather than being cut
         * short; one that would not fit above max_y is left out. */
        lv_obj_t *v = gui_label(parent, &gui_font_20, GUI_COL_TEXT, "");
        lv_label_set_recolor(v, false);
        lv_label_set_long_mode(v, LV_LABEL_LONG_BREAK);
        lv_obj_set_width(v, w);
        lv_label_set_text(v, val);
        lv_obj_set_pos(v, x, y + 24);
        lv_coord_t h = lv_obj_get_height(v);
        if (y + 24 + h > max_y) {
            lv_obj_del(k);
            lv_obj_del(v);
            break;
        }
        y += 24 + h + 8;
    }
    return y;
}

/* ---- shared bits -------------------------------------------------------- */

static const char *_status(st_t st)
{
    switch (st) {
    case ST_FAIL: return "#" GUI_HEX_FAIL " Problem found#";
    case ST_WARN: return "#" GUI_HEX_WARN " Worth a look#";
    case ST_PARTIAL: return "#" GUI_HEX_HINT " Partly checked#";
    case ST_OK:
    case ST_SOFT: return "#" GUI_HEX_OK " Working normally#";
    default:      return "#" GUI_HEX_HINT " Not checked#";
    }
}

static const char *_mark(st_t st)
{
    switch (st) {
    case ST_FAIL: return "#" GUI_HEX_FAIL " " ICON_FAIL "#";
    case ST_WARN: return "#" GUI_HEX_WARN " " ICON_WARN "#";
    case ST_SOFT:
    case ST_PARTIAL: return "#" GUI_HEX_HINT " " ICON_UNKNOWN "#";
    case ST_OK:   return "#" GUI_HEX_OK " " ICON_OK "#";
    default:      return "#" GUI_HEX_HINT " " ICON_UNKNOWN "#";
    }
}

static lv_res_t _act_again(lv_obj_t *b) { gui_show(gui_run); return LV_RES_OK; }

static lv_res_t _act_cat(lv_obj_t *b)
{
    s_cat_sel = lv_obj_get_free_num(b);
    gui_show(gui_cat_detail);
    return LV_RES_OK;
}

static lv_res_t _act_full_report(lv_obj_t *b)
{
    u32 n = gui_report_page_count();
    if (n > PAGE_LIST_MAX - 1)
        n = PAGE_LIST_MAX - 1;
    for (u32 i = 0; i < n; i++)
        s_tech_pages[i] = gui_report_page_name(i);
    s_tech_pages[n] = NULL;
    s_tech_title = "Technical Report";
    s_tech_back = gui_results;
    gui_show(gui_tech_report);
    return LV_RES_OK;
}

/* ---- level 1: the verdict --------------------------------------------- */

void gui_results(void)
{
    lv_obj_t *win = gui_win_new(ICON_HEALTH " Results");
    gui_win_btn(win, ICON_LIST " Report", _act_full_report);
    lv_obj_t *again = gui_win_btn(win, ICON_REFRESH " Check Again", _act_again);

    u32 n_fail = 0, n_warn = 0;
    st_t st[16];
    for (u32 i = 0; i < g_gui_cat_n; i++) {
        st[i] = _cat_walk(&g_gui_cats[i], NULL, NULL);
        n_fail += st[i] == ST_FAIL;
        n_warn += st[i] == ST_WARN;
    }

    /* Left: the answer to "is my console OK?" */
    const char *big = n_fail ? "#" GUI_HEX_FAIL " " ICON_FAIL "#" :
                      n_warn ? "#" GUI_HEX_WARN " " ICON_WARN "#" :
                               "#" GUI_HEX_OK " " ICON_OK "#";
    lv_obj_t *ic = gui_label(win, &gui_icons_120, GUI_COL_TEXT, big);
    lv_obj_set_pos(ic, GUI_MARGIN, GUI_TOP);

    const char *head = n_fail ? "Problems found" :
                       n_warn ? "Mostly healthy" : "All good";
    lv_obj_t *hl = gui_label(win, &gui_font_30, GUI_COL_TEXT, head);
    lv_obj_set_pos(hl, GUI_MARGIN, GUI_TOP + 140);

    char sub[160];
    if (n_fail)
        s_printf(sub, "%d part%s of the console %s a problem. Open %s to see "
                 "what it means and what to try.", n_fail, n_fail > 1 ? "s" : "",
                 n_fail > 1 ? "have" : "has", n_fail > 1 ? "them" : "it");
    else if (n_warn)
        s_printf(sub, "Nothing is broken, but %d thing%s %s worth a look.",
                 n_warn, n_warn > 1 ? "s" : "", n_warn > 1 ? "are" : "is");
    else
        s_printf(sub, "Every check passed. The console looks healthy.");
    lv_obj_t *sl = gui_hint(win, GUI_COL1_W, sub);
    lv_obj_set_pos(sl, GUI_MARGIN, GUI_TOP + 184);

    lv_obj_t *saved = gui_hint(win, GUI_COL1_W, gui_report_saved() ?
        "The full report is saved on the SD card, in the backup folder." :
        "#" GUI_HEX_WARN " The report could not be saved to the SD card.#");
    lv_obj_set_pos(saved, GUI_MARGIN, GUI_TOP + 184 + lv_obj_get_height(sl) + 24);
    if (g_run_mask != PROBE_G_ALL) {
        lv_obj_t *part = gui_hint(win, GUI_COL1_W,
            "A custom check: what was not picked shows as not checked.");
        lv_obj_set_pos(part, GUI_MARGIN,
                       lv_obj_get_y(saved) + lv_obj_get_height(saved) + 24);
    }

    gui_divider(win, GUI_DIVIDER_X, GUI_TOP, GUI_CONTENT_H);

    /* Right: one row per part of the console, worst first; eight rows fill
     * the list exactly, the rest scroll. */
    lv_obj_t *page = gui_page(win, GUI_COL2_X, GUI_TOP, GUI_COL2_W, GUI_CONTENT_H);
    lv_coord_t y = 0, rw = GUI_COL2_W - GUI_PAGE_SB_W, rh = GUI_CONTENT_H / 8;
    lv_obj_t *first = NULL;
    for (int want = ST_FAIL; want >= ST_UNTESTED; want--) {
        for (u32 i = 0; i < g_gui_cat_n; i++) {
            st_t s = st[i] == ST_SOFT ? ST_OK : st[i];
            if (s != (st_t)want)
                continue;
            const gui_cat_t *c = &g_gui_cats[i];
            lv_obj_t *b = gui_list_row(page, y, rw, rh, _act_cat);
            lv_obj_set_free_num(b, i);
            /* Icon and title are separate labels so the titles line up
             * whatever the icon's width. */
            char icon[24];
            s_printf(icon, "#" GUI_HEX_ACCENT " %s#", c->icon);
            lv_obj_t *ic = gui_label(b, &gui_font_30, GUI_COL_TEXT, icon);
            lv_obj_align(ic, NULL, LV_ALIGN_IN_LEFT_MID, 16, 0);
            lv_obj_t *t = gui_label(b, &gui_font_30, GUI_COL_TEXT, c->title);
            lv_obj_align(t, NULL, LV_ALIGN_IN_LEFT_MID, 16 + 40, 0);
            lv_obj_t *r = gui_label(b, &gui_font_20, GUI_COL_TEXT, _status(s));
            lv_obj_align(r, NULL, LV_ALIGN_IN_RIGHT_MID, -16, 0);
            if (!first)
                first = b;
            y += rh;
        }
    }

    lv_group_focus_obj(first ? first : again);
}

/* ---- level 2: one category --------------------------------------------- */

typedef struct {
    lv_obj_t *page;
    lv_coord_t y, w;
    bool problems;           /* this pass lists the warnings and failures */
} detail_ctx_t;

static void _detail_row(const char *key, st_t st, void *vctx)
{
    detail_ctx_t *ctx = vctx;
    if ((st == ST_WARN || st == ST_FAIL) != ctx->problems)
        return;
    const gui_check_text_t *t = gui_check_text(key);
    const dx_finding_t *f;
    _finding_state(key, &f);

    /* Each check is a focusable row, so VOL+/VOL- and the stick walk the
     * list and the page scrolls along with them. */
    lv_obj_t *row = gui_focus_row(ctx->page, ctx->y, ctx->w);
    /* Mark and name are separate labels: the names, and the text under
     * them, line up 40 px in from the mark whatever its width. */
    lv_obj_t *ml = gui_label(row, &gui_font_30, GUI_COL_TEXT, _mark(st));
    lv_obj_set_pos(ml, 16, 14);
    lv_obj_t *nl = gui_label(row, &gui_font_30, GUI_COL_TEXT, t ? t->name : key);
    lv_obj_set_pos(nl, 16 + 40, 14);
    lv_coord_t h = 14 + 44;

    const char *why = NULL;
    if (st == ST_FAIL)
        why = t && t->fail ? t->fail : "This check failed.";
    else if (st == ST_WARN)
        why = t && t->warn ? t->warn : "This check gave a warning.";
    else if (st == ST_SOFT)
        why = "Not checked on this console.";
    else if (st == ST_UNTESTED)
        why = "Not picked for this check.";
    if (why) {
        lv_obj_t *wl = gui_label_wrap(row, &gui_font_20, GUI_COL_TEXT,
                                      ctx->w - 72, why);
        lv_obj_set_pos(wl, 16 + 40, h);
        h += lv_obj_get_height(wl) + 6;
        if (f && f->detail[0] && st != ST_SOFT) {
            char tech[80];
            s_printf(tech, "Technical: %s", f->detail);
            lv_obj_t *tl = gui_hint(row, ctx->w - 72, tech);
            lv_obj_set_pos(tl, 16 + 40, h);
            h += lv_obj_get_height(tl) + 4;
        }
    }
    lv_obj_set_height(row, h + 12);
    ctx->y += h + 12;
}

static lv_res_t _act_cat_tech(lv_obj_t *b)
{
    const gui_cat_t *c = &g_gui_cats[s_cat_sel];
    u32 n = 0;
    for (u32 i = 0; i < ARRAY_SIZE(c->pages) && c->pages[i]; i++)
        s_tech_pages[n++] = c->pages[i];
    s_tech_pages[n] = NULL;
    s_tech_title = c->title;
    s_tech_back = gui_cat_detail;
    gui_show(gui_tech_report);
    return LV_RES_OK;
}

void gui_cat_detail(void)
{
    const gui_cat_t *c = &g_gui_cats[s_cat_sel];
    char title[64];
    s_printf(title, "%s %s", c->icon, c->title);
    lv_obj_t *win = gui_win_new(title);
    gui_set_back(gui_results);
    lv_obj_t *tech = gui_win_btn(win, ICON_LIST " Details", _act_cat_tech);

    /* Left: how it went, and the readings that say what this part is and
     * what state it is in. */
    st_t st = _cat_walk(c, NULL, NULL);
    char head[128];
    s_printf(head, "%s  %s", _mark(st), _status(st));
    lv_obj_t *hl = gui_label(win, &gui_font_30, GUI_COL_TEXT, head);
    lv_obj_set_pos(hl, GUI_MARGIN, GUI_TOP);
    lv_obj_t *say = gui_hint(win, GUI_COL1_W,
        st == ST_OK || st == ST_SOFT ? c->ok :
        st == ST_UNTESTED ? "These checks did not run." :
        st == ST_PARTIAL ? "What ran passed; the checks marked on the right "
                           "were not picked for this run." :
        "What was found and what to try is on the right.");
    lv_obj_set_pos(say, GUI_MARGIN, GUI_TOP + 46);
    _facts(win, c->facts, GUI_MARGIN, GUI_TOP + 46 + lv_obj_get_height(say) + 24,
           GUI_COL1_W, GUI_BOTTOM);

    gui_divider(win, GUI_DIVIDER_X, GUI_TOP, GUI_CONTENT_H);

    /* Right: every check behind it. */
    detail_ctx_t ctx;
    ctx.page = gui_page(win, GUI_COL2_X, GUI_TOP, GUI_COL2_W, GUI_CONTENT_H);
    ctx.y = 0;
    ctx.w = GUI_COL2_W - GUI_PAGE_SB_W;
    /* What needs a look first, so it is not below the fold. */
    ctx.problems = true;
    _cat_walk(c, _detail_row, &ctx);
    ctx.problems = false;
    _cat_walk(c, _detail_row, &ctx);
    if (ctx.y == 0)
        gui_hint(lv_page_get_scrl(ctx.page), 0, "Nothing was checked here on this run.");

    lv_group_focus_obj(tech);
}

/* ---- level 3: the report pages ------------------------------------------ */

/* One label per few dozen lines: LVGL lays a label's whole text out on
 * every redraw, so a single label of the full report would crawl. */
static u32 s_tech_labels;    /* how many the last build made, for the UART */

static lv_coord_t _text_block(lv_obj_t *scrl, lv_coord_t y, const char *text)
{
    static char chunk[4096];
    while (*text) {
        s_tech_labels++;
        u32 n = 0, lines = 0;
        while (text[n] && lines < 40 && n < sizeof(chunk) - 2) {
            if (text[n] == '\n')
                lines++;
            n++;
        }
        memcpy(chunk, text, n);
        chunk[n] = 0;
        text += n;
        lv_obj_t *l = lv_label_create(scrl, NULL);
        lv_label_set_style(l, &gui_st_mono);
        lv_label_set_recolor(l, true);
        lv_label_set_text(l, chunk);
        lv_obj_set_click(l, false);
        lv_obj_set_pos(l, 0, y);
        y += lv_obj_get_height(l);
    }
    return y;
}

static lv_res_t _act_scroll(lv_obj_t *b)
{
    lv_obj_t *page = lv_obj_get_free_ptr(b);
    lv_page_scroll_ver(page, lv_obj_get_free_num(b) ? 360 : -360);
    return LV_RES_OK;
}

void gui_tech_report(void)
{
    char title[64];
    s_printf(title, ICON_LIST " %s", s_tech_title ? s_tech_title : "Technical Report");
    lv_obj_t *win = gui_win_new(title);
    gui_set_back(s_tech_back ? s_tech_back : gui_results);
    lv_obj_t *dn = gui_win_btn(win, ICON_DOWN " Down", _act_scroll);
    lv_obj_t *up = gui_win_btn(win, ICON_UP " Up", _act_scroll);

    lv_obj_t *page = gui_page(win, GUI_MARGIN, GUI_TOP, GUI_CONTENT_W, GUI_CONTENT_H);
    lv_obj_t *scrl = lv_page_get_scrl(page);
    lv_coord_t y = 0;
    u32 t0 = get_tmr_ms(), pages = 0;
    s_tech_labels = 0;
    for (u32 i = 0; i < PAGE_LIST_MAX && s_tech_pages[i]; i++, pages++) {
        /* LVGL's coordinates are 16-bit: a report taller than that stops
         * here rather than wrap round. */
        if (y > 28000) {
            gui_hint(scrl, 0, "The rest is in the report saved on the SD card.");
            break;
        }
        const char *text = gui_report_page(s_tech_pages[i]);
        char hdr[48];
        s_printf(hdr, "#" GUI_HEX_ACCENT " %s#", s_tech_pages[i]);
        lv_obj_t *hl = gui_label(scrl, &gui_font_30, GUI_COL_TEXT, hdr);
        lv_obj_set_pos(hl, 0, y);
        y += 46;
        y = _text_block(scrl, y, text ? text : "(this page did not run)\n");
        y += 24;
    }
    /* A breadcrumb for a console that stops here. */
    char buf[96];
    s_printf(buf, "gui: report: %d pages, %d labels, %d px, built in %d ms\n",
             pages, s_tech_labels, y, get_tmr_ms() - t0);
    gui_uart_puts(buf);

    lv_obj_set_free_ptr(dn, page);
    lv_obj_set_free_num(dn, 0);
    lv_obj_set_free_ptr(up, page);
    lv_obj_set_free_num(up, 1);
    lv_group_focus_obj(dn);
}
