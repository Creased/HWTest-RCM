/*
 * hwtest GUI - the dock test.
 *
 * Two halves. The left one is a live view of the USB-C port's PD
 * controller, read only: its status, alerts, the power profiles on offer,
 * the contract, its DisplayPort bits and the hot plug detect line. It runs
 * from the moment the screen opens, docked or not, so what the controller
 * does on its own can be watched. The right one is the handshake the
 * console performs when it docks (usbpd.h), behind Run, step by step with
 * what each step found, and where it stopped if it did.
 */
#include "gui.h"
#include "../usbpd.h"
#include "../dpout.h"
#include <mem/heap.h>
#include <rtc/max77620-rtc.h>
#include <storage/sd.h>
#include <memory_map.h>
#include <soc/gpio.h>

#define DK_LEFT_W   560
#define DK_RIGHT_X  (GUI_MARGIN + DK_LEFT_W + GUI_GAP)
#define DK_RIGHT_W  (GUI_RIGHT - DK_RIGHT_X)
#define DK_TICK_MS  40
#define DK_VIEW_MS  500         /* the snapshot is twenty I2C reads     */
#define DK_STEPS    8

static lv_obj_t *s_regs, *s_step[DK_STEPS], *s_verdict, *s_hint, *s_start;
static pd_snapshot_t s_snap, s_prev;
static dock_t s_dock;
static pd_reset_t s_rst;
static bool s_resetting;
static bool s_ran;             /* a handshake ran: the dock is stopped on leave */
static bool s_tv_wait;         /* done with DisplayPort up, no HPD yet: the picture
                                * follows when it comes */
static lv_obj_t *s_reset;
static void _tv_link(void);
static void _reset_report(void);
static lv_task_t *s_dk_task;
static bool s_running;
static u32 s_view_at, s_uart_at, s_snap_us;
static u16 s_sticky;           /* alert bits seen on two reads in a row */

static const char *s_step_name[DK_STEPS] = {
    "Cable and power on offer", "Power contract", "Data host", "Who is there",
    "Dock mode", "DisplayPort", "TV on the dock", "Dock light and hub",
};

/* The USB-IF vendor ids met on docks and adapters, for the identity reply. */
static const char *_vendor(u16 vid)
{
    switch (vid) {
    case 0x057E: return "Nintendo ";
    case 0x1D5C: return "Fresco Logic ";
    case 0x2109: return "VIA Labs ";
    case 0x0BDA: return "Realtek ";
    case 0x05E3: return "Genesys Logic ";
    case 0x04B4: return "Cypress ";
    case 0x0451: return "Texas Instruments ";
    case 0x1DA0: return "Parade ";
    case 0x2188: return "CalDigit ";
    case 0x17EF: return "Lenovo ";
    case 0x413C: return "Dell ";
    case 0x05AC: return "Apple ";
    default:     return "";
    }
}

/* The product type in the identity reply (the UFP's, PD 3.0). */
static const char *_product_type(u32 t)
{
    static const char *n[] = {
        "type 0", "a USB hub", "a USB device", "a power sink", "type 4",
        "an alternate mode adapter", "a VCONN powered device", "type 7",
    };
    return n[t & 7];
}

static const char *_role(u16 s1)
{
    static const char *r[] = { "none", "device (UFP)", "host (DFP)", "accessory" };
    return r[(s1 & PD_S1_DR_MASK) >> PD_S1_DR_SHIFT];
}

/* ---- the live view --------------------------------------------------- */

/* bdk's s_printf returns nothing, so the lines are appended by hand. */
#define APPEND(...) do { s_printf(p, __VA_ARGS__); p += strlen(p); } while (0)

