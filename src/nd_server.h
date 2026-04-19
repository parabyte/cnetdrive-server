/*
 * nd_server.h - server configuration and top-level run entry point.
 */

#ifndef ND_SERVER_H
#define ND_SERVER_H

#include "nd_backend.h"
#include "nd_protocol.h"

#include <netinet/in.h>

/*
 * Top-level server configuration.
 */
struct nd_server_config
{
  char export_root[ND_PATH_MAX];
  unsigned short port;
  unsigned int max_sessions;
  unsigned int session_timeout_seconds;
};

/*
 * Run the UDP NetDrive server until shutdown.
 */
int nd_server_run (const struct nd_server_config *config);

#endif
