/*
 * main.c - command line entry point for the NetDrive server.
 */

#include "nd_server.h"

static void
nd_usage (const char *progname)
{
  fprintf (stderr,
           "Usage: %s [serve] [options]\n"
           "  -root DIR        Export root directory (default: current directory)\n"
           "  -export_dir DIR  Alias for -root\n"
           "  -image_dir DIR   Alias for -root\n"
           "  -port N          UDP port (default: 2002)\n"
           "  -max_sessions N  Maximum concurrent sessions (default: %u)\n"
           "  -timeout N       Session timeout in seconds (default: 0)\n"
           "  -help            Show this help text\n",
           progname, ND_DEFAULT_MAX_SESSIONS);
}

static int
nd_parse_uint (const char *text, unsigned int *value)
{
  char *endp;
  unsigned long parsed;

  parsed = strtoul (text, &endp, 10);
  if (text[0] == '\0' || *endp != '\0')
    return -1;

  *value = (unsigned int) parsed;
  return 0;
}

int
main (int argc, char **argv)
{
  struct nd_server_config config;
  int i;

  memset (&config, 0, sizeof (config));
  strncpy (config.export_root, ".", sizeof (config.export_root) - 1);
  config.port = 2002;
  config.max_sessions = ND_DEFAULT_MAX_SESSIONS;
  config.session_timeout_seconds = 0;

  for (i = 1; i < argc; i++)
    {
      unsigned int value;

      if (strcmp (argv[i], "serve") == 0)
        continue;

      if (strcmp (argv[i], "-help") == 0 || strcmp (argv[i], "--help") == 0)
        {
          nd_usage (argv[0]);
          return 0;
        }

      if ((strcmp (argv[i], "-root") == 0
           || strcmp (argv[i], "-export_dir") == 0
           || strcmp (argv[i], "-image_dir") == 0)
          && i + 1 < argc)
        {
          strncpy (config.export_root, argv[++i],
                   sizeof (config.export_root) - 1);
          continue;
        }

      if (strcmp (argv[i], "-port") == 0 && i + 1 < argc)
        {
          if (nd_parse_uint (argv[++i], &value) != 0 || value > 65535u)
            {
              fprintf (stderr, "Invalid port: %s\n", argv[i]);
              return 1;
            }
          config.port = (unsigned short) value;
          continue;
        }

      if (strcmp (argv[i], "-max_sessions") == 0 && i + 1 < argc)
        {
          if (nd_parse_uint (argv[++i], &value) != 0 || value == 0)
            {
              fprintf (stderr, "Invalid max_sessions: %s\n", argv[i]);
              return 1;
            }
          config.max_sessions = value;
          continue;
        }

      if (strcmp (argv[i], "-timeout") == 0 && i + 1 < argc)
        {
          if (nd_parse_uint (argv[++i], &value) != 0)
            {
              fprintf (stderr, "Invalid timeout: %s\n", argv[i]);
              return 1;
            }
          config.session_timeout_seconds = value;
          continue;
        }

      fprintf (stderr, "Unknown option: %s\n", argv[i]);
      nd_usage (argv[0]);
      return 1;
    }

  return nd_server_run (&config);
}
