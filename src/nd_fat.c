/*
 * nd_fat.c - FAT volume synthesis for exported host directories.
 */

#include "nd_fat_priv.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>

static int nd_fat_scan_tree (struct nd_fat_node *node, char *err, size_t errlen);

static int
nd_compare_nodes (const void *left, const void *right)
{
  const struct nd_fat_node *const *a = left;
  const struct nd_fat_node *const *b = right;
  return memcmp ((*a)->dos_name, (*b)->dos_name, sizeof ((*a)->dos_name));
}

static void
nd_make_volume_label (const char *hint, char out[11])
{
  char tmp[12];
  const char *base;
  size_t i;
  size_t j;

  memset (out, ' ', 11);
  memset (tmp, 0, sizeof (tmp));

  base = strrchr (hint, '/');
  base = (base == NULL) ? hint : base + 1;

  j = 0;
  for (i = 0; base[i] != '\0' && j < 11; i++)
    {
      unsigned char ch = (unsigned char) base[i];

      if (isalnum (ch))
        tmp[j++] = (char) toupper (ch);
      else if (ch == '_' || ch == '-')
        tmp[j++] = '_';
    }

  if (j == 0)
    memcpy (tmp, "NETDRIVE", 8);

  for (i = 0; i < 11 && tmp[i] != '\0'; i++)
    out[i] = tmp[i];
}

static void
nd_split_name (const char *name, char *base, size_t base_len,
               char *ext, size_t ext_len)
{
  const char *dot;
  size_t i;
  size_t j;

  dot = strrchr (name, '.');
  if (dot == NULL || dot == name)
    dot = NULL;

  j = 0;
  for (i = 0; name[i] != '\0' && &name[i] != dot && j + 1 < base_len; i++)
    {
      unsigned char ch = (unsigned char) name[i];

      if (isalnum (ch))
        base[j++] = (char) toupper (ch);
      else if (ch != ' ')
        base[j++] = '_';
    }
  base[j] = '\0';

  if (dot != NULL)
    {
      j = 0;
      for (i = 1; dot[i] != '\0' && j + 1 < ext_len; i++)
        {
          unsigned char ch = (unsigned char) dot[i];

          if (isalnum (ch))
            ext[j++] = (char) toupper (ch);
          else if (ch != ' ')
            ext[j++] = '_';
        }
      ext[j] = '\0';
    }
  else if (ext_len > 0)
    ext[0] = '\0';

  if (base[0] == '\0')
    strcpy (base, "FILE");
}

static void
nd_pack_short_name (char out[11], const char *base, const char *ext)
{
  size_t i;

  memset (out, ' ', 11);
  for (i = 0; i < 8 && base[i] != '\0'; i++)
    out[i] = base[i];
  for (i = 0; i < 3 && ext[i] != '\0'; i++)
    out[8 + i] = ext[i];
}

static int
nd_short_name_exists (const struct nd_fat_node *dir, const char name[11])
{
  size_t i;

  for (i = 0; i < dir->child_count; i++)
    {
      if (memcmp (dir->children[i]->dos_name, name, 11) == 0)
        return 1;
    }

  return 0;
}

