#include "nd_fat_priv.h"

#include "nd_bpb.h"

#include <dirent.h>
#include <fcntl.h>
#include <utime.h>

/*
 * nd_fat_sync.c
 *
 * Reverse path for folder exports.  After the client writes sectors into
 * the temporary FAT image, this file parses the mutated FAT structures
 * and reconciles the host directory tree immediately.
 */

struct nd_sync_entry
{
  enum nd_fat_node_kind kind;
  char dos_name[11];
  uint8_t attr;
  uint16_t dos_time;
  uint16_t dos_date;
  uint16_t first_cluster;
  uint32_t size_bytes;
  struct nd_sync_entry **children;
  size_t child_count;
  size_t child_cap;
};

static int
nd_write_all_at (int fd, const void *buffer, size_t count, off_t offset)
{
  const uint8_t *src;
  size_t done;

  src = (const uint8_t *) buffer;
  done = 0;

  while (done < count)
    {
      ssize_t wrote;

      if (lseek (fd, offset + (off_t) done, SEEK_SET) < 0)
        return -1;

      wrote = write (fd, src + done, count - done);
      if (wrote <= 0)
        return -1;
      done += (size_t) wrote;
    }

  return 0;
}

static int
nd_make_temp_path (const char *host_path, char *temp_path, size_t temp_path_len)
{
  const char *slash;
  const char *name;
  size_t dir_len;

  slash = strrchr (host_path, '/');
  if (slash == NULL)
    {
      name = host_path;
      dir_len = 0;
    }
  else
    {
      name = slash + 1;
      dir_len = (size_t) (slash - host_path);
    }

  if (dir_len == 0)
    return snprintf (temp_path, temp_path_len, "./.%s.tmp-XXXXXX", name)
      < (int) temp_path_len ? 0 : -1;

  return snprintf (temp_path, temp_path_len, "%.*s/.%s.tmp-XXXXXX",
                   (int) dir_len, host_path, name) < (int) temp_path_len
    ? 0 : -1;
}

/*
 * The materialized image is the protocol-visible disk.  This helper lays
 * down the initial synthesized FAT volume sector-by-sector so later
 * writes can be applied against a normal temporary file.
 */
int
nd_fat_volume_materialize (const struct nd_fat_volume *volume, int fd,
                           char *err, size_t errlen)
{
  uint8_t sector[ND_SECTOR_SIZE];
  uint32_t lba;

  for (lba = 0; lba < volume->total_sectors; lba++)
    {
      if (nd_fat_volume_read (volume, lba, 1, sector, err, errlen) != 0)
        return -1;

      if (nd_write_all_at (fd, sector, sizeof (sector),
                           (off_t) lba * ND_SECTOR_SIZE) != 0)
        {
          nd_set_error (err, errlen, "failed to materialize FAT image");
          return -1;
        }
    }

  return 0;
}

static struct nd_sync_entry *
nd_sync_entry_new (enum nd_fat_node_kind kind)
{
  struct nd_sync_entry *entry;

  entry = (struct nd_sync_entry *) calloc (1, sizeof (*entry));
  if (entry == NULL)
    return NULL;

  entry->kind = kind;
  return entry;
}

static void
nd_sync_entry_free (struct nd_sync_entry *entry)
{
  size_t i;

  if (entry == NULL)
    return;

  for (i = 0; i < entry->child_count; i++)
    nd_sync_entry_free (entry->children[i]);

  free (entry->children);
  free (entry);
}

static int
nd_sync_entry_add_child (struct nd_sync_entry *parent,
                         struct nd_sync_entry *child)
{
  struct nd_sync_entry **grown;
  size_t new_cap;

  if (parent->child_count == parent->child_cap)
    {
      new_cap = (parent->child_cap == 0) ? 8 : parent->child_cap * 2;
      grown = (struct nd_sync_entry **) realloc (parent->children,
                                                 new_cap * sizeof (*grown));
      if (grown == NULL)
        return -1;

      parent->children = grown;
      parent->child_cap = new_cap;
    }

  parent->children[parent->child_count++] = child;
  return 0;
}

