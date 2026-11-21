/*
 * hwtest GUI - the CYW4356 firmware and NVRAM out of the console's own HOS.
 * See gui_wlanfw.h for where they are and why; wlanfw_parse.c for the
 * formats. This file is the I/O and the crypto:
 *
 *   prod.keys    sd:/switch/prod.keys, as Lockpick_RCM writes it:
 *                bis_key_02 (SYSTEM), header_key, key_area_key_*_NN
 *   SYSTEM       bdk's BIS layer, keyslots 4/5 as nx_emmc_bis_init picks
 *                for it, read only
 *   NCA header   AES-XTS, Nintendo tweak, sectors 0-5 (keyslots 8/9)
 *   key area     AES-ECB with the key area key (keyslot 10)
 *   ExeFS        AES-CTR done as ECB over counter blocks (keyslot 11), so
 *                nothing depends on how the engine's own CTR mode counts
 *
 * Every keyslot this file loads is cleared before it returns.
 */
#include "gui.h"
#include "gui_wlanfw.h"
#include "wlanfw_parse.h"
#include <libs/compr/lz4.h>
#include <storage/nx_emmc_bis.h>

#define WLAN_TITLE_ID   0x0100000000000016ULL
#define KS_BIS_CRYPT    4
#define KS_BIS_TWEAK    5
#define KS_HDR_CRYPT    8
#define KS_HDR_TWEAK    9
#define KS_KAK          10
#define KS_CTR          11
#define SECTOR          512u
#define CTR_CHUNK       0x10000u

static char *s_why;
static u32 s_why_len;
static void (*s_step)(const char *line);

static int _fail(const char *msg)
{
    if (s_why) {
        u32 n = strlen(msg);
        if (n >= s_why_len)
            n = s_why_len - 1;
        memcpy(s_why, msg, n);
        s_why[n] = 0;
    }
    return -1;
}

static void _step(const char *line)
{
    char buf[128];
    s_printf(buf, "gui: wifi: %s\n", line);
    gui_uart_puts(buf);
    if (s_step)
        s_step(line);
}

/* ---- prod.keys -------------------------------------------------------- */

static int _hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `name = hex` on a line of its own; 0 when found with `len` bytes. */
static int _key(const char *body, const char *name, u8 *out, u32 len)
{
    u32 nl = strlen(name);
    for (const char *p = body; *p; ) {
        const char *s = p;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!strncmp(s, name, nl) &&
            (s[nl] == ' ' || s[nl] == '\t' || s[nl] == '=')) {
            const char *q = s + nl;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q++ != '=')
                return -1;
            while (*q == ' ' || *q == '\t')
                q++;
            for (u32 i = 0; i < len; i++) {
                int hi = _hex(q[2 * i]);
                int lo = hi < 0 ? -1 : _hex(q[2 * i + 1]);
                if (hi < 0 || lo < 0)
                    return -1;
                out[i] = (u8)(hi << 4 | lo);
            }
            return 0;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return -1;
}

/* ---- SYSTEM, FAT32, read only ------------------------------------------ */

typedef struct {
    u32 spc;            /* sectors per cluster */
    u32 fat_lba;
    u32 data_lba;
    u32 root;
    u32 nclus;
    u8 *clus_buf;
    u32 fat_cached;     /* FAT sector in fat_buf, ~0 when none */
    u8  fat_buf[SECTOR] __attribute__((aligned(64)));
} fat_t;

static fat_t *s_fat;

/* nx_emmc_bis_read: 0 is success. */
static int _bis(u32 lba, u32 n, void *buf)
{
    return nx_emmc_bis_read(lba, n, buf) ? -1 : 0;
}