static void _view(void)
{
    const pd_snapshot_t *s = &s_snap;
    char buf[1200];
    char *p = buf;
    /* Its pins on the SoC: the reset line HOS pulses, its interrupt line,
     * VCONN. Read only. */
    pd_pads_t pads;
    char pv5[12], pk4[12], pk5[12];
    pd_pads(&pads);
    pd_pad_text(&pads.reset, pv5);
    pd_pad_text(&pads.irq, pk4);
    pd_pad_text(&pads.vconn, pk5);
    if (!s->ok) {
        /* The fuel gauge shares the bus: its device name tells a silent
         * controller from a dead bus. */
        u8 dn[2] = { 0, 0 };
        int gauge = i2c_recv_buf_small(dn, 2, I2C_1, 0x36, 0x21);
        s_printf(buf, "The PD controller does not answer\n"
                 "on I2C_1 at 0x18.\n\n"
                 "Six tries, packet then normal mode.\n"
                 "Last read: ids %04X / %04X,\n"
                 "firmware type %04X\n"
                 "(expected 04B5 / 03B0 / 0603),\n"
                 "transfers %s.\n\n"
                 "Fuel gauge on the same bus: %s\n"
                 "(device name %02X%02X, expected 00AC).\n\n"
                 "Pins  PV5 %s  PK4 %s  PK5 %s\n\n"
                 "Reset Chip does what HOS does to it\n"
                 "at every boot: a pulse on PV5, then\n"
                 "SYS_RESET and its alerts.",
                 s->man_id, s->dev_id, s->fw_type,
                 s->rc ? "reporting an error" : "reporting no error",
                 gauge ? "silent too" : "answers", dn[1], dn[0], pv5, pk4, pk5);
        if (strcmp(lv_label_get_text(s_regs), buf))
            lv_label_set_text(s_regs, buf);
        return;
    }
    /* Lines of 46 characters at most: the column's width in this font. */
    /* The firmware type as bdk reads it: the two bytes as nibbles, 36. */
    APPEND("BM92T36  fw %02X  rev %04X  caps %04X\n",
           ((s->fw_type & 0xFF) << 4) | (s->fw_type >> 8), s->fw_rev, s->caps);
    APPEND("         %s\n", s->normal_mode ? "normal mode" : "packet mode");
    if (s_running)
        APPEND("Alert    %04X  held by the handshake\n", s_dock.alerts);
    else
        APPEND("Alert    %04X  %s\n", s->alert,
               s_sticky ? "bits stay set across reads" :
               (s_prev.alert && !s->alert) ? "cleared by the read" : "");
    APPEND("Status1  %04X  cable %s  %s%s\n", s->status1,
           (s->status1 & PD_S1_INSERT) ? "in" : "out",
           (s->status1 & PD_S1_SPDSNK) ? "sink " : "",
           (s->status1 & PD_S1_VSAFE) ? "vsafe" : "no power");
    APPEND("         role %s  busy %d  fault %d\n", _role(s->status1),
           !!(s->status1 & PD_S1_CMD_BUSY), s->status1 & PD_S1_FAULT_MASK);
    APPEND("         last command %s\n", pd_lastcmd_name(s->status1));
    APPEND("Status2  %04X  side CC%d  vconn %d  e-mark %d\n", s->status2,
           (s->status1 & PD_S1_CSIDE) ? 2 : 1, !!(s->status2 & PD_S2_VCONN_ON),
           !!(s->status2 & PD_S2_EM_CABLE));
    APPEND("         accessory %d  otg %d\n",
           (s->status2 & PD_S2_ACC_MASK) >> PD_S2_ACC_SHIFT, !!(s->status2 & PD_S2_OTG_INSERT));
    APPEND("DP       %04X  on %d  in %d  pins %d  signal %d\n", s->dp_status,
           !!(s->dp_status & PD_DP_EN), !!(s->dp_status & PD_DP_INSERT),
           !!(s->dp_status & PD_DP_PIN_CFG_DONE), !!(s->dp_status & PD_DP_SIGNAL_ON));
    APPEND("         1C %04X  1D %04X\n", s->dp_unk1c, s->dp_unk1d);
    APPEND("Config   %04X %04X  sys %04X %04X\n", s->config1, s->config2, s->sys1, s->sys2);
    /* Four profiles to a line: a charger offers up to seven. */
    APPEND("Offer    ");
    if (!s->pdo_n)
        APPEND("nothing");
    for (u32 i = 0; i < s->pdo_n; i++)
        APPEND("%s%dV %d.%dA", i == 0 ? "" : (i % 4) ? "  " : "\n         ",
               s->pdo_mv[i] / 1000, s->pdo_ma[i] / 1000, s->pdo_ma[i] % 1000 / 100);
    APPEND("\n");
    if (s->cur_pdo) {
        u32 mv = ((s->cur_pdo >> 10) & 0x3FF) * 50, ma = (s->cur_pdo & 0x3FF) * 10;
        u32 op = ((s->cur_rdo >> 10) & 0x3FF) * 10;
        APPEND("Contract %d.%d V %d.%d A, up to %d.%d A\n",
               mv / 1000, mv % 1000 / 100, ma / 1000, ma % 1000 / 100,
               op / 1000, op % 1000 / 100);
    } else {
        APPEND("Contract none\n");
    }
    APPEND("HPD      %s  (%s)\n", s->hpd ? "high" : "low",
           dpout_aux_ready() ? "DPAUX1" : "GPIO CC1");
    APPEND("Pins     PV5 reset %s  PK4 irq %s\n", pv5, pk4);
    APPEND("         PK5 vconn %s\n", pk5);
    APPEND("Switch   PI3USB30532: no I2C address here;\n"
           "         driven by the PD controller (inferred)");
    /* The label is most of the left column, and areas are full width:
     * redrawn only when it says something new. */
    if (strcmp(lv_label_get_text(s_regs), buf))
        lv_label_set_text(s_regs, buf);
}

