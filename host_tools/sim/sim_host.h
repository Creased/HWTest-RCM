/*
 * Force-included into every host-sim translation unit (host_tools/sim/build.sh).
 *
 * The console's addresses are kept, not replaced: sim.c maps the ones the
 * GUI draws into (LVGL's pool and the panel's framebuffer) at the very
 * addresses memory_map.h gives them, so the screen code's arithmetic over
 * them is the real thing. What cannot be mapped is the Wi-Fi mailbox, which
 * on the console CPU0 writes and the BPMP reads: the build points
 * WIFI_MBOX_ADDR at memory of this process instead, and that is what lets
 * the real _show_results(), _show_info() and _finish() draw a scan or a
 * join without a chip.
 */
#ifndef HOSTSIM_HOST_H
#define HOSTSIM_HOST_H

extern unsigned char sim_mbox_base[];   /* the Wi-Fi mailbox (sim_stubs.c) */
extern unsigned int  sim_us;            /* the clock, moved on by hand     */
extern const char   *sim_ini;           /* what hwtest/hwtest.ini holds    */
extern unsigned int  sim_chip_stops;    /* wifi_chip_stop() calls so far   */

/* The host's side (sim_io.c), which shares no headers with this one: bdk
 * declares malloc() and usleep() with types the host's own headers
 * contradict, so the two worlds meet over these three calls. */
void sim_map(unsigned long addr, unsigned long len, const char *what);
void sim_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sim_png_write(const char *dir, const char *name, const char *suffix,
                   const unsigned int *fb, unsigned w, unsigned h);

#endif
