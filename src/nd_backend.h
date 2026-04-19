/*
 * nd_backend.h - backend interfaces and shared export metadata.
 */

#ifndef ND_BACKEND_H
#define ND_BACKEND_H

#include "nd_common.h"

enum nd_backend_kind
{
  ND_BACKEND_IMAGE = 1,
  ND_BACKEND_FOLDER = 2
};

struct nd_backend;

struct nd_backend_ops
{
  int (*read_sectors) (struct nd_backend *backend, uint32_t start_sector,
                       uint16_t sector_count, uint8_t *buffer, char *err,
                       size_t errlen);
  int (*write_sectors) (struct nd_backend *backend, uint32_t start_sector,
                        uint16_t sector_count, const uint8_t *buffer,
                        char *err, size_t errlen);
  int (*commit) (struct nd_backend *backend, char *err, size_t errlen);
  int (*mark_checkpoint) (struct nd_backend *backend, const uint8_t *payload,
                          size_t payload_len, char *err, size_t errlen);
  int (*goto_checkpoint) (struct nd_backend *backend, const uint8_t *payload,
                          size_t payload_len, char *message,
                          size_t message_len);
  int (*list_checkpoints) (struct nd_backend *backend, uint8_t *payload,
                           size_t *payload_len, char *err, size_t errlen);
  void (*destroy) (struct nd_backend *backend);
};

struct nd_backend
{
  enum nd_backend_kind kind;
  const struct nd_backend_ops *ops;
  char export_name[ND_PATH_MAX];
  char export_path[ND_PATH_MAX];
  dev_t export_dev;
  ino_t export_ino;
  uint32_t size_bytes;
  uint16_t connect_flags;
  uint16_t properties;
  uint8_t boot_sector_prefix[64];
  uint8_t media_descriptor;
  void *impl;
};

#define ND_BACKEND_CONNECT_FLAG_READONLY 0x0001u
#define ND_BACKEND_CONNECT_FLAG_SESSION_SCOPED 0x0002u

#define ND_BACKEND_PROPERTY_WRITE_ALLOWED 0x0001u
#define ND_BACKEND_PROPERTY_EXCLUSIVE_OPEN 0x0002u

int nd_backend_open (const char *root_dir, const char *request_name,
                     struct nd_backend **out_backend, char *err,
                     size_t errlen);
int nd_backend_read (struct nd_backend *backend, uint32_t start_sector,
                     uint16_t sector_count, uint8_t *buffer, char *err,
                     size_t errlen);
int nd_backend_write (struct nd_backend *backend, uint32_t start_sector,
                      uint16_t sector_count, const uint8_t *buffer,
                      char *err, size_t errlen);
int nd_backend_commit (struct nd_backend *backend, char *err, size_t errlen);
int nd_backend_mark_checkpoint (struct nd_backend *backend,
                                const uint8_t *payload, size_t payload_len,
                                char *err, size_t errlen);
int nd_backend_goto_checkpoint (struct nd_backend *backend,
                                const uint8_t *payload, size_t payload_len,
                                char *message, size_t message_len);
int nd_backend_list_checkpoints (struct nd_backend *backend, uint8_t *payload,
                                 size_t *payload_len, char *err,
                                 size_t errlen);
void nd_backend_close (struct nd_backend *backend);
int nd_backend_write_allowed (const struct nd_backend *backend);
int nd_backend_requires_exclusive_open (const struct nd_backend *backend);
uint16_t nd_backend_connect_flags (const struct nd_backend *backend);
const char *nd_backend_kind_string (const struct nd_backend *backend);

int nd_backend_image_open (const char *full_path, const char *request_name,
                           struct nd_backend **out_backend, char *err,
                           size_t errlen);
int nd_backend_folder_open (const char *full_path, const char *request_name,
                            struct nd_backend **out_backend, char *err,
                            size_t errlen);

#endif
