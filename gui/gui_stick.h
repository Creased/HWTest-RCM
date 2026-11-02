/*
 * hwtest GUI - the stick model behind the Joy-Con test.
 *
 * Plain arithmetic on the raw 12-bit readings, with no LVGL in it, so the
 * host can check it (the emulator has no Joy-Con).
 *
 * A Joy-Con's stick is not centred on 0x800: each unit's resting point is
 * its own, up to a tenth of the travel away, and how far it reads from
 * there to the edge is the unit's own too - the factory calibration that
 * says what "full" is lives only in the Joy-Con's flash, and known-good
 * units read about 0x480 here, short of the figures published for that
 * calibration. So the test learns both: the resting point is wherever the
 * stick stays put for a second, the travel is the furthest it is pushed
 * each way. Once it has been pushed well out in all four directions, each
 * direction is judged against the stick's own best one (wear is lopsided:
 * a tired stick comes up short one way, not evenly all round), the middle
 * of that travel is where a calibrated stick would rest, and how far the
 * resting point is from it is the drift an owner feels.
 */
#ifndef HWTEST_GUI_STICK_H
#define HWTEST_GUI_STICK_H

#include <utils/types.h>

#define STICK_SPAN   0x480   /* a good stick's travel, rest to edge: the dot's scale */
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
} gui_stick_t;

enum {
    STICK_RESTED = 1,   /* it has just settled: the resting point is new  */
    STICK_GREW   = 2,   /* it reached further than before                 */
};

void gui_stick_reset(gui_stick_t *s);
/* One reading; now in ms. Returns STICK_* flags for what changed. */
u32  gui_stick_feed(gui_stick_t *s, u16 x, u16 y, u32 now);
/* Where to draw the dot for a reading, in pixels from the ring's centre,
 * for a ring of radius half. */
void gui_stick_dot(const gui_stick_t *s, u16 x, u16 y, s32 half, s32 *px, s32 *py);
/* How far it has gone each way, in STICK_LEFT.. order, raw (reach) and as
 * a share of the furthest direction (pct, the best one reads 100). Returns
 * how many of the four are covered; 4 means the travel is known. */
enum { STICK_LEFT, STICK_RIGHT, STICK_UP, STICK_DOWN };
u32  gui_stick_range(const gui_stick_t *s, u32 reach[4], u32 pct[4]);
/* The resting point's distance from the middle of the travel, as a share
 * of the travel (half the full extent, the larger axis). Meaningful once
 * gui_stick_range() is 4. */
u32  gui_stick_drift(const gui_stick_t *s);

#endif