static void
nd_make_short_name (struct nd_fat_node *dir, const char *host_name,
                    char out[11])
{
  char base[64];
  char ext[16];
  char try_base[9];
  char try_ext[4];
  unsigned int index;
  size_t copy_len;

  nd_split_name (host_name, base, sizeof (base), ext, sizeof (ext));

  memset (try_base, 0, sizeof (try_base));
  memset (try_ext, 0, sizeof (try_ext));
  copy_len = strlen (base);
  if (copy_len > 8)
    copy_len = 8;
  memcpy (try_base, base, copy_len);
  copy_len = strlen (ext);
  if (copy_len > 3)
    copy_len = 3;
  memcpy (try_ext, ext, copy_len);
  nd_pack_short_name (out, try_base, try_ext);

  if (!nd_short_name_exists (dir, out))
    return;

  for (index = 1; index < 100000; index++)
    {
      char number[8];
      size_t digits;
      size_t keep;

      snprintf (number, sizeof (number), "%u", index);
      digits = strlen (number);
      keep = (digits + 1 < 8) ? (8 - digits - 1) : 1;

      memset (try_base, 0, sizeof (try_base));
      copy_len = strlen (base);
      if (copy_len > keep)
        copy_len = keep;
      memcpy (try_base, base, copy_len);
      try_base[keep] = '\0';
      strncat (try_base, "~", sizeof (try_base) - strlen (try_base) - 1);
      strncat (try_base, number, sizeof (try_base) - strlen (try_base) - 1);

      memset (try_ext, 0, sizeof (try_ext));
      copy_len = strlen (ext);
      if (copy_len > 3)
        copy_len = 3;
      memcpy (try_ext, ext, copy_len);
      nd_pack_short_name (out, try_base, try_ext);
      if (!nd_short_name_exists (dir, out))
        return;
    }

  nd_pack_short_name (out, "OVERFLOW", "ERR");
}

static struct nd_fat_node *
nd_fat_node_new (enum nd_fat_node_kind kind, struct nd_fat_node *parent,
                 const char *host_name, const char *host_path,
                 const struct stat *st)
{
  struct nd_fat_node *node;

  node = (struct nd_fat_node *) calloc (1, sizeof (*node));
  if (node == NULL)
    return NULL;

  node->kind = kind;
  node->parent = parent;
  node->host_name = nd_strdup (host_name);
  node->host_path = nd_strdup (host_path);
  node->attr = (kind == ND_FAT_NODE_DIR) ? 0x10u : 0x20u;

  if (st != NULL)
    {
      node->dos_time = nd_dos_time_from_unix (st->st_mtime);
      node->dos_date = nd_dos_date_from_unix (st->st_mtime);
      node->size_bytes = (kind == ND_FAT_NODE_FILE) ? (uint32_t) st->st_size : 0;
    }

  if (node->host_name == NULL || node->host_path == NULL)
    {
      nd_fat_node_free (node);
      return NULL;
    }

  return node;
}

static int
nd_fat_node_add_child (struct nd_fat_node *dir, struct nd_fat_node *child)
{
  struct nd_fat_node **grown;
  size_t new_cap;

  if (dir->child_count == dir->child_cap)
    {
      new_cap = (dir->child_cap == 0) ? 8 : dir->child_cap * 2;
      grown = (struct nd_fat_node **) realloc (dir->children,
                                               new_cap * sizeof (*grown));
      if (grown == NULL)
        return -1;

      dir->children = grown;
      dir->child_cap = new_cap;
    }

  dir->children[dir->child_count++] = child;
  return 0;
}

void
nd_fat_node_free (struct nd_fat_node *node)
{
  size_t i;

  if (node == NULL)
    return;

  for (i = 0; i < node->child_count; i++)
    nd_fat_node_free (node->children[i]);

  free (node->children);
  free (node->dir_data);
  free (node->host_name);
  free (node->host_path);
  free (node);
}

