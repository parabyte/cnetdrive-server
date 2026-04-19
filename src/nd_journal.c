#include "nd_journal.h"

#include <fcntl.h>
#include <sys/time.h>

#define ND_JOURNAL_REC_HDR_SIZE 16U
#define ND_JOURNAL_REC_SIZE (ND_JOURNAL_REC_HDR_SIZE + ND_SECTOR_SIZE)
#define ND_JOURNAL_TAG_MAX 64U
#define ND_JOURNAL_LIST_MAX 1024U

struct nd_map_slot
{
  uint32_t lba;
  uint32_t rec_num;
  unsigned char used;
};

struct nd_lba_map
{
  struct nd_map_slot *slots;
  size_t capacity;
  size_t count;
};

struct nd_checkpoint
{
  uint32_t rec_num;
  int64_t written_ms;
  char tag[ND_JOURNAL_TAG_MAX + 1];
};

struct nd_journal
{
  enum nd_journal_type type;
  int fd;
  char path[ND_PATH_MAX];
  int delete_on_close;
  uint32_t journal_records;
  struct nd_lba_map map;
  struct nd_checkpoint *checkpoints;
  size_t checkpoint_count;
  size_t checkpoint_cap;
};

static uint64_t
nd_load_le64 (const uint8_t *src)
{
  return (uint64_t) src[0]
    | ((uint64_t) src[1] << 8)
    | ((uint64_t) src[2] << 16)
    | ((uint64_t) src[3] << 24)
    | ((uint64_t) src[4] << 32)
    | ((uint64_t) src[5] << 40)
    | ((uint64_t) src[6] << 48)
    | ((uint64_t) src[7] << 56);
}

static void
nd_store_le64 (uint8_t *dst, uint64_t value)
{
  dst[0] = (uint8_t) (value & 0xffu);
  dst[1] = (uint8_t) ((value >> 8) & 0xffu);
  dst[2] = (uint8_t) ((value >> 16) & 0xffu);
  dst[3] = (uint8_t) ((value >> 24) & 0xffu);
  dst[4] = (uint8_t) ((value >> 32) & 0xffu);
  dst[5] = (uint8_t) ((value >> 40) & 0xffu);
  dst[6] = (uint8_t) ((value >> 48) & 0xffu);
  dst[7] = (uint8_t) ((value >> 56) & 0xffu);
}