/* ---- the handshake's rows ---------------------------------------------- */

static void _row(u32 i, const char *mark, const char *text)
{
    char buf[200];
    s_printf(buf, "%s  %s: %s", mark, s_step_name[i], text);
    lv_label_set_text(s_step[i], buf);
}

#define MARK_OK    "#" GUI_HEX_OK " " ICON_OK "#"
#define MARK_WARN  "#" GUI_HEX_WARN " " ICON_WARN "#"
#define MARK_FAIL  "#" GUI_HEX_FAIL " " ICON_FAIL "#"
#define MARK_NOW   "#" GUI_HEX_ACCENT " " ICON_PLAY "#"
#define MARK_NONE  "#" GUI_HEX_HINT " " ICON_UNKNOWN "#"

static void _rows(void)
{
    const dock_t *d = &s_dock;
    char buf[160];
    dock_state_t st = d->state;
    /* Which step each state belongs to. */
    static const u8 step_of[] = {
        0, 0, 1, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8
    };
    bool failed = st == DOCK_FAILED, done = st == DOCK_DONE;
    u32 now = step_of[failed ? d->failed_state : st];
    if (done)
        now = DK_STEPS;

    for (u32 i = 0; i < DK_STEPS; i++) {
        const char *mark = MARK_NONE;
        const char *text = "not yet";
        if (failed && i == now) {
            _row(i, MARK_FAIL, d->fail);
            continue;
        }
        if (failed && i > now) {
            _row(i, MARK_NONE, "not reached");
            continue;
        }
        if (i < now || done) {
            mark = MARK_OK;
            switch (i) {
            case 0:
                if (!d->pdo_n) { mark = MARK_NONE; text = "not yet"; break; }
                s_printf(buf, "%d profile%s%s", d->pdo_n, d->pdo_n == 1 ? "" : "s",
                         d->dock_power ? ", the dock's 5 V then 15 V" : "");
                text = buf;
                break;
            case 1:
                s_printf(buf, "%d.%d V %d.%d A, drawing up to %d.%d A",
                         d->got_mv / 1000, d->got_mv % 1000 / 100,
                         d->got_ma / 1000, d->got_ma % 1000 / 100,
                         d->req_ma / 1000, d->req_ma % 1000 / 100);
                text = buf;
                break;
            case 2:
                text = "this console is the USB host";
                break;
            case 3:
                if (d->nin_dock)
                    s_printf(buf, "Nintendo dock (%04X:%04X)", d->vid, d->pid);
                else if (d->nin_charger)
                    s_printf(buf, "Nintendo charger (%04X:%04X), not a dock", d->vid, d->pid);
                else
                    s_printf(buf, "another device, %s%04X:%04X, %s", _vendor(d->vid),
                             d->vid, d->pid, _product_type(d->vdo_type));
                text = buf;
                if (!d->nin_dock)
                    mark = MARK_WARN;
                break;
            case 4:
                if (d->nin_charger) { mark = MARK_NONE; text = "not a dock"; break; }
                text = d->nin_mode ? "entered" : "DisplayPort offered";
                break;
            case 5:
                if (d->nin_charger) { mark = MARK_NONE; text = "not a dock"; break; }
                s_printf(buf, "%d lanes, pins %s, %sconfigured by the controller%s",
                         d->lanes, d->lanes == 2 ? "D" : "C",
                         (d->dp_status & PD_DP_PIN_CFG_DONE) ? "" : "NOT ",
                         (d->dp_status & PD_DP_EN) ? "" : ", mode not on");
                text = buf;
                if (!(d->dp_status & PD_DP_PIN_CFG_DONE))
                    mark = MARK_WARN;
                break;
            case 6:
                if (d->nin_charger) { mark = MARK_NONE; text = "not a dock"; break; }
                if (d->hpd || (d->dp_status & PD_DP_SIGNAL_ON)) {
                    text = "a TV is on the dock's HDMI (HPD high)";
                } else {
                    text = "nothing on the dock's HDMI (HPD low)";
                    mark = MARK_WARN;
                }
                break;
            case 7:
                if (!d->nin_dock) { mark = MARK_NONE; text = "not a Nintendo dock"; break; }
                if (d->hos_reject) {
                    s_printf(buf, "no answer to the dock's %s read: HOS waits 5 s for it "
                             "and does not dock without it", d->hos_reject);
                    text = buf;
                    mark = MARK_FAIL;
                    break;
                }
                if (d->nd_silent)
                    s_printf(buf, "no reply to its %s message (a clone?); ", d->nd_silent);
                else
                    s_printf(buf, "light %s, hub %s; ", d->led_lit ? "on" : "off",
                             d->hub_up ? "up" : "not confirmed");
                s_printf(buf + strlen(buf), "state %02X%02X%02X%02X, 0B %02X%02X%02X%02X, "
                         "18 %02X%02X%02X%02X%s",
                         d->dock_state[0], d->dock_state[1], d->dock_state[2], d->dock_state[3],
                         d->dock_0b[0], d->dock_0b[1], d->dock_0b[2], d->dock_0b[3],
                         d->dock_18[0], d->dock_18[1], d->dock_18[2], d->dock_18[3],
                         d->dock_old_fw ? " (old dock firmware)" : "");
                text = buf;
                if (d->nd_silent || !d->hub_up)
                    mark = MARK_WARN;
                break;
            }
        } else if (i == now && !failed && st != DOCK_IDLE) {
            mark = MARK_NOW;
            text = dock_state_name(st);
        }
        _row(i, mark, text);
    }

    /* The rows wrap, so each sits under the one before. */
    lv_coord_t y = GUI_TOP + 52;
    for (u32 i = 0; i < DK_STEPS; i++) {
        lv_obj_set_pos(s_step[i], DK_RIGHT_X, y);
        y += lv_obj_get_height(s_step[i]) + 8;
    }
    lv_obj_set_pos(s_verdict, DK_RIGHT_X, y + 8);

    if (failed) {
        s_printf(buf, "#" GUI_HEX_FAIL " " ICON_FAIL "  Stopped while %s:# %s",
                 d->failed_at, d->fail);
        lv_label_set_text(s_verdict, buf);
    } else if (done) {
        if (d->nin_dock && d->hos_reject) {
            s_printf(buf, "#" GUI_HEX_FAIL " " ICON_FAIL "  HOS would not dock with it:# "
                     "power, its own mode and DisplayPort answered, the %s read HOS waits "
                     "for did not.", d->hos_reject);
            lv_label_set_text(s_verdict, buf);
        } else if (d->nin_dock && d->nd_silent)
            lv_label_set_text(s_verdict, "#" GUI_HEX_WARN " " ICON_WARN "  Nintendo's ids, not "
                "all of a Nintendo dock:# power, its own mode and DisplayPort answered, its "
                "light and hub messages did not, as on dock clones.");
        else if (d->nin_dock)
            lv_label_set_text(s_verdict, "#" GUI_HEX_OK " " ICON_OK "  The dock answers all the "
                "way through:# power, its own mode, DisplayPort, light and hub.");
        else if (d->nin_charger)
            lv_label_set_text(s_verdict, "#" GUI_HEX_WARN " " ICON_WARN "  A Nintendo charger, "
                "not a dock:# the port and its controller negotiated with it.");
        else
            lv_label_set_text(s_verdict, "#" GUI_HEX_WARN " " ICON_WARN "  Not a Nintendo dock:#"
                " it negotiated power and DisplayPort as a generic adapter.");
    } else if (s_running) {
        lv_label_set_text(s_verdict, "");
    }
}

