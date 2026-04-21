#ifndef ND_PROTOCOL_H
#define ND_PROTOCOL_H

#include "nd_common.h"

enum nd_operation
{
  ND_OP_CONNECT = 1,
  ND_OP_DISCONNECT = 2,
  ND_OP_READ = 3,
  ND_OP_WRITE = 4,
  ND_OP_WRITE_VERIFY = 5,
  ND_OP_MARK_CHECKPOINT = 6,
  ND_OP_GOTO_CHECKPOINT = 7,
  ND_OP_LIST_CHECKPOINTS = 8
};

#define ND_PROTOCOL_MIN_VERSION 1U
#define ND_PROTOCOL_MAX_VERSION 3U
#define ND_COMMAND_HEADER_LEN_V1 14U
#define ND_COMMAND_HEADER_LEN_V3 16U

struct nd_command
{
  uint16_t version;
  uint16_t session;
  uint16_t sequence;
  uint8_t operation;
  uint8_t result;
  uint32_t start_sector;
  uint16_t sector_count;
  uint16_t header_extra;
};

struct nd_connect_info
{
  uint8_t mac[6];
  uint16_t client_type;
  uint16_t block_size;
  char os_string[41];
  char export_name[ND_PATH_MAX];
};

int nd_parse_command (const uint8_t *packet, size_t packet_len,
                      struct nd_command *cmd, const uint8_t **payload,
                      size_t *payload_len, char *err, size_t errlen);
size_t nd_build_packet (uint8_t *packet, size_t packet_len,
                        const struct nd_command *cmd, const uint8_t *payload,
                        size_t payload_len);
int nd_command_equal (const struct nd_command *left,
                      const struct nd_command *right);
int nd_parse_connect_info (const struct nd_command *cmd, const uint8_t *payload,
                           size_t payload_len, struct nd_connect_info *info,
                           char *err, size_t errlen);
int nd_normalize_export_name (const char *raw, char *normalized,
                              size_t normalized_len, char *err,
                              size_t errlen);

#endif
