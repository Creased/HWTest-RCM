/*
 * dpout.h - the SoC's DisplayPort output to the dock: the AUX channel
 * (DPAUX1) and the sink's DPCD, then SOR1 and the second display head.
 *
 * The PD controller has already routed SOR1's lanes to the port and the
 * dock has raised HPD (the Dock Test's handshake). What follows is what the
 * display driver does next, on the SoC side: the AUX channel to the dock's
 * DisplayPort sink, then link training, then video. coreboot's Tegra210
 * driver and L4T's are the references; the Switch uses SOR1, DPAUX1 and
 * the second display controller (DC1), two lanes, as its device tree says.
 */
#ifndef HWTEST_DPOUT_H
#define HWTEST_DPOUT_H

#include <utils/types.h>

typedef struct {
    bool hpd;                 /* as DPAUX1 sees the hot plug detect line  */
    bool answered;            /* the sink answered an AUX read            */
    u32  aux_stat;            /* the last AUXSTAT, for the log            */
    u8   dpcd[16];            /* DPCD 000..00F                            */
    u8   sink_count;          /* DPCD 200                                 */
    u8   lane_status[3];      /* DPCD 202..204, as the last link left it  */
    u8   sink_oui[3], sink_id[7];      /* DPCD 400..402, 403..408, text   */
    u8   branch_oui[3], branch_id[7];  /* DPCD 500..502, 503..508, text   */
    bool edid_ok;             /* the TV's EDID came through the converter */
    u8   edid[128];
    char tv_maker[4], tv_name[14];
    char why[96];
} dpout_sink_t;

/* The DPCD, decoded */
#define DPCD_REV(s)             ((s)->dpcd[0])
#define DPCD_MAX_RATE(s)        ((s)->dpcd[1])   /* 06 1.62, 0A 2.7, 14 5.4 Gbps */
#define DPCD_MAX_LANES(s)       ((s)->dpcd[2] & 0x1F)
#define DPCD_ENHANCED(s)        (((s)->dpcd[2] & 0x80) != 0)
#define DPCD_TPS3(s)            (((s)->dpcd[2] & 0x40) != 0)
#define DPCD_DOWNSTREAM(s)      (((s)->dpcd[5] & 0x01) != 0)
#define DPCD_DOWNSTREAM_TYPE(s) (((s)->dpcd[5] >> 1) & 3) /* 0 DP, 1 VGA, 2 DVI/HDMI, 3 other */

/* The AUX channel up: the SOR partition, DPAUX1's clock and pads. 0 ok. */
int  dpout_aux_init(void);
bool dpout_aux_ready(void);
bool dpout_hpd(void);                 /* DPAUX1's view of HPD, once up    */
int  dpout_dpcd_read(u32 addr, u8 *buf, u32 n);          /* 0 ok        */
int  dpout_dpcd_write(u32 addr, const u8 *buf, u32 n);   /* 0 ok        */
const char *dpout_aux_error(int r);

/* Stage one of the picture: what is on the other end of the AUX channel.
 * 0 ok, with the sink filled in; otherwise why. */
int  dpout_probe_sink(dpout_sink_t *s);
int  dpout_edid_read(u8 *edid, u32 size);                /* 0 ok        */
const char *dpout_rate_name(u8 rate);

/* Stages two and three: SOR1 up on PLLDP, the link trained on two lanes
 * against the sink's own status registers, then the second display head
 * (DC1) sending the frame at fb to the TV, 1280x720 at 60 Hz, the GUI's
 * own size. The picture stays until dpout_video_stop(). */
typedef struct {
    bool in_sync;             /* the sink says its port 0 is in sync       */
    bool assr;                /* alternate scrambler reset in use          */
    u32  margin_errors;       /* symbol errors in 20 ms of idle, last train */
    u32  margin_retries;      /* trainings again from a higher swing        */
    bool ldo7_on, ldo8_on;    /* the pads' regulators, after the start     */
    u8   lanes, rate;         /* as trained                                */
    u8   vs[4], pe[4];        /* the drive levels the sink settled on      */
    u8   lane_status[3];      /* DPCD 202..204 after training              */
    u32  cr_tries;            /* clock recovery rounds, last training      */
    /* How far the picture gets, read after the attach. */
    u32  dc_int, dc_crc;      /* the head: events over 3 frames, its CRC    */
    u32  dc_frames;           /* vsyncs counted in 100 ms (host1x syncpt)   */
    u32  sor_crc;             /* the SOR: CRC of the raster it receives      */
    bool sor_crc_valid;
    u8   dpcd_200[6];         /* DPCD 200..205 after the attach              */
    u16  sym_err[2];          /* DPCD 210..213, lanes 0 and 1                */
    u32  pclk_khz;            /* the pixel clock the SOR measures (MVID)    */
    bool sink_crc_test;       /* the sink has the DPCD test CRC             */
    u8   sink_crc_frames;     /* frames it counted in 100 ms                 */
    u16  sink_crc[3];
    char why[96];
    char log[200];            /* the last breadcrumb, for the UART         */
} dpout_video_t;

int  dpout_video_start(dpout_video_t *v, const dpout_sink_t *s, u32 fb);
void dpout_video_stop(void);
bool dpout_video_on(void);
void dpout_set_logger(void (*fn)(const char *));   /* the breadcrumbs */
/* Bench tests of the lanes, from hwtest.ini (dp_pol, dp_swap): both lanes
 * driven with inverted polarity, and / or lanes 0 and 1 exchanged on the
 * pads. For a console whose DisplayPort pairs are suspected swapped: a
 * Nintendo dock's STDP2550 needs the right polarity, a Realtek converter
 * corrects it on its own. Set before the Dock Test runs. */
void dpout_lane_test(bool invert, bool swap);

#endif