static int
nd_sync_map_add_child (struct nd_fat_node *parent, struct nd_fat_node *child)
{
  struct nd_fat_node **grown;
  size_t new_cap;

  if (parent->child_count == parent->child_cap)
    {
      new_cap = (parent->child_cap == 0) ? 8 : parent->child_cap * 2;
      grown = (struct nd_fat_node **) realloc (parent->children,
                                               new_cap * sizeof (*grown));
      if (grown == NULL)
        return -1;

      parent->children = grown;
      parent->child_cap = new_cap;
    }

  parent->children[parent->child_count++] = child;
  return 0;
}

static uint16_t
nd_sync_get_fat_entry (const uint8_t *fat, uint16_t cluster)
{
  return nd_load_le16 (fat + (uint32_t) cluster * 2u);
}

static int
nd_sync_decode_name (const char dos_name[11], char *out, size_t outlen)
{
  char base[9];
  char ext[4];
  size_t i;
  size_t ext_len;

  memset (base, 0, sizeof (base));
  memset (ext, 0, sizeof (ext));

  for (i = 0; i < 8 && dos_name[i] != ' '; i++)
    base[i] = dos_name[i];
  for (i = 0; i < 3 && dos_name[8 + i] != ' '; i++)
    ext[i] = dos_name[8 + i];

  ext_len = strlen (ext);

  if (ext_len > 0)
    return snprintf (out, outlen, "%s.%s", base, ext) < (int) outlen ? 0 : -1;

  return snprintf (out, outlen, "%s", base) < (int) outlen ? 0 : -1;
}

static const struct nd_fat_node *
nd_find_initial_child (const struct nd_fat_node *dir, const char dos_name[11])
{
  size_t i;

  if (dir == NULL)
    return NULL;

  for (i = 0; i < dir->child_count; i++)
    {
      if (memcmp (dir->children[i]->dos_name, dos_name, 11) == 0)
        return dir->children[i];
    }

  return NULL;
}

static const struct nd_sync_entry *
nd_find_sync_child (const struct nd_sync_entry *dir, const char dos_name[11])
{
  size_t i;

  if (dir == NULL)
    return NULL;

  for (i = 0; i < dir->child_count; i++)
    {
      if (memcmp (dir->children[i]->dos_name, dos_name, 11) == 0)
        return dir->children[i];
    }

  return NULL;
}

static struct nd_fat_node *
nd_sync_build_map_node (enum nd_fat_node_kind kind, struct nd_fat_node *parent,
                        const char *host_name, const char *host_path,
                        const char dos_name[11], char *err, size_t errlen)
{
  struct nd_fat_node *node;

  node = (struct nd_fat_node *) calloc (1, sizeof (*node));
  if (node == NULL)
    {
      nd_set_error (err, errlen, "out of memory");
      return NULL;
    }

  node->kind = kind;
  node->parent = parent;
  node->host_name = nd_strdup (host_name == NULL ? "" : host_name);
  node->host_path = nd_strdup (host_path == NULL ? "" : host_path);
  if (node->host_name == NULL || node->host_path == NULL)
    {
      nd_fat_node_free (node);
      nd_set_error (err, errlen, "out of memory");
      return NULL;
    }

  if (dos_name != NULL)
    memcpy (node->dos_name, dos_name, sizeof (node->dos_name));
  else
    memset (node->dos_name, ' ', sizeof (node->dos_name));

  return node;
}

/*
 * Mapping rebuild preserves as much of the original host naming as it
 * can.  Existing DOS names are matched back to the initial tree so a
 * rewrite does not gratuitously rename files on the Unix side.
 */
