#include "http_example_app.h"
#include <tlog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int example_port(const char *text, uint16_t *port) {
  unsigned value = 0u;
  if (*text == '\0') return SALTS_EINVAL;
  for (; *text != '\0'; ++text) {
    if (*text < '0' || *text > '9') return SALTS_EINVAL;
    const unsigned digit = (unsigned)(*text - '0');
    if (value > (UINT16_MAX - digit) / 10u) return SALTS_EINVAL;
    value = value * 10u + digit;
  }
  *port = (uint16_t)value;
  return SALTS_OK;
}

int main(int argc, char **argv) {
  /* Stable storage also preserves borrowed dependencies if stop times out. */
  static http_example_app app;
  http_example_config config = http_example_config_default();
  int status = SALTS_OK;
  uint16_t port = 0u;
  for (int index = 1; index < argc; ++index) {
    if (strcmp(argv[index], "--help") == 0) {
      puts("Usage: crpc_server_example [--port 0..65535] [--title TEXT] [--plugin PATH] [FILE]");
      return EXIT_SUCCESS;
    }
    if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
      status = example_port(argv[++index], &config.port);
    } else if (strcmp(argv[index], "--title") == 0 && index + 1 < argc) {
      config.title = argv[++index];
    } else if (strcmp(argv[index], "--plugin") == 0 && index + 1 < argc) {
      config.plugin_path = argv[++index];
    } else if (argv[index][0] != '-' && config.file_path == NULL) {
      config.file_path = argv[index];
    } else {
      status = SALTS_EINVAL;
    }
    if (status != SALTS_OK) {
      TLOG_ERROR("Invalid example arguments; use --help for usage.");
      return EXIT_FAILURE;
    }
  }
  status = http_example_configure(&app, &config);
  if (status == SALTS_OK) status = crpc_server_start(&app.rpc);
  if (status == SALTS_OK) status = crpc_server_port(&app.rpc, &port);
  if (status == SALTS_OK) {
    printf("Home page: http://127.0.0.1:%u/\n", (unsigned)port);
    printf("IDL endpoint: http://127.0.0.1:%u/api/add?left=3&right=4\n", (unsigned)port);
    printf("JSON-RPC endpoint: http://127.0.0.1:%u/rpc\n", (unsigned)port);
    if (config.file_path != NULL)
      printf("File endpoint: http://127.0.0.1:%u/file\n", (unsigned)port);
    puts("Press Enter to stop.");
    fflush(stdout);
    (void)getchar();
  }
  const int closed = http_example_close(&app);
  if (closed != SALTS_OK) TLOG_ERRORF("Example shutdown failed: status={}", closed);
  if (status != SALTS_OK) TLOG_ERRORF("Example startup failed: status={}", status);
  return status == SALTS_OK && closed == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
