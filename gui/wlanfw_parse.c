/*
 * Format parsing for the Wi-Fi firmware extraction: see wlanfw_parse.h.
 * Plain C over byte buffers, little-endian reads done by hand, no libc
 * beyond what the GUI and the host both have.
 */
#include "wlanfw_parse.h"

static u16 rd16le(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32le(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64le(const u8 *p)
{
    return rd32le(p) | ((u64)rd32le(p + 4) << 32);
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b)
        if (lower(*a++) != lower(*b++))
            return 0;
    return *a == *b;
}

int str_iends(const char *s, const char *suffix)
{
    u32 ls = 0, lx = 0;
    while (s[ls])
        ls++;
    while (suffix[lx])
        lx++;
    return ls >= lx && str_ieq(s + ls - lx, suffix);
}

/* ---- FAT32 ------------------------------------------------------------ */

static u8 lfn_checksum(const u8 *sfn)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (u8)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

int fat_dir_walk(const u8 *buf, u32 len, fat_lfn_t *st,
                 int (*cb)(const fat_dirent_t *e, void *ctx), void *ctx)
{
    static const u8 lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

    for (u32 off = 0; off + 32 <= len; off += 32) {
        const u8 *e = buf + off;
        if (e[0] == 0x00)
            return 1;
        if (e[0] == 0xE5) {
            st->lfn_ok = 0;
            continue;
        }
        if ((e[11] & 0x3F) == FAT_ATTR_LFN) {
            u32 seq = e[0] & 0x1F;
            if (e[0] & 0x40) {
                for (u32 i = 0; i < 260; i++)
                    st->lfn[i] = 0;
                st->lfn_len = seq * 13;
                st->lfn_sum = e[13];
                st->lfn_ok = 1;
            }
            if (!st->lfn_ok || seq == 0 || seq > 20 || e[13] != st->lfn_sum) {
                st->lfn_ok = 0;
                continue;
            }
            for (u32 i = 0; i < 13; i++)
                st->lfn[(seq - 1) * 13 + i] = rd16le(e + lfn_pos[i]);
            continue;
        }

        fat_dirent_t d;
        d.attr = e[11];
        d.clus = ((u32)rd16le(e + 20) << 16) | rd16le(e + 26);
        d.size = rd32le(e + 28);

        int have_lfn = st->lfn_ok && st->lfn_sum == lfn_checksum(e);
        st->lfn_ok = 0;
        if (d.attr & FAT_ATTR_VOL)
            continue;

        u32 n = 0;
        if (have_lfn) {
            for (u32 i = 0; i < st->lfn_len && i < 255; i++) {
                u16 c = st->lfn[i];
                if (c == 0x0000 || c == 0xFFFF)
                    break;
                d.name[n++] = c < 0x80 ? (char)c : '?';
            }
        } else {
            for (u32 i = 0; i < 8 && e[i] != ' '; i++)
                d.name[n++] = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
            if (e[8] != ' ') {
                d.name[n++] = '.';
                for (u32 i = 8; i < 11 && e[i] != ' '; i++)
                    d.name[n++] = (char)e[i];
            }
        }
        d.name[n] = 0;
        if ((d.name[0] == '.' && d.name[1] == 0) ||
            (d.name[0] == '.' && d.name[1] == '.' && d.name[2] == 0))
            continue;
        if (cb(&d, ctx))
            return 2;
    }
    return 0;
}

/* ---- NCA3 ------------------------------------------------------------- */

int nca_parse(const u8 *h, nca_info_t *ni)
{
    if (h[0x200] != 'N' || h[0x201] != 'C' || h[0x202] != 'A' || h[0x203] != '3')
        return -1;
    ni->content_type = h[0x205];
    u8 g_old = h[0x206], g_new = h[0x220];
    ni->key_gen = g_old > g_new ? g_old : g_new;
    ni->kak_index = h[0x207];
    ni->title_id = rd64le(h + 0x210);
    ni->sdk_ver = rd32le(h + 0x21C);
    ni->has_rights_id = 0;
    for (int i = 0; i < 16; i++)
        if (h[0x230 + i])
            ni->has_rights_id = 1;
    ni->sec0_start = (u64)rd32le(h + 0x240) * 0x200;
    ni->sec0_end = (u64)rd32le(h + 0x244) * 0x200;
    const u8 *fsh = h + 0x400;
    ni->sec0_fs_type = fsh[2];
    ni->sec0_hash_type = fsh[3];
    ni->sec0_enc_type = fsh[4];
    ni->pfs0_off = rd64le(fsh + 0x40);
    ni->pfs0_size = rd64le(fsh + 0x48);
    /* hactool: ctr[j] = section_ctr[7 - j] for the upper half. */
    for (int j = 0; j < 8; j++)
        ni->ctr_hi[j] = fsh[0x140 + 7 - j];
    ni->key_area = h + 0x300;
    return 0;
}

/* ---- PFS0 ------------------------------------------------------------- */

int pfs0_find(const u8 *h, u32 avail, const char *name,
              u64 *off, u64 *size, u32 *need)
{
    if (avail < 0x10)
        return *need = 0x10, 1;
    if (h[0] != 'P' || h[1] != 'F' || h[2] != 'S' || h[3] != '0')
        return -1;
    u32 n = rd32le(h + 4), strtab = rd32le(h + 8);
    if (n == 0 || n > 64 || strtab > 0x4000)
        return -1;
    u32 hdr = 0x10 + n * 0x18 + strtab;
    if (avail < hdr)
        return *need = hdr, 1;
    const char *names = (const char *)h + 0x10 + n * 0x18;
    for (u32 i = 0; i < n; i++) {
        const u8 *e = h + 0x10 + i * 0x18;
        u32 no = rd32le(e + 16);
        if (no >= strtab)
            continue;
        if (str_ieq(names + no, name)) {
            *off = hdr + rd64le(e);
            *size = rd64le(e + 8);
            return 0;
        }
    }
    return -1;
}

/* ---- NSO -------------------------------------------------------------- */

int nso_data_segment(const u8 *h, nso_seg_t *seg)
{
    if (h[0] != 'N' || h[1] != 'S' || h[2] != 'O' || h[3] != '0')
        return -1;
    u32 flags = rd32le(h + 0x0C);
    seg->file_off = rd32le(h + 0x30);
    seg->size = rd32le(h + 0x38);
    seg->comp_size = rd32le(h + 0x68);
    seg->compressed = (flags & 0x4u) != 0;
    if (!seg->size || seg->size > 0x01000000u)
        return -1;
    if (!seg->compressed)
        seg->comp_size = seg->size;
    if (!seg->comp_size || seg->comp_size > 0x01000000u)
        return -1;
    return 0;
}

/* ---- the blobs --------------------------------------------------------- */

static long find(const u8 *d, u32 n, u32 from, const char *s)
{
    u32 l = 0;
    while (s[l])
        l++;
    for (u32 i = from; i + l <= n; i++) {
        u32 k = 0;
        while (k < l && d[i + k] == (u8)s[k])
            k++;
        if (k == l)
            return (long)i;
    }
    return -1;
}

static long find_nul(const u8 *d, u32 n, u32 from)
{
    for (u32 i = from; i < n; i++)
        if (!d[i])
            return (long)i;
    return -1;
}

int wlanfw_locate(const u8 *d, u32 n, wlan_blobs_t *b)
{
    long nv = find(d, n, 0, "NVRAMRev=");
    if (nv < 0)
        return 1;
    long nv_end = find_nul(d, n, (u32)nv);
    if (nv_end < 0)
        return 1;
    b->nv_off = (u32)nv;
    b->nv_len = (u32)(nv_end - nv);

    long dbpp = find(d, n, 0, "DBPP");
    if (dbpp < 0)
        return 2;
    long start = -1;
    for (long i = dbpp; i > dbpp - 1024 && i >= 0; i--)
        if (d[i] == 0) {
            start = i + 1;
            break;
        }
    if (start < 0)
        return 3;
    long tail = find(d, n, (u32)start, "4356a3-roml");
    if (tail < 0)
        return 4;
    long end = find_nul(d, n, (u32)tail);
    if (end < 0)
        return 4;
    b->fw_off = (u32)start;
    b->fw_len = (u32)(end - start);
    b->tail_off = (u32)tail;
    if (b->fw_len < 0x20000u || b->fw_len > 0x100000u ||
        b->nv_len < 64 || b->nv_len > 0x8000u)
        return 5;
    return 0;
}

/* ---- NVRAM, brcmf_fw_nvram_strip's state machine --------------------- */

enum { NV_IDLE, NV_KEY, NV_VALUE, NV_COMMENT, NV_END };

static int nv_char(u8 c)
{
    return c != '#' && c >= 0x20 && c < 0x7F;
}

static int nv_ws(u8 c)
{
    return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}

static int nv_prefix(const u8 *p, const u8 *end, const char *s)
{
    while (*s) {
        if (p >= end || *p != (u8)*s)
            return 0;
        p++;
        s++;
    }
    return 1;
}

u32 wlanfw_nvram_binary(const u8 *d, u32 n, u8 *out, u32 cap)
{
    u32 pos = 0, entry = 0, o = 0;
    int st = NV_IDLE, boardrev = 0;
    const u8 *end = d + n;

    while (pos <= n && st != NV_END) {
        /* The end of the buffer reads as a NUL, so a last entry with no
         * newline after it still ends (HOS's text stops at a NUL). */
        u8 c = pos < n ? d[pos] : 0;
        switch (st) {
        case NV_IDLE:
            if (pos == n)
                st = NV_END;
            else if (c == '\n' || c == '#')
                st = NV_COMMENT;
            else if (nv_ws(c) || c == 0)
                pos++;
            else if (nv_char(c)) {
                entry = pos;
                st = NV_KEY;
            } else
                pos++;
            break;
        case NV_KEY:
            if (c == '=') {
                st = nv_prefix(d + entry, end, "RAW1") ? NV_COMMENT : NV_VALUE;
                if (nv_prefix(d + entry, end, "boardrev"))
                    boardrev = 1;
                pos++;
            } else if (!nv_char(c) || c == ' ') {
                st = NV_COMMENT;
            } else
                pos++;
            break;
        case NV_VALUE:
            if (!nv_char(c)) {
                u32 l = pos - entry;
                if (o + l + 1 >= cap)
                    return 0;
                for (u32 i = 0; i < l; i++)
                    out[o++] = d[entry + i];
                out[o++] = 0;
                st = NV_IDLE;
            } else
                pos++;
            break;
        case NV_COMMENT: {
            u32 i = pos;
            while (i < n && d[i] != '\n' && d[i] != 0)
                i++;
            if (i >= n)
                st = NV_END;
            else {
                pos = i + 1;
                st = NV_IDLE;
            }
            break;
        }
        }
    }

    if (!o)
        return 0;
    if (!boardrev) {
        static const char br[] = "boardrev=0xff";
        if (o + sizeof(br) >= cap)
            return 0;
        for (u32 i = 0; i < sizeof(br); i++)     /* the NUL too */
            out[o++] = (u8)br[i];
    }
    u32 len = (o + 1 + 3) & ~3u;
    if (len + 4 > cap)
        return 0;
    while (o < len)
        out[o++] = 0;
    u32 token = len / 4;
    token = (~token << 16) | (token & 0xFFFFu);
    out[o++] = token;
    out[o++] = token >> 8;
    out[o++] = token >> 16;
    out[o++] = token >> 24;
    return o;
}