static int _fat_mount(fat_t *f)
{
    u8 *b = f->fat_buf;
    if (_bis(0, 1, b))
        return _fail("SYSTEM: cannot read its first sector");
    if (b[510] != 0x55 || b[511] != 0xAA)
        return _fail("SYSTEM: no boot sector (wrong BIS key?)");
    u32 bps = b[11] | (b[12] << 8);
    f->spc = b[13];
    u32 rsvd = b[14] | (b[15] << 8);
    u32 nfats = b[16];
    u32 fatsz = b[36] | (b[37] << 8) | (b[38] << 16) | ((u32)b[39] << 24);
    f->root = b[44] | (b[45] << 8) | (b[46] << 16) | ((u32)b[47] << 24);
    u32 total = b[32] | (b[33] << 8) | (b[34] << 16) | ((u32)b[35] << 24);
    if (bps != SECTOR || !f->spc || (f->spc & (f->spc - 1)) || f->spc > 128 ||
        !nfats || !fatsz || f->root < 2)
        return _fail("SYSTEM: not FAT32 (wrong BIS key?)");
    f->fat_lba = rsvd;
    f->data_lba = rsvd + nfats * fatsz;
    f->nclus = (total - f->data_lba) / f->spc + 2;
    f->fat_cached = ~0u;
    f->clus_buf = malloc(f->spc * SECTOR);
    return f->clus_buf ? 0 : _fail("out of memory");
}

static u32 _fat_next(fat_t *f, u32 c)
{
    u32 sec = f->fat_lba + c / (SECTOR / 4);
    if (sec != f->fat_cached) {
        if (_bis(sec, 1, f->fat_buf))
            return 0x0FFFFFFF;
        f->fat_cached = sec;
    }
    u32 i = (c % (SECTOR / 4)) * 4;
    u8 *e = f->fat_buf + i;
    return (e[0] | (e[1] << 8) | (e[2] << 16) | ((u32)e[3] << 24)) & 0x0FFFFFFF;
}

static int _clus_ok(fat_t *f, u32 c)
{
    return c >= 2 && c < f->nclus;
}

static int _fat_read_clus(fat_t *f, u32 c, void *buf)
{
    return _bis(f->data_lba + (c - 2) * f->spc, f->spc, buf);
}

/* Bytes [off, off+len) of the file starting at cluster `first`. */
static int _fat_read(fat_t *f, u32 first, u32 off, void *dst, u32 len)
{
    u32 cs = f->spc * SECTOR;
    u32 c = first;
    for (u32 skip = off / cs; skip; skip--) {
        c = _fat_next(f, c);
        if (!_clus_ok(f, c))
            return -1;
    }
    u32 in = off % cs;
    u8 *d = dst;
    while (len) {
        if (!_clus_ok(f, c) || _fat_read_clus(f, c, f->clus_buf))
            return -1;
        u32 n = cs - in < len ? cs - in : len;
        memcpy(d, f->clus_buf + in, n);
        d += n;
        len -= n;
        in = 0;
        if (len)
            c = _fat_next(f, c);
    }
    return 0;
}

typedef int (*dir_cb_t)(const fat_dirent_t *e, void *ctx);

static int _fat_dir(fat_t *f, u32 clus, dir_cb_t cb, void *ctx)
{
    fat_lfn_t st;
    memset(&st, 0, sizeof(st));
    u8 *buf = malloc(f->spc * SECTOR);
    if (!buf)
        return -1;
    int r = 0;
    for (u32 n = 0; n < 4096 && _clus_ok(f, clus); n++) {
        if (_fat_read_clus(f, clus, buf)) {
            r = -1;
            break;
        }
        int w = fat_dir_walk(buf, f->spc * SECTOR, &st, cb, ctx);
        if (w)
            break;
        clus = _fat_next(f, clus);
    }
    free(buf);
    return r;
}

typedef struct {
    const char *name;
    fat_dirent_t found;
    int ok;
} find_t;

static int _find_cb(const fat_dirent_t *e, void *ctx)
{
    find_t *fd = ctx;
    if (str_ieq(e->name, fd->name)) {
        fd->found = *e;
        fd->ok = 1;
        return 1;
    }
    return 0;
}

static int _fat_lookup(fat_t *f, u32 dir, const char *name, fat_dirent_t *out)
{
    find_t fd = { name, {{0}}, 0 };
    if (_fat_dir(f, dir, _find_cb, &fd) || !fd.ok)
        return -1;
    *out = fd.found;
    return 0;
}

/* ---- NCA ------------------------------------------------------------- */

typedef struct {
    u32  first;          /* the file's first cluster (the NCA or its 00) */
    u32  size;
    nca_info_t ni;
    u8   hdr[0xC00];
} nca_t;

typedef struct {
    const char *body;    /* prod.keys */
    nca_t      *hit;
    u32         seen;
    int         depth;
    int         err;
} scan_t;

