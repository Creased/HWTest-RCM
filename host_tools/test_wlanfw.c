/*
 * Host test for gui/wlanfw_parse.c, the format parsing behind the Wi-Fi
 * scan's firmware extraction: run by CI, or by hand with
 *
 *   cc -DWLANFW_HOST -Wall -Wextra -o test_wlanfw host_tools/test_wlanfw.c \
 *      gui/wlanfw_parse.c && ./test_wlanfw
 *
 * Synthetic inputs only: no part of HOS is in here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gui/wlanfw_parse.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static void test_nvram(void)
{
    const char *txt =
        "NVRAMRev=$Rev: 1 $\n# a comment line\nsromrev=11\r\n"
        "boardtype=0x073e\nRAW1=ignored\n  aa2g=3\n"
        "bad key=skipped\nmacaddr=00:90:4c:c5:12:38";
    const char want[] =
        "NVRAMRev=$Rev: 1 $\0sromrev=11\0boardtype=0x073e\0aa2g=3\0"
        "macaddr=00:90:4c:c5:12:38\0boardrev=0xff";
    unsigned char out[512];
    u32 n = wlanfw_nvram_binary((const u8 *)txt, strlen(txt), out, sizeof(out));
    CHECK(n > 4 && (n & 3) == 0);
    CHECK(memcmp(out, want, sizeof(want)) == 0);
    u32 body = n - 4;
    u32 tok = out[body] | out[body + 1] << 8 | out[body + 2] << 16 |
              (u32)out[body + 3] << 24;
    CHECK((tok & 0xFFFF) == body / 4);
    CHECK(((~tok >> 16) & 0xFFFF) == body / 4);
    for (u32 i = sizeof(want); i < body; i++)
        CHECK(out[i] == 0);

    /* boardrev present: not added again. */
    const char *t2 = "boardrev=0x1101\nx=1\n";
    n = wlanfw_nvram_binary((const u8 *)t2, strlen(t2), out, sizeof(out));
    CHECK(n && memcmp(out, "boardrev=0x1101\0x=1\0\0\0\0", 24) == 0);
    /* too small a buffer: 0 rather than an overrun */
    CHECK(wlanfw_nvram_binary((const u8 *)txt, strlen(txt), out, 16) == 0);
    printf("ok   nvram\n");
}

static void lfn_entry(u8 *e, int ord, int last, const char *s13, u8 sum)
{
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    memset(e, 0, 32);
    e[0] = ord | (last ? 0x40 : 0);
    e[11] = 0x0F;
    e[13] = sum;
    int end = 0;
    for (int i = 0; i < 13; i++) {
        unsigned c;
        if (end)
            c = 0xFFFF;
        else if (s13[i])
            c = (u8)s13[i];
        else {
            c = 0;
            end = 1;
        }
        e[pos[i]] = c & 0xFF;
        e[pos[i] + 1] = c >> 8;
    }
}

static u8 sfn_sum(const u8 *s)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) << 7) + (sum >> 1) + s[i];
    return sum;
}

static char s_names[8][64];
static u32 s_clus[8], s_n;
static int collect(const fat_dirent_t *e, void *ctx)
{
    (void)ctx;
    if (s_n < 8) {
        strcpy(s_names[s_n], e->name);
        s_clus[s_n++] = e->clus;
    }
    return 0;
}

static void test_fat(void)
{
    u8 dir[32 * 10];
    memset(dir, 0, sizeof(dir));
    u8 s1[11];
    memcpy(s1, "REGIST~1   ", 11);
    lfn_entry(dir, 1, 1, "registered", sfn_sum(s1));
    memcpy(dir + 32, s1, 11);
    dir[32 + 11] = 0x10;
    dir[32 + 26] = 5;
    memcpy(dir + 64, ".          ", 11);           /* skipped */
    dir[64 + 11] = 0x10;
    memcpy(dir + 96, "SAVE       ", 11);
    dir[96 + 11] = 0x10;
    dir[96 + 20] = 1;                              /* cluster high half */
    dir[96 + 26] = 7;
    u8 s2[11];
    memcpy(s2, "0123AB~1NCA", 11);
    lfn_entry(dir + 128, 3, 1, "0abc.nca", sfn_sum(s2));
    lfn_entry(dir + 160, 2, 0, "0123456789abc", sfn_sum(s2));
    lfn_entry(dir + 192, 1, 0, "0123456789abc", sfn_sum(s2));
    memcpy(dir + 224, s2, 11);
    dir[224 + 11] = 0x20;
    dir[224 + 26] = 9;
    /* an LFN whose checksum does not match its SFN falls back to 8.3 */
    lfn_entry(dir + 256, 1, 1, "wrongname", 0x00);
    memcpy(dir + 288, "00         ", 11);
    dir[288 + 26] = 11;

    fat_lfn_t st;
    memset(&st, 0, sizeof(st));
    s_n = 0;
    int r = fat_dir_walk(dir, sizeof(dir), &st, collect, NULL);
    CHECK(r == 0);                                 /* ran out, no 0x00 */
    CHECK(s_n == 4);
    CHECK(!strcmp(s_names[0], "registered") && s_clus[0] == 5);
    CHECK(!strcmp(s_names[1], "SAVE") && s_clus[1] == 0x10007);
    CHECK(!strcmp(s_names[2], "0123456789abc0123456789abc0abc.nca"));
    CHECK(!strcmp(s_names[3], "00") && s_clus[3] == 11);
    CHECK(str_ieq("Contents", "CONTENTS") && !str_ieq("Content", "Contents"));
    CHECK(str_iends("x.NCA", ".nca") && !str_iends("nca", ".nca"));
    printf("ok   fat\n");
}