static void _dk_tick(void *param)
{
    u32 now = get_tmr_ms();
    if (s_resetting) {
        pd_reset_step(&s_rst);
        if (s_rst.log_new) {
            s_rst.log_new = false;
            gui_uart_puts(s_rst.log);
        }
        if (s_rst.state == PDR_DONE || s_rst.state == PDR_FAILED) {
            s_resetting = false;
            _reset_report();
            s_view_at = 0;          /* a snapshot now */
        } else {
            s_view_at = now;        /* no other reads in between */
            return;
        }
    }
    if (s_running) {
        gui_input_sample();
        dock_step(&s_dock);
        gui_input_sample();
        if (s_dock.log_new) {
            s_dock.log_new = false;
            gui_uart_puts(s_dock.log);
        }
        if (s_dock.state == DOCK_DONE || s_dock.state == DOCK_FAILED) {
            /* Over: the alerts are the view's again, Run is back. */
            bool tv = s_dock.state == DOCK_DONE && s_dock.dp_on && s_dock.hpd;
            s_tv_wait = s_dock.state == DOCK_DONE && s_dock.dp_on && !s_dock.hpd;
            s_running = false;
            s_ran = true;
            gui_hide(s_start, false);
            lv_label_set_text(s_hint, s_dock.state == DOCK_DONE
                ? "Done. Run does it again; leaving turns the dock's light off and exits "
                  "DisplayPort mode, the power contract stays."
                : "Stopped. Run tries again from the top; Reset Chip if the controller "
                  "went quiet.");
            _rows();
            /* Under the verdict, which may have wrapped down to where the
             * bottom-aligned hint sits. */
            lv_obj_align(s_hint, s_verdict, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
            /* DisplayPort up and a TV on the dock: the picture, at once. */
            if (tv)
                _tv_link();
        }
    }
    if (now - s_view_at >= DK_VIEW_MS) {
        s_view_at = now;
        s_prev = s_snap;
        u32 t0 = get_tmr_us();
        gui_input_sample();
        pd_snapshot(&s_snap, !s_running);
        gui_input_sample();
        s_snap_us = get_tmr_us() - t0;
        /* The moment it starts or stops answering, with the uptime: a chip
         * that dozes off shows up here. */
        if (s_snap.ok != s_prev.ok) {
            char line[80];
            s_printf(line, "gui: pd controller %s at %d s since boot\n",
                     s_snap.ok ? "answers" : "silent", now / 1000);
            gui_uart_puts(line);
        }
        if (!s_running)
            s_sticky = s_prev.alert & s_snap.alert;
        /* HPD after the run's wait, DisplayPort mode still on: the TV row
         * turns green and the picture is tried, as at the end of a run. */
        if (s_tv_wait && !s_running && !s_resetting && s_snap.hpd
            && (s_snap.dp_status & PD_DP_EN)) {
            s_tv_wait = false;
            s_dock.hpd = true;
            _rows();
            gui_uart_puts("gui: dock: HPD came up after the run, the picture follows\n");
            _tv_link();
        }
        _view();
        if (s_running)
            _rows();
        if (now - s_uart_at >= 1000 || s_snap.status1 != s_prev.status1
            || s_snap.dp_status != s_prev.dp_status || s_snap.hpd != s_prev.hpd) {
            s_uart_at = now;
            char line[128];
            s_printf(line, "gui: pd alert %04X s1 %04X s2 %04X dp %04X 1c %04X 1d %04X hpd %d%s (read in %d us)\n",
                     s_running ? s_dock.alerts : s_snap.alert, s_snap.status1, s_snap.status2,
                     s_snap.dp_status, s_snap.dp_unk1c, s_snap.dp_unk1d, s_snap.hpd,
                     s_sticky ? " (alert bits held)" : "", s_snap_us);
            gui_uart_puts(line);
        }
    }
}

/* ---- HOS's start-up, on request ---------------------------------------- */

static void _reset_report(void)
{
    const pd_reset_t *r = &s_rst;
    char buf[520], irq[32], a[12], b[12], c[12];
    if (r->state == PDR_FAILED) {
        pd_pad_text(&r->before.reset, a);
        pd_pad_text(&r->before.irq, b);
        pd_pad_text(&r->before.vconn, c);
        s_printf(buf, "#" GUI_HEX_FAIL " " ICON_FAIL "  Reset stopped while %s:# %s. "
                 "The pins before it: PV5 %s, PK4 %s, PK5 %s.",
                 pd_reset_state_name(r->failed_at), r->why, a, b, c);
        lv_label_set_text(s_hint, buf);
        return;
    }
    if (r->irq_ms == (u32)-1)
        s_printf(irq, "never fell");
    else
        s_printf(irq, "fell after %d ms", r->irq_ms);
    s_printf(buf, "#" GUI_HEX_OK " " ICON_OK "  Reset done:# the interrupt line %s, the "
             "device id came back after %d ms%s, SYS_RESET left alert %04X and status "
             "%04X / %04X, DisplayPort alerts %04X, vendor config %04X%s. The view on the "
             "left is what it says now; Run docks as before.",
             irq, r->id_ms, r->id_errors ? " (reads reported errors before that)" : "",
             r->alert, r->status1, r->status2, r->dp_alert_en, r->vendor_cfg,
             r->ocp_was_off ? " (its over-current protection was off)" : "");
    lv_label_set_text(s_hint, buf);
}

static lv_res_t _act_reset(lv_obj_t *b)
{
    if (s_running || s_resetting)
        return LV_RES_OK;
    dpout_video_stop();             /* the reset takes DisplayPort down */
    s_tv_wait = false;
    s_resetting = true;
    pd_reset_start(&s_rst);
    if (s_rst.log_new) {
        s_rst.log_new = false;
        gui_uart_puts(s_rst.log);
    }
    lv_label_set_text(s_hint, "Resetting the controller as HOS does at every boot: a 30 ms "
        "pulse on its reset line (PV5), its interrupt line watched, the device id polled "
        "for up to 10 s, then SYS_RESET and its alerts set.");
    /* A previous run's verdict would sit under the report: cleared. */
    lv_label_set_text(s_verdict, "");
    lv_obj_align(s_hint, NULL, LV_ALIGN_IN_BOTTOM_LEFT, DK_RIGHT_X, -(GUI_H - GUI_BOTTOM));
    return LV_RES_OK;
}

/* ---- the picture on the TV ------------------------------------------------ */

/* The picture's log: every line also kept here and saved to the SD card, so a
 * run on hardware can be read in full without the UART. */
#define TVLOG_SZ (48 * 1024)
static char *s_tvlog;
static u32 s_tvlog_len;

static void _tv_log(const char *text)
{
    gui_uart_puts(text);
    if (!s_tvlog)
        s_tvlog = malloc(TVLOG_SZ);
    if (!s_tvlog)
        return;
    u32 n = strlen(text);
    if (s_tvlog_len + n + 1 > TVLOG_SZ)
        return;
    memcpy(s_tvlog + s_tvlog_len, text, n);
    s_tvlog_len += n;
    s_tvlog[s_tvlog_len] = 0;
}

/* Saved as hwtest/tvlink_<date>.txt; the path, or "", into path. */
static bool _tv_log_save(char *path)
{
    path[0] = 0;
    if (!s_tvlog || !s_tvlog_len || !g_sd_ok || sd_mount())
        return false;
    rtc_time_t t;
    max77620_rtc_get_time_adjusted(&t);
    f_mkdir("hwtest");
    s_printf(path, "hwtest/tvlink_%04d%02d%02d_%02d%02d%02d.txt", t.year, t.month, t.day,
             t.hour, t.min, t.sec);
    if (sd_save_to_file(s_tvlog, s_tvlog_len, path)) {
        path[0] = 0;
        return false;
    }
    return true;
}

/* The picture, after a run that ended with DisplayPort up and a TV on the
 * dock: the sink probed, then the link, trained as L4T does (5.4 Gbps
 * first, lower rates if that fails), and this screen on the TV. */
static void _tv_link(void)
{
    dpout_sink_t k;
    char buf[1100], line[256];
    u32 t0, us;
    int r;

    if (s_running || s_resetting)
        return;
    s_tvlog_len = 0;
    if (s_tvlog)
        s_tvlog[0] = 0;
    t0 = get_tmr_us();
    r = dpout_probe_sink(&k);
    us = get_tmr_us() - t0;
    lv_label_set_text(s_verdict, "");
    if (r) {
        s_printf(buf, "#" GUI_HEX_FAIL " " ICON_FAIL "  No DisplayPort sink on the AUX channel:#"
                 " %s. HPD by DPAUX1 %d. Run first: the picture follows once the DisplayPort "
                 "step is green.", k.why, k.hpd);
        s_printf(line, "gui: tv link: failed (%d) %s, hpd dpaux %d gpio %d, auxstat %08X, %d us\n",
                 r, k.why, k.hpd, s_snap.hpd, k.aux_stat, us);
        _tv_log(line);
    } else {
        static const char *ds[] = { "DP", "VGA", "HDMI", "other" };
        s_printf(buf, "#" GUI_HEX_OK " " ICON_OK "  Sink:# DPCD %d.%d, %s, ",
                 DPCD_REV(&k) >> 4, DPCD_REV(&k) & 0xF,
                 DPCD_DOWNSTREAM(&k) ? ds[DPCD_DOWNSTREAM_TYPE(&k)] : "DP");
        if (k.edid_ok)
            s_printf(buf + strlen(buf), "TV %s.", k.tv_name[0] ? k.tv_name : k.tv_maker);
        else
            strcat(buf, "no EDID.");
        s_printf(line, "gui: tv link: edid %d: %02X %02X %02X %02X %02X %02X %02X %02X, maker %s, name '%s'\n",
                 k.edid_ok, k.edid[0], k.edid[1], k.edid[8], k.edid[9], k.edid[10], k.edid[11],
                 k.edid[18], k.edid[19], k.edid_ok ? k.tv_maker : "-", k.edid_ok ? k.tv_name : "-");
        _tv_log(line);
        s_printf(line, "gui: tv link: dpcd %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                 "%02X %02X %02X %02X, sinks %d, lanes %02X %02X %02X, %d us\n",
                 k.dpcd[0], k.dpcd[1], k.dpcd[2], k.dpcd[3], k.dpcd[4], k.dpcd[5], k.dpcd[6],
                 k.dpcd[7], k.dpcd[8], k.dpcd[9], k.dpcd[10], k.dpcd[11], k.dpcd[12], k.dpcd[13],
                 k.dpcd[14], k.dpcd[15], k.sink_count, k.lane_status[0], k.lane_status[1],
                 k.lane_status[2], us);
        _tv_log(line);
        s_printf(line, "gui: tv link: branch %02X%02X%02X '%s', sink %02X%02X%02X '%s'\n",
                 k.branch_oui[0], k.branch_oui[1], k.branch_oui[2], (const char *)k.branch_id,
                 k.sink_oui[0], k.sink_oui[1], k.sink_oui[2], (const char *)k.sink_id);
        _tv_log(line);

        /* Stages two and three: the link, then this very frame on the TV. */
        dpout_video_t tv;
        dpout_set_logger(_tv_log);
        t0 = get_tmr_us();
        {
            u32 tvfb = gui_tv_fb();
            r = dpout_video_start(&tv, &k, tvfb ? tvfb : NYX_FB2_ADDRESS);
        }
        us = get_tmr_us() - t0;
        if (r) {
            s_printf(buf + strlen(buf), "\n#" GUI_HEX_FAIL " " ICON_FAIL "  No picture (step %d):# "
                     "%s. Lanes %02X %02X %02X. LDO7 %s, LDO8 %s. %d ms.", r, tv.why,
                     tv.lane_status[0], tv.lane_status[1], tv.lane_status[2],
                     tv.ldo7_on ? "on" : "off", tv.ldo8_on ? "on" : "off", us / 1000);
            s_printf(line, "gui: tv: failed at step %d: %s (%d us)\n", r, tv.why, us);
        } else {
            bool frames = tv.dc_frames != 0, under = (tv.dc_int & BIT(8)) != 0;
            /* Converters that never set DPCD 205 bit 0, even while they show
             * the picture: the STDP2550 (branch OUI 0080E1, the V1 / V2
             * Nintendo docks; the Erista on a known-good dock and a working
             * Mariko on another, 2026-11-11) and the Synaptics one in a
             * generic dock (90CC24 'SYNAR', VIA Labs 2109:0103; four runs,
             * the picture on the monitor, 2026-11-13). Their sync bit
             * says nothing, so it does not colour the line. The Realtek
             * converters and the OLED dock's do set it. */
            static const u8 no_sync_ouis[][3] = {
                { 0x00, 0x80, 0xE1 },
                { 0x90, 0xCC, 0x24 },
            };
            bool no_sync_bit = false;
            for (u32 i = 0; i < ARRAY_SIZE(no_sync_ouis); i++)
                if (!memcmp(k.branch_oui, no_sync_ouis[i], 3))
                    no_sync_bit = true;
            bool link_ok = (tv.in_sync || no_sync_bit) && !tv.margin_errors;
            s_printf(buf + strlen(buf), "\n#%s %s  Link:# %dx %s Gbps%s, swing %d, %d errors%s.",
                     link_ok ? GUI_HEX_OK : GUI_HEX_FAIL, link_ok ? ICON_OK : ICON_FAIL,
                     tv.lanes, tv.rate == 0x14 ? "5.4" : tv.rate == 0x0A ? "2.7" : "1.62",
                     tv.assr ? ", ASSR" : "", tv.vs[0], tv.margin_errors,
                     tv.in_sync ? "" : no_sync_bit ? ", no sync bit" : ", NOT in sync");
            /* 74.25 MHz wanted; within 1 percent is right. */
            bool pclk_ok = tv.pclk_khz > 73500 && tv.pclk_khz < 75000;
            s_printf(buf + strlen(buf), "\n#%s %s  Head:# %d frames/100 ms, %d.%02d MHz%s, SOR %s.",
                     frames && !under && pclk_ok ? GUI_HEX_OK : GUI_HEX_FAIL,
                     frames && !under && pclk_ok ? ICON_OK : ICON_FAIL, tv.dc_frames,
                     tv.pclk_khz / 1000, (tv.pclk_khz % 1000) / 10,
                     under ? ", UNDERFLOW" : pclk_ok ? "" : ", NOT 74.25",
                     tv.sor_crc_valid ? "sees it" : "sees nothing");
            /* The converter's test CRC and the after-attach error counters
             * go to the log only: the test CRC counted 0 frames while the
             * Nintendo dock showed the picture. */
            s_printf(line, "gui: tv: picture on, %d us\n", us);
        }
        _tv_log(line);
    }
    {
        char path[64];
        if (_tv_log_save(path))
            s_printf(buf + strlen(buf), "\nLog: %s.", path);
        else
            strcat(buf, "\nLog not saved: no SD card.");
    }
    lv_label_set_text(s_hint, buf);
    lv_obj_align(s_hint, NULL, LV_ALIGN_IN_BOTTOM_LEFT, DK_RIGHT_X, -(GUI_H - GUI_BOTTOM));
    return;
}

static lv_res_t _act_start(lv_obj_t *b)
{
    if (s_running)
        return LV_RES_OK;
    dpout_video_stop();             /* the handshake sets DisplayPort up anew */
    s_tv_wait = false;
    s_running = true;
    s_ran = false;
    dock_start(&s_dock);
    gui_hide(s_start, true);
    lv_label_set_text(s_hint, "Running. The light on the dock comes on near the end; on "
        "the way out it goes off again and DisplayPort mode is left. The power "
        "contract stays, as it does when the console is docked.");
    lv_obj_align(s_hint, NULL, LV_ALIGN_IN_BOTTOM_LEFT, DK_RIGHT_X, -(GUI_H - GUI_BOTTOM));
    _rows();
    return LV_RES_OK;
}

static void _dk_leave(void)
{
    if (s_dk_task)
        lv_task_del(s_dk_task);
    s_dk_task = NULL;
    s_tv_wait = false;
    dpout_video_stop();             /* before the dock's DisplayPort mode goes */
    if (s_running || s_ran) {
        dock_stop(&s_dock);
        s_running = false;
        s_ran = false;
    }
    if (s_resetting) {
        /* The pulse is 30 ms; past it the line rests low, as HOS leaves it. */
        if (s_rst.state == PDR_PULSE)
            gpio_write(GPIO_PORT_V, GPIO_PIN_5, GPIO_LOW);
        s_resetting = false;
    }
    gui_clock_fast(true);
}

void gui_dock_test(void)
{
    lv_obj_t *win = gui_win_new(ICON_TV " Dock Test");
    gui_on_leave(_dk_leave);
    /* The payload's clocks, as the health check runs its probes: the PD
     * controller answers them there, and did not at the GUI's boost. */
    gui_clock_fast(false);
    s_running = false;
    s_ran = false;
    s_sticky = 0;
    memset(&s_dock, 0, sizeof(s_dock));
    memset(&s_prev, 0, sizeof(s_prev));
    s_start = gui_win_btn(win, ICON_PLAY " Run", _act_start);
    s_reset = gui_win_btn(win, ICON_REFRESH " Reset Chip", _act_reset);
    s_resetting = false;

    lv_obj_t *t = gui_label(win, &gui_font_30, GUI_COL_TEXT, "The USB-C port's controller");
    lv_obj_set_pos(t, GUI_MARGIN, GUI_TOP);
    s_regs = gui_label(win, &gui_font_20, GUI_COL_HINT, "");
    lv_obj_set_style(s_regs, &gui_st_mono);
    lv_obj_set_pos(s_regs, GUI_MARGIN, GUI_TOP + 52);

    lv_obj_t *t2 = gui_label(win, &gui_font_30, GUI_COL_TEXT, "Docking, step by step");
    lv_obj_set_pos(t2, DK_RIGHT_X, GUI_TOP);
    for (u32 i = 0; i < DK_STEPS; i++) {
        s_step[i] = gui_label_wrap(win, &gui_font_20, GUI_COL_TEXT, DK_RIGHT_W, "");
        lv_obj_set_pos(s_step[i], DK_RIGHT_X, GUI_TOP + 52 + i * 34);
        _row(i, MARK_NONE, "not yet");
    }
    s_verdict = gui_label_wrap(win, &gui_font_20, GUI_COL_TEXT, DK_RIGHT_W, "");
    lv_obj_set_pos(s_verdict, DK_RIGHT_X, GUI_TOP + 52 + DK_STEPS * 34 + 16);
    s_hint = gui_hint(win, DK_RIGHT_W,
        "Dock the console or plug a USB-C PD charger, then Run: what the console "
        "does when it docks, and with a TV on the dock, this screen on it. Reset "
        "Chip: HOS's boot-time reset of the controller, for one that does not "
        "answer. Nothing of it runs from the health check.");
    lv_obj_align(s_hint, NULL, LV_ALIGN_IN_BOTTOM_LEFT, DK_RIGHT_X, -(GUI_H - GUI_BOTTOM));

    pd_snapshot(&s_snap, true);
    s_view_at = get_tmr_ms();
    s_uart_at = 0;
    _view();
    if (s_dk_task)
        lv_task_del(s_dk_task);
    s_dk_task = lv_task_create(_dk_tick, DK_TICK_MS, LV_TASK_PRIO_MID, NULL);
}