static int
nd_fat_scan_tree (struct nd_fat_node *node, char *err, size_t errlen)
{
  DIR *dirp;
  struct dirent *entry;

  dirp = opendir (node->host_path);
  if (dirp == NULL)
    {
      nd_set_error (err, errlen, "unable to open directory: %s",
                    node->host_path);
      return -1;
    }

  while ((entry = readdir (dirp)) != NULL)
    {
      char child_path[ND_PATH_MAX];
      struct stat st;
      enum nd_fat_node_kind kind;
      struct nd_fat_node *child;

      if (strcmp (entry->d_name, ".") == 0 || strcmp (entry->d_name, "..") == 0)
        continue;

      if (snprintf (child_path, sizeof (child_path), "%s/%s",
                    node->host_path, entry->d_name) >= (int) sizeof (child_path))
        {
          closedir (dirp);
          nd_set_error (err, errlen, "path too long inside export");
          return -1;
        }

      if (lstat (child_path, &st) != 0)
        {
          closedir (dirp);
          nd_set_error (err, errlen, "unable to stat export entry");
          return -1;
        }

      if (S_ISLNK (st.st_mode))
        continue;

      if (S_ISDIR (st.st_mode))
        kind = ND_FAT_NODE_DIR;
      else if (S_ISREG (st.st_mode))
        kind = ND_FAT_NODE_FILE;
      else
        continue;

      if ((uint64_t) st.st_size > UINT32_MAX)
        {
          closedir (dirp);
          nd_set_error (err, errlen, "file too large for FAT export: %s",
                        entry->d_name);
          return -1;
        }

      child = nd_fat_node_new (kind, node, entry->d_name, child_path, &st);
      if (child == NULL)
        {
          closedir (dirp);
          nd_set_error (err, errlen, "out of memory");
          return -1;
        }

      nd_make_short_name (node, entry->d_name, child->dos_name);

      if (nd_fat_node_add_child (node, child) != 0)
        {
          nd_fat_node_free (child);
          closedir (dirp);
          nd_set_error (err, errlen, "out of memory");
          return -1;
        }

      if (kind == ND_FAT_NODE_DIR)
        {
          if (nd_fat_scan_tree (child, err, errlen) != 0)
            {
              closedir (dirp);
              return -1;
            }
        }
    }

  closedir (dirp);

  if (node->child_count > 1)
    qsort (node->children, node->child_count, sizeof (node->children[0]),
           nd_compare_nodes);

  return 0;
}

static uint32_t
nd_round_up_u32 (uint32_t value, uint32_t quantum)
{
  return ((value + quantum - 1) / quantum) * quantum;
}

static uint32_t
nd_assign_cluster_counts (struct nd_fat_node *node, uint16_t sectors_per_cluster)
{
  uint32_t cluster_bytes;
  uint32_t total;
  size_t i;

  cluster_bytes = (uint32_t) sectors_per_cluster * ND_SECTOR_SIZE;
  total = 0;

  if (node->kind == ND_FAT_NODE_FILE)
    {
      node->dir_data_len = 0;
      node->cluster_count = (uint16_t)
        ((node->size_bytes == 0)
         ? 0
         : ((node->size_bytes + cluster_bytes - 1) / cluster_bytes));
      return node->cluster_count;
    }

  if (node->parent != NULL)
    {
      node->dir_data_len = nd_round_up_u32 ((uint32_t) ((node->child_count + 2) * 32),
                                            cluster_bytes);
      if (node->dir_data_len == 0)
        node->dir_data_len = cluster_bytes;
      node->cluster_count = (uint16_t) (node->dir_data_len / cluster_bytes);
      total += node->cluster_count;
    }
  else
    {
      node->dir_data_len = 0;
      node->cluster_count = 0;
    }

  for (i = 0; i < node->child_count; i++)
    total += nd_assign_cluster_counts (node->children[i], sectors_per_cluster);

  return total;
}

static uint16_t
nd_root_entries_for (size_t top_level_entries)
{
  uint32_t entries;

  entries = (uint32_t) top_level_entries + 1;
  if (entries < 32)
    entries = 32;

  entries = nd_round_up_u32 (entries, 16);
  if (entries > 65535)
    entries = 65535;

  return (uint16_t) entries;
}

