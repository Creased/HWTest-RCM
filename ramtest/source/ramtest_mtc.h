/*
 * hwtest dedicated RAM build - universal Erista/Mariko MTC bridge.
 *
 * The training engine behind this API is Atmosphere-NX GPLv2 code.  This
 * interface is intentionally small so the rest of the RAM tester stays C.
 */
#ifndef HWTEST_RAMTEST_MTC_H
#define HWTEST_RAMTEST_MTC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


enum {
    RAM_MTC_PLATFORM_UNKNOWN = 0,
    RAM_MTC_PLATFORM_ERISTA  = 1,
    RAM_MTC_PLATFORM_MARIKO  = 2,
};

enum {
    RAM_MTC_ERR_TABLE          = 1u << 0,
    RAM_MTC_ERR_ENTRY_CLOCK    = 1u << 1,
    RAM_MTC_ERR_PLL_TIMEOUT    = 1u << 2,
    RAM_MTC_ERR_STATUS_TIMEOUT = 1u << 3,
    RAM_MTC_ERR_UNSUPPORTED    = 1u << 4,
};

enum {
    RAM_MTC_CA_TRAINING         = 1u << 0,
    RAM_MTC_CA_VREF_TRAINING    = 1u << 1,
    RAM_MTC_WRITE_TRAINING      = 1u << 4,
    RAM_MTC_WRITE_VREF_TRAINING = 1u << 5,
    RAM_MTC_READ_TRAINING       = 1u << 6,
    RAM_MTC_READ_VREF_TRAINING  = 1u << 7,
    RAM_MTC_SECOND_RANK         = 1u << 8,
    RAM_MTC_BIT_LEVEL_TRAINING  = 1u << 9,
};

/* Select exactly one platform before any training operation. */
void     ram_mtc_select_platform(uint32_t platform);
uint32_t ram_mtc_platform(void);

/* Returns 0 only when memory training reached 1600 MHz with no bounded-wait
 * failure.  On failure, query ram_mtc_errors(). */
int      ram_mtc_train(void);
void     ram_mtc_periodic(void);
void     ram_mtc_restore(void);
uint32_t ram_mtc_errors(void);
uint32_t ram_mtc_failed_training_mask(void);
uint32_t ram_mtc_dram_id(void);

#ifdef __cplusplus
}
#endif
#endif
