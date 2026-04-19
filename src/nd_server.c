#include "nd_server.h"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>

/*
 * nd_server.c
 *
 * UDP server core.  This file owns session lifetime, request sequencing,
 * stock MS-DOS compatibility quirks, backend dispatch, and the main I/O
 * loop that keeps the service running.
 */

struct nd_session
{
  int active;
  uint16_t id;
  uint16_t protocol;
  uint16_t sequence;
  struct sockaddr_in peer;
  struct nd_command last_cmd;
  struct nd_backend *backend;
  uint8_t mac[6];
  char os_string[41];
  time_t started_at;
  time_t last_active;
};

struct nd_server_state
{
  int sockfd;
  struct nd_server_config config;
  struct nd_session *sessions;
};

static const uint8_t nd_version_max_command[] =
{
  0,
  ND_OP_WRITE_VERIFY,
  ND_OP_LIST_CHECKPOINTS
};

static volatile sig_atomic_t nd_stop_requested;

/*
 * Session helpers below keep protocol processing deterministic.  A
 * session is bound to one UDP peer, tracks the last command for retry
 * detection, and remembers the opened backend for later I/O commands.
 */
static void
nd_signal_handler (int sig)
{
  (void) sig;
  nd_stop_requested = 1;
}

static const char *
nd_peer_string (const struct sockaddr_in *peer, char *buffer, size_t buflen)
{
  char addrbuf[INET_ADDRSTRLEN];

  if (inet_ntop (AF_INET, &peer->sin_addr, addrbuf, sizeof (addrbuf)) == NULL)
    snprintf (addrbuf, sizeof (addrbuf), "0.0.0.0");

  snprintf (buffer, buflen, "%s:%u", addrbuf, (unsigned) ntohs (peer->sin_port));
  return buffer;
}

static struct nd_session *
nd_find_session (struct nd_server_state *state, uint16_t id)
{
  unsigned int i;

  for (i = 0; i < state->config.max_sessions; i++)
    {
      if (state->sessions[i].active && state->sessions[i].id == id)
        return &state->sessions[i];
    }

  return NULL;
}

static struct nd_session *
nd_find_free_session_slot (struct nd_server_state *state)
{
  unsigned int i;

  for (i = 0; i < state->config.max_sessions; i++)
    {
      if (!state->sessions[i].active)
        return &state->sessions[i];
    }

  return NULL;
}

static int
nd_same_peer (const struct sockaddr_in *left, const struct sockaddr_in *right)
{
  return left->sin_family == right->sin_family
    && left->sin_port == right->sin_port
    && left->sin_addr.s_addr == right->sin_addr.s_addr;
}

static int
nd_session_id_in_use (struct nd_server_state *state, uint16_t id)
{
  return nd_find_session (state, id) != NULL;
}

static uint16_t
nd_next_session_id (struct nd_server_state *state)
{
  unsigned int tries;

  for (tries = 0; tries < 64; tries++)
    {
      uint16_t id = (uint16_t) (32768u + ((unsigned int) rand () % 32768u));
      if (id != 0 && !nd_session_id_in_use (state, id))
        return id;
    }

  return 0;
}

static int
nd_send_response (int sockfd, const struct sockaddr_in *peer,
                  const struct nd_command *resp, const uint8_t *payload,
                  size_t payload_len)
{
  uint8_t packet[ND_MAX_PACKET];
  size_t packet_len;

  packet_len = nd_build_packet (packet, sizeof (packet), resp, payload,
                                payload_len);
  if (packet_len == 0)
    return -1;

  if (sendto (sockfd, packet, packet_len, 0, (const struct sockaddr *) peer,
              sizeof (*peer)) < 0)
    return -1;

  return 0;
}

static void
nd_close_session (struct nd_session *session, const char *reason)
{
  char peerbuf[64];

  if (!session->active)
    return;

  fprintf (stderr, "Session %u closed from %s: %s\n",
           (unsigned) session->id,
           nd_peer_string (&session->peer, peerbuf, sizeof (peerbuf)),
           reason);

  nd_backend_close (session->backend);
  memset (session, 0, sizeof (*session));
}

