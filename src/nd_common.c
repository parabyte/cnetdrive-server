#include "nd_common.h"

#include <ctype.h>

static int
nd_open_for_fsync (const char *path)
{
  int flags;

  flags = O_RDONLY;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif

  return open (path, flags);
}

void
nd_set_error (char *err, size_t errlen, const char *fmt, ...)
{
  va_list ap;

  if (err == NULL || errlen == 0)
    return;

  va_start (ap, fmt);
  vsnprintf (err, errlen, fmt, ap);
  va_end (ap);
}

char *
nd_strdup (const char *input)
{
  size_t len;
  char *copy;

  if (input == NULL)
    return NULL;

  len = strlen (input);
  copy = (char *) malloc (len + 1);
  if (copy == NULL)
    return NULL;

  memcpy (copy, input, len + 1);
  return copy;
}

ssize_t
nd_pread_full (int fd, void *buffer, size_t count, off_t offset)
{
  uint8_t *dst;
  size_t done;

  dst = (uint8_t *) buffer;
  done = 0;

  while (done < count)
    {
      ssize_t got;

      if (lseek (fd, offset + (off_t) done, SEEK_SET) < 0)
        return -1;

      got = read (fd, dst + done, count - done);
      if (got <= 0)
        return (done > 0) ? (ssize_t) done : -1;

      done += (size_t) got;
    }

  return (ssize_t) done;
}

ssize_t
nd_pwrite_full (int fd, const void *buffer, size_t count, off_t offset)
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
        return (done > 0) ? (ssize_t) done : -1;

      done += (size_t) wrote;
    }

  return (ssize_t) done;
}

int
nd_fsync_fd (int fd, char *err, size_t errlen)
{
  if (fd < 0)
    {
      nd_set_error (err, errlen, "fsync error");
      return -1;
    }

  if (fsync (fd) != 0)
    {
      nd_set_error (err, errlen, "fsync error");
      return -1;
    }

  return 0;
}

int
nd_fsync_path (const char *path, char *err, size_t errlen)
{
  int fd;
  int rc;

  fd = open (path, O_RDONLY);
  if (fd < 0)
    {
      nd_set_error (err, errlen, "fsync error");
      return -1;
    }

  rc = nd_fsync_fd (fd, err, errlen);
  close (fd);
  return rc;
}

int
nd_fsync_parent_dir (const char *path, char *err, size_t errlen)
{
  char parent[ND_PATH_MAX];
  const char *slash;
  int fd;
  int rc;

  if (path == NULL || path[0] == '\0')
    {
      nd_set_error (err, errlen, "fsync error");
      return -1;
    }

  slash = strrchr (path, '/');
  if (slash == NULL)
    snprintf (parent, sizeof (parent), ".");
  else if (slash == path)
    snprintf (parent, sizeof (parent), "/");
  else
    {
      size_t len;

      len = (size_t) (slash - path);
      if (len >= sizeof (parent))
        {
          nd_set_error (err, errlen, "fsync error");
          return -1;
        }

      memcpy (parent, path, len);
      parent[len] = '\0';
    }

  fd = nd_open_for_fsync (parent);
  if (fd < 0)
    {
      nd_set_error (err, errlen, "fsync error");
      return -1;
    }

  rc = nd_fsync_fd (fd, err, errlen);
  if (rc != 0
      && (errno == EINVAL || errno == EROFS
#ifdef EOPNOTSUPP
          || errno == EOPNOTSUPP
#endif
#ifdef ENOTSUP
          || errno == ENOTSUP
#endif
          ))
    rc = 0;
  close (fd);
  return rc;
}

int
nd_make_temp_template (char *out, size_t outlen, const char *stem)
{
  const char *tmpdir;
  size_t len;

  if (out == NULL || outlen == 0 || stem == NULL || stem[0] == '\0')
    return -1;

  tmpdir = getenv ("TMPDIR");
  if (tmpdir == NULL || tmpdir[0] == '\0')
    tmpdir = "/tmp";

  len = strlen (tmpdir);
  return snprintf (out, outlen, "%s%s%sXXXXXX",
                   tmpdir,
                   (len > 0 && tmpdir[len - 1] == '/') ? "" : "/",
                   stem) < (int) outlen ? 0 : -1;
}

static int
nd_is_safe_path_component (const char *text)
{
  size_t i;

  if (text == NULL || text[0] == '\0')
    return 0;

  if (text[0] == '/')
    return 0;

  for (i = 0; text[i] != '\0'; i++)
    {
      if (text[i] == '\\')
        return 0;
    }

  if (strstr (text, "..") != NULL)
    return 0;

  return 1;
}

int
nd_safe_join (char *out, size_t outlen, const char *root, const char *relative)
{
  size_t root_len;

  if (out == NULL || outlen == 0 || root == NULL || relative == NULL)
    return -1;

  if (!nd_is_safe_path_component (relative))
    return -1;

  root_len = strlen (root);

  if (snprintf (out, outlen, "%s%s%s",
                root,
                (root_len > 0 && root[root_len - 1] == '/') ? "" : "/",
                relative) >= (int) outlen)
    return -1;

  return 0;
}

void
nd_uppercase_ascii (char *text)
{
  size_t i;

  if (text == NULL)
    return;

  for (i = 0; text[i] != '\0'; i++)
    text[i] = (char) toupper ((unsigned char) text[i]);
}

uint16_t
nd_dos_time_from_unix (time_t when)
{
  struct tm tmv;

  if (localtime_r (&when, &tmv) == NULL)
    return 0;

  return (uint16_t) (((tmv.tm_hour & 0x1f) << 11)
                     | ((tmv.tm_min & 0x3f) << 5)
                     | ((tmv.tm_sec / 2) & 0x1f));
}

uint16_t
nd_dos_date_from_unix (time_t when)
{
  struct tm tmv;
  int year;

  if (localtime_r (&when, &tmv) == NULL)
    return 0;

  year = tmv.tm_year + 1900;
  if (year < 1980)
    year = 1980;

  return (uint16_t) ((((year - 1980) & 0x7f) << 9)
                     | (((tmv.tm_mon + 1) & 0x0f) << 5)
                     | (tmv.tm_mday & 0x1f));
}
