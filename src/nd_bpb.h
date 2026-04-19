/*
 * nd_bpb.h - BIOS parameter block declarations.
 */

#ifndef ND_BPB_H
#define ND_BPB_H

#include "nd_common.h"

struct nd_bpb
{
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sector_count;
  uint8_t fats;
  uint16_t root_dir_entries;
  uint16_t sectors_16;
  uint8_t media_descriptor;
  uint16_t sectors_per_fat;
  uint16_t sectors_per_track;
  uint16_t heads;
  uint32_t hidden_sectors;
  uint32_t sectors_32;
};

int nd_bpb_parse (const uint8_t sector[ND_SECTOR_SIZE], struct nd_bpb *bpb,
                  char *err, size_t errlen);
uint32_t nd_bpb_total_sectors (const struct nd_bpb *bpb);

#endif
