/*
 * The host's side of the simulator: printing and PNG writing.
 *
 * Kept apart from sim.c because the two worlds do not share headers - bdk
 * declares malloc() and usleep() with its own types, which the host's
 * <stdlib.h> and <unistd.h> contradict. sim.c is the console's world; this
 * file is the PC's, and the two meet over the handful of calls in sim_host.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* A block of the console's address space, at the address the console puts
 * it at. The GUI's drawing buffer and LVGL's pool are fixed addresses in
 * DRAM (memory_map.h); mapping them here means the screen code, the
 * framebuffer arithmetic and LVGL's allocator all run unchanged. */
void sim_map(unsigned long addr, unsigned long len, const char *what)
{
    void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || (unsigned long)p != addr) {
        fprintf(stderr, "sim: cannot map %s at %#lx (%lu bytes)\n",
                what, addr, len);
        exit(2);
    }
}

void sim_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* The framebuffer is XRGB, a word a pixel; PNG wants three bytes a pixel in
 * rows, each row behind a filter byte, the lot in one deflate stream. */
void sim_png_write(const char *dir, const char *name, const char *suffix,
                   const unsigned int *fb, unsigned w, unsigned h)
{
    char path[1024];
    mkdir(dir, 0777);               /* there on the second run, and fine */
    snprintf(path, sizeof(path), "%s/%s%s.png", dir, name,
             suffix ? suffix : "");

    unsigned long raw_len = (unsigned long)h * (1 + (unsigned long)w * 3);
    unsigned char *raw = malloc(raw_len), *p = raw;
    for (unsigned y = 0; y < h; y++) {
        *p++ = 0;
        for (unsigned x = 0; x < w; x++) {
            unsigned c = fb[y * w + x];
            *p++ = (unsigned char)(c >> 16);
            *p++ = (unsigned char)(c >> 8);
            *p++ = (unsigned char)c;
        }
    }
    uLongf zlen = compressBound(raw_len);
    unsigned char *z = malloc(zlen);
    FILE *f = NULL;
    if (compress2(z, &zlen, raw, raw_len, 6) != Z_OK || !(f = fopen(path, "wb"))) {
        sim_say("FAIL  cannot write %s\n", path);
        free(raw);
        free(z);
        return;
    }
    static const unsigned char sig[8] = { 0x89,'P','N','G','\r','\n',0x1A,'\n' };
    fwrite(sig, 1, 8, f);

    unsigned char ihdr[25] = { 0,0,0,13, 'I','H','D','R' };
    for (int i = 0; i < 4; i++) {
        ihdr[8 + i]  = (unsigned char)(w >> (24 - 8 * i));
        ihdr[12 + i] = (unsigned char)(h >> (24 - 8 * i));
    }
    ihdr[16] = 8;                      /* bits a channel */
    ihdr[17] = 2;                      /* truecolour     */
    unsigned long c = crc32(0, ihdr + 4, 17);
    for (int i = 0; i < 4; i++)
        ihdr[21 + i] = (unsigned char)(c >> (24 - 8 * i));
    fwrite(ihdr, 1, 25, f);

    unsigned char hdr[8] = { (unsigned char)(zlen >> 24), (unsigned char)(zlen >> 16),
                             (unsigned char)(zlen >> 8), (unsigned char)zlen,
                             'I','D','A','T' };
    fwrite(hdr, 1, 8, f);
    fwrite(z, 1, zlen, f);
    c = crc32(crc32(0, hdr + 4, 4), z, zlen);
    unsigned char crcb[4];
    for (int i = 0; i < 4; i++)
        crcb[i] = (unsigned char)(c >> (24 - 8 * i));
    fwrite(crcb, 1, 4, f);

    static const unsigned char iend[12] = { 0,0,0,0,'I','E','N','D',
                                            0xAE,0x42,0x60,0x82 };
    fwrite(iend, 1, 12, f);
    fclose(f);
    free(raw);
    free(z);
    sim_say("      %s\n", path);
}