static struct nd_fat_node *
nd_sync_build_mapping_tree (const struct nd_sync_entry *current_dir,
                            const struct nd_fat_node *previous_dir,
                            struct nd_fat_node *parent,
                            const char *host_name, const char *host_path,
                            char *err, size_t errlen)
{
  struct nd_fat_node *dir_node;
  size_t i;

  dir_node = nd_sync_build_map_node (ND_FAT_NODE_DIR, parent, host_name,
                                     host_path, NULL, err, errlen);
  if (dir_node == NULL)
    return NULL;

  for (i = 0; i < current_dir->child_count; i++)
    {
      const struct nd_sync_entry *child = current_dir->children[i];
      const struct nd_fat_node *previous_child;
      struct nd_fat_node *mapped_child;
      char mapped_name[ND_PATH_MAX];
      char mapped_path[ND_PATH_MAX];

      previous_child = nd_find_initial_child (previous_dir, child->dos_name);
      if (previous_child != NULL)
        snprintf (mapped_name, sizeof (mapped_name), "%s",
                  previous_child->host_name);
      else if (nd_sync_decode_name (child->dos_name, mapped_name,
                                    sizeof (mapped_name)) != 0)
        {
          nd_fat_node_free (dir_node);
          nd_set_error (err, errlen, "DOS filename too long for host sync");
          return NULL;
        }

      if (snprintf (mapped_path, sizeof (mapped_path), "%s/%s",
                    host_path, mapped_name) >= (int) sizeof (mapped_path))
        {
          nd_fat_node_free (dir_node);
          nd_set_error (err, errlen, "host path too long during sync");
          return NULL;
        }

      if (child->kind == ND_FAT_NODE_DIR)
        mapped_child = nd_sync_build_mapping_tree (child, previous_child,
                                                   dir_node, mapped_name,
                                                   mapped_path, err, errlen);
      else
        mapped_child = nd_sync_build_map_node (ND_FAT_NODE_FILE, dir_node,
                                               mapped_name, mapped_path,
                                               child->dos_name,
                                               err, errlen);

      if (mapped_child == NULL)
        {
          nd_fat_node_free (dir_node);
          return NULL;
        }

      memcpy (mapped_child->dos_name, child->dos_name,
              sizeof (mapped_child->dos_name));
      if (nd_sync_map_add_child (dir_node, mapped_child) != 0)
        {
          nd_fat_node_free (mapped_child);
          nd_fat_node_free (dir_node);
          nd_set_error (err, errlen, "out of memory");
          return NULL;
        }
    }

  return dir_node;
}

static int
nd_remove_tree (const char *path)
{
  struct stat st;

  if (lstat (path, &st) != 0)
    return 0;

  if (S_ISDIR (st.st_mode))
    {
      DIR *dirp;
      struct dirent *entry;

      dirp = opendir (path);
      if (dirp == NULL)
        return -1;

      while ((entry = readdir (dirp)) != NULL)
        {
          char child_path[ND_PATH_MAX];

          if (strcmp (entry->d_name, ".") == 0 || strcmp (entry->d_name, "..") == 0)
            continue;

          if (snprintf (child_path, sizeof (child_path), "%s/%s",
                        path, entry->d_name) >= (int) sizeof (child_path))
            {
              closedir (dirp);
              return -1;
            }

          if (nd_remove_tree (child_path) != 0)
            {
              closedir (dirp);
              return -1;
            }
        }

      closedir (dirp);
      return rmdir (path);
    }

  return unlink (path);
}

static int
nd_apply_timestamp (const char *path, uint16_t dos_date, uint16_t dos_time)
{
  struct utimbuf utb;
  struct tm tmv;
  time_t when;

  memset (&tmv, 0, sizeof (tmv));
  tmv.tm_year = ((dos_date >> 9) & 0x7f) + 80;
  tmv.tm_mon = ((dos_date >> 5) & 0x0f) - 1;
  tmv.tm_mday = dos_date & 0x1f;
  tmv.tm_hour = (dos_time >> 11) & 0x1f;
  tmv.tm_min = (dos_time >> 5) & 0x3f;
  tmv.tm_sec = (dos_time & 0x1f) * 2;

  if (tmv.tm_mon < 0 || tmv.tm_mday == 0)
    return 0;

  when = mktime (&tmv);
  if (when == (time_t) -1)
    return 0;

  utb.actime = when;
  utb.modtime = when;
  return utime (path, &utb);
}

/*
 * File write-back copies the cluster chain out of the temporary FAT
 * image into a host temp file, preserves the prior mode when possible,
 * and then atomically renames the finished file into place.
 */