/*
 * DOS clients resend the same packet when they think one reply was lost.
 * The server accepts an exact retry of the current sequence number, but
 * rejects gaps or rewinds so one session cannot drift out of sync.
 */
static int
nd_update_sequence (struct nd_session *session, const struct nd_command *cmd,
                    int *retry, char *err, size_t errlen)
{
  *retry = 0;

  if (cmd->sequence == session->sequence)
    {
      if (nd_command_equal (&session->last_cmd, cmd))
        *retry = 1;
      return 0;
    }

  if (cmd->sequence == (uint16_t) (session->sequence + 1u))
    {
      session->sequence = cmd->sequence;
      return 0;
    }

  nd_set_error (err, errlen, "Bad sequence");
  return -1;
}

static int
nd_export_conflicts (const struct nd_server_state *state,
                     const struct nd_backend *backend)
{
  unsigned int i;

  for (i = 0; i < state->config.max_sessions; i++)
    {
      const struct nd_session *session = &state->sessions[i];

      if (!session->active)
        continue;

      if (session->backend->export_dev != 0 && session->backend->export_ino != 0
          && backend->export_dev != 0 && backend->export_ino != 0)
        {
          if (session->backend->export_dev != backend->export_dev
              || session->backend->export_ino != backend->export_ino)
            continue;
        }
      else if (strcmp (session->backend->export_path, backend->export_path) != 0)
        continue;

      if (nd_backend_requires_exclusive_open (session->backend)
          || nd_backend_requires_exclusive_open (backend))
        return 1;
    }

  return 0;
}

/*
 * Connect is the only command that creates server state.  Once the
 * export has been normalized and opened, the response returns enough DOS
 * disk metadata for the client to treat the remote export like a local
 * block device.
 */
