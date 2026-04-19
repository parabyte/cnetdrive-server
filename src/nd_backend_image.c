/*
 * nd_backend_image.c - raw image backend and journal integration.
 */

#include "nd_backend.h"
#include "nd_bpb.h"
#include "nd_journal.h"

#include <fcntl.h>

struct nd_image_backend
{
  int fd;
  struct nd_journal *journal;
};

static int
nd_image_read_base (struct nd_image_backend *image, uint32_t start_sector,
                    uint16_t sector_count, uint8_t *buffer, char *err,
                    size_t errlen)
{
  size_t bytes;
  off_t offset;
  ssize_t got;

  bytes = (size_t) sector_count * ND_SECTOR_SIZE;
  offset = (off_t) start_sector * ND_SECTOR_SIZE;

  got = nd_pread_full (image->fd, buffer, bytes, offset);
  if (got != (ssize_t) bytes)
    {
      nd_set_error (err, errlen, "read error");
      return -1;
    }

  return 0;
}

static int
nd_image_read_sectors (struct nd_backend *backend, uint32_t start_sector,
                       uint16_t sector_count, uint8_t *buffer, char *err,
                       size_t errlen)
{
  struct nd_image_backend *image;
  uint16_t i;

  image = (struct nd_image_backend *) backend->impl;
  if (image->journal == NULL)
    return nd_image_read_base (image, start_sector, sector_count, buffer,
                               err, errlen);

  for (i = 0; i < sector_count; i++)
    {
      uint8_t *dst;

      dst = buffer + (size_t) i * ND_SECTOR_SIZE;
      if (nd_journal_has_lba (image->journal, start_sector + i))
        {
          if (nd_journal_read_lba (image->journal, start_sector + i, dst,
                                   err, errlen) != 0)
            return -1;
        }
      else if (nd_image_read_base (image, start_sector + i, 1, dst,
                                   err, errlen) != 0)
        return -1;
    }

  return 0;
}

static int
nd_image_write_sectors (struct nd_backend *backend, uint32_t start_sector,
                        uint16_t sector_count, const uint8_t *buffer,
                        char *err, size_t errlen)
{
  struct nd_image_backend *image;
  size_t bytes;
  off_t offset;
  ssize_t wrote;
  uint16_t i;

  if (!nd_backend_write_allowed (backend))
    {
      nd_set_error (err, errlen, "Write protect");
      return -1;
    }

  image = (struct nd_image_backend *) backend->impl;
  if (image->journal != NULL)
    {
      for (i = 0; i < sector_count; i++)
        {
          if (nd_journal_write_lba (image->journal, start_sector + i,
                                    buffer + (size_t) i * ND_SECTOR_SIZE,
                                    err, errlen) != 0)
            return -1;
        }
      return 0;
    }

  bytes = (size_t) sector_count * ND_SECTOR_SIZE;
  offset = (off_t) start_sector * ND_SECTOR_SIZE;
  wrote = nd_pwrite_full (image->fd, buffer, bytes, offset);
  if (wrote != (ssize_t) bytes)
    {
      nd_set_error (err, errlen, "write error");
      return -1;
    }

  if (nd_fsync_fd (image->fd, err, errlen) != 0)
    return -1;

  return 0;
}

static int
nd_image_mark_checkpoint (struct nd_backend *backend, const uint8_t *payload,
                          size_t payload_len, char *err, size_t errlen)
{
  struct nd_image_backend *image;

  image = (struct nd_image_backend *) backend->impl;
  return nd_journal_mark_checkpoint (image->journal, payload, payload_len,
                                     err, errlen);
}

static int
nd_image_goto_checkpoint (struct nd_backend *backend, const uint8_t *payload,
                          size_t payload_len, char *message,
                          size_t message_len)
{
  struct nd_image_backend *image;

  image = (struct nd_image_backend *) backend->impl;
  return nd_journal_goto_checkpoint (image->journal, payload, payload_len,
                                     message, message_len);
}

static int
nd_image_list_checkpoints (struct nd_backend *backend, uint8_t *payload,
                           size_t *payload_len, char *err, size_t errlen)
{
  struct nd_image_backend *image;

  image = (struct nd_image_backend *) backend->impl;
  return nd_journal_list_checkpoints (image->journal, payload, payload_len,
                                      err, errlen);
}

static void
nd_image_destroy (struct nd_backend *backend)
{
  struct nd_image_backend *image;
  char errbuf[160];

  if (backend == NULL)
    return;

  image = (struct nd_image_backend *) backend->impl;
  if (image != NULL)
    {
      if (image->journal != NULL)
        {
          nd_journal_close (image->journal, errbuf, sizeof (errbuf));
          nd_journal_destroy (image->journal);
        }
      if (image->fd >= 0)
        close (image->fd);
      free (image);
    }

  free (backend);
}

static const struct nd_backend_ops nd_image_ops =
{
  nd_image_read_sectors,
  nd_image_write_sectors,
  NULL,
  nd_image_mark_checkpoint,
  nd_image_goto_checkpoint,
  nd_image_list_checkpoints,
  nd_image_destroy
};

static int
nd_session_scoped_requested (const char *full_path)
{
  char path[ND_PATH_MAX];

  if (snprintf (path, sizeof (path), "%s.session_scoped", full_path)
      >= (int) sizeof (path))
    return 0;

  return access (path, F_OK) == 0;
}

