/*
 * hwtest - self-extracting loader.
 *
 * TegraRcmGUI shells out to TegraRcmSmash, which prepends a fixed 66216-byte
 * exploit prologue and then SILENTLY truncates the whole buffer to 192 KiB.
 * Anything past 130392 bytes of payload is dropped without a word - the GUI
 * still reports "successfully injected" and the console jumps into a partial
 * image. hwtest passed that ceiling at v0.6 and had been shipping truncated
 * to anyone using that injector ever since.
 *
 * So the payload ships compressed and unpacks itself, which is what hekate
 * does for the same reason. This stub is what RCM actually loads: about two
 * kilobytes of code at LDR_LOAD_ADDR with the payload trailing it as a raw
 * deflate stream (tools/mkpayload.py --codec deflate). It lifts the stream
 * out of the way, expands it down into IPL_LOAD_ADDR with inflate.c and
 * jumps. Deflate rather than the BDK's LZ77: it packs the payload about a
 * fifth tighter, and the LZ image had run out of room under the limit. The
 * RAM test (ramtest/Makefile) builds this same loader with -DLDR_LZ and
 * keeps the LZ77 image, checked by ramtest/check_overlap.py.
 *
 * The layout has to be checked, not assumed, or the expanding output would
 * overwrite the input it is still reading. With the stream parked so it ENDS
 * at IPL_RELOC_TOP, the read pointer starts far above the write pointer, and
 * tools/check_inflate.py replays the whole expansion at build time and fails
 * the build if the writer ever comes within 256 bytes of the unread input.
 * The loader's own code must also stay below IPL_LOAD_ADDR, where the output
 * starts: link.ld asserts that.
 */

#include <memory_map.h>
#include <soc/clock.h>
#include <soc/t210.h>

#ifdef LDR_LZ
#include <libs/compr/lz.h>
#else
#include "inflate.h"
#endif
#include "payload_lz.h"

/* Top of the region the compressed image is parked under. Chosen as hekate's
 * loader does: high enough to clear the expanded payload, low enough to leave
 * the very top of IRAM alone for panic/debug use. */
#define IPL_RELOC_TOP 0x40038000

void loader_main(void)
{
    /* Boost the BPMP before decompressing - this is the one place where the
     * clock rate is worth real time, and the payload reprograms it anyway. */
    CLOCK(CLK_RST_CONTROLLER_CLK_SYSTEM_RATE)    = 0x10;
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_SYS)     = 0;
    CLOCK(CLK_RST_CONTROLLER_SCLK_BURST_POLICY)  = 0x20004444;
    CLOCK(CLK_RST_CONTROLLER_SUPER_SCLK_DIVIDER) = 0x80000000;
    CLOCK(CLK_RST_CONTROLLER_CLK_SYSTEM_RATE)    = 2;
    CLOCK(CLK_RST_CONTROLLER_SCLK_BURST_POLICY)  = 0x20003333;

    /* Move the stream up so it ends at IPL_RELOC_TOP. Copied back to front
     * because source and destination overlap. */
    u32 size  = (sizeof(payload_lz) + 3) & ~3u;
    u32 words = size >> 2;
    u32 *src  = (u32 *)payload_lz + words - 1;
    u32 *dst  = (u32 *)(IPL_RELOC_TOP - 4);

    while (words--)
        *dst-- = *src--;

    /* Expand it into place and hand over. The build has already proved the
     * stream sound (mkpayload.py round-trips it), so a failure here can only
     * be a damaged transfer: stop rather than jump into a partial image. */
#ifdef LDR_LZ
    LZ_Uncompress((const u8 *)(IPL_RELOC_TOP - size), (u8 *)IPL_LOAD_ADDR,
                  sizeof(payload_lz));
#else
    if (inflate_raw((const u8 *)(IPL_RELOC_TOP - size), (u8 *)IPL_LOAD_ADDR) <= 0)
        while (1)
            ;
#endif

    void (*payload)(void) = (void *)IPL_LOAD_ADDR;
    payload();

    while (1)
        ;
}