static void put64(u8 *p, unsigned long long v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (u8)(v >> (8 * i));
}

static void test_nca_pfs0_nso(void)
{
    static u8 h[0xC00];
    memset(h, 0, sizeof(h));
    memcpy(h + 0x200, "NCA3", 4);
    h[0x205] = 0;          /* program */
    h[0x206] = 2;
    h[0x207] = 2;          /* system */
    put64(h + 0x210, 0x0100000000000016ULL);
    h[0x220] = 5;
    h[0x240] = 0x06;       /* section 0 at 6 * 0x200 */
    h[0x244] = 0x40;
    h[0x400 + 2] = 1;
    h[0x400 + 3] = 2;
    h[0x400 + 4] = 3;
    put64(h + 0x400 + 0x40, 0x4000);
    put64(h + 0x400 + 0x48, 0x123456);
    for (int i = 0; i < 8; i++)
        h[0x400 + 0x140 + i] = (u8)(0x10 + i);
    nca_info_t ni;
    CHECK(nca_parse(h, &ni) == 0);
    CHECK(ni.title_id == 0x0100000000000016ULL && ni.content_type == 0);
    CHECK(ni.key_gen == 5 && ni.kak_index == 2 && !ni.has_rights_id);
    CHECK(ni.sec0_start == 0xC00 && ni.pfs0_off == 0x4000);
    CHECK(ni.sec0_fs_type == 1 && ni.sec0_hash_type == 2 && ni.sec0_enc_type == 3);
    CHECK(ni.ctr_hi[0] == 0x17 && ni.ctr_hi[7] == 0x10);
    h[0x203] = '2';
    CHECK(nca_parse(h, &ni) != 0);

    u8 p[0x100];
    memset(p, 0, sizeof(p));
    memcpy(p, "PFS0", 4);
    p[4] = 2;
    p[8] = 12;
    put64(p + 0x10, 0x0);
    put64(p + 0x18, 0x1000);
    p[0x10 + 16] = 0;
    put64(p + 0x28, 0x140);
    put64(p + 0x30, 0x2000);
    p[0x28 + 16] = 5;
    memcpy(p + 0x10 + 2 * 0x18, "rtld\0main\0\0\0", 12);
    unsigned long long off = 0, size = 0;
    u32 need = 0;
    CHECK(pfs0_find(p, sizeof(p), "main", &off, &size, &need) == 0);
    CHECK(off == 0x4C + 0x140 && size == 0x2000);
    CHECK(pfs0_find(p, 0x20, "main", &off, &size, &need) == 1 && need == 0x4C);
    CHECK(pfs0_find(p, sizeof(p), "sdk", &off, &size, &need) == -1);

    u8 nso[0x100];
    memset(nso, 0, sizeof(nso));
    memcpy(nso, "NSO0", 4);
    nso[0x0C] = 0x7;
    nso[0x30] = 0x00; nso[0x31] = 0x80;            /* file off 0x8000  */
    nso[0x38] = 0x00; nso[0x39] = 0x00; nso[0x3A] = 0x10;  /* 1 MiB     */
    nso[0x68] = 0x00; nso[0x69] = 0x00; nso[0x6A] = 0x08;  /* 512 KiB   */
    nso_seg_t seg;
    CHECK(nso_data_segment(nso, &seg) == 0);
    CHECK(seg.file_off == 0x8000 && seg.size == 0x100000 &&
          seg.comp_size == 0x80000 && seg.compressed);
    printf("ok   nca/pfs0/nso\n");
}

static void test_locate(void)
{
    u32 n = 0x60000;
    u8 *d = calloc(1, n);
    strcpy((char *)d + 0x100, "NVRAMRev=$Rev$");
    memset(d + 0x100 + 14, 'a', 100);
    d[0x100 + 114] = 0;
    memset(d + 0x1000, 0x11, 0x30000);
    memcpy(d + 0x1000 + 0x200, "DBPP", 4);
    d[0x1000 - 1] = 0;
    strcpy((char *)d + 0x31000, "4356a3-roml/pcie-ag Version: 7.35");
    wlan_blobs_t b;
    CHECK(wlanfw_locate(d, n, &b) == 0);
    CHECK(b.nv_off == 0x100 && b.nv_len == 114);
    CHECK(b.fw_off == 0x1000);
    CHECK(b.fw_len == 0x30000 + strlen("4356a3-roml/pcie-ag Version: 7.35"));
    CHECK(b.tail_off == 0x31000);
    d[0x1000 + 0x200] = 'X';                       /* no DBPP */
    CHECK(wlanfw_locate(d, n, &b) == 2);
    free(d);
    printf("ok   locate\n");
}

int main(void)
{
    test_nvram();
    test_fat();
    test_nca_pfs0_nso();
    test_locate();
    if (s_fail) {
        printf("%d check(s) failed\n", s_fail);
        return 1;
    }
    printf("all OK\n");
    return 0;
}
