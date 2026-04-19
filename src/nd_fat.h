/*
 * nd_fat.h - public FAT volume entry points for folder exports.
 */

#ifndef ND_FAT_H
#define ND_FAT_H

#include "nd_common.h"

struct nd_fat_volume;

int nd_fat_volume_open (const char *host_dir, const char *label_hint,
                        struct nd_fat_volume **out_volume, char *err,
                        size_t errlen);
uint32_t nd_fat_volume_size (const struct nd_fat_volume *volume);
const uint8_t *nd_fat_volume_boot_sector (const struct nd_fat_volume *volume);
uint8_t nd_fat_volume_media_descriptor (const struct nd_fat_volume *volume);
int nd_fat_volume_read (const struct nd_fat_volume *volume, uint32_t start_sector,
                        uint16_t sector_count, uint8_t *buffer, char *err,
                        size_t errlen);
int nd_fat_volume_materialize (const struct nd_fat_volume *volume, int fd,
                               char *err, size_t errlen);
int nd_fat_volume_sync_to_host (struct nd_fat_volume *volume, int fd,
                                char *err, size_t errlen);
void nd_fat_volume_close (struct nd_fat_volume *volume);

#endif
