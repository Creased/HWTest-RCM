/* Standalone hwtest declaration replacing fusee UI fatal handling. */
#pragma once
#include <exosphere.hpp>
namespace ams::nxboot {
NORETURN void ShowFatalError(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}
