#include "nd_backend.h"
#include "nd_fat.h"

#include <fcntl.h>

struct nd_folder_backend
{
  struct nd_fat_volume *volume;
  int fd;
  int writable;
  int dirty;
  char temp_path[ND_PATH_MAX];
};

static int
nd_folder_read_sectors (struct nd_backend *backend, uint32_t start_sector,
                        uint16_t sector_count, uint8_t *buffer, char *err,
                        size_t errlen)
{
  struct nd_folder_backend *folder;
  size_t bytes;
  ssize_t got;

  folder = (struct nd_folder_backend *) backend->impl;
  bytes = (size_t) sector_count * ND_SECTOR_SIZE;
  got = nd_pread_full (folder->fd, buffer, bytes,
                       (off_t) start_sector * ND_SECTOR_SIZE);
  if (got != (ssize_t) bytes)
    {
      nd_set_error (err, errlen, "read error");
      return -1;
    }

  return 0;
}

static int
nd_folder_write_sectors (struct nd_backend *backend, uint32_t start_sector,
                         uint16_t sector_count, const uint8_t *buffer,
                         char *err, size_t errlen)
{
  struct nd_folder_backend *folder;
  size_t bytes;
  ssize_t wrote;

  folder = (struct nd_folder_backend *) backend->impl;
  if (!folder->writable)
    {
      nd_set_error (err, errlen, "Write protect");
      return -1;
    }

  bytes = (size_t) sector_count * ND_SECTOR_SIZE;
  wrote = nd_pwrite_full (folder->fd, buffer, bytes,
                          (off_t) start_sector * ND_SECTOR_SIZE);
  if (wrote != (ssize_t) bytes)
    {
      nd_set_error (err, errlen, "write error");
      return -1;
    }

  if (nd_fsync_fd (folder->fd, err, errlen) != 0)
    return -1;

  folder->dirty = 1;
  if (nd_fat_volume_sync_to_host (folder->volume, folder->fd, err, errlen) != 0)
    return -1;

  folder->dirty = 0;
  return 0;
}

static int
nd_folder_commit (struct nd_backend *backend, char *err, size_t errlen)
{
  struct nd_folder_backend *folder;

  folder = (struct nd_folder_backend *) backend->impl;
  if (!folder->writable || !folder->dirty)
    return 0;

  if (nd_fat_volume_sync_to_host (folder->volume, folder->fd, err, errlen) != 0)
    return -1;

  folder->dirty = 0;
  return 0;
}

static void
nd_folder_destroy (struct nd_backend *backend)
{
  struct nd_folder_backend *folder;

  if (backend == NULL)
    return;

  folder = (struct nd_folder_backend *) backend->impl;
  if (folder != NULL)
    {
      char errmsg[160];

      if (folder->dirty)
        nd_fat_volume_sync_to_host (folder->volume, folder->fd,
                                    errmsg, sizeof (errmsg));
      if (folder->fd >= 0)
        close (folder->fd);
      if (folder->temp_path[0] != '\0')
        unlink (folder->temp_path);
      nd_fat_volume_close (folder->volume);
      free (folder);
    }

  free (backend);
}

static const struct nd_backend_ops nd_folder_ops =
{
  nd_folder_read_sectors,
  nd_folder_write_sectors,
  nd_folder_commit,
  NULL,
  NULL,
  NULL,
  nd_folder_destroy
};

int
nd_backend_folder_open (const char *full_path, const char *request_name,
                        struct nd_backend **out_backend, char *err,
                        size_t errlen)
{
  struct nd_backend *backend;
  struct nd_folder_backend *folder;
  int writable;

  backend = (struct nd_backend *) calloc (1, sizeof (*backend));
  folder = (struct nd_folder_backend *) calloc (1, sizeof (*folder));
  if (backend == NULL || folder == NULL)
    {
      free (backend);
      free (folder);
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  if (nd_fat_volume_open (full_path, request_name, &folder->volume,
                          err, errlen) != 0)
    {
      free (backend);
      free (folder);
      return -1;
    }

  folder->fd = -1;
  writable = (access (full_path, W_OK) == 0);
  if (nd_make_temp_template (folder->temp_path, sizeof (folder->temp_path),
                             "cnetdrive-folder-") != 0)
    {
      nd_fat_volume_close (folder->volume);
      free (backend);
      free (folder);
      nd_set_error (err, errlen, "unable to create folder image");
      return -1;
    }
  folder->fd = mkstemp (folder->temp_path);
  if (folder->fd < 0)
    {
      nd_fat_volume_close (folder->volume);
      free (backend);
      free (folder);
      nd_set_error (err, errlen, "unable to create folder image");
      return -1;
    }

  if (nd_fat_volume_materialize (folder->volume, folder->fd, err, errlen) != 0)
    {
      close (folder->fd);
      unlink (folder->temp_path);
      nd_fat_volume_close (folder->volume);
      free (backend);
      free (folder);
      return -1;
    }

  folder->writable = writable;

  backend->kind = ND_BACKEND_FOLDER;
  backend->ops = &nd_folder_ops;
  backend->impl = folder;
  backend->connect_flags = writable ? 0 : ND_BACKEND_CONNECT_FLAG_READONLY;
  backend->properties = writable
    ? ND_BACKEND_PROPERTY_WRITE_ALLOWED | ND_BACKEND_PROPERTY_EXCLUSIVE_OPEN
    : 0;
  backend->size_bytes = nd_fat_volume_size (folder->volume);
  backend->media_descriptor = nd_fat_volume_media_descriptor (folder->volume);
  snprintf (backend->export_name, sizeof (backend->export_name), "%s",
            request_name);
  snprintf (backend->export_path, sizeof (backend->export_path), "%s",
            full_path);
  memcpy (backend->boot_sector_prefix,
          nd_fat_volume_boot_sector (folder->volume),
          sizeof (backend->boot_sector_prefix));

  *out_backend = backend;
  return 0;
}
