/*
 * The format parsing behind gui_wlanfw.c, kept free of bdk so the host can
 * test it (host_tools/test_wlanfw.c): FAT32 directory entries with long
 * names, the NCA3 header fields, PFS0, the NSO header, where the firmware
 * and NVRAM sit in the wlan sysmodule's .data, and the NVRAM's binary form.
 * No I/O and no crypto here; gui_wlanfw.c does both.
 */
#ifndef WLANFW_PARSE_H
#define WLANFW_PARSE_H

#ifdef WLANFW_HOST
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
#else
#include <utils/types.h>
#endif

/* ---- FAT32 directories ---- */

#define FAT_ATTR_DIR   0x10u
#define FAT_ATTR_VOL   0x08u
#define FAT_ATTR_LFN   0x0Fu

typedef struct {
    char name[256];     /* the long name when it checks out, else 8.3 */
    u8   attr;
    u32  clus;
    u32  size;
} fat_dirent_t;

typedef struct {
    u16 lfn[260];
    u32 lfn_len;
    u8  lfn_sum;
    int lfn_ok;
} fat_lfn_t;

/* Walk `len` bytes of directory entries. `cb` gets every live file and
 * directory except "." and ".."; returning non-zero stops the walk.
 * Returns 1 when the end-of-directory entry was met, 2 when cb stopped it,
 * 0 when the buffer ran out first. `st` carries a long name across calls. */
int fat_dir_walk(const u8 *buf, u32 len, fat_lfn_t *st,
                 int (*cb)(const fat_dirent_t *e, void *ctx), void *ctx);

int str_ieq(const char *a, const char *b);
int str_iends(const char *s, const char *suffix);

/* ---- NCA3 header, already decrypted (0xC00 bytes) ---- */

typedef struct {
    u8   content_type;   /* 0 = Program                                 */
    u8   kak_index;      /* 0 application, 1 ocean, 2 system            */
    u8   key_gen;        /* max(old, new)                               */
    u64  title_id;
    u32  sdk_ver;
    int  has_rights_id;
    u64  sec0_start;     /* bytes                                      */
    u64  sec0_end;
    u8   sec0_fs_type;   /* 1 = PartitionFs                             */
    u8   sec0_hash_type; /* 2 = HierarchicalSha256                      */
    u8   sec0_enc_type;  /* 3 = AesCtr                                   */
    u64  pfs0_off;       /* within section 0                            */
    u64  pfs0_size;
    u8   ctr_hi[8];      /* the counter's upper half, big-endian        */
    const u8 *key_area;  /* 4 x 16 bytes, still wrapped                 */
} nca_info_t;

/* 0 when hdr is an NCA3 header and *ni is filled. */
int nca_parse(const u8 *hdr, nca_info_t *ni);

/* ---- PFS0 ---- */

/* Find `name`. 0: found, *off is its data offset from the PFS0 start;
 * 1: `avail` bytes are not the whole header yet, *need says how many;
 * -1: not a PFS0, or no such file. */
int pfs0_find(const u8 *h, u32 avail, const char *name,
              u64 *off, u64 *size, u32 *need);

/* ---- NSO ---- */

typedef struct {
    u32 file_off, comp_size, size;
    int compressed;
} nso_seg_t;

/* The .data segment of an NSO header (0x100 bytes). 0 on success. */
int nso_data_segment(const u8 *h, nso_seg_t *seg);

/* ---- the firmware and NVRAM in the wlan sysmodule's .data ---- */

typedef struct {
    u32 fw_off, fw_len;
    u32 nv_off, nv_len;
    u32 tail_off;        /* the "4356a3-roml..." version string         */
} wlan_blobs_t;

/* nx-fwextract's search. 0 on success, else which part is missing:
 * 1 NVRAM, 2 DBPP, 3 the start, 4 the version string, 5 a size. */
int wlanfw_locate(const u8 *d, u32 n, wlan_blobs_t *b);

/* brcmf_fw_nvram_strip, see gui_wlanfw.h. */
u32 wlanfw_nvram_binary(const u8 *txt, u32 len, u8 *out, u32 cap);

#endif