static int
nd_plan_geometry (struct nd_fat_volume *volume, char *err, size_t errlen)
{
  static const uint16_t candidates[] = { 1, 2, 4, 8, 16, 32, 64 };
  size_t i;

  for (i = 0; i < ND_ARRAY_LEN (candidates); i++)
    {
      uint32_t data_clusters_needed;
      uint32_t cluster_count_total;
      uint32_t fat_entries;
      uint32_t fat_bytes;
      uint32_t fat_sectors;
      uint32_t root_entries;
      uint32_t root_dir_sectors;
      uint64_t total_sectors;
      uint64_t total_bytes;

      data_clusters_needed = nd_assign_cluster_counts (volume->root,
                                                       candidates[i]);
      if (data_clusters_needed > 65524u)
        continue;

      cluster_count_total = (data_clusters_needed < 4096u)
        ? 4096u : data_clusters_needed;
      fat_entries = cluster_count_total + 2u;
      fat_bytes = fat_entries * 2u;
      fat_sectors = nd_round_up_u32 (fat_bytes, ND_SECTOR_SIZE) / ND_SECTOR_SIZE;
      root_entries = nd_root_entries_for (volume->root->child_count);
      root_dir_sectors = (root_entries * 32u) / ND_SECTOR_SIZE;

      total_sectors = 1u + fat_sectors * 2u + root_dir_sectors
        + (uint64_t) cluster_count_total * candidates[i];
      total_bytes = total_sectors * ND_SECTOR_SIZE;

      if (total_bytes > UINT32_MAX)
        continue;

      volume->sectors_per_cluster = candidates[i];
      volume->cluster_count_total = cluster_count_total;
      volume->sectors_per_fat = (uint16_t) fat_sectors;
      volume->root_dir_entries = (uint16_t) root_entries;
      volume->root_dir_sectors = (uint16_t) root_dir_sectors;
      volume->total_sectors = (uint32_t) total_sectors;
      volume->total_bytes = (uint32_t) total_bytes;
      volume->fat1_start = 1u;
      volume->fat2_start = volume->fat1_start + fat_sectors;
      volume->root_start = volume->fat2_start + fat_sectors;
      volume->data_start = volume->root_start + root_dir_sectors;
      return 0;
    }

  nd_set_error (err, errlen, "folder export is too large for FAT16");
  return -1;
}

static void
nd_write_dir_entry (uint8_t *entry, const char name[11], uint8_t attr,
                    uint16_t dos_time, uint16_t dos_date,
                    uint16_t first_cluster, uint32_t size_bytes)
{
  memset (entry, 0, 32);
  memcpy (entry, name, 11);
  entry[11] = attr;
  nd_store_le16 (entry + 22, dos_time);
  nd_store_le16 (entry + 24, dos_date);
  nd_store_le16 (entry + 26, first_cluster);
  nd_store_le32 (entry + 28, size_bytes);
}

static int
nd_allocate_clusters_rec (struct nd_fat_node *node, uint32_t cluster_bytes,
                          uint16_t *fat_entries,
                          struct nd_cluster_ref *cluster_map,
                          uint16_t cluster_limit, uint16_t *next_cluster,
                          char *err, size_t errlen)
{
  size_t i;

  for (i = 0; i < node->child_count; i++)
    {
      struct nd_fat_node *child;
      uint16_t cluster;
      uint16_t j;

      child = node->children[i];
      child->first_cluster = 0;

      if (child->cluster_count > 0)
        {
          if ((uint32_t) *next_cluster + child->cluster_count - 1u > cluster_limit + 1u)
            {
              nd_set_error (err, errlen, "internal FAT allocation error");
              return -1;
            }

          child->first_cluster = *next_cluster;
          for (j = 0; j < child->cluster_count; j++)
            {
              cluster = (uint16_t) (*next_cluster + j);
              fat_entries[cluster] = (j + 1 < child->cluster_count)
                ? (uint16_t) (cluster + 1) : 0xffffu;
              cluster_map[cluster].kind = (child->kind == ND_FAT_NODE_DIR)
                ? ND_CLUSTER_REF_DIR : ND_CLUSTER_REF_FILE;
              cluster_map[cluster].node = child;
              cluster_map[cluster].offset = (uint32_t) j * cluster_bytes;
            }
          *next_cluster = (uint16_t) (*next_cluster + child->cluster_count);
        }

      if (child->kind == ND_FAT_NODE_DIR)
        {
          if (nd_allocate_clusters_rec (child, cluster_bytes, fat_entries,
                                        cluster_map, cluster_limit, next_cluster,
                                        err, errlen) != 0)
            return -1;
        }
    }

  return 0;
}