static int
nd_sync_copy_file (const struct nd_fat_volume *volume, int fd,
                   const uint8_t *fat, const struct nd_sync_entry *entry,
                   const char *host_path, char *err, size_t errlen)
{
  char temp_path[ND_PATH_MAX];
  struct stat st;
  int outfd;
  uint16_t cluster;
  uint32_t copied;
  uint32_t cluster_bytes;
  mode_t mode;
  int rc;

  if (nd_make_temp_path (host_path, temp_path, sizeof (temp_path)) != 0)
    {
      nd_set_error (err, errlen, "host path too long during sync");
      return -1;
    }

  outfd = mkstemp (temp_path);
  if (outfd < 0)
    {
      nd_set_error (err, errlen, "unable to open host file for write");
      return -1;
    }

  mode = 0644;
  if (stat (host_path, &st) == 0)
    mode = st.st_mode & 07777;

  if (fchmod (outfd, mode) != 0)
    {
      nd_set_error (err, errlen, "unable to set host file mode");
      goto cleanup;
    }

  copied = 0;
  cluster = entry->first_cluster;
  cluster_bytes = (uint32_t) volume->sectors_per_cluster * ND_SECTOR_SIZE;
  rc = -1;

  if (entry->size_bytes == 0)
    {
      if (nd_fsync_fd (outfd, err, errlen) != 0)
        goto cleanup;
    }
  else
    {
      while (copied < entry->size_bytes)
        {
          uint8_t cluster_buf[64 * ND_SECTOR_SIZE];
          uint32_t lba;
          uint32_t take;
          ssize_t got;

          if (cluster < 2u || cluster >= volume->cluster_count_total + 2u)
            {
              nd_set_error (err, errlen, "invalid FAT chain in folder export");
              goto cleanup;
            }

          if (cluster_bytes > sizeof (cluster_buf))
            {
              nd_set_error (err, errlen, "cluster size too large for sync");
              goto cleanup;
            }

          lba = volume->data_start
            + (uint32_t) (cluster - 2u) * volume->sectors_per_cluster;
          got = nd_pread_full (fd, cluster_buf, cluster_bytes,
                               (off_t) lba * ND_SECTOR_SIZE);
          if (got != (ssize_t) cluster_bytes)
            {
              nd_set_error (err, errlen,
                            "error reading back folder image data");
              goto cleanup;
            }

          take = entry->size_bytes - copied;
          if (take > cluster_bytes)
            take = cluster_bytes;

          if (nd_pwrite_full (outfd, cluster_buf, take, (off_t) copied)
              != (ssize_t) take)
            {
              nd_set_error (err, errlen, "error writing host file");
              goto cleanup;
            }

          copied += take;
          if (copied >= entry->size_bytes)
            break;

          cluster = nd_sync_get_fat_entry (fat, cluster);
          if (cluster >= 0xfff8u)
            break;
        }

      if (copied != entry->size_bytes)
        {
          nd_set_error (err, errlen, "short FAT chain in folder export");
          goto cleanup;
        }

      if (nd_fsync_fd (outfd, err, errlen) != 0)
        goto cleanup;
    }

  if (close (outfd) != 0)
    {
      unlink (temp_path);
      outfd = -1;
      nd_set_error (err, errlen, "error writing host file");
      return -1;
    }
  outfd = -1;

  if (rename (temp_path, host_path) != 0)
    {
      unlink (temp_path);
      nd_set_error (err, errlen, "error writing host file");
      return -1;
    }

  nd_apply_timestamp (host_path, entry->dos_date, entry->dos_time);
  if (nd_fsync_path (host_path, err, errlen) != 0)
    return -1;
  if (nd_fsync_parent_dir (host_path, err, errlen) != 0)
    return -1;

  return 0;

cleanup:
  if (outfd >= 0)
    close (outfd);
  unlink (temp_path);
  return rc;
}

static int
nd_sync_parse_entry (const uint8_t *raw, struct nd_sync_entry **out_entry)
{
  struct nd_sync_entry *entry;

  *out_entry = NULL;

  if (raw[0] == 0x00)
    return 1;
  if (raw[0] == 0xe5)
    return 0;
  if (raw[11] == 0x0f)
    return 0;
  if ((raw[11] & 0x08u) != 0)
    return 0;
  if (raw[0] == '.')
    return 0;

  entry = nd_sync_entry_new ((raw[11] & 0x10u) ? ND_FAT_NODE_DIR : ND_FAT_NODE_FILE);
  if (entry == NULL)
    return -1;

  memcpy (entry->dos_name, raw, 11);
  entry->attr = raw[11];
  entry->dos_time = nd_load_le16 (raw + 22);
  entry->dos_date = nd_load_le16 (raw + 24);
  entry->first_cluster = nd_load_le16 (raw + 26);
  entry->size_bytes = nd_load_le32 (raw + 28);
  *out_entry = entry;
  return 2;
}

