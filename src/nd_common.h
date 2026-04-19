/*
 * nd_common.h - shared constants, helpers, and small inline utilities.
 */

#ifndef ND_COMMON_H
#define ND_COMMON_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
# define PATH_MAX 512
#endif

#define ND_PATH_MAX PATH_MAX
#define ND_SECTOR_SIZE 512U
#define ND_MAX_PACKET 1514U
#define ND_MAX_PAYLOAD 1400U
#define ND_MAX_SECTORS_PER_OP 2U
#define ND_DEFAULT_MAX_SESSIONS 16U

#define ND_ARRAY_LEN(a) (sizeof (a) / sizeof ((a)[0]))

void nd_set_error (char *err, size_t errlen, const char *fmt, ...);
char *nd_strdup (const char *input);
ssize_t nd_pread_full (int fd, void *buffer, size_t count, off_t offset);
ssize_t nd_pwrite_full (int fd, const void *buffer, size_t count, off_t offset);
int nd_fsync_fd (int fd, char *err, size_t errlen);
int nd_fsync_path (const char *path, char *err, size_t errlen);
int nd_fsync_parent_dir (const char *path, char *err, size_t errlen);
int nd_make_temp_template (char *out, size_t outlen, const char *stem);
int nd_safe_join (char *out, size_t outlen, const char *root,
                  const char *relative);
void nd_uppercase_ascii (char *text);
uint16_t nd_dos_time_from_unix (time_t when);
uint16_t nd_dos_date_from_unix (time_t when);

static inline uint16_t
nd_load_le16 (const uint8_t *src)
{
  return (uint16_t) ((uint16_t) src[0] | ((uint16_t) src[1] << 8));
}

static inline uint32_t
nd_load_le32 (const uint8_t *src)
{
  return (uint32_t) src[0]
    | ((uint32_t) src[1] << 8)
    | ((uint32_t) src[2] << 16)
    | ((uint32_t) src[3] << 24);
}

static inline void
nd_store_le16 (uint8_t *dst, uint16_t value)
{
  dst[0] = (uint8_t) (value & 0xffu);
  dst[1] = (uint8_t) ((value >> 8) & 0xffu);
}

static inline void
nd_store_le32 (uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t) (value & 0xffu);
  dst[1] = (uint8_t) ((value >> 8) & 0xffu);
  dst[2] = (uint8_t) ((value >> 16) & 0xffu);
  dst[3] = (uint8_t) ((value >> 24) & 0xffu);
}

#endif
