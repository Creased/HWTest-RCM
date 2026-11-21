/*
 * The Wi-Fi scan's BPMP <-> CPU0 mailbox and memory map.
 *
 * Included by the GUI (ARM7 BPMP, gui/gui_wifi.c) and by the CPU0 driver
 * (AArch64, cpu_wifi/wifi.c), like cpu_mbox.h: fixed-width fields only, no
 * pointers, identical layout on both sides.
 *
 * WHY A SECOND CPU0 IMAGE
 *
 * The health check's stub (cpu_stub/) proves the CYW4356 enumerates and
 * stops. A scan needs the rest of a driver: BARs, the chip's backplane, its
 * ARM core, a firmware download and the msgbuf rings, all of it through the
 * PCIe apertures only a CPU-complex master can reach (see cpu_mbox.h). That
 * is far past the stub's 32 KiB window and has no place in the RCM payload,
 * so it is its own image, built into the GUI stage only, and it reuses the
 * stub's bring-up by compiling stub.c in (cpu_wifi/wifi.c).
 *
 * MEMORY
 *
 * Everything sits in DRAM above the stub's window, which this image leaves
 * alone apart from the stub's own mailbox at CPU_MBOX_ADDR:
 *
 *   0xA0030000  cpu_mbox_t           the stub's (bring-up, exceptions)
 *   0xA0100000  this image           <= 192 KiB, linked here
 *   0xA0130000  L1 page table        4 KiB
 *   0xA0131000  L2 table, 2-3 GiB    4 KiB: the one block below is uncached
 *   0xA0170000  stack top            grows down to 0xA0140000
 *   0xA0200000  firmware             staged by the BPMP, <= 1 MiB
 *   0xA0300000  NVRAM                staged by the BPMP, already in the
 *                                    chip's binary form, <= 64 KiB
 *   0xA0400000  DMA block            2 MiB, Normal non-cacheable for CPU0:
 *                                    the rings and buffers the chip DMAs
 *                                    into, and this mailbox at its end
 *
 * The DMA block is uncached on CPU0's side so neither direction needs cache
 * maintenance; the BPMP still cleans/invalidates its own cache around the
 * mailbox, as for cpu_mbox_t.
 */

#ifndef WIFI_MBOX_H
#define WIFI_MBOX_H

#define WIFI_IMG_ADDR      0xA0100000u
#define WIFI_IMG_MAX       0x00030000u   /* 192 KiB                         */
#define WIFI_L1_ADDR       0xA0130000u
#define WIFI_L2_ADDR       0xA0131000u
#define WIFI_STACK_TOP     0xA0170000u
#define WIFI_FW_ADDR       0xA0200000u
#define WIFI_FW_MAX        0x00100000u
#define WIFI_NVRAM_ADDR    0xA0300000u
#define WIFI_NVRAM_MAX     0x00010000u
#define WIFI_DMA_ADDR      0xA0400000u
#define WIFI_DMA_SIZE      0x00200000u
/* L2 index of the uncached block: (0xA0400000 - 0x80000000) / 2 MiB. */
#define WIFI_DMA_L2_IDX    258u
#define WIFI_MBOX_ADDR     0xA05F0000u   /* last 64 KiB of the DMA block    */

#define WIFI_MBOX_MAGIC    0x57494649u   /* 'WIFI', CPU0 is running         */
#define WIFI_MBOX_DONE     0x444F4E45u   /* 'DONE', it stopped: see result  */

/* Steps, in order. The BPMP shows the last one entered; a stall names it. */
#define WIFI_ST_START        1u
#define WIFI_ST_ENUM         2u   /* the stub's bring-up finished          */
#define WIFI_ST_BARS         3u   /* BAR0/BAR2 sized and assigned          */
#define WIFI_ST_CHIPID       4u   /* ChipCommon read through BAR0          */
#define WIFI_ST_EROM         5u   /* backplane cores enumerated            */
#define WIFI_ST_HALT         6u   /* ARM CR4 halted, D11 cores disabled    */
#define WIFI_ST_RESET        7u   /* chip watchdog reset                   */
#define WIFI_ST_RAMINFO      8u
#define WIFI_ST_DOWNLOAD     9u   /* firmware and NVRAM into TCM           */
#define WIFI_ST_BOOT        10u   /* CR4 released, waiting for shared RAM  */
#define WIFI_ST_SHARED      11u
#define WIFI_ST_RINGS       12u
#define WIFI_ST_IOCTL       13u   /* first ioctls: version, MAC            */
#define WIFI_ST_UP          14u   /* event mask, mpc, UP                   */
#define WIFI_ST_SCAN        15u   /* escan running                         */
#define WIFI_ST_DONE        16u

