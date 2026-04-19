/*
 * nd_protocol.c - packet parsing and protocol payload helpers.
 */

#include "nd_protocol.h"

#include <ctype.h>

int
nd_parse_command (const uint8_t *packet, size_t packet_len,
                  struct nd_command *cmd, const uint8_t **payload,
                  size_t *payload_len, char *err, size_t errlen)
{
  if (packet == NULL || cmd == NULL || payload == NULL || payload_len == NULL)
    {
      nd_set_error (err, errlen, "internal protocol error");
      return -1;
    }

  if (packet_len < ND_COMMAND_HEADER_LEN)
    {
      nd_set_error (err, errlen, "packet too short");
      return -1;
    }

  cmd->version = nd_load_le16 (packet + 0);
  cmd->session = nd_load_le16 (packet + 2);
  cmd->sequence = nd_load_le16 (packet + 4);
  cmd->operation = packet[6];
  cmd->result = packet[7];
  cmd->start_sector = nd_load_le32 (packet + 8);
  cmd->sector_count = nd_load_le16 (packet + 12);

  *payload = packet + ND_COMMAND_HEADER_LEN;
  *payload_len = packet_len - ND_COMMAND_HEADER_LEN;
  return 0;
}

size_t
nd_build_packet (uint8_t *packet, size_t packet_len,
                 const struct nd_command *cmd, const uint8_t *payload,
                 size_t payload_len)
{
  if (packet == NULL || cmd == NULL)
    return 0;

  if (packet_len < ND_COMMAND_HEADER_LEN + payload_len)
    return 0;

  nd_store_le16 (packet + 0, cmd->version);
  nd_store_le16 (packet + 2, cmd->session);
  nd_store_le16 (packet + 4, cmd->sequence);
  packet[6] = cmd->operation;
  packet[7] = cmd->result;
  nd_store_le32 (packet + 8, cmd->start_sector);
  nd_store_le16 (packet + 12, cmd->sector_count);

  if (payload_len > 0 && payload != NULL)
    memcpy (packet + ND_COMMAND_HEADER_LEN, payload, payload_len);

  return ND_COMMAND_HEADER_LEN + payload_len;
}

int
nd_command_equal (const struct nd_command *left, const struct nd_command *right)
{
  if (left == NULL || right == NULL)
    return 0;

  return left->version == right->version
    && left->session == right->session
    && left->sequence == right->sequence
    && left->operation == right->operation
    && left->result == right->result
    && left->start_sector == right->start_sector
    && left->sector_count == right->sector_count;
}

static int
nd_copy_cstring_field (char *dst, size_t dstlen, const uint8_t *src,
                       size_t srclen)
{
  size_t i;

  for (i = 0; i < srclen; i++)
    {
      if (src[i] == '\0')
        {
          if (i >= dstlen)
            return -1;
          memcpy (dst, src, i);
          dst[i] = '\0';
          return 0;
        }
    }

  return -1;
}

int
nd_parse_connect_info (const struct nd_command *cmd, const uint8_t *payload,
                       size_t payload_len, struct nd_connect_info *info,
                       char *err, size_t errlen)
{
  if (cmd == NULL || payload == NULL || info == NULL)
    {
      nd_set_error (err, errlen, "internal connect parse error");
      return -1;
    }

  memset (info, 0, sizeof (*info));

  if (cmd->version == 1)
    {
      if (payload_len < 8)
        {
          nd_set_error (err, errlen, "protocol error");
          return -1;
        }

      memcpy (info->mac, payload, 6);
      if (nd_copy_cstring_field (info->export_name, sizeof (info->export_name),
                                 payload + 6, payload_len - 6) != 0)
        {
          nd_set_error (err, errlen, "protocol error");
          return -1;
        }
      return 0;
    }

  if (cmd->version == 2)
    {
      if (payload_len < 52)
        {
          nd_set_error (err, errlen, "protocol error");
          return -1;
        }

      memcpy (info->mac, payload, 6);
      info->client_type = nd_load_le16 (payload + 6);
      info->block_size = nd_load_le16 (payload + 8);

      if (nd_copy_cstring_field (info->os_string, sizeof (info->os_string),
                                 payload + 10, 40) != 0)
        {
          nd_set_error (err, errlen, "protocol error");
          return -1;
        }

      if (nd_copy_cstring_field (info->export_name, sizeof (info->export_name),
                                 payload + 50, payload_len - 50) != 0)
        {
          nd_set_error (err, errlen, "protocol error");
          return -1;
        }
      return 0;
    }

  nd_set_error (err, errlen, "bad protocol version");
  return -1;
}

int
nd_normalize_export_name (const char *raw, char *normalized,
                          size_t normalized_len, char *err, size_t errlen)
{
  size_t in_i;
  size_t out_i;
  char last;

  if (raw == NULL || normalized == NULL || normalized_len == 0)
    {
      nd_set_error (err, errlen, "bad export name");
      return -1;
    }

  if (raw[0] == '\0')
    {
      nd_set_error (err, errlen, "empty export name");
      return -1;
    }

  out_i = 0;
  last = '\0';

  for (in_i = 0; raw[in_i] != '\0'; in_i++)
    {
      char ch;

      ch = raw[in_i];
      if (ch == '\\')
        ch = '/';

      if (out_i + 1 >= normalized_len)
        {
          nd_set_error (err, errlen, "export name too long");
          return -1;
        }

      if (in_i == 0)
        {
          if (!(isalnum ((unsigned char) ch) || ch == '_'))
            {
              nd_set_error (err, errlen, "bad characters in export name");
              return -1;
            }
        }
      else if (!(isalnum ((unsigned char) ch)
                 || ch == '_' || ch == '-' || ch == '.' || ch == '/'))
        {
          nd_set_error (err, errlen, "bad characters in export name");
          return -1;
        }

      if ((ch == '.' || ch == '/') && last == ch)
        {
          nd_set_error (err, errlen, "bad characters in export name");
          return -1;
        }

      normalized[out_i++] = ch;
      last = ch;
    }

  normalized[out_i] = '\0';

  if (normalized[0] == '/' || strstr (normalized, "..") != NULL)
    {
      nd_set_error (err, errlen, "bad characters in export name");
      return -1;
    }

  return 0;
}
