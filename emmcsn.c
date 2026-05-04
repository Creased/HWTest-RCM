/*
 * Hekate-style emmcsn path helper.
 *
 * Builds `backup/<emmc_serial_hex>/<sub_dir>/<filename>` exactly the way
 * Nyx and Hekate's frontend tools do, so an hwtest report drops next to
 * any existing emmc backup the same console produced.
 *
 * Source: hekate/nyx/nyx_gui/nyx.c::emmcsn_path_impl, vendored verbatim
 * with the Nyx-only fallback path removed (we always pass an explicit
 * `storage` argument).
 */

#include <bdk.h>
#include <storage/sdmmc.h>
#include <libs/fatfs/ff.h>

#include <string.h>

extern void itoa(int value, char *sp, int radix);   /* bdk/utils/sprintf.c */

char *emmcsn_path_impl(char *path, char *sub_dir, char *filename,
                       sdmmc_storage_t *storage)
{
    static char emmc_sn[9] = {0};

    /* Cache the serial across calls. */
    if (!emmc_sn[0] || !strcmp(emmc_sn, "00000000"))
        itoa(storage->cid.serial, emmc_sn, 16);

    /* If the caller only asked for the serial, return it. */
    if (!path)
        return emmc_sn;

    /* backup/ */
    strcpy(path, "backup");
    f_mkdir(path);

    /* backup/<serial>/ */
    strcat(path, "/");
    strcat(path, emmc_sn);
    f_mkdir(path);

    /* optional sub-directory, expected to start with '/' if non-empty */
    strcat(path, sub_dir);
    if (sub_dir && *sub_dir)
        f_mkdir(path);

    /* /filename */
    strcat(path, "/");
    strcat(path, filename);

    return emmc_sn;
}