static int _nca_check(fat_t *f, u32 first, u32 size, scan_t *sc)
{
    static u8 enc[0xC00] __attribute__((aligned(64)));
    nca_t *n = sc->hit;
    if (size < 0xC00 || _fat_read(f, first, 0, enc, 0xC00))
        return 0;
    sc->seen++;
    if (se_aes_crypt_xts(KS_HDR_TWEAK, KS_HDR_CRYPT, DECRYPT, 0, n->hdr, enc,
                         0x200, 6))
        return 0;
    if (nca_parse(n->hdr, &n->ni) || n->ni.title_id != WLAN_TITLE_ID ||
        n->ni.content_type != 0)
        return 0;
    n->first = first;
    n->size = size;
    return 1;
}

static int _scan_cb(const fat_dirent_t *e, void *ctx);

static int _scan_dir(fat_t *f, u32 clus, scan_t *sc)
{
    if (sc->depth > 3)
        return 0;
    sc->depth++;
    _fat_dir(f, clus, _scan_cb, sc);
    sc->depth--;
    return sc->hit->first != 0;
}

/* Registered content is Contents/registered/<00..FF dir>/<id>.nca, where the
 * .nca is either a file or, for HOS's concatenation files, a directory with
 * the archive bit holding "00", "01"... Only "00" is needed: the header and
 * the ExeFS sit at the front, and the wlan NCA is a few MB. */
static int _scan_cb(const fat_dirent_t *e, void *ctx)
{
    scan_t *sc = ctx;
    if (e->attr & FAT_ATTR_DIR) {
        if (str_iends(e->name, ".nca")) {
            fat_dirent_t part;
            if (!_fat_lookup(s_fat, e->clus, "00", &part) &&
                _nca_check(s_fat, part.clus, part.size, sc))
                return 1;
            return 0;
        }
        return _scan_dir(s_fat, e->clus, sc);
    }
    if (str_iends(e->name, ".nca") && _nca_check(s_fat, e->clus, e->size, sc))
        return 1;
    return 0;
}

/* AES-CTR over [abs, abs+len) of the NCA, in place; abs 16-aligned. */
static int _ctr(const nca_t *n, u64 abs, u8 *buf, u32 len)
{
    static u8 ks[CTR_CHUNK] __attribute__((aligned(64)));
    for (u32 done = 0; done < len; ) {
        u32 chunk = len - done < CTR_CHUNK ? len - done : CTR_CHUNK;
        u32 blocks = (chunk + 15) / 16;
        u64 ctr = (abs + done) >> 4;
        for (u32 b = 0; b < blocks; b++) {
            u8 *c = ks + 16 * b;
            memcpy(c, n->ni.ctr_hi, 8);
            u64 v = ctr + b;
            for (int j = 0; j < 8; j++)
                c[15 - j] = (u8)(v >> (8 * j));
        }
        if (se_aes_crypt_ecb(KS_CTR, ENCRYPT, ks, ks, blocks * 16))
            return -1;
        for (u32 i = 0; i < chunk; i++)
            buf[done + i] ^= ks[i];
        done += chunk;
    }
    return 0;
}

/* Read and decrypt [abs, abs+len) of the NCA into dst. */
static int _nca_read(fat_t *f, const nca_t *n, u64 abs, u8 *dst, u32 len)
{
    u64 a = abs & ~0xFULL;
    u32 pre = (u32)(abs - a);
    u32 tot = (pre + len + 15) & ~15u;
    u8 *tmp = malloc(tot);
    if (!tmp)
        return -1;
    int r = _fat_read(f, n->first, (u32)a, tmp, tot);
    if (!r)
        r = _ctr(n, a, tmp, tot);
    if (!r)
        memcpy(dst, tmp + pre, len);
    free(tmp);
    return r;
}

/* ---- the whole way -------------------------------------------------- */

static const char *s_kak_kind[] = { "application", "ocean", "system" };

