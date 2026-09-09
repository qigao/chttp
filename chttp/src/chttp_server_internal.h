#ifndef CHTTP_SERVER_INTERNAL_H
#define CHTTP_SERVER_INTERNAL_H

#include <chttp/chttp.h>

#include <stddef.h>

typedef struct chttp_server_parser {
  void *impl;
} chttp_server_parser;

enum { CHTTP_SERVER_REQUEST_DEFERRED = 1 };

typedef int (*chttp_server_parser_request_fn)(void *user, const chttp_server_request_view *request);

typedef enum chttp_server_parser_headers_action {
  CHTTP_SERVER_HEADERS_CONTINUE = 0,
  CHTTP_SERVER_HEADERS_STOP
} chttp_server_parser_headers_action;

typedef int (*chttp_server_parser_headers_fn)(
    void *user, const chttp_server_request_view *request,
    chttp_server_parser_headers_action *out_action);
typedef int (*chttp_server_parser_continue_fn)(void *user);
typedef int (*chttp_server_parser_body_open_fn)(void *user,
                                                const chttp_server_request_view *request,
                                                chttp_body_sink *out_sink);
typedef void (*chttp_server_parser_body_close_fn)(void *user, chttp_body_sink *sink, int status);
typedef int (*chttp_server_parser_buffer_grow_fn)(void *context, unsigned char **buffer,
                                                  size_t *capacity, size_t required,
                                                  size_t limit, size_t preserve_size);
typedef void (*chttp_server_parser_buffer_release_fn)(void *context, unsigned char *buffer,
                                                       size_t capacity);

typedef enum chttp_server_parser_upgrade_action {
  CHTTP_SERVER_UPGRADE_IGNORE = 0,
  CHTTP_SERVER_UPGRADE_STOP
} chttp_server_parser_upgrade_action;

typedef int (*chttp_server_parser_upgrade_fn)(void *user, const chttp_server_request_view *request,
                                              chttp_server_parser_upgrade_action *out_action,
                                              unsigned int *out_http_status);

typedef struct chttp_server_parser_config {
  size_t max_target_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_body_bytes;
  chttp_server_parser_request_fn on_request;
  chttp_server_parser_headers_fn on_headers;
  chttp_server_parser_continue_fn on_continue;
  chttp_server_parser_body_open_fn on_body_open;
  chttp_server_parser_body_close_fn on_body_close;
  chttp_server_parser_upgrade_fn on_upgrade;
  chttp_server_parser_buffer_grow_fn buffer_grow;
  chttp_server_parser_buffer_release_fn buffer_release;
  void *buffer_context;
  void *user;
} chttp_server_parser_config;

int chttp_server_parser_init(chttp_server_parser *parser, const chttp_server_parser_config *config);
int chttp_server_parser_execute(chttp_server_parser *parser, const void *data, size_t size,
                                unsigned int *out_http_status);
int chttp_server_parser_execute_consumed(chttp_server_parser *parser, const void *data, size_t size,
                                         size_t *out_consumed, unsigned int *out_http_status);
int chttp_server_parser_resume(chttp_server_parser *parser);
int chttp_server_parser_reset(chttp_server_parser *parser);
void chttp_server_parser_destroy(chttp_server_parser *parser);

#endif /* CHTTP_SERVER_INTERNAL_H */