/* Results. 0 is success; anything else ends the run at `step`. */
#define WIFI_OK              0u
#define WIFI_E_NOLINK        1u   /* the stub did not enumerate 14E4:43EC  */
#define WIFI_E_BAR           2u   /* a BAR did not size or take its base   */
#define WIFI_E_WINDOW        3u   /* the backplane window write did not stick */
#define WIFI_E_CHIPID        4u   /* ChipCommon read back as garbage       */
#define WIFI_E_EROM          5u   /* no CR4, PCIE2 or ChipCommon in EROM   */
#define WIFI_E_RAM           6u   /* RAM size not plausible                */
#define WIFI_E_INPUT         7u   /* no firmware or NVRAM handed over      */
#define WIFI_E_VERIFY        8u   /* TCM did not read back what was written*/
#define WIFI_E_FWUP          9u   /* firmware never wrote its shared RAM   */
#define WIFI_E_SHARED       10u   /* shared RAM address or version bad     */
#define WIFI_E_RINGS        11u   /* ring info not plausible               */
#define WIFI_E_IOCTL        12u   /* an ioctl got no completion in time    */
#define WIFI_E_FWERR        13u   /* the firmware refused an ioctl         */
#define WIFI_E_SCAN         14u   /* escan ended with an error status      */
#define WIFI_E_SCANTIME     15u   /* no final escan event in time          */

#define WIFI_BSS_MAX        64u
#define WIFI_CONSOLE_MAX  3072u   /* tail of the firmware's own log        */
#define WIFI_CORES_MAX      24u

#ifndef __ASSEMBLER__

typedef struct {
    unsigned char  bssid[6];
    unsigned char  ssid_len;
    unsigned char  channel;
    char           ssid[33];
    unsigned char  flags;          /* bit0 privacy, bit1 5 GHz            */
    short          rssi;           /* dBm                                 */
    unsigned short chanspec;
    unsigned short seen;           /* partial results that named it      */
} wifi_bss_t;                      /* 48 bytes                            */

typedef struct {
    unsigned short id;
    unsigned char  rev;
    unsigned char  pad;
    unsigned int   base;
    unsigned int   wrap;
} wifi_core_t;

typedef struct {
    /* ---- written by CPU0 ---- */
    volatile unsigned int magic;
    volatile unsigned int step;        /* WIFI_ST_*                        */
    volatile unsigned int heartbeat;
    volatile unsigned int done;        /* WIFI_MBOX_DONE at the end        */
    volatile unsigned int result;      /* WIFI_OK or WIFI_E_*              */
    volatile unsigned int detail;      /* a number that goes with it       */
    volatile int          fwerr;       /* the firmware's own error code    */
    char                  why[96];     /* one line, for the screen         */

    volatile unsigned int bar0, bar2, bar2_size;
    volatile unsigned int chipid;      /* ChipCommon register 0            */
    volatile unsigned int ncores;
    wifi_core_t           cores[WIFI_CORES_MAX];
    volatile unsigned int pcie2_rev;
    volatile unsigned int rambase, ramsize;
    volatile unsigned int resetintr;
    volatile unsigned int shared_addr;
    volatile unsigned int shared_flags;
    volatile unsigned int fw_up_ms;    /* release to shared RAM            */
    volatile unsigned int verify_bad;  /* TCM words that read back wrong   */
    char                  fw_ver[128]; /* "ver" iovar                      */
    unsigned char         mac[8];
    volatile unsigned int scan_ms;
    volatile unsigned int events;      /* escan events seen                */
    volatile unsigned int nbss;
    wifi_bss_t            bss[WIFI_BSS_MAX];
    volatile unsigned int console_len;
    char                  console[WIFI_CONSOLE_MAX];

    /* ---- written by the BPMP before CPU0 is released ---- */
    volatile unsigned int in_magic;    /* WIFI_MBOX_MAGIC: the inputs below */
    volatile unsigned int fw_len;      /* at WIFI_FW_ADDR                   */
    volatile unsigned int nvram_len;   /* at WIFI_NVRAM_ADDR, binary form   */
    volatile unsigned int scan_time_ms;/* how long to wait for the scan     */
} wifi_mbox_t;

/* The same bytes on both sides: checked with both compilers when this was
 * written, and held there so a field added on one side only fails the
 * build rather than reading the other side's data. */
_Static_assert(sizeof(wifi_bss_t) == 48, "wifi_bss_t layout");
_Static_assert(sizeof(wifi_mbox_t) == 6776, "wifi_mbox_t layout");
_Static_assert(__builtin_offsetof(wifi_mbox_t, in_magic) == 6760,
               "wifi_mbox_t layout");
_Static_assert(sizeof(wifi_mbox_t) <= 0x10000, "wifi_mbox_t past its 64 KiB");

#endif /* __ASSEMBLER__ */
#endif /* WIFI_MBOX_H */