static int
nd_build_subdir_data (struct nd_fat_node *node, uint32_t cluster_bytes,
                      char *err, size_t errlen)
{
  uint8_t dot_name[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
  uint8_t dotdot_name[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
  size_t i;

  if (node->kind != ND_FAT_NODE_DIR)
    return 0;

  if (node->parent != NULL)
    {
      node->dir_data = (uint8_t *) calloc (1, node->cluster_count * cluster_bytes);
      if (node->dir_data == NULL)
        {
          nd_set_error (err, errlen, "out of memory");
          return -1;
        }

      nd_write_dir_entry (node->dir_data, (const char *) dot_name, 0x10u,
                          node->dos_time, node->dos_date, node->first_cluster, 0);
      nd_write_dir_entry (node->dir_data + 32, (const char *) dotdot_name, 0x10u,
                          node->parent->dos_time, node->parent->dos_date,
                          node->parent->parent == NULL ? 0 : node->parent->first_cluster,
                          0);

      for (i = 0; i < node->child_count; i++)
        {
          struct nd_fat_node *child = node->children[i];
          nd_write_dir_entry (node->dir_data + ((i + 2) * 32), child->dos_name,
                              child->attr, child->dos_time, child->dos_date,
                              child->first_cluster,
                              (child->kind == ND_FAT_NODE_FILE)
                              ? child->size_bytes : 0);
        }
    }

  for (i = 0; i < node->child_count; i++)
    {
      if (node->children[i]->kind == ND_FAT_NODE_DIR)
        {
          if (nd_build_subdir_data (node->children[i], cluster_bytes,
                                    err, errlen) != 0)
            return -1;
        }
    }

  return 0;
}

static int
nd_build_root_dir (struct nd_fat_volume *volume, char *err, size_t errlen)
{
  size_t i;

  volume->root_dir = (uint8_t *) calloc (1,
                                         (size_t) volume->root_dir_sectors
                                         * ND_SECTOR_SIZE);
  if (volume->root_dir == NULL)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  nd_write_dir_entry (volume->root_dir, volume->volume_label, 0x08u,
                      volume->root->dos_time, volume->root->dos_date, 0, 0);

  for (i = 0; i < volume->root->child_count; i++)
    {
      struct nd_fat_node *child = volume->root->children[i];
      nd_write_dir_entry (volume->root_dir + ((i + 1) * 32), child->dos_name,
                          child->attr, child->dos_time, child->dos_date,
                          child->first_cluster,
                          (child->kind == ND_FAT_NODE_FILE)
                          ? child->size_bytes : 0);
    }

  return 0;
}

static int
nd_build_fat_bytes (struct nd_fat_volume *volume, char *err, size_t errlen)
{
  uint16_t *fat_entries;
  uint32_t cluster_bytes;
  uint16_t next_cluster;
  uint32_t entry_count;
  uint32_t i;

  entry_count = volume->cluster_count_total + 2u;
  fat_entries = (uint16_t *) calloc (entry_count, sizeof (*fat_entries));
  volume->cluster_map = (struct nd_cluster_ref *)
    calloc (entry_count, sizeof (*volume->cluster_map));
  volume->fat_bytes = (uint8_t *) calloc ((size_t) volume->sectors_per_fat
                                          * ND_SECTOR_SIZE, 1);
  if (fat_entries == NULL || volume->cluster_map == NULL || volume->fat_bytes == NULL)
    {
      free (fat_entries);
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  fat_entries[0] = (uint16_t) (0xff00u | volume->media_descriptor);
  fat_entries[1] = 0xffffu;

  cluster_bytes = (uint32_t) volume->sectors_per_cluster * ND_SECTOR_SIZE;
  next_cluster = 2;
  if (nd_allocate_clusters_rec (volume->root, cluster_bytes, fat_entries,
                                volume->cluster_map,
                                (uint16_t) volume->cluster_count_total,
                                &next_cluster, err, errlen) != 0)
    {
      free (fat_entries);
      return -1;
    }

  for (i = 0; i < entry_count; i++)
    nd_store_le16 (volume->fat_bytes + i * 2u, fat_entries[i]);

  free (fat_entries);
  return 0;
}

static void
nd_build_boot_sector (struct nd_fat_volume *volume)
{
  time_t now;
  uint32_t serial;

  memset (volume->boot_sector, 0, sizeof (volume->boot_sector));

  volume->boot_sector[0] = 0xeb;
  volume->boot_sector[1] = 0x3c;
  volume->boot_sector[2] = 0x90;
  memcpy (volume->boot_sector + 3, "NETDRIVE", 8);
  nd_store_le16 (volume->boot_sector + 11, ND_SECTOR_SIZE);
  volume->boot_sector[13] = (uint8_t) volume->sectors_per_cluster;
  nd_store_le16 (volume->boot_sector + 14, 1);
  volume->boot_sector[16] = 2;
  nd_store_le16 (volume->boot_sector + 17, volume->root_dir_entries);

  if (volume->total_sectors <= 65535u)
    nd_store_le16 (volume->boot_sector + 19, (uint16_t) volume->total_sectors);
  else
    nd_store_le16 (volume->boot_sector + 19, 0);

  volume->boot_sector[21] = volume->media_descriptor;
  nd_store_le16 (volume->boot_sector + 22, volume->sectors_per_fat);
  nd_store_le16 (volume->boot_sector + 24, 32);
  nd_store_le16 (volume->boot_sector + 26, 64);
  nd_store_le32 (volume->boot_sector + 28, 0);
  nd_store_le32 (volume->boot_sector + 32,
                 (volume->total_sectors > 65535u) ? volume->total_sectors : 0);
  volume->boot_sector[36] = 0x80;
  volume->boot_sector[38] = 0x29;

  now = time (NULL);
  serial = (uint32_t) now ^ 0x4e445256u;
  nd_store_le32 (volume->boot_sector + 39, serial);
  memcpy (volume->boot_sector + 43, volume->volume_label, 11);
  memcpy (volume->boot_sector + 54, "FAT16   ", 8);
  volume->boot_sector[510] = 0x55;
  volume->boot_sector[511] = 0xaa;
}

static int
nd_read_file_sector (const char *path, uint32_t offset, uint8_t *buffer,
                     char *err, size_t errlen)
{
  int fd;
  ssize_t got;

  fd = open (path, O_RDONLY);
  if (fd < 0)
    {
      nd_set_error (err, errlen, "read error");
      return -1;
    }

  memset (buffer, 0, ND_SECTOR_SIZE);
  got = nd_pread_full (fd, buffer, ND_SECTOR_SIZE, (off_t) offset);
  close (fd);

  if (got < 0)
    {
      nd_set_error (err, errlen, "read error");
      return -1;
    }

  return 0;
}

int
nd_fat_volume_open (const char *host_dir, const char *label_hint,
                    struct nd_fat_volume **out_volume, char *err,
                    size_t errlen)
{
  struct stat st;
  struct nd_fat_volume *volume;
  struct nd_fat_node *root;
  uint32_t cluster_bytes;

  if (stat (host_dir, &st) != 0 || !S_ISDIR (st.st_mode))
    {
      nd_set_error (err, errlen, "unable to open directory export");
      return -1;
    }

  volume = (struct nd_fat_volume *) calloc (1, sizeof (*volume));
  root = nd_fat_node_new (ND_FAT_NODE_DIR, NULL, label_hint, host_dir, &st);
  if (volume == NULL || root == NULL)
    {
      free (volume);
      nd_fat_node_free (root);
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  volume->root = root;
  volume->media_descriptor = 0xf8u;
  nd_make_volume_label (label_hint, volume->volume_label);

  if (nd_fat_scan_tree (volume->root, err, errlen) != 0)
    {
      nd_fat_volume_close (volume);
      return -1;
    }

  if (nd_plan_geometry (volume, err, errlen) != 0)
    {
      nd_fat_volume_close (volume);
      return -1;
    }

  if (nd_build_fat_bytes (volume, err, errlen) != 0)
    {
      nd_fat_volume_close (volume);
      return -1;
    }

  cluster_bytes = (uint32_t) volume->sectors_per_cluster * ND_SECTOR_SIZE;
  if (nd_build_subdir_data (volume->root, cluster_bytes, err, errlen) != 0)
    {
      nd_fat_volume_close (volume);
      return -1;
    }

  if (nd_build_root_dir (volume, err, errlen) != 0)
    {
      nd_fat_volume_close (volume);
      return -1;
    }

  nd_build_boot_sector (volume);

  *out_volume = volume;
  return 0;
}

uint32_t
nd_fat_volume_size (const struct nd_fat_volume *volume)
{
  return volume->total_bytes;
}

const uint8_t *
nd_fat_volume_boot_sector (const struct nd_fat_volume *volume)
{
  return volume->boot_sector;
}

uint8_t
nd_fat_volume_media_descriptor (const struct nd_fat_volume *volume)
{
  return volume->media_descriptor;
}

int
nd_fat_volume_read (const struct nd_fat_volume *volume, uint32_t start_sector,
                    uint16_t sector_count, uint8_t *buffer, char *err,
                    size_t errlen)
{
  uint16_t i;

  for (i = 0; i < sector_count; i++)
    {
      uint32_t lba;
      uint8_t *dst;

      lba = start_sector + i;
      dst = buffer + (size_t) i * ND_SECTOR_SIZE;

      if (lba >= volume->total_sectors)
        {
          nd_set_error (err, errlen, "Attempted read past end of image");
          return -1;
        }

      if (lba == 0)
        {
          memcpy (dst, volume->boot_sector, ND_SECTOR_SIZE);
        }
      else if (lba >= volume->fat1_start
               && lba < volume->fat1_start + volume->sectors_per_fat)
        {
          memcpy (dst,
                  volume->fat_bytes + (size_t) (lba - volume->fat1_start)
                  * ND_SECTOR_SIZE,
                  ND_SECTOR_SIZE);
        }
      else if (lba >= volume->fat2_start
               && lba < volume->fat2_start + volume->sectors_per_fat)
        {
          memcpy (dst,
                  volume->fat_bytes + (size_t) (lba - volume->fat2_start)
                  * ND_SECTOR_SIZE,
                  ND_SECTOR_SIZE);
        }
      else if (lba >= volume->root_start
               && lba < volume->root_start + volume->root_dir_sectors)
        {
          memcpy (dst,
                  volume->root_dir + (size_t) (lba - volume->root_start)
                  * ND_SECTOR_SIZE,
                  ND_SECTOR_SIZE);
        }
      else
        {
          uint32_t rel_sector;
          uint32_t cluster_index;
          uint32_t sector_in_cluster;
          const struct nd_cluster_ref *ref;

          rel_sector = lba - volume->data_start;
          cluster_index = rel_sector / volume->sectors_per_cluster + 2u;
          sector_in_cluster = rel_sector % volume->sectors_per_cluster;

          if (cluster_index >= volume->cluster_count_total + 2u)
            {
              memset (dst, 0, ND_SECTOR_SIZE);
              continue;
            }

          ref = &volume->cluster_map[cluster_index];
          switch (ref->kind)
            {
            case ND_CLUSTER_REF_ZERO:
              memset (dst, 0, ND_SECTOR_SIZE);
              break;

            case ND_CLUSTER_REF_DIR:
              {
                uint32_t offset = ref->offset + sector_in_cluster * ND_SECTOR_SIZE;

                memset (dst, 0, ND_SECTOR_SIZE);
                if (offset < ref->node->dir_data_len)
                  memcpy (dst, ref->node->dir_data + offset, ND_SECTOR_SIZE);
              }
              break;

            case ND_CLUSTER_REF_FILE:
              if (nd_read_file_sector (ref->node->host_path,
                                       ref->offset + sector_in_cluster * ND_SECTOR_SIZE,
                                       dst, err, errlen) != 0)
                return -1;
              break;
            }
        }
    }

  return 0;
}

void
nd_fat_volume_close (struct nd_fat_volume *volume)
{
  if (volume == NULL)
    return;

  nd_fat_node_free (volume->root);
  free (volume->fat_bytes);
  free (volume->root_dir);
  free (volume->cluster_map);
  free (volume);
}