/*
 * Parsing walks the on-disk FAT structures rather than trusting the
 * cached synthesis tree.  That is what lets live writes create, delete,
 * or resize entries and have those changes reflected on the host.
 */
static int
nd_sync_parse_directory (const struct nd_fat_volume *volume, int fd,
                         const uint8_t *fat, uint16_t start_cluster,
                         struct nd_sync_entry *out_dir,
                         char *err, size_t errlen);

static int
nd_sync_parse_children_from_buffer (const struct nd_fat_volume *volume, int fd,
                                    const uint8_t *fat,
                                    const uint8_t *buffer, size_t len,
                                    struct nd_sync_entry *out_dir,
                                    char *err, size_t errlen)
{
  size_t off;

  for (off = 0; off + 32 <= len; off += 32)
    {
      struct nd_sync_entry *entry;
      int rc;

      rc = nd_sync_parse_entry (buffer + off, &entry);
      if (rc == 1)
        break;
      if (rc == 0)
        continue;
      if (rc < 0 || entry == NULL)
        {
          nd_set_error (err, errlen, "out of memory parsing folder image");
          return -1;
        }

      if (entry->kind == ND_FAT_NODE_DIR)
        {
          if (nd_sync_parse_directory (volume, fd, fat, entry->first_cluster,
                                       entry, err, errlen) != 0)
            {
              nd_sync_entry_free (entry);
              return -1;
            }
        }

      if (nd_sync_entry_add_child (out_dir, entry) != 0)
        {
          nd_sync_entry_free (entry);
          nd_set_error (err, errlen, "out of memory parsing folder image");
          return -1;
        }
    }

  return 0;
}

static int
nd_sync_parse_directory (const struct nd_fat_volume *volume, int fd,
                         const uint8_t *fat, uint16_t start_cluster,
                         struct nd_sync_entry *out_dir,
                         char *err, size_t errlen)
{
  uint16_t cluster;
  uint32_t cluster_bytes;
  unsigned int guard;

  cluster = start_cluster;
  cluster_bytes = (uint32_t) volume->sectors_per_cluster * ND_SECTOR_SIZE;

  for (guard = 0; guard < volume->cluster_count_total && cluster >= 2u
       && cluster < 0xfff8u; guard++)
    {
      uint8_t cluster_buf[64 * ND_SECTOR_SIZE];
      uint32_t lba;
      ssize_t got;

      if (cluster_bytes > sizeof (cluster_buf))
        {
          nd_set_error (err, errlen, "cluster size too large for parse");
          return -1;
        }

      lba = volume->data_start + (uint32_t) (cluster - 2u) * volume->sectors_per_cluster;
      got = nd_pread_full (fd, cluster_buf, cluster_bytes, (off_t) lba * ND_SECTOR_SIZE);
      if (got != (ssize_t) cluster_bytes)
        {
          nd_set_error (err, errlen, "unable to read directory cluster");
          return -1;
        }

      if (nd_sync_parse_children_from_buffer (volume, fd, fat, cluster_buf,
                                              cluster_bytes, out_dir,
                                              err, errlen) != 0)
        return -1;

      cluster = nd_sync_get_fat_entry (fat, cluster);
    }

  return 0;
}

static int
nd_sync_read_tree (const struct nd_fat_volume *volume, int fd,
                   struct nd_sync_entry **out_root, char *err, size_t errlen)
{
  struct nd_sync_entry *root;
  uint8_t *fat;
  uint8_t *root_buf;
  uint8_t sector0[ND_SECTOR_SIZE];
  struct nd_bpb bpb;
  ssize_t got;

