/*
 * hwtest GUI - the stick model behind the Joy-Con test (see gui_stick.h).
 */
#include "gui_stick.h"
#include <string.h>

static u32 _isqrt(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

void gui_stick_reset(gui_stick_t *s)
{
    memset(s, 0, sizeof(*s));
}

void gui_stick_set_cal(gui_stick_t *s, u16 cx, u16 cy, u16 l, u16 r, u16 u, u16 d)
{
    s->cal = true;
    s->cal_cx = cx;
    s->cal_cy = cy;
    s->cal_l = l;
    s->cal_r = r;
    s->cal_u = u;
    s->cal_d = d;
}

u32 gui_stick_feed(gui_stick_t *s, u16 x, u16 y, u32 now)
{
    u32 ev = 0;

    /* The first reading stands in for the resting point until it has
     * really rested, so the dot can be drawn from the start. */
    if (!s->started) {
        s->started = true;
        s->rest_x = s->min_x = s->max_x = s->still_x = x;
        s->rest_y = s->min_y = s->max_y = s->still_y = y;
        s->still_since = now;
        return 0;
    }

    if (x < s->min_x) { s->min_x = x; ev |= STICK_GREW; }
    if (x > s->max_x) { s->max_x = x; ev |= STICK_GREW; }
    if (y < s->min_y) { s->min_y = y; ev |= STICK_GREW; }
    if (y > s->max_y) { s->max_y = y; ev |= STICK_GREW; }

    s32 mx = (s32)x - s->still_x, my = (s32)y - s->still_y;
    if (mx * mx + my * my > STICK_STILL * STICK_STILL) {
        s->still_x = x;
        s->still_y = y;
        s->still_since = now;
    } else if (s->still_since && now - s->still_since > 1000) {
        s->still_since = 0;
        /* Held against the edge is not resting: a resting point is well
         * inside the travel. */
        if (x > 0x400 && x < 0xC00 && y > 0x400 && y < 0xC00) {
            s->rest_x = x;
            s->rest_y = y;
            s->rest_known = true;
            ev |= STICK_RESTED;
        }
    }
    return ev;
}

void gui_stick_dot(const gui_stick_t *s, u16 x, u16 y, s32 half, s32 *px, s32 *py)
{
    s32 dx, dy, ox, oy;
    if (s->cal) {
        dx = (s32)x - s->cal_cx;
        dy = (s32)y - s->cal_cy;
        ox = dx * half / (dx >= 0 ? s->cal_r : s->cal_l);
        oy = -dy * half / (dy >= 0 ? s->cal_u : s->cal_d);
    } else {
        dx = (s32)x - s->rest_x;
        dy = (s32)y - s->rest_y;
        ox = dx * half / STICK_SPAN;
        oy = -dy * half / STICK_SPAN;
    }
    if (ox > half) ox = half;
    if (ox < -half) ox = -half;
    if (oy > half) oy = half;
    if (oy < -half) oy = -half;
    *px = ox;
    *py = oy;
}

/* How far it has been pushed each way: from the calibrated centre when
 * known, else from where it rests. Returns what the shares are measured
 * against without a calibration - the furthest of the four, never less
 * than STICK_COVER so an untouched stick does not read as full. */
static u32 _reach(const gui_stick_t *s, u32 reach[4])
{
    s32 cx = s->cal ? s->cal_cx : s->rest_x;
    s32 cy = s->cal ? s->cal_cy : s->rest_y;
    s32 r[4] = {
        cx - s->min_x,
        (s32)s->max_x - cx,
        (s32)s->max_y - cy,
        cy - s->min_y,
    };
    u32 span = STICK_COVER;
    for (u32 i = 0; i < 4; i++) {
        reach[i] = r[i] < 0 ? 0 : (u32)r[i];
        if (reach[i] > span)
            span = reach[i];
    }
    return span;
}

u32 gui_stick_range(const gui_stick_t *s, u32 reach[4], u32 pct[4])
{
    u32 span = _reach(s, reach), covered = 0;
    const u16 cal[4] = { s->cal_l, s->cal_r, s->cal_u, s->cal_d };
    for (u32 i = 0; i < 4; i++) {
        u32 p = reach[i] * 100 / (s->cal ? cal[i] : span);
        pct[i] = p > 100 ? 100 : p;
        covered += reach[i] >= STICK_COVER;
    }
    return covered;
}

u32 gui_stick_drift(const gui_stick_t *s)
{
    s32 dx, dy;
    u32 against;
    if (s->cal) {
        dx = (s32)s->rest_x - s->cal_cx;
        dy = (s32)s->rest_y - s->cal_cy;
        against = ((u32)s->cal_l + s->cal_r + s->cal_u + s->cal_d) / 4;
    } else {
        /* Against half the full extent, so a resting point that is off to
         * one side does not stretch its own yardstick. */
        u32 half_x = ((u32)s->max_x - s->min_x) / 2;
        u32 half_y = ((u32)s->max_y - s->min_y) / 2;
        against = half_x > half_y ? half_x : half_y;
        if (against < STICK_COVER)
            against = STICK_COVER;
        dx = (s32)s->rest_x - ((s32)s->min_x + s->max_x) / 2;
        dy = (s32)s->rest_y - ((s32)s->min_y + s->max_y) / 2;
    }
    u32 p = _isqrt((u32)(dx * dx + dy * dy)) * 100 / against;
    return p > 100 ? 100 : p;
}
