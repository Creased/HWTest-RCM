/* Standalone declaration for Atmosphere's LZ4 table decoder. */
#pragma once
#include <exosphere.hpp>
namespace ams::nxboot {
void Uncompress(void *dst, size_t dst_size, const void *src, size_t src_size);
}