  root = nd_sync_entry_new (ND_FAT_NODE_DIR);
  if (root == NULL)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  got = nd_pread_full (fd, sector0, sizeof (sector0), 0);
  if (got != (ssize_t) sizeof (sector0))
    {
      nd_sync_entry_free (root);
      nd_set_error (err, errlen, "unable to read boot sector");
      return -1;
    }

  if (nd_bpb_parse (sector0, &bpb, err, errlen) != 0)
    {
      nd_sync_entry_free (root);
      return -1;
    }

  if (bpb.sectors_per_cluster != volume->sectors_per_cluster
      || bpb.reserved_sector_count != volume->fat1_start
      || bpb.fats != 2
      || bpb.root_dir_entries != volume->root_dir_entries
      || bpb.sectors_per_fat != volume->sectors_per_fat
      || nd_bpb_total_sectors (&bpb) != volume->total_sectors)
    {
      nd_sync_entry_free (root);
      nd_set_error (err, errlen, "folder export reformat is not supported");
      return -1;
    }

  fat = (uint8_t *) malloc ((size_t) volume->sectors_per_fat * ND_SECTOR_SIZE);
  root_buf = (uint8_t *) malloc ((size_t) volume->root_dir_sectors * ND_SECTOR_SIZE);
  if (fat == NULL || root_buf == NULL)
    {
      free (fat);
      free (root_buf);
      nd_sync_entry_free (root);
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  got = nd_pread_full (fd, fat, (size_t) volume->sectors_per_fat * ND_SECTOR_SIZE,
                       (off_t) volume->fat1_start * ND_SECTOR_SIZE);
  if (got != (ssize_t) ((size_t) volume->sectors_per_fat * ND_SECTOR_SIZE))
    {
      free (fat);
      free (root_buf);
      nd_sync_entry_free (root);
      nd_set_error (err, errlen, "unable to read FAT");
      return -1;
    }

  got = nd_pread_full (fd, root_buf, (size_t) volume->root_dir_sectors * ND_SECTOR_SIZE,
                       (off_t) volume->root_start * ND_SECTOR_SIZE);
  if (got != (ssize_t) ((size_t) volume->root_dir_sectors * ND_SECTOR_SIZE))
    {
      free (fat);
      free (root_buf);
      nd_sync_entry_free (root);
      nd_set_error (err, errlen, "unable to read root directory");
      return -1;
    }

  if (nd_sync_parse_children_from_buffer (volume, fd, fat, root_buf,
                                          (size_t) volume->root_dir_sectors * ND_SECTOR_SIZE,
                                          root, err, errlen) != 0)
    {
      free (fat);
      free (root_buf);
      nd_sync_entry_free (root);
      return -1;
    }

  free (fat);
  free (root_buf);
  *out_root = root;
  return 0;
}

/*
 * Reconciliation applies the parsed directory tree onto the host tree.
 * Existing names are reused when possible, type conflicts are replaced,
 * and host paths missing from the current FAT view are removed.
 */
static int
nd_sync_reconcile_dir (const struct nd_fat_volume *volume, int fd,
                       const uint8_t *fat, const struct nd_fat_node *initial_dir,
                       const struct nd_sync_entry *current_dir,
                       const char *host_dir, char *err, size_t errlen)
{
  size_t i;

  if (mkdir (host_dir, 0777) != 0)
    {
      if (errno != EEXIST)
        {
          nd_set_error (err, errlen, "unable to create host directory");
          return -1;
        }
    }
  else if (nd_fsync_parent_dir (host_dir, err, errlen) != 0)
    return -1;

  for (i = 0; i < current_dir->child_count; i++)
    {
      const struct nd_sync_entry *child = current_dir->children[i];
      const struct nd_fat_node *initial_child;
      char host_name[ND_PATH_MAX];
      char host_path[ND_PATH_MAX];
      struct stat st;

      initial_child = nd_find_initial_child (initial_dir, child->dos_name);
      if (initial_child != NULL)
        snprintf (host_name, sizeof (host_name), "%s", initial_child->host_name);
      else if (nd_sync_decode_name (child->dos_name, host_name, sizeof (host_name)) != 0)
        {
          nd_set_error (err, errlen, "DOS filename too long for host sync");
          return -1;
        }

      if (snprintf (host_path, sizeof (host_path), "%s/%s",
                    host_dir, host_name) >= (int) sizeof (host_path))
        {
          nd_set_error (err, errlen, "host path too long during sync");
          return -1;
        }

      if (lstat (host_path, &st) == 0)
        {
          if (child->kind == ND_FAT_NODE_DIR)
            {
              if (!S_ISDIR (st.st_mode) && nd_remove_tree (host_path) != 0)
                {
                  nd_set_error (err, errlen, "unable to replace host path");
                  return -1;
                }
              if (!S_ISDIR (st.st_mode)
                  && nd_fsync_parent_dir (host_path, err, errlen) != 0)
                return -1;
            }
          else if (S_ISDIR (st.st_mode) && nd_remove_tree (host_path) != 0)
            {
              nd_set_error (err, errlen, "unable to replace host path");
              return -1;
            }
          else if (S_ISDIR (st.st_mode)
                   && nd_fsync_parent_dir (host_path, err, errlen) != 0)
            return -1;
        }

      if (child->kind == ND_FAT_NODE_DIR)
        {
          const struct nd_fat_node *initial_subdir =
            (initial_child != NULL && initial_child->kind == ND_FAT_NODE_DIR)
            ? initial_child : NULL;

          if (nd_sync_reconcile_dir (volume, fd, fat, initial_subdir, child,
                                     host_path, err, errlen) != 0)
            return -1;

          nd_apply_timestamp (host_path, child->dos_date, child->dos_time);
        }
      else if (nd_sync_copy_file (volume, fd, fat, child, host_path,
                                  err, errlen) != 0)
        return -1;
    }

  if (initial_dir != NULL)
    {
      for (i = 0; i < initial_dir->child_count; i++)
        {
          const struct nd_fat_node *child = initial_dir->children[i];

          if (nd_find_sync_child (current_dir, child->dos_name) == NULL)
            {
              if (nd_remove_tree (child->host_path) != 0)
                {
                  nd_set_error (err, errlen, "unable to remove deleted host path");
                  return -1;
                }
              if (nd_fsync_parent_dir (child->host_path, err, errlen) != 0)
                return -1;
            }
        }
    }

  return 0;
}

/*
 * Top-level sync is called after each successful folder write.  It reads
 * the current FAT, parses the directory tree back out of the temp image,
 * updates the host filesystem, and then swaps in a refreshed mapping
 * tree for the next write-back cycle.
 */
int
nd_fat_volume_sync_to_host (struct nd_fat_volume *volume, int fd,
                            char *err, size_t errlen)
{
  struct nd_sync_entry *current_root;
  struct nd_fat_node *mapped_root;
  struct nd_fat_node *old_root;
  uint8_t *fat;
  ssize_t got;
  int rc;

  current_root = NULL;
  fat = (uint8_t *) malloc ((size_t) volume->sectors_per_fat * ND_SECTOR_SIZE);
  if (fat == NULL)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  got = nd_pread_full (fd, fat, (size_t) volume->sectors_per_fat * ND_SECTOR_SIZE,
                       (off_t) volume->fat1_start * ND_SECTOR_SIZE);
  if (got != (ssize_t) ((size_t) volume->sectors_per_fat * ND_SECTOR_SIZE))
    {
      free (fat);
      nd_set_error (err, errlen, "unable to read FAT for sync");
      return -1;
    }

  if (nd_sync_read_tree (volume, fd, &current_root, err, errlen) != 0)
    {
      free (fat);
      return -1;
    }

  rc = nd_sync_reconcile_dir (volume, fd, fat, volume->root, current_root,
                              volume->root->host_path, err, errlen);
  mapped_root = NULL;
  if (rc == 0)
    {
      mapped_root = nd_sync_build_mapping_tree (current_root, volume->root,
                                                NULL, volume->root->host_name,
                                                volume->root->host_path,
                                                err, errlen);
      if (mapped_root == NULL)
        rc = -1;
    }

  if (rc == 0 && mapped_root != NULL)
    {
      old_root = volume->root;
      volume->root = mapped_root;
      nd_fat_node_free (old_root);
    }

  nd_sync_entry_free (current_root);
  free (fat);
  return rc;
}
