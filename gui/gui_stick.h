/*
 * hwtest GUI - the stick model behind the Joy-Con test.
 *
 * Plain arithmetic on the raw 12-bit readings, with no LVGL in it, so the
 * host can check it (the emulator has no Joy-Con).
 *
 * A Joy-Con's stick is not centred on 0x800: each unit's resting point is
 * its own, up to a tenth of the travel away, and how far it reads from
 * there to the edge is the unit's own too. What "full" means is the
 * calibration in the Joy-Con's flash (gui_jcwire.h): a centre, and the
 * travel from it each way. With that known, every direction is judged
 * against its calibrated travel and the resting point against the
 * calibrated centre, which is what the console's firmware does and so what
 * an owner feels in a game. Without it (a third-party pad, a Lite's
 * controls, or a read that failed) the test learns both: the resting
 * point is wherever the stick stays put for a second, the travel is the
 * furthest it is pushed each way, each direction is judged against the
 * stick's own best one (wear is lopsided: a tired stick comes up short one
 * way, not evenly all round), and drift is how far the resting point sits
 * from the middle of that travel.
 */
#ifndef HWTEST_GUI_STICK_H
#define HWTEST_GUI_STICK_H

#include <utils/types.h>

#define STICK_SPAN   0x480   /* a good stick's travel, rest to edge: the dot's
                                scale when no calibration is known          */
#define STICK_COVER  0x280   /* pushed this far: that direction is covered (a
                                stick drifting a third of the way out still
                                gets there on its short side)             */
#define STICK_SHORT  0x400   /* no direction reaches this: short all round */
#define STICK_STILL  40      /* no further than this for a second: at rest */

typedef struct {
    bool started;
    bool rest_known;
    u16 rest_x, rest_y;              /* where it sits when let go          */
    u16 min_x, max_x, min_y, max_y;  /* the furthest it has been pushed    */
    u16 still_x, still_y;            /* where it last stopped moving...    */
    u32 still_since;                 /* ...and since when (0: moving)      */
    bool cal;                        /* the calibration below is known     */
    u16 cal_cx, cal_cy;              /* the firmware's centre              */
    u16 cal_l, cal_r, cal_u, cal_d;  /* full travel from it, each way      */
} gui_stick_t;

enum {
    STICK_RESTED = 1,   /* it has just settled: the resting point is new  */
    STICK_GREW   = 2,   /* it reached further than before                 */
};

void gui_stick_reset(gui_stick_t *s);
/* The calibration from the Joy-Con's flash, once read. */
void gui_stick_set_cal(gui_stick_t *s, u16 cx, u16 cy, u16 l, u16 r, u16 u, u16 d);
/* One reading; now in ms. Returns STICK_* flags for what changed. */
u32  gui_stick_feed(gui_stick_t *s, u16 x, u16 y, u32 now);
/* Where to draw the dot for a reading, in pixels from the ring's centre,
 * for a ring of radius half. The ring is the calibrated travel when known,
 * so a drifting stick's dot rests off centre, as the firmware sees it. */
void gui_stick_dot(const gui_stick_t *s, u16 x, u16 y, s32 half, s32 *px, s32 *py);
/* How far it has gone each way, in STICK_LEFT.. order, raw (reach) and as
 * a share (pct) of the calibrated travel, or of the furthest direction
 * when none is known (the best one then reads 100). Returns how many of
 * the four are covered; 4 means the travel is known. */
enum { STICK_LEFT, STICK_RIGHT, STICK_UP, STICK_DOWN };
u32  gui_stick_range(const gui_stick_t *s, u32 reach[4], u32 pct[4]);
/* The resting point's distance from the calibrated centre, as a share of
 * the calibrated travel; without a calibration, from the middle of the
 * travel seen, as a share of half its extent. Meaningful once
 * gui_stick_range() is 4, or at once with a calibration. */
u32  gui_stick_drift(const gui_stick_t *s);

#endif