static void
nd_handle_connect (struct nd_server_state *state, const struct sockaddr_in *peer,
                   const struct nd_command *cmd, const uint8_t *payload,
                   size_t payload_len)
{
  struct nd_connect_info info;
  struct nd_backend *backend;
  struct nd_session *session;
  struct nd_command resp;
  char export_name[ND_PATH_MAX];
  char errmsg[160];
  uint8_t connect_data[71];
  uint16_t flags;
  uint16_t session_id;
  char peerbuf[64];

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = cmd->session;
  resp.sequence = cmd->sequence;
  resp.operation = ND_OP_CONNECT;
  resp.result = 1;

  if (cmd->session != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "protocol error", 14);
      return;
    }

  if (nd_parse_connect_info (cmd, payload, payload_len, &info,
                             errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  if (nd_normalize_export_name (info.export_name, export_name,
                                sizeof (export_name),
                                errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  session = nd_find_free_session_slot (state);
  if (session == NULL)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "too many connections", 20);
      return;
    }

  if (nd_backend_open (state->config.export_root, export_name, &backend,
                       errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  if (nd_export_conflicts (state, backend))
    {
      nd_backend_close (backend);
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "export already open for write", 29);
      return;
    }

  session_id = nd_next_session_id (state);
  if (session_id == 0)
    {
      nd_backend_close (backend);
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "unable to allocate session", 26);
      return;
    }

  memset (session, 0, sizeof (*session));
  session->active = 1;
  session->id = session_id;
  session->protocol = cmd->version;
  session->sequence = cmd->sequence;
  session->peer = *peer;
  session->last_cmd = *cmd;
  session->backend = backend;
  memcpy (session->mac, info.mac, sizeof (info.mac));
  memcpy (session->os_string, info.os_string, sizeof (session->os_string) - 1);
  session->os_string[sizeof (session->os_string) - 1] = '\0';
  session->started_at = time (NULL);
  session->last_active = session->started_at;

  flags = nd_backend_connect_flags (backend);
  nd_store_le32 (connect_data + 0, backend->size_bytes);
  nd_store_le16 (connect_data + 4, flags);
  memcpy (connect_data + 6, backend->boot_sector_prefix, 64);
  connect_data[70] = backend->media_descriptor;

  resp.session = session_id;
  resp.result = 0;
  nd_send_response (state->sockfd, peer, &resp, connect_data, sizeof (connect_data));

  fprintf (stderr, "Session %u opened from %s export=%s kind=%s readonly=%s\n",
           (unsigned) session_id,
           nd_peer_string (peer, peerbuf, sizeof (peerbuf)),
           backend->export_name,
           nd_backend_kind_string (backend),
           (flags & ND_BACKEND_CONNECT_FLAG_READONLY) ? "yes" : "no");
}

/*
 * Read, write, and checkpoint handlers all follow the same pattern:
 * validate the sequence, reject out-of-range media access, call the
 * backend, then echo a reply that matches the incoming command version.
 */
static void
nd_send_error_for_command (struct nd_server_state *state,
                           const struct sockaddr_in *peer,
                           const struct nd_command *cmd,
                           const char *message)
{
  struct nd_command resp;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = cmd->session;
  resp.sequence = cmd->sequence;
  resp.operation = cmd->operation;
  resp.start_sector = cmd->start_sector;
  resp.sector_count = cmd->sector_count;
  resp.result = 1;

  nd_send_response (state->sockfd, peer, &resp,
                    (const uint8_t *) message, strlen (message));
}

static void
nd_handle_disconnect (struct nd_server_state *state, struct nd_session *session,
                      const struct sockaddr_in *peer,
                      const struct nd_command *cmd)
{
  struct nd_command resp;
  char errmsg[64];
  int retry;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = session->id;
  resp.sequence = cmd->sequence;
  resp.operation = ND_OP_DISCONNECT;
  resp.result = 1;

  if (nd_update_sequence (session, cmd, &retry, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  (void) retry;
  session->last_cmd = *cmd;
  resp.result = 0;
  nd_send_response (state->sockfd, peer, &resp, (const uint8_t *) "OK", 2);
  nd_close_session (session, "client disconnected");
}

static void
nd_handle_read (struct nd_server_state *state, struct nd_session *session,
                const struct sockaddr_in *peer, const struct nd_command *cmd)
{
  struct nd_command resp;
  char errmsg[128];
  uint8_t payload[ND_SECTOR_SIZE * ND_MAX_SECTORS_PER_OP];
  uint64_t end_byte;
  int retry;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = session->id;
  resp.sequence = cmd->sequence;
  resp.operation = ND_OP_READ;
  resp.start_sector = cmd->start_sector;
  resp.sector_count = cmd->sector_count;
  resp.result = 1;

  if (nd_update_sequence (session, cmd, &retry, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  if (cmd->sector_count < 1 || cmd->sector_count > ND_MAX_SECTORS_PER_OP)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "Read error: bad sector count", 28);
      return;
    }

  end_byte = ((uint64_t) cmd->start_sector + cmd->sector_count) * ND_SECTOR_SIZE;
  if (end_byte > session->backend->size_bytes)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "Attempted read past end of image", 32);
      return;
    }

  if (nd_backend_read (session->backend, cmd->start_sector, cmd->sector_count,
                       payload, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  session->last_cmd = *cmd;
  session->last_active = time (NULL);
  resp.result = 0;
  nd_send_response (state->sockfd, peer, &resp, payload,
                    (size_t) cmd->sector_count * ND_SECTOR_SIZE);
  (void) retry;
}

static void
nd_handle_write (struct nd_server_state *state, struct nd_session *session,
                 const struct sockaddr_in *peer, const struct nd_command *cmd,
                 const uint8_t *payload, size_t payload_len)
{
  struct nd_command resp;
  char errmsg[128];
  uint8_t verify[ND_SECTOR_SIZE * ND_MAX_SECTORS_PER_OP];
  uint64_t end_byte;
  int retry;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = session->id;
  resp.sequence = cmd->sequence;
  resp.operation = cmd->operation;
  resp.start_sector = cmd->start_sector;
  resp.sector_count = cmd->sector_count;
  resp.result = 1;

  if (nd_update_sequence (session, cmd, &retry, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  if (cmd->sector_count < 1 || cmd->sector_count > ND_MAX_SECTORS_PER_OP)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "Bad sector count", 16);
      return;
    }

  if (payload_len != (size_t) cmd->sector_count * ND_SECTOR_SIZE)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "Bad write payload", 17);
      return;
    }

  end_byte = ((uint64_t) cmd->start_sector + cmd->sector_count) * ND_SECTOR_SIZE;
  if (end_byte > session->backend->size_bytes)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "Attempted write past end of image", 33);
      return;
    }

  if (nd_backend_write (session->backend, cmd->start_sector, cmd->sector_count,
                        payload, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  session->last_cmd = *cmd;
  session->last_active = time (NULL);
  resp.result = 0;

  if (cmd->operation == ND_OP_WRITE_VERIFY)
    {
      if (nd_backend_read (session->backend, cmd->start_sector, cmd->sector_count,
                           verify, errmsg, sizeof (errmsg)) != 0)
        {
          resp.result = 1;
          nd_send_response (state->sockfd, peer, &resp,
                            (const uint8_t *) errmsg, strlen (errmsg));
          return;
        }

      if (memcmp (verify, payload,
                  (size_t) cmd->sector_count * ND_SECTOR_SIZE) != 0)
        {
          resp.result = 1;
          nd_send_response (state->sockfd, peer, &resp,
                            (const uint8_t *) "Write verify error: miscompare",
                            30);
          return;
        }

      nd_send_response (state->sockfd, peer, &resp, verify,
                        (size_t) cmd->sector_count * ND_SECTOR_SIZE);
      return;
    }

  nd_send_response (state->sockfd, peer, &resp, NULL, 0);
  (void) retry;
}

static void
nd_handle_unknown (struct nd_server_state *state, struct nd_session *session,
                   const struct sockaddr_in *peer,
                   const struct nd_command *cmd)
{
  struct nd_command resp;
  char errmsg[64];
  int retry;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = session->id;
  resp.sequence = cmd->sequence;
  resp.operation = cmd->operation;
  resp.start_sector = cmd->start_sector;
  resp.sector_count = cmd->sector_count;
  resp.result = 1;

  if (nd_update_sequence (session, cmd, &retry, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  session->last_cmd = *cmd;
  session->last_active = time (NULL);
  nd_send_response (state->sockfd, peer, &resp,
                    (const uint8_t *) "Unknown command", 15);
  (void) retry;
}

static void
nd_handle_mark_checkpoint (struct nd_server_state *state,
                           struct nd_session *session,
                           const struct sockaddr_in *peer,
                           const struct nd_command *cmd,
                           const uint8_t *payload, size_t payload_len)
{
  struct nd_command resp;
  char errmsg[160];
  int retry;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = session->id;
  resp.sequence = cmd->sequence;
  resp.operation = ND_OP_MARK_CHECKPOINT;
  resp.result = 1;

  if (nd_update_sequence (session, cmd, &retry, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  if (nd_backend_mark_checkpoint (session->backend, payload, payload_len,
                                  errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  session->last_cmd = *cmd;
  session->last_active = time (NULL);
  resp.result = 0;
  nd_send_response (state->sockfd, peer, &resp, (const uint8_t *) "OK", 2);
  (void) retry;
}

static void
nd_handle_goto_checkpoint (struct nd_server_state *state,
                           struct nd_session *session,
                           const struct sockaddr_in *peer,
                           const struct nd_command *cmd,
                           const uint8_t *payload, size_t payload_len)
{
  struct nd_command resp;
  char message[256];
  int retry;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = session->id;
  resp.sequence = cmd->sequence;
  resp.operation = ND_OP_GOTO_CHECKPOINT;
  resp.result = 1;

  if (nd_update_sequence (session, cmd, &retry, message, sizeof (message)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) message, strlen (message));
      return;
    }

  if (retry)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *)
                        "Packet loss and retry detected, command not executed again.",
                        59);
      return;
    }

  if (nd_backend_goto_checkpoint (session->backend, payload, payload_len,
                                  message, sizeof (message)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) message, strlen (message));
      return;
    }

  session->last_cmd = *cmd;
  session->last_active = time (NULL);
  resp.result = 0;
  nd_send_response (state->sockfd, peer, &resp,
                    (const uint8_t *) message, strlen (message));
}

static void
nd_handle_list_checkpoints (struct nd_server_state *state,
                            struct nd_session *session,
                            const struct sockaddr_in *peer,
                            const struct nd_command *cmd)
{
  struct nd_command resp;
  char errmsg[160];
  uint8_t payload[ND_MAX_PAYLOAD];
  size_t payload_len;
  int retry;

  memset (&resp, 0, sizeof (resp));
  resp.version = cmd->version;
  resp.session = session->id;
  resp.sequence = cmd->sequence;
  resp.operation = ND_OP_LIST_CHECKPOINTS;
  resp.result = 1;

  if (nd_update_sequence (session, cmd, &retry, errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  if (nd_backend_list_checkpoints (session->backend, payload, &payload_len,
                                   errmsg, sizeof (errmsg)) != 0)
    {
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) errmsg, strlen (errmsg));
      return;
    }

  session->last_cmd = *cmd;
  session->last_active = time (NULL);
  resp.result = 0;
  nd_send_response (state->sockfd, peer, &resp, payload, payload_len);
  (void) retry;
}

/*
 * Packet dispatch enforces protocol version limits, session ownership,
 * and command availability before any backend call is attempted.  That
 * preserves mixed v1/v2 DOS behavior while still rejecting stray peers.
 */
static void
nd_process_packet (struct nd_server_state *state, const struct sockaddr_in *peer,
                   const uint8_t *packet, size_t packet_len)
{
  struct nd_command cmd;
  const uint8_t *payload;
  size_t payload_len;
  char errmsg[160];
  struct nd_session *session;

  if (nd_parse_command (packet, packet_len, &cmd, &payload, &payload_len,
                        errmsg, sizeof (errmsg)) != 0)
    return;

  if (cmd.version < ND_PROTOCOL_MIN_VERSION
      || cmd.version > ND_PROTOCOL_MAX_VERSION)
    {
      struct nd_command resp;

      memset (&resp, 0, sizeof (resp));
      resp.version = cmd.version;
      resp.session = cmd.session;
      resp.sequence = cmd.sequence;
      resp.operation = ND_OP_CONNECT;
      resp.result = 1;
      nd_send_response (state->sockfd, peer, &resp,
                        (const uint8_t *) "Bad protocol version", 20);
      return;
    }

  if (cmd.operation == 0
      || (cmd.session == 0 && cmd.operation != ND_OP_CONNECT))
    {
      nd_send_error_for_command (state, peer, &cmd, "Protocol error");
      return;
    }

  if (cmd.operation == ND_OP_CONNECT)
    {
      nd_handle_connect (state, peer, &cmd, payload, payload_len);
      return;
    }

  session = nd_find_session (state, cmd.session);
  if (session == NULL || !session->active)
    {
      nd_send_error_for_command (state, peer, &cmd, "Bad session");
      return;
    }

  if (!nd_same_peer (&session->peer, peer))
    {
      nd_send_error_for_command (state, peer, &cmd, "Bad session");
      return;
    }

  if (cmd.operation > nd_version_max_command[cmd.version])
    {
      nd_handle_unknown (state, session, peer, &cmd);
      return;
    }

  switch (cmd.operation)
    {
    case ND_OP_DISCONNECT:
      nd_handle_disconnect (state, session, peer, &cmd);
      break;

    case ND_OP_READ:
      nd_handle_read (state, session, peer, &cmd);
      break;

    case ND_OP_WRITE:
    case ND_OP_WRITE_VERIFY:
      nd_handle_write (state, session, peer, &cmd, payload, payload_len);
      break;

    case ND_OP_MARK_CHECKPOINT:
      nd_handle_mark_checkpoint (state, session, peer, &cmd, payload,
                                 payload_len);
      break;

    case ND_OP_GOTO_CHECKPOINT:
      nd_handle_goto_checkpoint (state, session, peer, &cmd, payload,
                                 payload_len);
      break;

    case ND_OP_LIST_CHECKPOINTS:
      nd_handle_list_checkpoints (state, session, peer, &cmd);
      break;

    default:
      nd_handle_unknown (state, session, peer, &cmd);
      break;
    }
}

static void
nd_reap_timeouts (struct nd_server_state *state)
{
  unsigned int i;
  time_t now;

  if (state->config.session_timeout_seconds == 0)
    return;

  now = time (NULL);
  for (i = 0; i < state->config.max_sessions; i++)
    {
      struct nd_session *session = &state->sessions[i];

      if (!session->active)
        continue;

      if ((unsigned int) (now - session->last_active)
          > state->config.session_timeout_seconds)
        nd_close_session (session, "timeout");
    }
}

/*
 * The main loop is intentionally small: wait on the UDP socket, process
 * one datagram at a time, and reap idle sessions once per select tick.
 * That keeps the service portable across the target Unix-like systems.
 */
int
nd_server_run (const struct nd_server_config *config)
{
  struct nd_server_state state;
  struct sockaddr_in addr;
  struct sigaction sa;
  int optval;

  memset (&state, 0, sizeof (state));
  state.config = *config;

  state.sessions = (struct nd_session *) calloc (config->max_sessions,
                                                 sizeof (*state.sessions));
  if (state.sessions == NULL)
    {
      fprintf (stderr, "Out of memory allocating sessions\n");
      return 1;
    }

  state.sockfd = socket (AF_INET, SOCK_DGRAM, 0);
  if (state.sockfd < 0)
    {
      perror ("socket");
      free (state.sessions);
      return 1;
    }

  optval = 1;
  setsockopt (state.sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof (optval));

  memset (&addr, 0, sizeof (addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_ANY);
  addr.sin_port = htons (config->port);

  if (bind (state.sockfd, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      perror ("bind");
      close (state.sockfd);
      free (state.sessions);
      return 1;
    }

  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = nd_signal_handler;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);

  srand ((unsigned) time (NULL));

  fprintf (stderr, "cnetdrive listening on UDP port %u with export root %s\n",
           (unsigned) config->port, config->export_root);

  while (!nd_stop_requested)
    {
      fd_set readfds;
      struct timeval tv;
      int ready;

      FD_ZERO (&readfds);
      FD_SET (state.sockfd, &readfds);
      tv.tv_sec = 1;
      tv.tv_usec = 0;

      ready = select (state.sockfd + 1, &readfds, NULL, NULL, &tv);
      if (ready < 0)
        {
          if (errno == EINTR)
            continue;
          perror ("select");
          break;
        }

      if (ready > 0 && FD_ISSET (state.sockfd, &readfds))
        {
          uint8_t packet[ND_MAX_PACKET];
          struct sockaddr_in peer;
          socklen_t peer_len;
          ssize_t got;

          peer_len = sizeof (peer);
          got = recvfrom (state.sockfd, packet, sizeof (packet), 0,
                          (struct sockaddr *) &peer, &peer_len);
          if (got > 0)
            nd_process_packet (&state, &peer, packet, (size_t) got);
        }

      nd_reap_timeouts (&state);
    }

  {
    unsigned int i;
    for (i = 0; i < state.config.max_sessions; i++)
      nd_close_session (&state.sessions[i], "server shutdown");
  }

  close (state.sockfd);
  free (state.sessions);
  fprintf (stderr, "cnetdrive shut down\n");
  return 0;
}