static int64_t
nd_time_now_millis (void)
{
  struct timeval tv;

  if (gettimeofday (&tv, NULL) != 0)
    return (int64_t) time (NULL) * 1000;

  return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint32_t
nd_map_hash (uint32_t lba)
{
  return lba * 2654435761u;
}

static int
nd_map_init (struct nd_lba_map *map, size_t capacity)
{
  size_t actual;

  if (map == NULL)
    return -1;

  actual = 16;
  while (actual < capacity)
    actual <<= 1;

  map->slots = (struct nd_map_slot *) calloc (actual, sizeof (*map->slots));
  if (map->slots == NULL)
    return -1;

  map->capacity = actual;
  map->count = 0;
  return 0;
}

static void
nd_map_reset (struct nd_lba_map *map)
{
  if (map == NULL || map->slots == NULL)
    return;

  memset (map->slots, 0, map->capacity * sizeof (*map->slots));
  map->count = 0;
}

static void
nd_map_free (struct nd_lba_map *map)
{
  if (map == NULL)
    return;

  free (map->slots);
  memset (map, 0, sizeof (*map));
}

static int
nd_map_rehash (struct nd_lba_map *map, size_t new_capacity)
{
  struct nd_lba_map fresh;
  size_t i;

  if (nd_map_init (&fresh, new_capacity) != 0)
    return -1;

  for (i = 0; i < map->capacity; i++)
    {
      size_t pos;

      if (!map->slots[i].used)
        continue;

      pos = nd_map_hash (map->slots[i].lba) & (fresh.capacity - 1);
      while (fresh.slots[pos].used)
        pos = (pos + 1) & (fresh.capacity - 1);

      fresh.slots[pos] = map->slots[i];
      fresh.count++;
    }

  free (map->slots);
  *map = fresh;
  return 0;
}

static int
nd_map_put (struct nd_lba_map *map, uint32_t lba, uint32_t rec_num)
{
  size_t pos;

  if (map == NULL || map->slots == NULL)
    return -1;

  if ((map->count + 1) * 10 >= map->capacity * 7)
    {
      if (nd_map_rehash (map, map->capacity * 2) != 0)
        return -1;
    }

  pos = nd_map_hash (lba) & (map->capacity - 1);
  while (map->slots[pos].used)
    {
      if (map->slots[pos].lba == lba)
        {
          map->slots[pos].rec_num = rec_num;
          return 0;
        }
      pos = (pos + 1) & (map->capacity - 1);
    }

  map->slots[pos].used = 1;
  map->slots[pos].lba = lba;
  map->slots[pos].rec_num = rec_num;
  map->count++;
  return 0;
}

static int
nd_map_get (const struct nd_lba_map *map, uint32_t lba, uint32_t *rec_num)
{
  size_t pos;
  size_t scanned;

  if (map == NULL || map->slots == NULL || map->capacity == 0)
    return 0;

  pos = nd_map_hash (lba) & (map->capacity - 1);
  for (scanned = 0; scanned < map->capacity; scanned++)
    {
      if (!map->slots[pos].used)
        return 0;

      if (map->slots[pos].lba == lba)
        {
          if (rec_num != NULL)
            *rec_num = map->slots[pos].rec_num;
          return 1;
        }

      pos = (pos + 1) & (map->capacity - 1);
    }

  return 0;
}

static void
nd_journal_clear_checkpoints (struct nd_journal *journal)
{
  free (journal->checkpoints);
  journal->checkpoints = NULL;
  journal->checkpoint_count = 0;
  journal->checkpoint_cap = 0;
}

static int
nd_journal_add_checkpoint (struct nd_journal *journal, uint32_t rec_num,
                           int64_t written_ms, const char *tag)
{
  struct nd_checkpoint *grown;
  size_t new_cap;

  if (journal->checkpoint_count == journal->checkpoint_cap)
    {
      new_cap = (journal->checkpoint_cap == 0) ? 8 : journal->checkpoint_cap * 2;
      grown = (struct nd_checkpoint *) realloc (journal->checkpoints,
                                                new_cap * sizeof (*grown));
      if (grown == NULL)
        return -1;

      journal->checkpoints = grown;
      journal->checkpoint_cap = new_cap;
    }

  journal->checkpoints[journal->checkpoint_count].rec_num = rec_num;
  journal->checkpoints[journal->checkpoint_count].written_ms = written_ms;
  snprintf (journal->checkpoints[journal->checkpoint_count].tag,
            sizeof (journal->checkpoints[journal->checkpoint_count].tag),
            "%s", tag);
  journal->checkpoint_count++;
  return 0;
}

static int
nd_journal_format_timestamp (int64_t written_ms, char *out, size_t outlen)
{
  time_t secs;
  int millis;
  struct tm tmv;

  secs = (time_t) (written_ms / 1000);
  millis = (int) (written_ms % 1000);
  if (millis < 0)
    millis += 1000;

  if (localtime_r (&secs, &tmv) == NULL)
    return -1;

  return snprintf (out, outlen, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec, millis) < (int) outlen
    ? 0 : -1;
}

static int
nd_journal_record_offset (uint32_t rec_num, off_t *offset_out)
{
  *offset_out = (off_t) ((uint64_t) rec_num * ND_JOURNAL_REC_SIZE);
  return 0;
}

static int
nd_journal_read_record (const struct nd_journal *journal, uint32_t rec_num,
                        uint8_t *record, char *err, size_t errlen)
{
  off_t offset;
  ssize_t got;

  if (nd_journal_record_offset (rec_num, &offset) != 0)
    {
      nd_set_error (err, errlen, "journal offset overflow");
      return -1;
    }

  got = nd_pread_full (journal->fd, record, ND_JOURNAL_REC_SIZE, offset);
  if (got != (ssize_t) ND_JOURNAL_REC_SIZE)
    {
      nd_set_error (err, errlen, "journal read error");
      return -1;
    }

  return 0;
}

static int
nd_journal_write_record (struct nd_journal *journal, uint32_t rec_num,
                         uint32_t lba, uint8_t type, const uint8_t *payload,
                         char *err, size_t errlen)
{
  uint8_t record[ND_JOURNAL_REC_SIZE];
  off_t offset;
  ssize_t wrote;

  if (nd_journal_record_offset (rec_num, &offset) != 0)
    {
      nd_set_error (err, errlen, "journal offset overflow");
      return -1;
    }

  memset (record, 0, sizeof (record));
  nd_store_le32 (record + 0, lba);
  nd_store_le64 (record + 4, (uint64_t) nd_time_now_millis ());
  record[12] = type;
  memcpy (record + ND_JOURNAL_REC_HDR_SIZE, payload, ND_SECTOR_SIZE);

  wrote = nd_pwrite_full (journal->fd, record, sizeof (record), offset);
  if (wrote != (ssize_t) sizeof (record))
    {
      nd_set_error (err, errlen, "journal write error");
      return -1;
    }

  if (nd_fsync_fd (journal->fd, err, errlen) != 0)
    return -1;

  if (rec_num >= journal->journal_records)
    journal->journal_records = rec_num + 1;

  return 0;
}

static int
nd_journal_replay (const struct nd_journal *journal, struct nd_lba_map *out_map,
                   char *err, size_t errlen)
{
  uint8_t record[ND_JOURNAL_REC_SIZE];
  uint32_t rec_num;

  if (nd_map_init (out_map, 256) != 0)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  for (rec_num = 0; rec_num < journal->journal_records; rec_num++)
    {
      uint8_t type;
      uint32_t lba;

      if (nd_journal_read_record (journal, rec_num, record, err, errlen) != 0)
        {
          nd_map_free (out_map);
          return -1;
        }

      type = record[12];
      if (type != 0)
        continue;

      lba = nd_load_le32 (record + 0);
      if (nd_map_put (out_map, lba, rec_num) != 0)
        {
          nd_map_free (out_map);
          nd_set_error (err, errlen, "out of memory");
          return -1;
        }
    }

  return 0;
}

static int
nd_journal_read_mapfile (const char *journal_path, struct nd_lba_map *out_map)
{
  char map_path[ND_PATH_MAX];
  uint8_t header[4];
  int fd;
  ssize_t got;
  uint32_t expected;
  uint32_t i;

  if (snprintf (map_path, sizeof (map_path), "%s.map", journal_path)
      >= (int) sizeof (map_path))
    return -1;

  fd = open (map_path, O_RDONLY);
  if (fd < 0)
    return -1;

  got = nd_pread_full (fd, header, sizeof (header), 0);
  if (got != (ssize_t) sizeof (header))
    {
      close (fd);
      return -1;
    }

  expected = nd_load_le32 (header);
  if (nd_map_init (out_map, expected * 2 + 16) != 0)
    {
      close (fd);
      return -1;
    }

  for (i = 0; i < expected; i++)
    {
      uint8_t pair[8];
      off_t offset;

      offset = (off_t) (4 + (off_t) i * 8);
      got = nd_pread_full (fd, pair, sizeof (pair), offset);
      if (got != (ssize_t) sizeof (pair))
        {
          nd_map_free (out_map);
          close (fd);
          return -1;
        }

      if (nd_map_put (out_map, nd_load_le32 (pair), nd_load_le32 (pair + 4)) != 0)
        {
          nd_map_free (out_map);
          close (fd);
          return -1;
        }
    }

  close (fd);
  return 0;
}

static int
nd_journal_write_mapfile (const char *journal_path, const struct nd_lba_map *map,
                          char *err, size_t errlen)
{
  char map_path[ND_PATH_MAX];
  int fd;
  size_t i;
  uint8_t header[4];
  off_t offset;

  if (snprintf (map_path, sizeof (map_path), "%s.map", journal_path)
      >= (int) sizeof (map_path))
    {
      nd_set_error (err, errlen, "map path too long");
      return -1;
    }

  fd = open (map_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      nd_set_error (err, errlen, "error writing map file");
      return -1;
    }

  nd_store_le32 (header, (uint32_t) map->count);
  if (nd_pwrite_full (fd, header, sizeof (header), 0) != (ssize_t) sizeof (header))
    {
      close (fd);
      unlink (map_path);
      nd_set_error (err, errlen, "error writing map file");
      return -1;
    }

  offset = 4;
  for (i = 0; i < map->capacity; i++)
    {
      uint8_t pair[8];

      if (!map->slots[i].used)
        continue;

      nd_store_le32 (pair + 0, map->slots[i].lba);
      nd_store_le32 (pair + 4, map->slots[i].rec_num);
      if (nd_pwrite_full (fd, pair, sizeof (pair), offset) != (ssize_t) sizeof (pair))
        {
          close (fd);
          unlink (map_path);
          nd_set_error (err, errlen, "error writing map file");
          return -1;
        }
      offset += (off_t) sizeof (pair);
    }

  if (nd_fsync_fd (fd, err, errlen) != 0)
    {
      close (fd);
      unlink (map_path);
      return -1;
    }

  close (fd);
  if (nd_fsync_parent_dir (map_path, err, errlen) != 0)
    {
      unlink (map_path);
      return -1;
    }
  return 0;
}

static int
nd_journal_scan_checkpoints (struct nd_journal *journal, char *err, size_t errlen)
{
  uint8_t record[ND_JOURNAL_REC_SIZE];
  uint32_t rec_num;

  nd_journal_clear_checkpoints (journal);

  for (rec_num = 0; rec_num < journal->journal_records; rec_num++)
    {
      char tag[ND_JOURNAL_TAG_MAX + 1];

      if (nd_journal_read_record (journal, rec_num, record, err, errlen) != 0)
        return -1;

      if (record[12] != 1)
        continue;

      memcpy (tag, record + ND_JOURNAL_REC_HDR_SIZE, ND_JOURNAL_TAG_MAX);
      tag[ND_JOURNAL_TAG_MAX] = '\0';
      if (nd_journal_add_checkpoint (journal, rec_num,
                                     (int64_t) nd_load_le64 (record + 4),
                                     tag) != 0)
        {
          nd_set_error (err, errlen, "out of memory");
          nd_journal_clear_checkpoints (journal);
          return -1;
        }
    }

  return 0;
}

static int
nd_journal_write_checkpoint_mark (struct nd_journal *journal, const char *tag,
                                  char *err, size_t errlen)
{
  uint8_t payload[ND_SECTOR_SIZE];

  if (tag == NULL || tag[0] == '\0' || strlen (tag) > ND_JOURNAL_TAG_MAX)
    {
      nd_set_error (err, errlen, "Journal: Bad size for checkpoint tag");
      return -1;
    }

  memset (payload, 0, sizeof (payload));
  memcpy (payload, tag, strlen (tag));
  if (nd_journal_write_record (journal, journal->journal_records, 0, 1,
                               payload, err, errlen) != 0)
    return -1;

  return 0;
}

static struct nd_journal *
nd_journal_alloc (enum nd_journal_type type)
{
  struct nd_journal *journal;

  journal = (struct nd_journal *) calloc (1, sizeof (*journal));
  if (journal == NULL)
    return NULL;

  journal->type = type;
  journal->fd = -1;
  if (nd_map_init (&journal->map, 256) != 0)
    {
      free (journal);
      return NULL;
    }

  return journal;
}

int
nd_journal_open_goback (const char *image_path, struct nd_journal **out_journal,
                        char *err, size_t errlen)
{
  struct stat st;
  struct nd_journal *journal;
  struct nd_lba_map recovered;
  char journal_path[ND_PATH_MAX];

  *out_journal = NULL;
  journal = NULL;
  memset (&recovered, 0, sizeof (recovered));

  if (snprintf (journal_path, sizeof (journal_path), "%s.journal", image_path)
      >= (int) sizeof (journal_path))
    {
      nd_set_error (err, errlen, "journal path too long");
      return -1;
    }

  if (stat (journal_path, &st) != 0)
    {
      if (errno == ENOENT)
        return 0;
      nd_set_error (err, errlen, "error opening journal");
      return -1;
    }

  journal = nd_journal_alloc (ND_JOURNAL_GOBACK);
  if (journal == NULL)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  snprintf (journal->path, sizeof (journal->path), "%s", journal_path);
  journal->fd = open (journal_path, O_RDWR);
  if (journal->fd < 0)
    {
      nd_journal_destroy (journal);
      nd_set_error (err, errlen, "error opening journal");
      return -1;
    }

  if (fstat (journal->fd, &st) != 0)
    {
      nd_journal_destroy (journal);
      nd_set_error (err, errlen, "error opening journal");
      return -1;
    }

  if ((st.st_size % ND_JOURNAL_REC_SIZE) != 0)
    {
      nd_journal_destroy (journal);
      nd_set_error (err, errlen, "journal file is not a valid size");
      return -1;
    }

  journal->journal_records = (uint32_t) (st.st_size / ND_JOURNAL_REC_SIZE);

  nd_map_reset (&journal->map);
  if (nd_journal_read_mapfile (journal_path, &recovered) == 0)
    {
      nd_map_free (&journal->map);
      journal->map = recovered;
      memset (&recovered, 0, sizeof (recovered));
    }
  else if (nd_journal_replay (journal, &recovered, err, errlen) != 0)
    {
      nd_journal_destroy (journal);
      return -1;
    }
  else
    {
      nd_map_free (&journal->map);
      journal->map = recovered;
      memset (&recovered, 0, sizeof (recovered));
    }

  if (nd_journal_write_checkpoint_mark (journal, "Session Start", err, errlen) != 0)
    {
      nd_journal_destroy (journal);
      return -1;
    }

  if (nd_journal_scan_checkpoints (journal, err, errlen) != 0)
    {
      nd_journal_destroy (journal);
      return -1;
    }

  *out_journal = journal;
  return 0;
}

int
nd_journal_open_session_scoped (const char *image_path,
                                struct nd_journal **out_journal,
                                char *err, size_t errlen)
{
  struct nd_journal *journal;

  (void) image_path;

  *out_journal = NULL;
  journal = nd_journal_alloc (ND_JOURNAL_SESSION_SCOPED);
  if (journal == NULL)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  if (nd_make_temp_template (journal->path, sizeof (journal->path),
                             "cnetdrive-ss-") != 0)
    {
      nd_journal_destroy (journal);
      nd_set_error (err, errlen, "error opening session journal");
      return -1;
    }
  journal->fd = mkstemp (journal->path);
  if (journal->fd < 0)
    {
      nd_journal_destroy (journal);
      nd_set_error (err, errlen, "error opening session journal");
      return -1;
    }

  journal->delete_on_close = 1;
  *out_journal = journal;
  return 0;
}

int
nd_journal_close (struct nd_journal *journal, char *err, size_t errlen)
{
  int rc;

  if (journal == NULL)
    return 0;

  rc = 0;
  if (journal->type == ND_JOURNAL_GOBACK)
    {
      if (nd_journal_write_mapfile (journal->path, &journal->map, err, errlen) != 0)
        rc = -1;
    }

  if (journal->fd >= 0 && close (journal->fd) != 0)
    {
      if (rc == 0)
        nd_set_error (err, errlen, "error closing journal");
      rc = -1;
    }
  journal->fd = -1;

  if (journal->delete_on_close && journal->path[0] != '\0')
    {
      if (unlink (journal->path) != 0 && errno != ENOENT)
        {
          if (rc == 0)
            nd_set_error (err, errlen, "error closing journal");
          rc = -1;
        }
    }

  return rc;
}

void
nd_journal_destroy (struct nd_journal *journal)
{
  if (journal == NULL)
    return;

  if (journal->fd >= 0)
    close (journal->fd);
  if (journal->delete_on_close && journal->path[0] != '\0')
    unlink (journal->path);
  nd_map_free (&journal->map);
  nd_journal_clear_checkpoints (journal);
  free (journal);
}

enum nd_journal_type
nd_journal_type (const struct nd_journal *journal)
{
  return journal == NULL ? ND_JOURNAL_NONE : journal->type;
}

int
nd_journal_has_lba (const struct nd_journal *journal, uint32_t lba)
{
  if (journal == NULL)
    return 0;

  return nd_map_get (&journal->map, lba, NULL);
}

int
nd_journal_read_lba (const struct nd_journal *journal, uint32_t lba,
                     uint8_t *buffer, char *err, size_t errlen)
{
  uint8_t record[ND_JOURNAL_REC_SIZE];
  uint32_t rec_num;

  if (!nd_map_get (&journal->map, lba, &rec_num))
    {
      nd_set_error (err, errlen, "journal read error");
      return -1;
    }

  if (nd_journal_read_record (journal, rec_num, record, err, errlen) != 0)
    return -1;

  if (record[12] != 0 || nd_load_le32 (record + 0) != lba)
    {
      nd_set_error (err, errlen, "journal read error");
      return -1;
    }

  memcpy (buffer, record + ND_JOURNAL_REC_HDR_SIZE, ND_SECTOR_SIZE);
  return 0;
}

int
nd_journal_write_lba (struct nd_journal *journal, uint32_t lba,
                      const uint8_t *buffer, char *err, size_t errlen)
{
  uint32_t rec_num;

  if (journal->type == ND_JOURNAL_SESSION_SCOPED
      && nd_map_get (&journal->map, lba, &rec_num))
    {
      if (nd_journal_write_record (journal, rec_num, lba, 0, buffer,
                                   err, errlen) != 0)
        return -1;
      return 0;
    }

  rec_num = journal->journal_records;
  if (nd_journal_write_record (journal, rec_num, lba, 0, buffer,
                               err, errlen) != 0)
    return -1;

  if (nd_map_put (&journal->map, lba, rec_num) != 0)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  return 0;
}

int
nd_journal_mark_checkpoint (struct nd_journal *journal, const uint8_t *tag,
                            size_t tag_len, char *err, size_t errlen)
{
  char ctag[ND_JOURNAL_TAG_MAX + 1];
  int64_t written_ms;

  if (journal == NULL || journal->type != ND_JOURNAL_GOBACK)
    {
      nd_set_error (err, errlen, "Journal: feature not supported");
      return -1;
    }

  if (tag_len == 0 || tag_len > ND_JOURNAL_TAG_MAX)
    {
      nd_set_error (err, errlen, "Journal: Bad size for checkpoint tag");
      return -1;
    }

  memcpy (ctag, tag, tag_len);
  ctag[tag_len] = '\0';

  if (nd_journal_write_checkpoint_mark (journal, ctag, err, errlen) != 0)
    return -1;

  written_ms = nd_time_now_millis ();
  if (nd_journal_add_checkpoint (journal, journal->journal_records - 1,
                                 written_ms, ctag) != 0)
    {
      nd_set_error (err, errlen, "out of memory");
      return -1;
    }

  return 0;
}

int
nd_journal_goto_checkpoint (struct nd_journal *journal, const uint8_t *payload,
                            size_t payload_len, char *message,
                            size_t message_len)
{
  uint8_t req_type;
  uint8_t delete_marker;
  uint16_t tag_num;
  int target_index;
  int target_rec_num;
  int rc;
  struct nd_lba_map replayed;
  char map_path[ND_PATH_MAX];

  if (journal == NULL || journal->type != ND_JOURNAL_GOBACK)
    {
      nd_set_error (message, message_len, "Journal: feature not supported");
      return -1;
    }

  if (payload_len < 4)
    {
      nd_set_error (message, message_len, "Bad request parameters");
      return -1;
    }

  req_type = payload[0];
  delete_marker = payload[1];
  tag_num = nd_load_le16 (payload + 2);

  if (req_type > 1 || delete_marker > 1
      || (req_type == 0 && payload_len == 4)
      || (req_type == 1 && (payload_len != 4 || tag_num == 0)))
    {
      nd_set_error (message, message_len, "Bad request parameters");
      return -1;
    }

  target_index = -1;
  target_rec_num = -1;

  if (req_type == 0)
    {
      size_t i;
      char tag[ND_JOURNAL_TAG_MAX + 1];

      if (payload_len - 4 > ND_JOURNAL_TAG_MAX)
        {
          nd_set_error (message, message_len, "Checkpoint mark not found");
          return -1;
        }

      memcpy (tag, payload + 4, payload_len - 4);
      tag[payload_len - 4] = '\0';

      for (i = journal->checkpoint_count; i > 0; i--)
        {
          if (strcmp (journal->checkpoints[i - 1].tag, tag) == 0)
            {
              target_index = (int) i - 1;
              target_rec_num = (int) journal->checkpoints[i - 1].rec_num;
              break;
            }
        }

      if (target_index < 0)
        {
          nd_set_error (message, message_len,
                        "Checkpoint mark not found for tag %s", tag);
          return -1;
        }
    }
  else
    {
      if (tag_num < 1 || tag_num > journal->checkpoint_count)
        {
          nd_set_error (message, message_len,
                        "Checkpoint number out of range: %u", (unsigned) tag_num);
          return -1;
        }

      target_index = (int) tag_num - 1;
      target_rec_num = (int) journal->checkpoints[target_index].rec_num;
    }

  if (delete_marker)
    {
      nd_set_error (message, message_len,
                    "Going to checkpoint (num: %d, tag %s) and deleting checkpoint marker",
                    target_index + 1, journal->checkpoints[target_index].tag);
      target_index--;
      target_rec_num--;
    }
  else
    {
      nd_set_error (message, message_len,
                    "Going to checkpoint (num: %d, tag %s) and leaving checkpoint marker",
                    target_index + 1, journal->checkpoints[target_index].tag);
    }

  if (ftruncate (journal->fd,
                 (off_t) (uint64_t) (target_rec_num + 1) * ND_JOURNAL_REC_SIZE) != 0)
    {
      nd_set_error (message, message_len, "GotoCheckpoint failed");
      return -1;
    }

  if (nd_fsync_fd (journal->fd, message, message_len) != 0)
    return -1;

  journal->journal_records = (uint32_t) (target_rec_num + 1);
  if (target_index < 0)
    journal->checkpoint_count = 0;
  else
    journal->checkpoint_count = (size_t) target_index + 1;

  if (snprintf (map_path, sizeof (map_path), "%s.map", journal->path)
      < (int) sizeof (map_path))
    unlink (map_path);

  memset (&replayed, 0, sizeof (replayed));
  if (nd_journal_replay (journal, &replayed, message, message_len) != 0)
    return -1;

  nd_map_free (&journal->map);
  journal->map = replayed;

  rc = nd_journal_write_mapfile (journal->path, &journal->map, message, message_len);
  return rc;
}

int
nd_journal_list_checkpoints (struct nd_journal *journal, uint8_t *payload,
                             size_t *payload_len, char *err, size_t errlen)
{
  size_t used;
  size_t i;

  if (payload_len == NULL)
    return -1;

  *payload_len = 0;

  if (journal == NULL || journal->type != ND_JOURNAL_GOBACK)
    {
      nd_set_error (err, errlen, "Journal: feature not supported");
      return -1;
    }

  used = 0;
  for (i = journal->checkpoint_count; i > 0; i--)
    {
      char line[160];
      char ts[32];
      int line_len;

      if (nd_journal_format_timestamp (journal->checkpoints[i - 1].written_ms,
                                       ts, sizeof (ts)) != 0)
        {
          nd_set_error (err, errlen, "Journal failure");
          return -1;
        }

      line_len = snprintf (line, sizeof (line), "%5u %s %s\n",
                           (unsigned) i, ts, journal->checkpoints[i - 1].tag);
      if (line_len < 0)
        {
          nd_set_error (err, errlen, "Journal failure");
          return -1;
        }

      if (used + (size_t) line_len >= ND_JOURNAL_LIST_MAX)
        break;

      memcpy (payload + used, line, (size_t) line_len);
      used += (size_t) line_len;
    }

  *payload_len = used;
  return 0;
}
