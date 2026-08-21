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
 * does for the same reason. This stub is what RCM actually loads: a few
 * hundred bytes of code at LDR_LOAD_ADDR with the LZ-compressed payload
 * trailing it. It lifts the compressed image out of the way, expands it down
 * into IPL_LOAD_ADDR and jumps.
 *
 * The layout has to be checked, not assumed, or the expanding output would
 * overwrite the compressed input it is still reading. With the blob parked so
 * it ENDS at IPL_RELOC_TOP, the read pointer starts ~77 KB above the write
 * pointer and the stream expands by ~31 KB over its whole length, so the
 * write pointer never catches up. Recheck that margin if either size moves.
 */

#include <memory_map.h>
#include <libs/compr/lz.h>
#include <soc/clock.h>
#include <soc/t210.h>

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

    /* Move the compressed image up so it ends at IPL_RELOC_TOP. Copied back
     * to front because source and destination overlap. */
    u32 size  = (sizeof(payload_lz) + 3) & ~3u;
    u32 words = size >> 2;
    u32 *src  = (u32 *)payload_lz + words - 1;
    u32 *dst  = (u32 *)(IPL_RELOC_TOP - 4);

    while (words--)
        *dst-- = *src--;

    /* Expand it into place and hand over. */
    LZ_Uncompress((const u8 *)(IPL_RELOC_TOP - size), (u8 *)IPL_LOAD_ADDR,
                  sizeof(payload_lz));

    void (*payload)(void) = (void *)IPL_LOAD_ADDR;
    payload();

    while (1)
        ;
}
