/*
 * nd_fat_priv.h - internal FAT volume structures shared by the FAT modules.
 */

#ifndef ND_FAT_PRIV_H
#define ND_FAT_PRIV_H

#include "nd_fat.h"

/*
 * Internal synthesized FAT node type.
 */
enum nd_fat_node_kind
{
  ND_FAT_NODE_FILE = 1,
  ND_FAT_NODE_DIR = 2
};

/*
 * Cluster ownership marker used while materializing the volume.
 */
enum nd_cluster_ref_kind
{
  ND_CLUSTER_REF_ZERO = 0,
  ND_CLUSTER_REF_FILE = 1,
  ND_CLUSTER_REF_DIR = 2
};

/*
 * In-memory representation of a host file or directory.
 */
struct nd_fat_node
{
  enum nd_fat_node_kind kind;
  struct nd_fat_node *parent;
  char *host_name;
  char *host_path;
  char dos_name[11];
  uint8_t attr;
  uint16_t dos_time;
  uint16_t dos_date;
  uint32_t size_bytes;
  uint16_t first_cluster;
  uint16_t cluster_count;
  uint8_t *dir_data;
  size_t dir_data_len;
  struct nd_fat_node **children;
  size_t child_count;
  size_t child_cap;
};

/*
 * Cluster-to-node mapping entry.
 */
struct nd_cluster_ref
{
  enum nd_cluster_ref_kind kind;
  struct nd_fat_node *node;
  uint32_t offset;
};

/*
 * Fully synthesized FAT volume state.
 */
struct nd_fat_volume
{
  struct nd_fat_node *root;
  char volume_label[11];
  uint8_t boot_sector[ND_SECTOR_SIZE];
  uint8_t *fat_bytes;
  uint8_t *root_dir;
  struct nd_cluster_ref *cluster_map;
  uint32_t total_sectors;
  uint32_t total_bytes;
  uint32_t fat1_start;
  uint32_t fat2_start;
  uint32_t root_start;
  uint32_t data_start;
  uint32_t cluster_count_total;
  uint16_t sectors_per_cluster;
  uint16_t sectors_per_fat;
  uint16_t root_dir_entries;
  uint16_t root_dir_sectors;
  uint8_t media_descriptor;
};

/*
 * Internal node cleanup helper.
 */
void nd_fat_node_free (struct nd_fat_node *node);

#endif
