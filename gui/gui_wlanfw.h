/*
 * The CYW4356 firmware and NVRAM, taken from the console's own HOS.
 *
 * HOS does not keep them as files: both are embedded in the wlan sysmodule
 * (title 0100000000000016), in the .data segment of its ExeFS "main". The
 * locations follow nx-fwextract (perillamint/nx-fwextract), the extractor
 * the Switch Linux ports use: the NVRAM is the NUL-terminated text that
 * starts "NVRAMRev=", and the firmware runs from just after the last NUL
 * before the first "DBPP" up to the NUL after its "4356a3-roml" version
 * string.
 *
 * Getting there: prod.keys from the SD card (Lockpick_RCM's dump), the
 * SYSTEM partition through bdk's BIS layer, a read-only FAT32 walk of
 * Contents/registered, each NCA's header decrypted until the wlan program
 * NCA turns up, its key area, its ExeFS (AES-CTR, PFS0), main's NSO header
 * and its LZ4 .data. Nothing is written anywhere but DRAM. A card carrying
 * hwtest/wlan_fw.bin and hwtest/wlan_nvram.txt skips all of it.
 */
#ifndef GUI_WLANFW_H
#define GUI_WLANFW_H

#include "../hwtest.h"

typedef struct {
    u8   *fw;            /* heap, kept for the session                   */
    u32   fw_len;
    u8   *nvram;         /* the text as HOS has it, NUL-terminated        */
    u32   nvram_len;
    char  fw_tail[64];   /* the firmware's own version string            */
    u32   nca_count;     /* NCA headers looked at                        */
    u32   sdk_ver;       /* the wlan NCA's SDK version, for the record   */
} wlanfw_t;

/* Fills *out; 0 on success, else `why` says what stopped it. `step` is
 * called between stages with a short line for the screen (may be NULL). */
int wlanfw_extract(wlanfw_t *out, char *why, u32 why_len,
                   void (*step)(const char *line));

/* The NVRAM text in the chip's binary form (brcmf_fw_nvram_strip): the
 * key=value entries NUL-separated, boardrev added if missing, padded to a
 * word, and the length token at the end. Returns its length, 0 if it does
 * not fit `cap`. */
u32 wlanfw_nvram_binary(const u8 *txt, u32 len, u8 *out, u32 cap);

#endif
