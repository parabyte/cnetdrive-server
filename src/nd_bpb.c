#include "nd_bpb.h"

int
nd_bpb_parse (const uint8_t sector[ND_SECTOR_SIZE], struct nd_bpb *bpb,
              char *err, size_t errlen)
{
  const uint8_t *src;

  if (sector == NULL || bpb == NULL)
    {
      nd_set_error (err, errlen, "bad BPB input");
      return -1;
    }

  src = sector + 11;

  bpb->bytes_per_sector = nd_load_le16 (src + 0);
  bpb->sectors_per_cluster = src[2];
  bpb->reserved_sector_count = nd_load_le16 (src + 3);
  bpb->fats = src[5];
  bpb->root_dir_entries = nd_load_le16 (src + 6);
  bpb->sectors_16 = nd_load_le16 (src + 8);
  bpb->media_descriptor = src[10];
  bpb->sectors_per_fat = nd_load_le16 (src + 11);
  bpb->sectors_per_track = nd_load_le16 (src + 13);
  bpb->heads = nd_load_le16 (src + 15);
  bpb->hidden_sectors = nd_load_le32 (src + 17);
  bpb->sectors_32 = nd_load_le32 (src + 21);

  if (bpb->bytes_per_sector != ND_SECTOR_SIZE)
    {
      nd_set_error (err, errlen, "bytes per sector is not 512");
      return -1;
    }

  if (bpb->sectors_per_cluster == 0
      || bpb->reserved_sector_count == 0
      || (bpb->fats != 1 && bpb->fats != 2)
      || bpb->root_dir_entries == 0
      || bpb->sectors_per_fat == 0)
    {
      nd_set_error (err, errlen, "invalid BPB");
      return -1;
    }

  if (bpb->sectors_16 == 0 && bpb->sectors_32 == 0)
    {
      nd_set_error (err, errlen, "invalid BPB size fields");
      return -1;
    }

  return 0;
}

uint32_t
nd_bpb_total_sectors (const struct nd_bpb *bpb)
{
  if (bpb == NULL)
    return 0;

  return bpb->sectors_16 ? bpb->sectors_16 : bpb->sectors_32;
}
