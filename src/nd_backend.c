/*
 * nd_backend.c - backend dispatch and export open policy handling.
 */

#include "nd_backend.h"

int
nd_backend_open (const char *root_dir, const char *request_name,
                 struct nd_backend **out_backend, char *err, size_t errlen)
{
  char full_path[ND_PATH_MAX];
  struct stat st;

  if (root_dir == NULL || request_name == NULL || out_backend == NULL)
    {
      nd_set_error (err, errlen, "internal backend error");
      return -1;
    }

  if (nd_safe_join (full_path, sizeof (full_path), root_dir, request_name) != 0)
    {
      nd_set_error (err, errlen, "bad characters in image name");
      return -1;
    }

  if (stat (full_path, &st) != 0)
    {
      nd_set_error (err, errlen, "image file not found: %s", request_name);
      return -1;
    }

  if (S_ISREG (st.st_mode))
    {
      int rc;

      rc = nd_backend_image_open (full_path, request_name, out_backend,
                                  err, errlen);
      if (rc == 0)
        {
          (*out_backend)->export_dev = st.st_dev;
          (*out_backend)->export_ino = st.st_ino;
        }
      return rc;
    }

  if (S_ISDIR (st.st_mode))
    {
      int rc;

      rc = nd_backend_folder_open (full_path, request_name, out_backend,
                                   err, errlen);
      if (rc == 0)
        {
          (*out_backend)->export_dev = st.st_dev;
          (*out_backend)->export_ino = st.st_ino;
        }
      return rc;
    }

  nd_set_error (err, errlen, "unsupported export type: %s", request_name);
  return -1;
}

int
nd_backend_read (struct nd_backend *backend, uint32_t start_sector,
                 uint16_t sector_count, uint8_t *buffer, char *err,
                 size_t errlen)
{
  if (backend == NULL || backend->ops == NULL || backend->ops->read_sectors == NULL)
    {
      nd_set_error (err, errlen, "backend read error");
      return -1;
    }

  return backend->ops->read_sectors (backend, start_sector, sector_count,
                                     buffer, err, errlen);
}

int
nd_backend_write (struct nd_backend *backend, uint32_t start_sector,
                  uint16_t sector_count, const uint8_t *buffer,
                  char *err, size_t errlen)
{
  if (backend == NULL || backend->ops == NULL
      || backend->ops->write_sectors == NULL)
    {
      nd_set_error (err, errlen, "backend write error");
      return -1;
    }

  return backend->ops->write_sectors (backend, start_sector, sector_count,
                                      buffer, err, errlen);
}

int
nd_backend_commit (struct nd_backend *backend, char *err, size_t errlen)
{
  if (backend == NULL || backend->ops == NULL || backend->ops->commit == NULL)
    return 0;

  return backend->ops->commit (backend, err, errlen);
}

int
nd_backend_mark_checkpoint (struct nd_backend *backend,
                            const uint8_t *payload, size_t payload_len,
                            char *err, size_t errlen)
{
  if (backend == NULL || backend->ops == NULL
      || backend->ops->mark_checkpoint == NULL)
    {
      nd_set_error (err, errlen, "Journal: feature not supported");
      return -1;
    }

  return backend->ops->mark_checkpoint (backend, payload, payload_len,
                                        err, errlen);
}

int
nd_backend_goto_checkpoint (struct nd_backend *backend,
                            const uint8_t *payload, size_t payload_len,
                            char *message, size_t message_len)
{
  if (backend == NULL || backend->ops == NULL
      || backend->ops->goto_checkpoint == NULL)
    {
      nd_set_error (message, message_len, "Journal: feature not supported");
      return -1;
    }

  return backend->ops->goto_checkpoint (backend, payload, payload_len,
                                        message, message_len);
}

int
nd_backend_list_checkpoints (struct nd_backend *backend, uint8_t *payload,
                             size_t *payload_len, char *err, size_t errlen)
{
  if (payload_len != NULL)
    *payload_len = 0;

  if (backend == NULL || backend->ops == NULL
      || backend->ops->list_checkpoints == NULL)
    {
      nd_set_error (err, errlen, "Journal: feature not supported");
      return -1;
    }

  return backend->ops->list_checkpoints (backend, payload, payload_len,
                                         err, errlen);
}

void
nd_backend_close (struct nd_backend *backend)
{
  if (backend == NULL)
    return;

  if (backend->ops != NULL && backend->ops->destroy != NULL)
    backend->ops->destroy (backend);
}

int
nd_backend_write_allowed (const struct nd_backend *backend)
{
  return backend != NULL
    && (backend->properties & ND_BACKEND_PROPERTY_WRITE_ALLOWED) != 0;
}

int
nd_backend_requires_exclusive_open (const struct nd_backend *backend)
{
  return backend != NULL
    && (backend->properties & ND_BACKEND_PROPERTY_EXCLUSIVE_OPEN) != 0;
}

uint16_t
nd_backend_connect_flags (const struct nd_backend *backend)
{
  return backend == NULL ? 0 : backend->connect_flags;
}

const char *
nd_backend_kind_string (const struct nd_backend *backend)
{
  if (backend == NULL)
    return "unknown";

  switch (backend->kind)
    {
    case ND_BACKEND_IMAGE:
      return "image";
    case ND_BACKEND_FOLDER:
      return "folder";
    default:
      return "unknown";
    }
}
