/*
 * hwtest - raw deflate decoder for the loader (inflate.c).
 */
#ifndef HWTEST_INFLATE_H
#define HWTEST_INFLATE_H

/* Expand a raw deflate stream (no zlib or gzip header) from in into out.
 * The bytes written, or -1 for a damaged stream. */
long inflate_raw(const unsigned char *in, unsigned char *out);

#endif