int
nd_backend_image_open (const char *full_path, const char *request_name,
                       struct nd_backend **out_backend, char *err,
                       size_t errlen)
{
  struct stat st;
  int fd;
  int readonly;
  int session_scoped;
  uint8_t sector0[ND_SECTOR_SIZE];
  uint8_t fat_sector[ND_SECTOR_SIZE];
  struct nd_bpb bpb;
  struct nd_backend *backend;
  struct nd_image_backend *image;
  struct nd_journal *journal;
  ssize_t got;

  if (stat (full_path, &st) != 0 || !S_ISREG (st.st_mode))
    {
      nd_set_error (err, errlen, "image file not found: %s", request_name);
      return -1;
    }

  if (st.st_size <= 0 || (st.st_size % ND_SECTOR_SIZE) != 0)
    {
      nd_set_error (err, errlen,
                    "specified image file size is not a multiple of 512 bytes");
      return -1;
    }

  if ((uint64_t) st.st_size > UINT32_MAX)
    {
      nd_set_error (err, errlen, "image is too large for the protocol");
      return -1;
    }

  readonly = 0;
  fd = open (full_path, O_RDWR);
  if (fd < 0)
    {
      fd = open (full_path, O_RDONLY);
      if (fd < 0)
        {
          nd_set_error (err, errlen, "error opening image file: %s",
                        request_name);
          return -1;
        }
      readonly = 1;
    }

  got = nd_pread_full (fd, sector0, sizeof (sector0), 0);
  if (got != (ssize_t) sizeof (sector0))
    {
      close (fd);
      nd_set_error (err, errlen, "read error");
      return -1;
    }

  if (nd_bpb_parse (sector0, &bpb, NULL, 0) == 0)
    {
      off_t fat_offset;

      fat_offset = (off_t) bpb.reserved_sector_count * ND_SECTOR_SIZE;
      got = nd_pread_full (fd, fat_sector, sizeof (fat_sector), fat_offset);
      if (got != (ssize_t) sizeof (fat_sector))
        {
          close (fd);
          nd_set_error (err, errlen, "error reading media descriptor byte");
          return -1;
        }
    }
  else if (st.st_size == 160 * 1024 || st.st_size == 320 * 1024)
    {
      got = nd_pread_full (fd, fat_sector, sizeof (fat_sector), ND_SECTOR_SIZE);
      if (got != (ssize_t) sizeof (fat_sector))
        {
          close (fd);
          nd_set_error (err, errlen, "error reading media descriptor byte");
          return -1;
        }
    }
  else
    {
      close (fd);
      nd_set_error (err, errlen,
                    "bad BPB and file size doesn't match a DOS 1.x disk");
      return -1;
    }

  journal = NULL;
  session_scoped = nd_session_scoped_requested (full_path);
  if (nd_journal_open_goback (full_path, &journal, err, errlen) != 0)
    {
      close (fd);
      return -1;
    }

  if (session_scoped && journal != NULL)
    {
      nd_journal_close (journal, err, errlen);
      nd_journal_destroy (journal);
      close (fd);
      nd_set_error (err, errlen, "unable to open journal file for: %s",
                    request_name);
      return -1;
    }

  if (session_scoped
      && nd_journal_open_session_scoped (full_path, &journal, err, errlen) != 0)
    {
      close (fd);
      return -1;
    }

  backend = (struct nd_backend *) calloc (1, sizeof (*backend));
  image = (struct nd_image_backend *) calloc (1, sizeof (*image));
  if (backend == NULL || image == NULL)
    {
      if (journal != NULL)
        {
          nd_journal_close (journal, err, errlen);
          nd_journal_destroy (journal);
        }
      close (fd);
      free (backend);
      free (image);
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  image->fd = fd;
  image->journal = journal;

  backend->kind = ND_BACKEND_IMAGE;
  backend->ops = &nd_image_ops;
  backend->impl = image;
  backend->size_bytes = (uint32_t) st.st_size;
  backend->media_descriptor = fat_sector[0];
  strncpy (backend->export_name, request_name, sizeof (backend->export_name) - 1);
  strncpy (backend->export_path, full_path, sizeof (backend->export_path) - 1);
  memcpy (backend->boot_sector_prefix, sector0, sizeof (backend->boot_sector_prefix));

  if (journal != NULL && nd_journal_type (journal) == ND_JOURNAL_SESSION_SCOPED)
    {
      backend->connect_flags = ND_BACKEND_CONNECT_FLAG_SESSION_SCOPED;
      backend->properties = ND_BACKEND_PROPERTY_WRITE_ALLOWED;
    }
  else if (journal != NULL && nd_journal_type (journal) == ND_JOURNAL_GOBACK)
    {
      backend->connect_flags = 0;
      backend->properties = ND_BACKEND_PROPERTY_WRITE_ALLOWED
        | ND_BACKEND_PROPERTY_EXCLUSIVE_OPEN;
    }
  else if (readonly)
    {
      backend->connect_flags = ND_BACKEND_CONNECT_FLAG_READONLY;
      backend->properties = 0;
    }
  else
    {
      backend->connect_flags = 0;
      backend->properties = ND_BACKEND_PROPERTY_WRITE_ALLOWED
        | ND_BACKEND_PROPERTY_EXCLUSIVE_OPEN;
    }

  *out_backend = backend;
  return 0;
}