int wlanfw_extract(wlanfw_t *out, char *why, u32 why_len,
                   void (*step)(const char *line))
{
    s_why = why;
    s_why_len = why_len;
    s_step = step;
    memset(out, 0, sizeof(*out));
    if (why_len)
        why[0] = 0;

    int ret = -1;
    char *body = NULL;
    u32 ksz = 0;
    u8 key[32] __attribute__((aligned(4)));
    link_t gpt;
    bool gpt_parsed = false, bis_open = false;
    fat_t *f = NULL;
    nca_t *n = NULL;
    u8 *nso = NULL, *data = NULL;
    char line[96];

    _step("reading sd:/switch/prod.keys");
    gui_hw_prepare();
    if (!g_sd_ok || sd_mount()) {
        _fail("no SD card");
        goto out;
    }
    char *raw = sd_file_read("switch/prod.keys", &ksz);
    if (!raw) {
        _fail("no sd:/switch/prod.keys - run Lockpick_RCM first");
        goto out;
    }
    body = malloc(ksz + 1);
    if (!body) {
        free(raw);
        _fail("out of memory");
        goto out;
    }
    memcpy(body, raw, ksz);
    body[ksz] = 0;
    memset(raw, 0, ksz);
    free(raw);

    if (_key(body, "bis_key_02", key, 32)) {
        _fail("prod.keys has no bis_key_02");
        goto out;
    }
    se_aes_key_set(KS_BIS_CRYPT, key, 16);
    se_aes_key_set(KS_BIS_TWEAK, key + 16, 16);
    if (_key(body, "header_key", key, 32)) {
        _fail("prod.keys has no header_key");
        goto out;
    }
    se_aes_key_set(KS_HDR_CRYPT, key, 16);
    se_aes_key_set(KS_HDR_TWEAK, key + 16, 16);

    _step("opening SYSTEM on the eMMC");
    if (!g_emmc_ok) {
        _fail("eMMC did not initialise");
        goto out;
    }
    emmc_set_partition(EMMC_GPP);
    list_init(&gpt);
    emmc_gpt_parse(&gpt);
    gpt_parsed = true;
    emmc_part_t *part = emmc_part_find(&gpt, "SYSTEM");
    u32 sec_cnt = emmc_storage.sec_cnt;
    if (!part || !part->lba_start || part->lba_end < part->lba_start ||
        (sec_cnt && part->lba_end >= sec_cnt)) {
        _fail("no usable SYSTEM partition in the GPT");
        goto out;
    }
    nx_emmc_bis_init(part, false, 0);
    bis_open = true;

    f = calloc(1, sizeof(*f));
    n = calloc(1, sizeof(*n));
    if (!f || !n) {
        _fail("out of memory");
        goto out;
    }
    s_fat = f;
    if (_fat_mount(f))
        goto out;
    fat_dirent_t d;
    if (_fat_lookup(f, f->root, "Contents", &d) || !(d.attr & FAT_ATTR_DIR) ||
        _fat_lookup(f, d.clus, "registered", &d) || !(d.attr & FAT_ATTR_DIR)) {
        _fail("SYSTEM has no Contents/registered");
        goto out;
    }

    _step("looking for the wlan sysmodule's NCA");
    scan_t sc = { body, n, 0, 0, 0 };
    _scan_dir(f, d.clus, &sc);
    out->nca_count = sc.seen;
    if (!n->first) {
        s_printf(line, "no wlan program NCA among %d (wrong header_key?)",
                 sc.seen);
        _fail(line);
        goto out;
    }
    out->sdk_ver = n->ni.sdk_ver;
    if (n->ni.has_rights_id || n->ni.sec0_fs_type != 1 ||
        n->ni.sec0_hash_type != 2 || n->ni.sec0_enc_type != 3 ||
        n->ni.kak_index > 2) {
        _fail("wlan NCA: unexpected ExeFS layout");
        goto out;
    }

    /* Key area key for this NCA's generation, then its CTR key. */
    {
        u32 rev = n->ni.key_gen ? n->ni.key_gen - 1u : 0u;
        char kn[48];
        s_printf(kn, "key_area_key_%s_%02x", s_kak_kind[n->ni.kak_index], rev);
        if (_key(body, kn, key, 16)) {
            s_printf(line, "prod.keys has no %s", kn);
            _fail(line);
            goto out;
        }
        se_aes_key_set(KS_KAK, key, 16);
        u8 ctr_key[16] __attribute__((aligned(16)));
        u8 wrapped[16] __attribute__((aligned(16)));
        memcpy(wrapped, n->ni.key_area + 2 * 16, 16);
        if (se_aes_crypt_ecb(KS_KAK, DECRYPT, ctr_key, wrapped, 16)) {
            _fail("security engine: key area");
            goto out;
        }
        se_aes_key_set(KS_CTR, ctr_key, 16);
        memset(ctr_key, 0, sizeof(ctr_key));
    }

    _step("reading the ExeFS");
    u64 pfs = n->ni.sec0_start + n->ni.pfs0_off;
    u8 *ph = malloc(0x1000);
    if (!ph) {
        _fail("out of memory");
        goto out;
    }
    u64 m_off = 0, m_size = 0;
    u32 need = 0;
    int pr = _nca_read(f, n, pfs, ph, 0x1000) ? -2
             : pfs0_find(ph, 0x1000, "main", &m_off, &m_size, &need);
    free(ph);
    if (pr == -2) {
        _fail("cannot read the ExeFS");
        goto out;
    }
    if (pr) {
        _fail(pr == 1 ? "ExeFS header too large"
                      : "ExeFS is not a PFS0 with main (wrong key area key?)");
        goto out;
    }

    _step("unpacking main's .data");
    u8 nh[0x100] __attribute__((aligned(16)));
    nso_seg_t seg;
    if (_nca_read(f, n, pfs + m_off, nh, sizeof(nh)) ||
        nso_data_segment(nh, &seg) ||
        seg.file_off + (u64)seg.comp_size > m_size) {
        _fail("main is not an NSO");
        goto out;
    }
    nso = malloc(seg.comp_size);
    data = malloc(seg.size);
    if (!nso || !data) {
        _fail("out of memory");
        goto out;
    }
    if (_nca_read(f, n, pfs + m_off + seg.file_off, nso, seg.comp_size)) {
        _fail("cannot read main's .data");
        goto out;
    }
    if (seg.compressed) {
        int got = LZ4_decompress_safe((const char *)nso, (char *)data,
                                      seg.comp_size, seg.size);
        if (got != (int)seg.size) {
            _fail("main's .data did not decompress");
            goto out;
        }
    } else {
        memcpy(data, nso, seg.size);
    }

    _step("finding the firmware and NVRAM");
    wlan_blobs_t b;
    int lr = wlanfw_locate(data, seg.size, &b);
    if (lr) {
        static const char *what[] = { "", "no NVRAM text", "no DBPP marker",
            "no firmware start", "no 4356a3-roml version string",
            "firmware or NVRAM size not plausible" };
        _fail(what[lr]);
        goto out;
    }
    out->fw = malloc(b.fw_len);
    out->nvram = malloc(b.nv_len + 1);
    if (!out->fw || !out->nvram) {
        _fail("out of memory");
        goto out;
    }
    memcpy(out->fw, data + b.fw_off, b.fw_len);
    out->fw_len = b.fw_len;
    memcpy(out->nvram, data + b.nv_off, b.nv_len);
    out->nvram[b.nv_len] = 0;
    out->nvram_len = b.nv_len;
    u32 tl = b.fw_off + b.fw_len - b.tail_off;
    if (tl > sizeof(out->fw_tail) - 1)
        tl = sizeof(out->fw_tail) - 1;
    memcpy(out->fw_tail, data + b.tail_off, tl);
    out->fw_tail[tl] = 0;
    s_printf(line, "firmware %d bytes, NVRAM %d bytes", b.fw_len, b.nv_len);
    _step(line);
    ret = 0;

out:
    if (ret) {
        free(out->fw);
        free(out->nvram);
        out->fw = out->nvram = NULL;
    }
    free(nso);
    free(data);
    if (f)
        free(f->clus_buf);
    free(f);
    free(n);
    s_fat = NULL;
    if (bis_open)
        nx_emmc_bis_end();
    if (gpt_parsed)
        gpt_free_bounded(&gpt);
    memset(key, 0, sizeof(key));
    if (body) {
        memset(body, 0, ksz);
        free(body);
    }
    se_aes_key_clear(KS_BIS_CRYPT);
    se_aes_key_clear(KS_BIS_TWEAK);
    se_aes_key_clear(KS_HDR_CRYPT);
    se_aes_key_clear(KS_HDR_TWEAK);
    se_aes_key_clear(KS_KAK);
    se_aes_key_clear(KS_CTR);
    return ret;
}

