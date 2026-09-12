/* GPLv2 standalone helpers for the hwtest RAM payload. */
#include <exosphere.hpp>
#include "fusee_fatal.hpp"
#include "fusee_uncompress.hpp"
#include <stdarg.h>

namespace ams::nxboot {

NORETURN void ShowFatalError(const char *fmt, ...)
{
    (void)fmt;
    __builtin_trap();
    for (;;) { }
}

void Uncompress(void *dst_, size_t dst_size, const void *src_, size_t src_size)
{
    const u8 *src = static_cast<const u8 *>(src_);
    u8 *dst = static_cast<u8 *>(dst_);
    size_t si = 0, di = 0;

    while (si < src_size) {
        u8 token = src[si++];
        size_t literals = token >> 4;
        if (literals == 15) {
            for (;;) {
                AMS_ABORT_UNLESS(si < src_size);
                u8 n = src[si++];
                literals += n;
                if (n != 255) break;
            }
        }
        AMS_ABORT_UNLESS(si + literals <= src_size);
        AMS_ABORT_UNLESS(di + literals <= dst_size);
        for (size_t i = 0; i < literals; ++i)
            dst[di++] = src[si++];

        if (si >= src_size)
            break;

        AMS_ABORT_UNLESS(si + 2 <= src_size);
        u16 offset = src[si++];
        offset |= static_cast<u16>(src[si++]) << 8;
        AMS_ABORT_UNLESS(offset != 0 && offset <= di);

        size_t match = token & 0x0F;
        if (match == 15) {
            for (;;) {
                AMS_ABORT_UNLESS(si < src_size);
                u8 n = src[si++];
                match += n;
                if (n != 255) break;
            }
        }
        match += 4;
        AMS_ABORT_UNLESS(di + match <= dst_size);
        for (size_t i = 0; i < match; ++i) {
            dst[di] = dst[di - offset];
            ++di;
        }
    }

    AMS_ABORT_UNLESS(di == dst_size);
}

} // namespace ams::nxboot
